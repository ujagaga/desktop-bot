#pragma once
#define FIRMWARE_VERSION 7
// Standard CAM UART0: U0R/GPIO3 to S3 TX GPIO13, U0T/GPIO1 to S3 RX GPIO14.
#define COMMS_RX_PIN 3
#define COMMS_TX_PIN 1
#define COMMS_BAUD 115200
// S3 GPIO7 drives this HIGH while awake, LOW before requesting sleep.
#define CAM_WAKE_GPIO 13
#define CAM_POWER_DOWN_GPIO 32
#define OTA_GITHUB_REPOSITORY "ujagaga/desktop-bot"
#define OTA_GITHUB_BRANCH "main"
#define OTA_FIRMWARE_DIRECTORY "MiniBot/ESP32_CAM"
// Setup AP remains available even when joining another network fails.
#define WIFI_AP_SSID "MiniBot-CAM"
#define WIFI_AP_PASSWORD ""
// Retain the old project's default station network until configured in the UI.
#define WIFI_DEFAULT_SSID "Rada_i_Slavica"
#define WIFI_DEFAULT_PASSWORD "ohana130315"
