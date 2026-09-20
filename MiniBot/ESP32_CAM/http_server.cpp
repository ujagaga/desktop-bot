#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_http_server.h>
#include <atomic>
#include <errno.h>
#include <limits.h>
#include "camera.h"
#include "comms.h"
#include "config.h"
#include "http_client.h"
#include "logger.h"
#include "wifi_connection.h"
#include "ui_home_html.h"
#include "ui_api_html.h"
static httpd_handle_t commands = nullptr, stream = nullptr;
static std::atomic<bool> stopping{false};
static esp_err_t text(httpd_req_t *req, const char *value) {
  httpd_resp_set_type(req, "text/plain");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, value, HTTPD_RESP_USE_STRLEN);
}
static esp_err_t json(httpd_req_t *req, const String &value) {
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, value.c_str(), value.length());
}
static esp_err_t unavailable(httpd_req_t *req) {
  httpd_resp_set_status(req, "503 Service Unavailable");
  return text(req, "Camera unavailable; see logs");
}
static esp_err_t indexHandler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, index_html, HTTPD_RESP_USE_STRLEN);
}
static esp_err_t apiHandler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, api_html, HTTPD_RESP_USE_STRLEN);
}
static esp_err_t captureHandler(httpd_req_t *req) {
  camera_fb_t *frame = CAM_Capture();
  if (!frame) return unavailable(req);
  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  esp_err_t result = httpd_resp_send(req, (const char *)frame->buf, frame->len);
  CAM_Dispose(frame);
  return result;
}
static esp_err_t streamHandler(httpd_req_t *req) {
  if (!CAM_isInitialized()) return unavailable(req);
  httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=minibotframe");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  while (!stopping.load()) {
    camera_fb_t *frame = CAM_Capture();
    if (!frame) break;
    char header[96];
    int length = snprintf(header, sizeof(header), "\r\n--minibotframe\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n", (unsigned)frame->len);
    esp_err_t result = httpd_resp_send_chunk(req, header, length);
    if (result == ESP_OK) result = httpd_resp_send_chunk(req, (const char *)frame->buf, frame->len);
    CAM_Dispose(frame);
    if (result != ESP_OK) return result;
    vTaskDelay(pdMS_TO_TICKS(30));
  }
  return httpd_resp_send_chunk(req, nullptr, 0);
}
static esp_err_t statusHandler(httpd_req_t *req) { return json(req, CAM_Status()); }
static esp_err_t configHandler(httpd_req_t *req) {
  char query[128], name[32], value[16];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
      httpd_query_key_value(query, "var", name, sizeof(name)) != ESP_OK ||
      httpd_query_key_value(query, "val", value, sizeof(value)) != ESP_OK)
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Expected var and val");
  char *end; errno = 0;
  long number = strtol(value, &end, 10);
  if (!*value || *end || errno || number < INT_MIN || number > INT_MAX || !CAM_Set(name, number))
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid camera setting or camera unavailable");
  return text(req, "OK");
}
static esp_err_t saveHandler(httpd_req_t *req) {
  if (!CAM_Save()) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot save camera settings");
  return text(req, "OK camera settings saved");
}
static esp_err_t logsHandler(httpd_req_t *req) {
  char *buffer = (char *)malloc(LOG_CAPACITY);
  if (!buffer) return httpd_resp_send_500(req);
  LOG_get(buffer, LOG_CAPACITY);
  esp_err_t result = text(req, buffer); free(buffer); return result;
}
static esp_err_t consoleHandler(httpd_req_t *req) {
  if (req->method == HTTP_GET) {
    JsonDocument doc;
    doc["ready"] = COMMS_IsReady();
    doc["transcript"] = COMMS_ConsoleRead();
    String value; serializeJson(doc, value); return json(req, value);
  }
  if (!req->content_len || req->content_len > 127)
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Expected one ASCII command, maximum 127 bytes");
  char command[128]; size_t received = 0;
  bool nonSpace = false;
  while (received < req->content_len) {
    int n = httpd_req_recv(req, command + received, req->content_len - received);
    if (n <= 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Incomplete command body");
    received += n;
  }
  for (size_t i = 0; i < received; ++i) {
    if ((unsigned char)command[i] < 32 || (unsigned char)command[i] > 126)
      return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Expected one printable ASCII command without CR/LF");
    if (command[i] != ' ') nonSpace = true;
  }
  if (!nonSpace) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty command");
  command[received] = 0;
  if (!COMMS_IsReady()) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    return text(req, "UART unavailable; see device log");
  }
  if (!COMMS_SendCommand(command)) {
    httpd_resp_set_status(req, "409 Conflict");
    return text(req, "UART command queue full; try again");
  }
  httpd_resp_set_status(req, "202 Accepted");
  return text(req, "Queued for UART transmission; this does not confirm an S3 reply.");
}
static esp_err_t infoHandler(httpd_req_t *req) {
  JsonDocument doc;
  doc["version"] = FIRMWARE_VERSION;
  doc["invalid_version"] = HTTPC_invalidVersion();
  doc["ota_busy"] = HTTPC_fwUpdateInProgress();
  doc["ota_pending"] = HTTPC_checkPending();
  doc["ota_found_version"] = HTTPC_foundVersion();
  doc["ota_status"] = HTTPC_updateStatus();
  doc["free_heap"] = ESP.getFreeHeap();
  String value; serializeJson(doc, value); return json(req, value);
}
static esp_err_t wifiHandler(httpd_req_t *req) {
  if (req->method == HTTP_GET) return json(req, WIFIC_status());
  if (!req->content_len || req->content_len > 512)
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Expected JSON body up to 512 bytes");
  char body[513]; size_t received = 0;
  while (received < req->content_len) {
    int n = httpd_req_recv(req, body + received, req->content_len - received);
    if (n <= 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Incomplete request body");
    received += n;
  }
  body[received] = 0;
  JsonDocument doc;
  if (deserializeJson(doc, body, received) || !doc["mode"].is<String>() ||
      !doc["ssid"].is<String>() || !doc["password"].is<String>())
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Expected mode, ssid and password strings");
  if (!WIFIC_configure(doc["mode"].as<String>(), doc["ssid"].as<String>(), doc["password"].as<String>()))
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid Wi-Fi settings or save failed");
  return text(req, "OK saved; reconnect using the new station address or setup AP");
}
static esp_err_t otaHandler(httpd_req_t *req) {
  if (!HTTPC_requestCheck()) {
    httpd_resp_set_status(req, "409 Conflict"); return text(req, "OTA requires station Wi-Fi; check may already be queued or busy");
  }
  httpd_resp_set_status(req, "202 Accepted");
  return text(req, "OK firmware check scheduled; see logs");
}
static void route(httpd_handle_t server, const char *uri, httpd_method_t method, esp_err_t (*handler)(httpd_req_t *)) {
  httpd_uri_t entry = {}; entry.uri = uri; entry.method = method; entry.handler = handler;
  if (httpd_register_uri_handler(server, &entry) != ESP_OK) LOG_printf("ERR registering %s", uri);
}
void HTTPSRV_stop() {
  stopping.store(true);
  // Join handlers before deinitializing the sensor or releasing any frame buffers.
  if (stream) { httpd_stop(stream); stream = nullptr; }
  if (commands) { httpd_stop(commands); commands = nullptr; }
}
void HTTPSRV_init() {
  if (commands || stream) return;
  stopping.store(false);
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.max_uri_handlers = 14; config.stack_size = 8192;
  config.recv_wait_timeout = 3; config.send_wait_timeout = 3;
  config.lru_purge_enable = true;
  if (httpd_start(&commands, &config) == ESP_OK) {
    route(commands, "/", HTTP_GET, indexHandler);
    route(commands, "/api", HTTP_GET, apiHandler);
    route(commands, "/status", HTTP_GET, statusHandler);
    route(commands, "/config", HTTP_GET, configHandler);
    route(commands, "/capture", HTTP_GET, captureHandler);
    route(commands, "/api/camera/save", HTTP_POST, saveHandler);
    route(commands, "/api/logs", HTTP_GET, logsHandler);
    route(commands, "/api/console", HTTP_GET, consoleHandler);
    route(commands, "/api/console", HTTP_POST, consoleHandler);
    route(commands, "/api/info", HTTP_GET, infoHandler);
    route(commands, "/api/wifi", HTTP_GET, wifiHandler);
    route(commands, "/api/wifi", HTTP_POST, wifiHandler);
    route(commands, "/api/ota", HTTP_POST, otaHandler);
  } else LOG_append("ERR HTTP server startup failed");
  config.server_port = 81; config.ctrl_port += 1; config.stack_size = 4096;
  if (httpd_start(&stream, &config) == ESP_OK) route(stream, "/stream", HTTP_GET, streamHandler);
  else LOG_append("ERR stream server startup failed");
  LOG_append("HTTP UI on port 80; camera stream on port 81");
}
