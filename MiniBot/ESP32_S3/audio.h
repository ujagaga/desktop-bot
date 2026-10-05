#ifndef MINIBOT_AUDIO_H
#define MINIBOT_AUDIO_H

#include <Arduino.h>

bool AUDIO_Init();
bool AUDIO_Test(Print &output);
// Read mono samples (32-bit, 16 kHz) for the wake word; returns samples read, 0 if busy/unavailable.
size_t AUDIO_Read(int32_t *samples, size_t count);
// Speaker playback of 16 kHz mono samples; blocks until queued to DMA. Runs alongside AUDIO_Read.
size_t AUDIO_Write(const int16_t *samples, size_t count);
void AUDIO_SetAmp(bool on);

#endif