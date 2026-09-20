#include "comms.h"
#include "config.h"
#include "logger.h"
#include <WiFi.h>
#include <esp_sleep.h>
#include <driver/rtc_io.h>
#include "camera.h"
#include "http_server.h"
#include "http_client.h"
#include <strings.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

std::atomic<uint32_t> COMMS_peerWifiIP{0};
static TaskHandle_t rxTask = nullptr;
static QueueHandle_t commandQueue = nullptr;
static std::atomic<bool> sleepRequested{false}, rxPaused{false};
static portMUX_TYPE consoleMux = portMUX_INITIALIZER_UNLOCKED;
static char transcript[4096];
static size_t transcriptHead = 0, transcriptCount = 0;

static void consoleAppend(const char *text) {
  portENTER_CRITICAL(&consoleMux);
  while (*text) {
    transcript[transcriptHead] = *text++;
    transcriptHead = (transcriptHead + 1) % sizeof(transcript);
    if (transcriptCount < sizeof(transcript)) ++transcriptCount;
  }
  portEXIT_CRITICAL(&consoleMux);
}

bool COMMS_IsReady() { return rxTask != nullptr; }

bool COMMS_SendCommand(const char *command) {
  if (!COMMS_IsReady() || sleepRequested.load() || !command || !*command || strlen(command) > 127) return false;
  bool nonSpace = false;
  for (const char *p = command; *p; ++p) {
    if ((unsigned char)*p < 32 || (unsigned char)*p > 126) return false;
    if (*p != ' ') nonSpace = true;
  }
  if (!nonSpace) return false;
  char queued[128] = {};
  strcpy(queued, command);
  return xQueueSend(commandQueue, queued, 0) == pdPASS;
}

String COMMS_ConsoleRead() {
  // Allocate outside the critical section and off the HTTP task's stack.
  char *snapshot = (char *)malloc(sizeof(transcript) + 1);
  if (!snapshot) return String("ERR cannot allocate console snapshot\n");
  portENTER_CRITICAL(&consoleMux);
  size_t count = transcriptCount;
  for (size_t i = 0; i < count; ++i)
    snapshot[i] = transcript[(transcriptHead + sizeof(transcript) - count + i) % sizeof(transcript)];
  portEXIT_CRITICAL(&consoleMux);
  snapshot[count] = 0;
  String result(snapshot);
  free(snapshot);
  return result;
}

// Accept only a complete dotted-decimal IPv4 line, never OK/ERR or log text.
static bool parseIP(const char *line, uint32_t &address) {
  address = 0;
  for (unsigned octet = 0; octet < 4; ++octet) {
    unsigned value = 0, digits = 0;
    while (*line >= '0' && *line <= '9') {
      value = value * 10 + (*line++ - '0');
      if (++digits > 3 || value > 255) return 0;
    }
    if (!digits) return 0;
    address = (address << 8) | value;
    if (octet < 3 && *line++ != '.') return 0;
  }
  if (*line || address == 0xffffffffUL) return 0;
  return true;
}

// Dispatch recognized command families only: other lines may be replies to
// commands sent from the console, so never answer them with another error.
static bool dispatchCommand(const char *line) {
  // S3 help entries are indented text, including "sleep", not requests.
  if (*line == ' ' || *line == '\t') return false;
  char command[16], argument[16], addressText[16], extra[2];
  int fields = sscanf(line, "%15s %15s %15s %1s", command, argument, addressText, extra);
  if (fields < 1) return false;
  if (!strcasecmp(command, "sleep")) {
    if (fields != 1) Serial.println("ERR CAM SLEEP expected sleep without arguments");
    else if (HTTPC_fwUpdateInProgress()) Serial.println("ERR CAM SLEEP firmware update busy");
    else if (digitalRead(CAM_WAKE_GPIO)) Serial.println("ERR CAM SLEEP wake line is HIGH");
    else sleepRequested.store(true); // Main loop joins HTTP/camera work before ACK.
    return true;
  }
  if (!strcasecmp(command, "report")) {
    uint32_t address;
    if (fields != 3 || strcasecmp(argument, "ip") || !parseIP(addressText, address)) {
      Serial.println("ERR report ip <s3 ip addr>");
      consoleAppend("\n[reply] ERR report ip <s3 ip addr>\n");
      return true;
    }
    COMMS_peerWifiIP.store(address); // Refresh on every report, including disconnects.
    LOG_printf("S3 reported IP: %s", addressText);
    String ownIP = (WiFi.status() == WL_CONNECTED ? WiFi.localIP() : IPAddress(0, 0, 0, 0)).toString();
    Serial.println(ownIP);
    Serial.println("OK");
    consoleAppend("\n[reply] ");
    consoleAppend(ownIP.c_str());
    consoleAppend("\nOK\n");
    return true;
  }
  return false;
}

