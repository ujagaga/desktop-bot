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

/* MPU6050 (GY-521) on its own I2C port; port 0 is the camera's SCCB. */
#define IMU_I2C_PORT     1
#define IMU_SDA_PIN      14
#define IMU_SCL_PIN      15
#define IMU_I2C_ADDR     0x68
/* Weight of the gyro in the X/Y complementary filter (rest: accelerometer). */
#define IMU_GYRO_WEIGHT  0.98f

/* Head servos (SG90): 50 Hz, 500-2500 us pulse for 0-180 degrees. */
#define HEAD_TILT_PIN    28
#define HEAD_PAN_PIN     29
#define HEAD_MIN_US      500
#define HEAD_MAX_US      2500
#define HEAD_CENTER_DEG  90
/* The head moves toward the target at this speed instead of jumping. */
#define HEAD_SPEED_DPS   120

/* Audio: I2S0 master, full duplex, 16 kHz, 32-bit stereo slots.
 * INMP441 mics (L/R pin picks the slot) and the MAX98357A share BCLK/WS. */
#define AUDIO_BCLK_PIN   16
#define AUDIO_WS_PIN     17
/* Mic data line A (mic3 left, mic4 right); line B (IO21) is not used yet. */
#define AUDIO_MIC_PIN    18
#define AUDIO_DOUT_PIN   19
#define AUDIO_AMP_SD_PIN 20
#define AUDIO_RATE       16000
/* Mic slot the wake word listens to: 0 = left (L/R to GND), 1 = right. */
#define AUDIO_WAKE_SLOT  0
