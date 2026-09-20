#ifndef MINIBOT_CONFIG_H
#define MINIBOT_CONFIG_H

// Update before building a new firmware release.
#define FIRMWARE_VERSION 5

#define OTA_GITHUB_REPOSITORY "ujagaga/desktop-bot"
#define OTA_GITHUB_BRANCH "main"
#define OTA_FIRMWARE_DIRECTORY "MiniBot/ESP32_S3"

// GPIO7 touch pad: run "touch calibrate" with the pad untouched.
#define TOUCH_GPIO 7
#define TOUCH_HOLD_MS 3000UL
// Increase above the untouched baseline needed to register a touch.
#define TOUCH_THRESHOLD_PERCENT 20

// QMI8658 INT1 is wired to GPIO46. WoM threshold is acceleration, not angle.
#define GYRO_MOTION_WAKE_GPIO 46
#define GYRO_WAKE_THRESHOLD_MG 20
// Write NVS only when any bias differs this much from the last saved value.
#define GYRO_BIAS_SAVE_DELTA_DPS 0.25f

#endif
