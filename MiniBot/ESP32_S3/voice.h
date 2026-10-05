#ifndef MINIBOT_VOICE_H
#define MINIBOT_VOICE_H

#include <Arduino.h>

// Gemini voice conversation through the Pi gateway (server/conversation.py).
bool VOICE_Init();
// Start a session after the wake word: chime, stream the mic, play the reply.
void VOICE_Start();
void VOICE_Stop();
void VOICE_Process();
bool VOICE_SetUrl(const char *url);
bool VOICE_SetKey(const char *key);
void VOICE_PrintStatus(Print &output);

#endif
