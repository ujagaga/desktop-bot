#include <esp_log.h>
#include "camera.h"
#include "comms.h"
#include "http_client.h"
#include "http_server.h"
#include "logger.h"
#include "wifi_connection.h"

void setup() {
  // UART0 belongs to the S3 link; runtime SDK logs must not become commands.
  esp_log_level_set("*", ESP_LOG_NONE);
  LOG_init();
  LOG_append("MiniBot CAM starting");
  COMMS_Init();
  CAM_Init();
  HTTPC_init();
  WIFIC_init();
  HTTPSRV_init();
}
void loop() {
  COMMS_ProcessSleep();
  WIFIC_process();
  HTTPC_process();
  delay(50);
}
