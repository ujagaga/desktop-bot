// Spotpear ESP32-S3-LCD-1.3 (ESP32-S3R8, ST7789 240x240 SPI) - battery level display.
// Board: https://spotpear.com/wiki/ESP32-S3-1.3-inch-LCD-ST7789-240x240-Display-Screen.html

#include "battery.h"
#include "LCD.h"
#include "comms.h"
#include "motor.h"
#include "wifi_connection.h"
#include "clock.h"
#include "http_client.h"
#include "http_server.h"
#include "touch_button.h"


void setup() {
  LCD_Init();
  COMMS_Init();
  HTTP_CLIENT_Init();
  WIFI_Init();
  MOTOR_Init();
  HTTP_SERVER_Init();
  TOUCH_Init();
}

void loop() {
  COMMS_Poll();
  CLOCK_Process();
  BATT_process();
  MOTOR_Process();
  TOUCH_Process();
  HTTP_SERVER_Process();
  HTTP_CLIENT_Process();
}
