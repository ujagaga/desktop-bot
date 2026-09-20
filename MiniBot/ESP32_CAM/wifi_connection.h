#pragma once
#include <Arduino.h>
void WIFIC_init();
void WIFIC_process();
// Queue a saved configuration to apply after the HTTP response has been sent.
bool WIFIC_configure(const String &mode, const String &ssid, const String &password);
String WIFIC_status();
