#include "wifi_connection.h"
#include "config.h"
#include "comms.h"
#include "logger.h"
#include <WiFi.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <freertos/semphr.h>
static SemaphoreHandle_t mutex = nullptr;
static String stationSSID, stationPassword;
static bool stationEnabled = true, pending = false, wasConnected = false;
static uint32_t applyAt = 0;
static void applyStation() {
  WiFi.disconnect(false, false);
  if (stationEnabled && stationSSID.length()) {
    WiFi.begin(stationSSID.c_str(), stationPassword.c_str());
    LOG_append("Wi-Fi: joining station network; setup AP remains available");
  } else LOG_append("Wi-Fi: access-point-only mode");
}
static int chooseAPChannel() {
  const int candidates[] = {1, 6, 11};
  int scores[3] = {};
  int count = WiFi.scanNetworks(false, true);
  for (int i = 0; i < count; ++i) {
    int strength = constrain(100 + WiFi.RSSI(i), 1, 100);
    for (unsigned c = 0; c < 3; ++c) {
      int overlap = 5 - abs(WiFi.channel(i) - candidates[c]);
      if (overlap > 0) scores[c] += strength * overlap;
    }
  }
  WiFi.scanDelete();
  unsigned best = 0;
  for (unsigned c = 1; c < 3; ++c) if (scores[c] < scores[best]) best = c;
  return candidates[best];
}
void WIFIC_init() {
  mutex = xSemaphoreCreateMutex();
  stationSSID = WIFI_DEFAULT_SSID;
  stationPassword = WIFI_DEFAULT_PASSWORD;
  Preferences prefs;
  if (prefs.begin("camWiFi", true)) {
    String stored = prefs.getString("config", "");
    JsonDocument doc;
    if (stored.length() && !deserializeJson(doc, stored) && doc["ssid"].is<String>() && doc["password"].is<String>()) {
      stationSSID = doc["ssid"].as<String>();
      stationPassword = doc["password"].as<String>();
      stationEnabled = doc["station"] | true;
    }
    prefs.end();
  }
  WiFi.persistent(false);
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.setMinSecurity(WIFI_AUTH_OPEN); // Permit explicitly configured open networks.
  WiFi.softAPConfig(IPAddress(192,168,4,1), IPAddress(192,168,4,1), IPAddress(255,255,255,0));
  if (!WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASSWORD, chooseAPChannel())) LOG_append("ERR Wi-Fi AP startup failed");
  else LOG_append("Wi-Fi: setup AP ready at http://192.168.4.1/");
  applyStation();
}
bool WIFIC_configure(const String &mode, const String &ssid, const String &password) {
  if (!mutex || (mode != "ap" && mode != "station") || ssid.length() > 32 ||
      password.length() > 63 || strlen(ssid.c_str()) != ssid.length() ||
      strlen(password.c_str()) != password.length() || (mode == "station" && ssid.isEmpty()) ||
      (password.length() && password.length() < 8)) return false;
  JsonDocument doc;
  doc["station"] = mode == "station";
  doc["ssid"] = ssid;
  doc["password"] = password;
  String value; serializeJson(doc, value);
  xSemaphoreTake(mutex, portMAX_DELAY);
  Preferences prefs;
  bool ok = prefs.begin("camWiFi", false);
  if (ok) {
    ok = prefs.getString("config", "") == value || prefs.putString("config", value) == value.length();
    prefs.end();
  }
  if (ok) {
    stationEnabled = mode == "station";
    stationSSID = ssid; stationPassword = password;
    applyAt = millis() + 1000; pending = true;
  }
  xSemaphoreGive(mutex);
  LOG_append(ok ? "Wi-Fi: configuration saved; applying shortly" : "ERR Wi-Fi settings could not be saved");
  return ok;
}
void WIFIC_process() {
  if (!mutex) return;
  xSemaphoreTake(mutex, portMAX_DELAY);
  if (pending && (int32_t)(millis() - applyAt) >= 0) {
    pending = false; applyStation();
  }
  xSemaphoreGive(mutex);
  bool connected = WiFi.status() == WL_CONNECTED;
  if (connected != wasConnected) {
    wasConnected = connected;
    if (connected) LOG_printf("Wi-Fi connected: %s", WiFi.localIP().toString().c_str());
    else LOG_append("Wi-Fi disconnected; setup AP is available");
  }
}
String WIFIC_status() {
  JsonDocument doc;
  if (mutex) xSemaphoreTake(mutex, portMAX_DELAY);
  doc["mode"] = stationEnabled ? "station" : "ap";
  doc["ssid"] = stationSSID;
  if (mutex) xSemaphoreGive(mutex);
  doc["connected"] = WiFi.status() == WL_CONNECTED;
  doc["ip"] = WiFi.localIP().toString();
  doc["ap_ssid"] = WIFI_AP_SSID;
  doc["ap_ip"] = WiFi.softAPIP().toString();
  doc["s3_ip"] = COMMS_GetPeerWifiIP();
  String result; serializeJson(doc, result); return result;
}
