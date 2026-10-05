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
// Gateway voice: name null lists the current voice and choices; a name or "default" saves it on the server.
bool VOICE_ServerVoice(Print &output, const char *name);
void VOICE_PrintStatus(Print &output);
// A session or its failure face owns the screen.
bool VOICE_IsBusy();
// Result of the last gateway health check (checked every 60 s while idle).
bool VOICE_ServerOk();

#endif
