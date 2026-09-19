#ifndef WIFI_CONNECTION_H
#define WIFI_CONNECTION_H

#include <Arduino.h>

bool WIFI_LoadStoredCredentials(String &ssid, String &pass);
bool WIFI_SaveCredentials(const char *ssid, const char *pass);
bool WIFI_ClearStoredCredentials();
bool WIFI_Connect(const char *ssid, const char *pass, bool persist);
void WIFI_Disconnect();
// Turn off Wi-Fi for sleep and reconnect on wake only if it was enabled.
void WIFI_PrepareForSleep();
void WIFI_RestoreAfterSleep();
void WIFI_Init();

#endif
