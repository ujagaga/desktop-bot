#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include "wifi_connection.h"

#define WIFI_NAMESPACE "miniBotWiFi"
#define WIFI_SSID_KEY "ssid"
#define WIFI_PASS_KEY "pass"

bool WIFI_SaveCredentials(const char *ssid, const char *pass) {
  Preferences prefs;
  if (!prefs.begin(WIFI_NAMESPACE, false)) return false;
  prefs.putString(WIFI_SSID_KEY, ssid);
  prefs.putString(WIFI_PASS_KEY, pass);
  prefs.end();
  return true;
}

bool WIFI_ClearStoredCredentials() {
  Preferences prefs;
  if (!prefs.begin(WIFI_NAMESPACE, false)) return false;
  prefs.remove(WIFI_SSID_KEY);
  prefs.remove(WIFI_PASS_KEY);
  prefs.end();
  return true;
}

bool WIFI_LoadStoredCredentials(String &ssid, String &pass) {
  Preferences prefs;
  if (!prefs.begin(WIFI_NAMESPACE, true)) return false;
  ssid = prefs.getString(WIFI_SSID_KEY, "");
  pass = prefs.getString(WIFI_PASS_KEY, "");
  prefs.end();
  return !ssid.isEmpty() && !pass.isEmpty();
}

bool WIFI_Connect(const char *ssid, const char *pass, bool persist) {
  if (ssid == nullptr || *ssid == '\0' || pass == nullptr) return false;

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, pass);

  uint32_t startMs = millis();
  while (millis() - startMs < 15000UL) {
    if (WiFi.status() == WL_CONNECTED) {
      if (persist) {
        WIFI_SaveCredentials(ssid, pass);
      }
      return true;
    }
    delay(100);
  }

  return false;
}

void WIFI_Disconnect() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

void WIFI_Init() {
  String ssid, pass;
  if (WIFI_LoadStoredCredentials(ssid, pass)) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());
  }
}
