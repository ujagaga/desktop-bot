#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_http_server.h"

void audio_start(void);

/* Reads mono samples (32-bit, AUDIO_WAKE_SLOT) for the wake word. Returns
 * the number read, 0 while `audio test` or /audio.wav has the microphone. */
size_t audio_read(int32_t *samples, size_t count);

/* Registers `audio test`: tone on the speaker, record 2 s, play it back. */
void audio_register_command(void);

/* Registers GET /audio.wav?s=<1-10>: records and returns a 16-bit stereo
 * WAV (left and right mic slots of the data line). */
void audio_register_http(httpd_handle_t server);
