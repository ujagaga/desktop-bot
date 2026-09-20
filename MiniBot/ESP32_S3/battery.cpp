#include <Arduino.h>
#include <WiFi.h>
#include <stdio.h>
#include "battery.h"
#include "lcd.h"
#include "clock.h"


// Battery: GPIO6, fed by an onboard 100K/100K divider off the battery rail,
// so the ADC reads half of the actual battery voltage.
#define BAT_ADC_PIN 6
#define BAT_DIVIDER_RATIO 2.0f  // R8=R10=100K on-board divider

// LiIon 1S range used to map voltage to a 0-100% level.
#define BAT_VOLTAGE_MIN 3.3f
#define BAT_VOLTAGE_MAX 4.2f

uint32_t readTime = 0;

float readBatteryVoltage() {
  const int samples = 8;
  uint32_t sum = 0;
  for (int i = 0; i < samples; i++) {
    sum += analogReadMilliVolts(BAT_ADC_PIN);
  }
  float adcVolts = (sum / (float)samples) / 1000.0f;
  return adcVolts * BAT_DIVIDER_RATIO;
}

int batteryPercent(float voltage) {
  float pct = (voltage - BAT_VOLTAGE_MIN) / (BAT_VOLTAGE_MAX - BAT_VOLTAGE_MIN) * 100.0f;
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  return (int)(pct + 0.5f);
}

float BATT_GetVoltage() {
  return readBatteryVoltage();
}

int BATT_GetPercent() {
  return batteryPercent(readBatteryVoltage());
}

void BATT_ShowStatus() {
  float voltage = readBatteryVoltage();
  int percent = batteryPercent(voltage);

  char wifiLabel[33] = "";
  char ipLabel[16] = "";
  char timeLabel[6] = "";
  CLOCK_GetTimeText(timeLabel, sizeof(timeLabel));

  if (WiFi.status() == WL_CONNECTED) {
    const String ssid = WiFi.SSID();
    const String ip = WiFi.localIP().toString();
    snprintf(wifiLabel, sizeof(wifiLabel), "%s", ssid.c_str());
    snprintf(ipLabel, sizeof(ipLabel), "%s", ip.c_str());
  }

  LCD_DrawStatus(voltage, percent, timeLabel,
              (WiFi.status() == WL_CONNECTED) ? wifiLabel : nullptr,
              (WiFi.status() == WL_CONNECTED) ? ipLabel : nullptr);
}

void BATT_process(){
  if(millis() - readTime > 2000){
    BATT_ShowStatus();
    readTime = millis();
  }
}
