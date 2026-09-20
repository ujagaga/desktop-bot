#ifndef MINIBOT_CONFIG_H
#define MINIBOT_CONFIG_H

// Update before building a new firmware release.
#define FIRMWARE_VERSION 13

#define OTA_GITHUB_REPOSITORY "ujagaga/desktop-bot"
#define OTA_GITHUB_BRANCH "main"
#define OTA_FIRMWARE_DIRECTORY "MiniBot/ESP32_S3"

// CAM GPIO13 wake line; common ground and 10k pull-down at the CAM.
#define CAM_WAKE_OUTPUT_GPIO 7
#define CAM_IP_REPORT_INTERVAL_MS 5000UL
#define CAM_SLEEP_ACK_TIMEOUT_MS 10000UL
#define SLEEPY_FACE_MIN_MS 3000UL

// QMI8658 INT1 is wired to GPIO46. WoM threshold is acceleration, not angle.
#define GYRO_MOTION_WAKE_GPIO 46
#define GYRO_WAKE_THRESHOLD_MG 25
// Write NVS only when any bias differs this much from the last saved value.
#define GYRO_BIAS_SAVE_DELTA_DPS 0.25f

#endif
