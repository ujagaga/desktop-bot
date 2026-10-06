#pragma once
#include <Arduino.h>
// Voice gateway link (server/conversation.py): the S3 asks over UART for a frame,
// and the main loop POSTs it to <url>/snapshot?session=<id> for face recognition.
void GATEWAY_init();
void GATEWAY_process();
// Base URL such as https://face.ujagaga.in.rs; an empty key keeps the saved one.
bool GATEWAY_configure(const String &url, const String &key);
String GATEWAY_status();
// RX task safe: queue one upload for the main loop; a newer request replaces it.
bool GATEWAY_requestSnapshot(const char *session);