static void receiveTask(void *) {
  char line[128];
  size_t length = 0;
  bool discard = false;
  for (;;) {
    if (sleepRequested.load()) {
      rxPaused.store(true);
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    rxPaused.store(false);
    // Bound each batch so continuous input still allows the task to yield.
    for (unsigned count = 0; count < 256 && Serial.available(); ++count) {
      int byte = Serial.read();
      if (byte < 0) break;
      if (byte != '\r') {
        char visible[5] = {};
        if (byte == '\n' || byte == '\t' || (byte >= 32 && byte <= 126)) visible[0] = (char)byte;
        else snprintf(visible, sizeof(visible), "\\x%02X", (unsigned)byte & 255U);
        consoleAppend(visible);
      }
      if (byte == '\r' || byte == '\n') {
        if (!discard && length) {
          line[length] = 0;
          dispatchCommand(line);
        }
        length = 0;
        discard = false;
      } else if (byte < 32 || byte > 126 || length == sizeof(line) - 1) {
        discard = true; // Ignore the entire damaged/oversized line.
      } else if (!discard) {
        line[length++] = (char)byte;
      }
    }
    if (sleepRequested.load()) continue;
    char command[128];
    if (xQueueReceive(commandQueue, command, 0) == pdPASS) {
      consoleAppend("\n> ");
      consoleAppend(command);
      consoleAppend("\n");
      Serial.print(command);
      Serial.print("\n");
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

bool COMMS_Init() {
  if (rxTask) return true;
  rtc_gpio_hold_dis((gpio_num_t)CAM_POWER_DOWN_GPIO);
  rtc_gpio_deinit((gpio_num_t)CAM_POWER_DOWN_GPIO);
  rtc_gpio_deinit((gpio_num_t)CAM_WAKE_GPIO);
  pinMode(CAM_WAKE_GPIO, INPUT_PULLDOWN);
  if (!commandQueue) commandQueue = xQueueCreate(1, 128);
  if (!commandQueue) {
    LOG_append("ERR UART command queue allocation failed");
    return false;
  }
  Serial.setRxBufferSize(1024);
  Serial.begin(COMMS_BAUD, SERIAL_8N1, COMMS_RX_PIN, COMMS_TX_PIN);
  if (!Serial) {
    LOG_append("ERR command UART startup failed");
    return false;
  }
  Serial.setDebugOutput(false);
  Serial.print("\n"); // Terminate any partial boot output seen by the S3.
  // No camera locks, HTTP handlers, or loop() work in the RX task.
  if (xTaskCreate(receiveTask, "commsRx", 3072, nullptr, 2, &rxTask) != pdPASS) {
    Serial.end();
    LOG_append("ERR UART RX task startup failed");
    return false;
  }
  LOG_append("UART ready; waiting for S3 report ip");
  return true;
}

String COMMS_GetPeerWifiIP() {
  uint32_t address = COMMS_peerWifiIP.load();
  if (!address) return String();
  char text[16];
  snprintf(text, sizeof(text), "%u.%u.%u.%u", (unsigned)(address >> 24),
           (unsigned)((address >> 16) & 255), (unsigned)((address >> 8) & 255),
           (unsigned)(address & 255));
  return String(text);
}

// Main-loop only: never tear down the camera or HTTP server from the RX task.
void COMMS_ProcessSleep() {
  if (!sleepRequested.load() || !rxPaused.load()) return;
  const gpio_num_t wakePin = (gpio_num_t)CAM_WAKE_GPIO;
  if (HTTPC_fwUpdateInProgress() || digitalRead(CAM_WAKE_GPIO) ||
      esp_sleep_enable_ext0_wakeup(wakePin, 1) != ESP_OK) {
    Serial.println("ERR CAM SLEEP unavailable or wake line HIGH");
    sleepRequested.store(false);
    return;
  }
  if (!HTTPSRV_PrepareSleep()) {
    Serial.println("ERR CAM SLEEP client connected");
    consoleAppend("\n[reply] ERR CAM SLEEP client connected\n");
    LOG_append("CAM sleep refused: client connected");
    sleepRequested.store(false);
    return;
  }
  LOG_append("CAM preparing for deep sleep");
  HTTPSRV_stop();
  CAM_Stop();
  // A timed-out S3 keeps the wake line HIGH. Restart if it cancelled while
  // handlers were shutting down; never enter sleep with a stale request.
  if (digitalRead(CAM_WAKE_GPIO)) {
    Serial.println("ERR CAM SLEEP cancelled");
    Serial.flush();
    ESP.restart();
    return;
  }
  WiFi.mode(WIFI_OFF);
  rtc_gpio_pullup_dis(wakePin);
  rtc_gpio_pulldown_en(wakePin);
  // Keep the OV2640 powered down throughout deep sleep.
  const gpio_num_t powerPin = (gpio_num_t)CAM_POWER_DOWN_GPIO;
  rtc_gpio_init(powerPin);
  rtc_gpio_set_direction(powerPin, RTC_GPIO_MODE_OUTPUT_ONLY);
  rtc_gpio_set_level(powerPin, 1);
  rtc_gpio_hold_en(powerPin);
  Serial.println("CAM SLEEP READY");
  Serial.flush();
  esp_deep_sleep_start();
}
