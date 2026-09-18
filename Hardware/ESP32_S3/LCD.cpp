#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include "LCD.h"

// LCD SPI: SCLK=40 MOSI=41 CS=39 DC=38 RST=42 (no MISO). Pins come from the
// board schematic, not the datasheet, since they're fixed by the PCB traces.
#define LCD_SCLK 40
#define LCD_MOSI 41
#define LCD_CS 39
#define LCD_DC 38
#define LCD_RST 42

// Backlight: GP20 drives the LEDA switch transistor (Q5) through a PWM gate
// signal (BL_PWM net on the schematic), not the display's SPI logic.
#define BACKLIGHT_PIN 20
#define BACKLIGHT_FREQ 5000
#define BACKLIGHT_RES_BITS 8
#define BACKLIGHT_DEFAULT_PERCENT 30

static SPIClass lcdSPI(HSPI);
static Adafruit_ST7789 tft(&lcdSPI, LCD_CS, LCD_DC, LCD_RST);
static int currentBacklightPercent = BACKLIGHT_DEFAULT_PERCENT;

static uint32_t dutyForPercent(int percent) {
  return ((1 << BACKLIGHT_RES_BITS) - 1) * percent / 100;
}

void initLCD() {
  lcdSPI.begin(LCD_SCLK, -1, LCD_MOSI, LCD_CS);
  tft.init(240, 240);
  tft.setRotation(0);

  ledcAttach(BACKLIGHT_PIN, BACKLIGHT_FREQ, BACKLIGHT_RES_BITS);
  ledcWrite(BACKLIGHT_PIN, dutyForPercent(currentBacklightPercent));
}

void setBacklightPercent(int percent) {
  if (percent < 0) percent = 0;
  if (percent > 100) percent = 100;
  currentBacklightPercent = percent;
  ledcWrite(BACKLIGHT_PIN, dutyForPercent(percent));
}

void turnOffBacklight() {
  ledcWrite(BACKLIGHT_PIN, 0);
}

void restoreBacklight() {
  ledcWrite(BACKLIGHT_PIN, dutyForPercent(currentBacklightPercent));
}

void drawBattery(float voltage, int percent) {
  tft.fillScreen(ST77XX_BLACK);

  const int bodyX = 60, bodyY = 80, bodyW = 120, bodyH = 60;
  const int capW = 8, capH = 24;
  tft.drawRoundRect(bodyX, bodyY, bodyW, bodyH, 6, ST77XX_WHITE);
  tft.fillRect(bodyX + bodyW, bodyY + (bodyH - capH) / 2, capW, capH, ST77XX_WHITE);

  int fillW = (bodyW - 6) * percent / 100;
  uint16_t fillColor = percent < 20 ? ST77XX_RED : (percent < 50 ? ST77XX_YELLOW : ST77XX_GREEN);
  tft.fillRect(bodyX + 3, bodyY + 3, fillW, bodyH - 6, fillColor);

  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(3);
  tft.setCursor(70, 160);
  tft.printf("%d%%", percent);

  tft.setTextSize(1);
  tft.setCursor(85, 200);
  tft.printf("%.2f V", voltage);
}
