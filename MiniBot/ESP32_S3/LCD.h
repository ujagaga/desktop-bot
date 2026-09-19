#ifndef LCD_H
#define LCD_H

#define LCD_DEFAULT_ROTATION 1

// Initializes the SPI bus and the ST7789 display.
void LCD_Init();

// Draws a text-only battery/Wi‑Fi status summary on the display.
void LCD_DrawBattery(float voltage, int percent, const char *timeLabel,
					 const char *wifiLabel, const char *ipLabel);

// Shows text at size 2, wrapping to the next line as needed.
void LCD_ShowText(const char *text);

// Shows a local time and weekday/date screen.
void LCD_ShowTime(const char *timeText, const char *dateText);

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
