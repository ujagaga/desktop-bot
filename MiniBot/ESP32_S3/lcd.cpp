#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <Preferences.h>
#include "lcd.h"
#include "config.h"
#include "http_client.h"

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
#define BACKLIGHT_DEFAULT_PERCENT 20

static SPIClass lcdSPI(HSPI);
static Adafruit_ST7789 tft(&lcdSPI, LCD_CS, LCD_DC, LCD_RST);
static int currentBacklightPercent = BACKLIGHT_DEFAULT_PERCENT;
static bool textMode = false;
static int updatePercent = -1;

static uint16_t backgroundColor = LCD_BACKGROUND_COLOR;
static uint16_t foregroundColor = LCD_TEXT_COLOR;
static String lastText, lastTime, lastDate;
enum class Content { Status, Text, Time, Update };
static Content content = Content::Status;

bool LCD_SetColor(bool background, uint16_t color) {
  Preferences prefs;
  if (!prefs.begin("miniBotLCD", false)) return false;
  const char *key = background ? "bg" : "fg";
  bool ok = (prefs.isKey(key) && prefs.getUShort(key) == color) ||
            prefs.putUShort(key, color) == sizeof(color);
  prefs.end();
  if (!ok) return false;
  if (background) backgroundColor = color;
  else foregroundColor = color;
  if (content == Content::Text) LCD_ShowText(lastText.c_str());
  else if (content == Content::Time) LCD_ShowTime(lastTime.c_str(), lastDate.c_str());
  return true;
}

static uint32_t dutyForPercent(int percent) {
  return ((1 << BACKLIGHT_RES_BITS) - 1) * percent / 100;
}

void LCD_Init() {
  Preferences prefs;
  if (prefs.begin("miniBotLCD", true)) {
    backgroundColor = prefs.getUShort("bg", backgroundColor);
    foregroundColor = prefs.getUShort("fg", foregroundColor);
    prefs.end();
  }
  lcdSPI.begin(LCD_SCLK, -1, LCD_MOSI, LCD_CS);
  tft.init(240, 240);
  tft.setRotation(LCD_DEFAULT_ROTATION);

  ledcAttach(BACKLIGHT_PIN, BACKLIGHT_FREQ, BACKLIGHT_RES_BITS);
  ledcWrite(BACKLIGHT_PIN, dutyForPercent(currentBacklightPercent));
  LCD_Clear();
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
  content = Content::Status;
  tft.fillScreen(backgroundColor);
  textMode = false;
}

void LCD_DrawStatus(float voltage, int percent, const char *timeLabel,
                     const char *wifiLabel, const char *ipLabel) {
  if (textMode) return;

  tft.setTextColor(foregroundColor);
  tft.setTextSize(2);
  tft.setCursor(10, 20);

  String textContent = "Firmware V";
  textContent += FIRMWARE_VERSION;
  textContent += "\n";

  uint32_t invalidVersion = HTTP_CLIENT_GetInvalidVersion();
  if (invalidVersion) {

    char label[40];
    snprintf(label, sizeof(label), "max fw V%lu - invalid", (unsigned long)invalidVersion);
    textContent += label;
    textContent += "\n";
  }

  textContent += "BAT " + String(percent) + "%  " + String(voltage, 2) + "V\n";

  textContent += "TIME ";
  textContent += (timeLabel != nullptr && timeLabel[0] != '\0') ? timeLabel : "unavailable";
  textContent += "\n";
  textContent += "WIFI ";
  textContent += (wifiLabel != nullptr && wifiLabel[0] != '\0') ? wifiLabel : "disconnected";
  textContent += "\n";
  textContent += "IP ";
  textContent += (ipLabel != nullptr && ipLabel[0] != '\0') ? ipLabel : "disconnected";

  tft.print(textContent);

}

void LCD_ShowText(const char *text) {
  lastText = text ? text : "";
  text = lastText.c_str();
  content = Content::Text;
  textMode = true;
  tft.fillScreen(backgroundColor);
  tft.setTextColor(foregroundColor);
  tft.setTextSize(2);
  tft.setCursor(0, 0);
  if (text != nullptr) tft.print(text);
}

void LCD_ShowTime(const char *timeText, const char *dateText) {
  lastTime = timeText ? timeText : "";
  lastDate = dateText ? dateText : "";
  timeText = lastTime.c_str();
  dateText = lastDate.c_str();
  content = Content::Time;
  textMode = true;
  tft.fillScreen(backgroundColor);
  tft.setTextColor(foregroundColor);

  tft.setTextSize(5);
  tft.setCursor(30, 62);
  if (timeText != nullptr) tft.print(timeText);

  tft.setTextSize(2);
  tft.setCursor(10, 210);
  if (dateText != nullptr) tft.print(dateText);
}

void LCD_UpdateStatus(const char *status) {
  tft.fillRect(10, 180, 220, 48, backgroundColor);
  tft.setTextColor(foregroundColor);
  tft.setTextSize(2);
  tft.setCursor(12, 184);
  tft.print(status);
}

void LCD_UpdateProgress(int percent) {
  percent = constrain(percent, 0, 100);
  if (percent == updatePercent) return;
  updatePercent = percent;

  // Only redraw the changing regions, avoiding a full-screen flash per chunk.
  tft.fillRect(60, 92, 120, 28, backgroundColor);
  tft.setTextColor(foregroundColor);
  tft.setTextSize(3);
  tft.setCursor(84, 94);
  tft.printf("%d%%", percent);
  tft.fillRect(22, 137, 196, 22, backgroundColor);
  if (percent > 0) tft.fillRect(22, 137, 196 * percent / 100, 22, ST77XX_CYAN);
  if (percent == 100) LCD_UpdateStatus("Verifying...");
}

void LCD_UpdateBegin(unsigned long version) {
  content = Content::Update;
  textMode = true;
  updatePercent = -1;
  tft.fillScreen(backgroundColor);
  tft.setTextColor(foregroundColor);
  tft.setTextSize(2);
  tft.setCursor(30, 24);
  tft.print("Firmware update");
  tft.setCursor(12, 58);
  tft.printf("Version %lu", version);
  tft.drawRect(20, 135, 200, 26, foregroundColor);
  LCD_UpdateProgress(0);
  LCD_UpdateStatus("Connecting...");
}

Adafruit_GFX *LCD_GetGraphics() {
  return &tft;
}

bool LCD_IsTextMode() {
  return textMode;
}
