// Spotpear ESP32-S3-LCD-1.3 (ESP32-S3R8, ST7789 240x240 SPI) - battery level display.
// Board: https://spotpear.com/wiki/ESP32-S3-1.3-inch-LCD-ST7789-240x240-Display-Screen.html

#include "battery.h"
#include "LCD.h"
#include "comms.h"
#include "motor.h"

void setup() {
  initLCD();
  initComms();
  initMotors();
}

void loop() {
  commsPoll();
  BATT_process();
  motorProcess();
}
