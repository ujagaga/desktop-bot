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
#define JPEG_QUALITY     80
