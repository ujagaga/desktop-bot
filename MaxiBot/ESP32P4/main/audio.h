#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_http_server.h"

void audio_start(void);

/* Reads mono samples (32-bit, mic AUDIO_WAKE_MIC) for the wake word. Waits
 * up to 100 ms; returns the number read. */
size_t audio_read(int32_t *samples, size_t count);

/* Registers `audio test`: tone on the speaker, record 2 s, levels of all
 * four mics, play back the loudest. */
void audio_register_command(void);

/* Registers GET /audio.wav?s=<1-10>: records and returns a 16-bit WAV with
 * four channels, mic1-mic4. */
void audio_register_http(httpd_handle_t server);
