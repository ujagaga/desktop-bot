#ifndef MINIBOT_WAKE_WORD_H
#define MINIBOT_WAKE_WORD_H

#include <Arduino.h>
#include <freertos/stream_buffer.h>

// Start the microWakeWord ("hey jarvis") listener task on the I2S microphone.
bool WAKEWORD_Init();
// True once per detection; call from loop() to react on the main thread.
bool WAKEWORD_TakeDetection();
bool WAKEWORD_PrintStatus(Print &output);
// Non-null: forward 16-bit mic samples there and pause detection. Null: resume detection.
void WAKEWORD_SetCapture(StreamBufferHandle_t buffer);

#endif
