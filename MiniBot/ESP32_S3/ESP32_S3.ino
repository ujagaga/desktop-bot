// Spotpear ESP32-S3-LCD-1.3 (ESP32-S3R8, ST7789 240x240 SPI) - battery level display.
// Board: https://spotpear.com/wiki/ESP32-S3-1.3-inch-LCD-ST7789-240x240-Display-Screen.html

#include "battery.h"
#include "lcd.h"
#include "comms.h"
#include "motor.h"
#include "wifi_connection.h"
#include "clock.h"
#include "http_client.h"
#include "http_server.h"
#include "gyro.h"
#include "audio.h"
#include "wake_word.h"
#include "voice.h"
#include "screen.h"
#include "faces.h"
#include <esp_ota_ops.h>

// Keep a freshly installed OTA image pending until it has run for a minute;
// a crash or reset before that makes the bootloader roll back to the previous image.
#define APP_CONFIRM_MS 60000UL

extern "C" bool verifyRollbackLater() {
  return true;
}


void setup() {
  LCD_Init();
  COMMS_Init();
  BATT_Init();
  if (!AUDIO_Init()) Serial.println("AUDIO: I2S init failed");
  if (!WAKEWORD_Init()) Serial.println("WAKEWORD: init failed");
  if (!VOICE_Init()) Serial.println("VOICE: init failed");
  HTTP_CLIENT_Init();
  WIFI_Init();
  MOTOR_Init();
  HTTP_SERVER_Init();
  SCREEN_StartWake();
}

void loop() {
  static bool appConfirmed = false;
  if (!appConfirmed && millis() > APP_CONFIRM_MS) {
    esp_ota_mark_app_valid_cancel_rollback();
    appConfirmed = true;
  }
  COMMS_Poll();
  CLOCK_Process();
  SCREEN_Process();
  MOTOR_Process();
  uint8_t taps = GYRO_PollTaps();
  if (taps) Serial.printf("GYRO: %u tap(s)\n", taps);
  if (taps >= GYRO_GetSleepTapThreshold()) COMMS_Execute("sleep", Serial);
  HTTP_SERVER_Process();
  HTTP_CLIENT_Process();
  if (WAKEWORD_TakeDetection()) {
    FACE_Show(4);  // 04_surprised: listening
    VOICE_Start();
  }
  VOICE_Process();
}
