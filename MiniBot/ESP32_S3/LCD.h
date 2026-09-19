#ifndef LCD_H
#define LCD_H

#define LCD_DEFAULT_ROTATION 0

// Initializes the SPI bus and the ST7789 display.
void LCD_Init();

// Draws a text-only battery/Wi‑Fi status summary on the display.
void LCD_DrawBattery(float voltage, int percent, const char *wifiLabel, const char *ipLabel);

// Sets the backlight brightness, 0-100.
void LCD_BacklightSet(int percent);

// Turns the backlight off without forgetting the last percent set.
void LCD_BacklightOff();

// Restores the backlight to the last percent set via setBacklightPercent().
void LCD_BacklightRestore();

// Sets the display rotation, 0..3.
void LCD_SetRotation(int rotation);

#endif
