#include "camera.h"
#include "http_client.h"
#include "http_server.h"
#include "logger.h"
#include "wifi_connection.h"

void setup() {
  Serial.begin(115200);
  LOG_init();
  LOG_append("MiniBot CAM starting");
  CAM_Init();
  HTTPC_init();
  WIFIC_init();
  HTTPSRV_init();
}
void loop() {
  WIFIC_process();
  HTTPC_process();
  delay(50);
}
