#include "comms.h"
#include "config.h"
#include "logger.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/timers.h>

std::atomic<uint32_t> COMMS_peerWifiIP{0};
static std::atomic<bool> pollDue{true};
static TimerHandle_t pollTimer = nullptr;
static TaskHandle_t rxTask = nullptr;

// Accept only a complete dotted-decimal IPv4 line, never OK/ERR or log text.
static uint32_t parseIP(const char *line) {
  uint32_t address = 0;
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
  return address;
}

static void receiveTask(void *) {
  char line[128];
  size_t length = 0;
  bool discard = false, requested = false;
  for (;;) {
    // Bound each batch so continuous input still allows the task to yield.
    for (unsigned count = 0; count < 256 && Serial.available(); ++count) {
      int byte = Serial.read();
      if (byte < 0) break;
      if (byte == '\r' || byte == '\n') {
        if (!discard && length && requested && !COMMS_peerWifiIP.load()) {
          line[length] = 0;
          uint32_t address = parseIP(line);
          if (address) {
            COMMS_peerWifiIP.store(address);
            LOG_printf("S3 Wi-Fi IP: %s", COMMS_GetPeerWifiIP().c_str());
          }
        }
        length = 0;
        discard = false;
      } else if (byte < 32 || byte > 126 || length == sizeof(line) - 1) {
        discard = true; // Ignore the entire damaged/oversized line.
      } else if (!discard) {
        line[length++] = (char)byte;
      }
    }
    if (COMMS_peerWifiIP.load()) {
      // Retry a stop if the timer command queue was temporarily full.
      if (xTimerIsTimerActive(pollTimer)) xTimerStop(pollTimer, 0);
    } else if (pollDue.exchange(false)) {
      Serial.print("wifi ip\n");
      requested = true;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

bool COMMS_Init() {
  if (rxTask) return true;
  Serial.setRxBufferSize(1024);
  Serial.begin(COMMS_BAUD, SERIAL_8N1, COMMS_RX_PIN, COMMS_TX_PIN);
  if (!Serial) {
    LOG_append("ERR command UART startup failed");
    return false;
  }
  Serial.setDebugOutput(false);
  Serial.print("\n"); // Terminate any partial boot output seen by the S3.
  pollDue.store(true);
  // Timer callbacks never perform serial I/O or wait for a response.
  pollTimer = xTimerCreate("s3WifiIP", pdMS_TO_TICKS(COMMS_IP_POLL_MS), pdTRUE,
                          nullptr, [](TimerHandle_t) { pollDue.store(true); });
  if (!pollTimer || xTimerStart(pollTimer, 0) != pdPASS) {
    if (pollTimer) xTimerDelete(pollTimer, portMAX_DELAY);
    pollTimer = nullptr;
    Serial.end();
    LOG_append("ERR UART IP poll timer startup failed");
    return false;
  }
  // No camera locks, HTTP handlers, or loop() work in the RX task.
  if (xTaskCreate(receiveTask, "commsRx", 3072, nullptr, 2, &rxTask) != pdPASS) {
    xTimerDelete(pollTimer, portMAX_DELAY);
    pollTimer = nullptr;
    Serial.end();
    LOG_append("ERR UART RX task startup failed");
    return false;
  }
  LOG_append("UART ready; discovering S3 Wi-Fi IP");
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
