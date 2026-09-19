#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include "LCD.h"
#include "config.h"

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
static bool textMode = false;

static uint32_t dutyForPercent(int percent) {
  return ((1 << BACKLIGHT_RES_BITS) - 1) * percent / 100;
}

void LCD_Init() {
  lcdSPI.begin(LCD_SCLK, -1, LCD_MOSI, LCD_CS);
  tft.init(240, 240);
  tft.setRotation(LCD_DEFAULT_ROTATION);

  ledcAttach(BACKLIGHT_PIN, BACKLIGHT_FREQ, BACKLIGHT_RES_BITS);
  ledcWrite(BACKLIGHT_PIN, dutyForPercent(currentBacklightPercent));
}

void LCD_BacklightSet(int percent) {
  if (percent < 0) percent = 0;
  if (percent > 100) percent = 100;
  currentBacklightPercent = percent;
  ledcWrite(BACKLIGHT_PIN, dutyForPercent(percent));
}

void LCD_BacklightOff() {
  ledcWrite(BACKLIGHT_PIN, 0);
}

void LCD_BacklightRestore() {
  ledcWrite(BACKLIGHT_PIN, dutyForPercent(currentBacklightPercent));
}

void LCD_SetRotation(int rotation) {
  if (rotation < 0) rotation = 0;
  if (rotation > 3) rotation = 3;
  tft.setRotation(rotation);
}

void LCD_Clear() {
  tft.fillScreen(ST77XX_BLACK);
  textMode = false;
}

void LCD_DrawBattery(float voltage, int percent, const char *timeLabel,
                     const char *wifiLabel, const char *ipLabel) {
  if (textMode) return;

  tft.fillScreen(ST77XX_BLACK);
  tft.setTextColor(ST77XX_WHITE);

  tft.setTextSize(2);
  tft.setCursor(10, 20);
  tft.print("FW ");
  tft.print(FIRMWARE_VERSION);

  tft.setCursor(10, 55);
  tft.printf("BAT %d%%  %.2fV", percent, voltage);

  tft.setCursor(10, 90);
  tft.print("TIME ");
  tft.print((timeLabel != nullptr && timeLabel[0] != '\0') ? timeLabel : "unavailable");

  tft.setTextSize(2);
  tft.setCursor(10, 125);
  if (wifiLabel != nullptr && wifiLabel[0] != '\0') {
    tft.print("WIFI ");
    tft.print(wifiLabel);
  } else {
    tft.print("WIFI disconnected");
  }

  tft.setCursor(10, 195);
  if (ipLabel != nullptr && ipLabel[0] != '\0') {
    tft.print("IP ");
    tft.print(ipLabel);
  } else {
    tft.print("IP disconnected");
  }

}

void LCD_ShowText(const char *text) {
  textMode = true;
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(2);
  tft.setCursor(0, 0);
  if (text != nullptr) tft.print(text);
}

void LCD_ShowTime(const char *timeText, const char *dateText) {
  textMode = true;
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextColor(ST77XX_WHITE);

  tft.setTextSize(5);
  tft.setCursor(30, 62);
  if (timeText != nullptr) tft.print(timeText);

  tft.setTextSize(2);
  tft.setCursor(10, 210);
  if (dateText != nullptr) tft.print(dateText);
}

Adafruit_GFX *LCD_GetGraphics() {
  return &tft;
}

bool LCD_IsTextMode() {
  return textMode;
}
