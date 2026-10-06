#include "gateway.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <Preferences.h>
#include <WebSocketsClient.h>
#include <WiFi.h>
#include <freertos/semphr.h>
#include "camera.h"
#include "comms.h"
#include "logger.h"

extern const uint8_t bundleStart[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t bundleEnd[] asm("_binary_x509_crt_bundle_end");

static SemaphoreHandle_t mutex = nullptr;
static String baseUrl, apiKey;
static portMUX_TYPE sessionMux = portMUX_INITIALIZER_UNLOCKED;
static char pendingSession[33] = "";

// Persistent link for the server's drive page: frames out while asked, drive commands in.
static WebSocketsClient driveLink;
static bool linkStarted = false, streaming = false;
static uint32_t configVersion = 0, linkVersion = 0;
static unsigned long lastFrameMs = 0;
static constexpr unsigned long FRAME_INTERVAL_MS = 100;

static void onLinkEvent(WStype_t type, uint8_t *payload, size_t length) {
  if (type == WStype_DISCONNECTED) streaming = false;
  if (type == WStype_CONNECTED) LOG_append("Gateway: drive link connected");
  if (type != WStype_TEXT) return;
  JsonDocument doc;
  if (deserializeJson(doc, payload, length)) return;
  const char *kind = doc["type"] | "";
  if (!strcmp(kind, "stream")) {
    streaming = doc["on"] | false;
  } else if (!strcmp(kind, "drive")) {
    const char *dir = doc["dir"] | "";
    int speed = doc["speed"] | -1;
    if (strlen(dir) != 1 || !strchr("fblr", dir[0]) || speed < 0 || speed > 100) return;
    char command[24];
    snprintf(command, sizeof(command), "drive %c %d", dir[0], speed);
    COMMS_SendCommand(command);  // Queue full: dropped; the page repeats every 100 ms.
  }
}

// https://host[:port] -> host, port; http:// is plain.
static bool startLink(const String &base, const String &key) {
  bool secure = base.startsWith("https://");
  String rest = base.substring(secure ? 8 : 7);
  int colon = rest.indexOf(':');
  String host = colon < 0 ? rest : rest.substring(0, colon);
  uint16_t port = colon < 0 ? (secure ? 443 : 80) : rest.substring(colon + 1).toInt();
  if (!host.length() || !port) return false;
  driveLink.onEvent(onLinkEvent);
  driveLink.setExtraHeaders(("X-API-Key: " + key).c_str());
  driveLink.setReconnectInterval(5000);
  driveLink.enableHeartbeat(30000, 10000, 2);
  if (secure) driveLink.beginSslWithBundle(host.c_str(), port, "/robot/camera", bundleStart, bundleEnd - bundleStart, "");
  else driveLink.begin(host.c_str(), port, "/robot/camera", "");
  return true;
}

static void processLink() {
  xSemaphoreTake(mutex, portMAX_DELAY);
  String base = baseUrl, key = apiKey;
  uint32_t version = configVersion;
  xSemaphoreGive(mutex);
  if (linkStarted && version != linkVersion) {
    driveLink.disconnect();
    linkStarted = streaming = false;
  }
  if (!linkStarted) {
    if (!base.length() || !key.length() || WiFi.status() != WL_CONNECTED) return;
    linkStarted = startLink(base, key);
    linkVersion = version;
    return;
  }
  driveLink.loop();
  if (!streaming || !driveLink.isConnected() || millis() - lastFrameMs < FRAME_INTERVAL_MS) return;
  lastFrameMs = millis();
  camera_fb_t *frame = CAM_Capture();
  if (!frame) return;
  driveLink.sendBIN(frame->buf, frame->len);
  CAM_Dispose(frame);
}

void GATEWAY_init() {
  mutex = xSemaphoreCreateMutex();
  Preferences prefs;
  if (prefs.begin("camGateway", true)) {
    baseUrl = prefs.getString("url", "");
    apiKey = prefs.getString("key", "");
    prefs.end();
  }
}

bool GATEWAY_configure(const String &url, const String &key) {
  String base = url;
  base.trim();
  while (base.endsWith("/")) base.remove(base.length() - 1);
  if (!(base.startsWith("https://") || base.startsWith("http://")) || base.length() > 200 || key.length() > 128)
    return false;
  Preferences prefs;
  if (!prefs.begin("camGateway", false)) return false;
  bool ok = prefs.putString("url", base) && (!key.length() || prefs.putString("key", key));
  prefs.end();
  if (!ok) return false;
  xSemaphoreTake(mutex, portMAX_DELAY);
  baseUrl = base;
  if (key.length()) apiKey = key;
  ++configVersion;
  xSemaphoreGive(mutex);
  LOG_append("Gateway: settings saved");
  return true;
}

String GATEWAY_status() {
  JsonDocument doc;
  xSemaphoreTake(mutex, portMAX_DELAY);
  doc["url"] = baseUrl;
  doc["key_set"] = apiKey.length() > 0;
  xSemaphoreGive(mutex);
  String value;
  serializeJson(doc, value);
  return value;
}

bool GATEWAY_requestSnapshot(const char *session) {
  size_t length = strlen(session);
  if (!length || length >= sizeof(pendingSession)) return false;
  for (size_t i = 0; i < length; ++i) if (!isalnum((unsigned char)session[i])) return false;
  portENTER_CRITICAL(&sessionMux);
  strcpy(pendingSession, session);
  portEXIT_CRITICAL(&sessionMux);
  return true;
}

void GATEWAY_process() {
  processLink();
  char session[sizeof(pendingSession)];
  portENTER_CRITICAL(&sessionMux);
  strcpy(session, pendingSession);
  pendingSession[0] = 0;
  portEXIT_CRITICAL(&sessionMux);
  if (!session[0]) return;

  xSemaphoreTake(mutex, portMAX_DELAY);
  String target = baseUrl + "/snapshot?session=" + session, key = apiKey;
  bool configured = baseUrl.length() && apiKey.length();
  xSemaphoreGive(mutex);
  if (!configured || WiFi.status() != WL_CONNECTED) {
    LOG_append("Gateway: snapshot skipped; gateway not configured or Wi-Fi offline");
    return;
  }
  // Copy the frame so the camera is free for the stream during the upload.
  camera_fb_t *frame = CAM_Capture();
  if (!frame) {
    LOG_append("Gateway: snapshot capture failed");
    return;
  }
  size_t length = frame->len;
  uint8_t *jpeg = (uint8_t *)ps_malloc(length);
  if (jpeg) memcpy(jpeg, frame->buf, length);
  CAM_Dispose(frame);
  if (!jpeg) {
    LOG_append("Gateway: snapshot buffer allocation failed");
    return;
  }

  NetworkClientSecure secureClient;
  NetworkClient plainClient;
  bool secure = target.startsWith("https://");
  if (secure) {
    secureClient.setCACertBundle(bundleStart, bundleEnd - bundleStart);
    secureClient.setHandshakeTimeout(15);
  }
  HTTPClient http;
  int status = -1;
  if (secure ? http.begin(secureClient, target) : http.begin(plainClient, target)) {
    http.setConnectTimeout(10000);
    http.setTimeout(15000);
    http.addHeader("X-API-Key", key);
    http.addHeader("Content-Type", "image/jpeg");
    status = http.POST(jpeg, length);
    http.end();
  }
  free(jpeg);
  LOG_printf("Gateway: snapshot %u bytes, HTTP %d", (unsigned)length, status);
}
