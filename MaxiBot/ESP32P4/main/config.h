#pragma once

/* SCCB I2C wiring and CSI reset/pwdn per WT9932P4-TINY schematic V1.3. */
#define CAM_I2C_PORT     0
#define CAM_I2C_SCL_PIN  8
#define CAM_I2C_SDA_PIN  7
#define CAM_RESET_PIN    -1
#define CAM_PWDN_PIN     -1
/* J2 pin 5 (RPi CAM_GPIO): enables the camera module's LDOs / releases reset. */
#define CAM_ENABLE_PIN   0
#define BUF_COUNT        2
#define JPEG_QUALITY     60

/* Wi-Fi setup hotspot, started when no network is saved or the saved one
 * cannot be joined for STA_TIMEOUT_S. Name gets the last 3 MAC bytes. */
#define AP_SSID_PREFIX   "MaxiBot_"
#define AP_PASSWORD      ""
#define STA_TIMEOUT_S    30
/* While the hotspot runs with nobody on it, look for the saved network. */
#define SCAN_INTERVAL_S  10

/* mDNS / DHCP name, so the robot is reachable as <name>.local.
 * Changed with `wifi host <name>` or on the setup page, saved in NVS. */
#define DEFAULT_HOSTNAME "maxibot"

/* ST7789 2.25" 76x284 LCD, used in landscape (284x76). */
#define LCD_SPI_HOST     SPI3_HOST
#define LCD_SCL_PIN      5
#define LCD_SDA_PIN      6
#define LCD_RST_PIN      9
#define LCD_DC_PIN       10
#define LCD_CS_PIN       11
#define LCD_BL_PIN       12
/* Backlight is on with BL low (the module's transistor inverts it). */
#define LCD_BL_ON        0
/* Colors in RGB565. The first line of a screen uses the accent color. */
#define LCD_COLOR_BG     0x0000  /* black */
#define LCD_COLOR_TEXT   0x867F  /* light blue */
#define LCD_COLOR_ACCENT 0x867F  /* light blue */
#define LCD_W            284
#define LCD_H            76
/* Panel RAM is 240x320; the visible 76x284 window is centered in it. */
#define LCD_X_GAP        18
#define LCD_Y_GAP        82
