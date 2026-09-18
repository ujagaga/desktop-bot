#ifndef LCD_H
#define LCD_H

// Initializes the SPI bus and the ST7789 display.
void initLCD();

// Draws the battery icon, percent and voltage on the display.
void drawBattery(float voltage, int percent);

// Sets the backlight brightness, 0-100.
void setBacklightPercent(int percent);

// Turns the backlight off without forgetting the last percent set.
void turnOffBacklight();

// Restores the backlight to the last percent set via setBacklightPercent().
void restoreBacklight();

#endif
