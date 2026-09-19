#ifndef WIFI_CONNECTION_H
#define WIFI_CONNECTION_H

#include <Arduino.h>

bool WIFI_LoadStoredCredentials(String &ssid, String &pass);
bool WIFI_SaveCredentials(const char *ssid, const char *pass);
bool WIFI_ClearStoredCredentials();
bool WIFI_Connect(const char *ssid, const char *pass, bool persist);
void WIFI_Disconnect();
void WIFI_Init();

#endif
