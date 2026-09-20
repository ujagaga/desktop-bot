#ifndef LCD_H
#define LCD_H

#include <stdint.h>

#define LCD_DEFAULT_ROTATION 1
#define LCD_DEFAULT_TEXT_SIZE 2
#define LCD_ROW_LEFT 10
#define LCD_ROW_TOP 20
#define LCD_ROW_GAP 12
// Classic GFX glyphs are 8 pixels high before scaling.
#define LCD_STATUS_LINE_HEIGHT (8 * LCD_DEFAULT_TEXT_SIZE + LCD_ROW_GAP)

// Default RGB565 colors for status, text, clock, and OTA screens.
#define LCD_BACKGROUND_COLOR 0x0000  // Black
#define LCD_TEXT_COLOR       0xFFFF  // White

// Initializes the SPI bus and the ST7789 display.
void LCD_Init();
// Zero-based row grid: y = LCD_ROW_TOP + row * (8 * textSize + LCD_ROW_GAP).
// Use the same textSize for positioning and clearing a given grid.
// Returns false for a zero size or a row that cannot fit on the screen.
bool LCD_SetRow(uint16_t row, uint8_t textSize = LCD_DEFAULT_TEXT_SIZE);
bool LCD_ClearRow(uint16_t row, uint8_t textSize = LCD_DEFAULT_TEXT_SIZE);
// Persist an RGB565 color; return false without changing colors on save failure.
bool LCD_SetColor(bool background, uint16_t color);

// Draws a text-only battery/Wi‑Fi status summary on the display.
void LCD_DrawStatus(float voltage, int percent, const char *timeLabel,
					 const char *wifiLabel, const char *ipLabel, const char *cameraIP = nullptr);

// Shows text at the default size, wrapping to the next line as needed.
void LCD_ShowText(const char *text);

// Shows a local time and weekday/date screen.
void LCD_ShowTime(const char *timeText, const char *dateText);

// Clear the display for OTA and suppress normal status refreshes.
void LCD_UpdateBegin(unsigned long version);
void LCD_UpdateProgress(int percent);
void LCD_UpdateStatus(const char *status);

// Returns whether custom text is currently shown instead of live status.
bool LCD_IsTextMode();

// Sets the backlight brightness, 0-100.
void LCD_BacklightSet(int percent);

// Turns the backlight off without forgetting the last percent set.
void LCD_BacklightOff();

// Restores the backlight to the last percent set via setBacklightPercent().
void LCD_BacklightRestore();

// Sets the display rotation, 0..3.
void LCD_SetRotation(int rotation);

// Clears the LCD screen.
void LCD_Clear();

// Include the Adafruit GFX library for graphics rendering
#include <Adafruit_GFX.h>

// Returns the initialized display surface for dedicated renderers.
Adafruit_GFX *LCD_GetGraphics();

#endif
