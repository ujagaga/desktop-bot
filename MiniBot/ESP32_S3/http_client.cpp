#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <NetworkClientSecure.h>
#include <atomic>
#include <time.h>
#include "http_client.h"
#include "config.h"
#include "firmware_version.h"
#include "motor.h"
#include "LCD.h"

// Root certificate bundle supplied by the installed ESP32 Arduino core.
extern const uint8_t bundleStart[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t bundleEnd[] asm("_binary_x509_crt_bundle_end");

static std::atomic<bool> connectedEvent{false};
static bool pending = false;
static unsigned long nextAttempt = 0;
static unsigned long connectedAt = 0;
static unsigned int attempts = 0;

static void configureTLS(NetworkClientSecure &client) {
  client.setCACertBundle(bundleStart, bundleEnd - bundleStart);
  client.setHandshakeTimeout(15);
}

static bool getText(const String &url, const char *accept, size_t limit, String &body) {
  NetworkClientSecure client;
  configureTLS(client);
  HTTPClient http;
  http.setConnectTimeout(10000);
  http.setTimeout(15000);
  if (!http.begin(client, url)) return false;
  http.addHeader("User-Agent", "MiniBot-OTA");
  http.addHeader("Accept", accept);
  int status = http.GET();
  int size = http.getSize();
  bool ok = status == HTTP_CODE_OK && size > 0 && (size_t)size <= limit;
  if (ok) {
    body = http.getString();
    ok = body.length() == (size_t)size;
  }
  if (!ok) Serial.printf("OTA: HTTP request failed (status %d, size %d)\n", status, size);
  http.end();
  return ok;
}

static bool checkRepository() {
  // Network requests block the main loop, including timed motor stopping.
  MOTOR_StopAll();
  String commit;
  String api = String("https://api.github.com/repos/") + OTA_GITHUB_REPOSITORY +
               "/commits/" + OTA_GITHUB_BRANCH;
  if (!getText(api, "application/vnd.github.sha", 128, commit)) return false;
  commit.trim();
  if (commit.length() != 40) return false;
  for (unsigned int i = 0; i < commit.length(); ++i) {
    if (!isxdigit((unsigned char)commit[i])) return false;
  }

  String base = String("https://raw.githubusercontent.com/") + OTA_GITHUB_REPOSITORY +
                "/" + commit + "/" + OTA_FIRMWARE_DIRECTORY;
  String config;
  uint32_t remoteVersion = 0;
  if (!getText(base + "/config.h", "text/plain", 8192, config)) return false;
  if (!parseFirmwareVersion(config.c_str(), remoteVersion)) {
    Serial.println("OTA: invalid remote FIRMWARE_VERSION; skipping");
    return false;
  }
  Serial.printf("OTA: device %lu, GitHub %lu\n", (unsigned long)FIRMWARE_VERSION,
                (unsigned long)remoteVersion);
  if (remoteVersion <= FIRMWARE_VERSION) return true;

  Serial.println("OTA: downloading newer firmware...");
  LCD_UpdateBegin(remoteVersion);
  NetworkClientSecure client;
  configureTLS(client);
  HTTPUpdate updater(15000);
  updater.rebootOnUpdate(false);
  updater.onStart([]() { LCD_UpdateStatus("Downloading..."); });
  updater.onProgress([](int current, int total) {
    if (total > 0) LCD_UpdateProgress((int)((uint64_t)current * 100 / total));
  });
  // HTTPUpdate checks image headers and slot capacity, streams to the inactive
  // partition, and selects it for boot only after a complete, valid download.
  if (updater.update(client, base + "/build/ESP32_S3.ino.bin") != HTTP_UPDATE_OK) {
    Serial.printf("OTA: update failed: %s\n", updater.getLastErrorString().c_str());
    LCD_UpdateStatus("Update failed");
    return false;
  }
  Serial.println("OTA: installed; restarting");
  LCD_UpdateProgress(100);
  LCD_UpdateStatus("Done. Restarting...");
  Serial.flush();
  delay(1000);
  ESP.restart();
  return true;
}

void HTTP_CLIENT_Init() {
  WiFi.onEvent([](WiFiEvent_t) {
    connectedEvent.store(true);
  }, ARDUINO_EVENT_WIFI_STA_GOT_IP);
}

void HTTP_CLIENT_Process() {
  if (connectedEvent.exchange(false)) {
    pending = true;
    attempts = 0;
    nextAttempt = millis();
    connectedAt = nextAttempt;
  }
  if (!pending || WiFi.status() != WL_CONNECTED ||
      (int32_t)(millis() - nextAttempt) < 0) return;

  // NTP is started by CLOCK_Process; do not bypass TLS validation without time.
  if (time(nullptr) < 1704067200) {
    nextAttempt = millis() + 1000;
    if (millis() - connectedAt >= 180000) {
      pending = false;
      Serial.println("OTA: time unavailable; retry on next Wi-Fi connection");
    }
    return;
  }
  if (checkRepository() || ++attempts >= 3) pending = false;
  else nextAttempt = millis() + 60000;
}
