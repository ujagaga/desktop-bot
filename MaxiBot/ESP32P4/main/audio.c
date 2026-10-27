/*
 * Microphone and speaker on one full-duplex I2S channel pair, as on MiniBot:
 * 16 kHz, 32-bit slots. The INMP441 gives 24-bit samples in the top bits;
 * the MAX98357A plays the left slot (its SD pin is driven to 3.3 V).
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_console.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "config.h"
#include "cmd.h"
#include "audio.h"

#define CHUNK_FRAMES   256
#define MAX_SECONDS    10
/* 32-bit mic sample to 16-bit WAV: >> 12 gives 16x gain for speech level. */
#define WAV_SHIFT      12

static const char *TAG = "audio";

static i2s_chan_handle_t s_tx;
static i2s_chan_handle_t s_rx;
static SemaphoreHandle_t s_lock;
/* Scratch for record() and play_tone(), used under s_lock; too big for the
 * HTTP server task's stack. */
static int32_t s_chunk[CHUNK_FRAMES * 2];

void audio_start(void)
{
    const gpio_config_t amp_cfg = {
        .pin_bit_mask = 1ULL << AUDIO_AMP_SD_PIN,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&amp_cfg));
    gpio_set_level(AUDIO_AMP_SD_PIN, 0);

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear_after_cb = true;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_tx, &s_rx));
    const i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = AUDIO_BCLK_PIN,
            .ws = AUDIO_WS_PIN,
            .dout = AUDIO_DOUT_PIN,
            .din = AUDIO_MIC_PIN,
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx));
    ESP_ERROR_CHECK(i2s_channel_enable(s_rx));
    s_lock = xSemaphoreCreateMutex();
}

/* Records frames of stereo 32-bit samples (left, right). Drops what was
 * buffered before the call so the recording starts now. */
static esp_err_t record(int32_t *buf, size_t frames)
{
    size_t got;
    while (i2s_channel_read(s_rx, s_chunk, sizeof(s_chunk), &got, 0) == ESP_OK && got > 0) {
    }
    return i2s_channel_read(s_rx, buf, frames * 2 * sizeof(int32_t), &got, 1000 + frames * 1000 / AUDIO_RATE);
}

size_t audio_read(int32_t *samples, size_t count)
{
    static int32_t frames[CHUNK_FRAMES * 2];
    if (count > CHUNK_FRAMES) {
        count = CHUNK_FRAMES;
    }
    if (xSemaphoreTake(s_lock, 0) != pdTRUE) {
        return 0;
    }
    size_t got = 0;
    i2s_channel_read(s_rx, frames, count * 2 * sizeof(int32_t), &got, 100);
    xSemaphoreGive(s_lock);
    got /= 2 * sizeof(int32_t);
    for (size_t i = 0; i < got; i++) {
        samples[i] = frames[i * 2 + AUDIO_WAKE_SLOT];
    }
    return got;
}

static esp_err_t play(const int32_t *buf, size_t frames)
{
    size_t written;
    return i2s_channel_write(s_tx, buf, frames * 2 * sizeof(int32_t), &written, 1000 + frames * 1000 / AUDIO_RATE);
}

static esp_err_t play_tone(void)
{
    int32_t *chunk = s_chunk;
    const size_t frames = AUDIO_RATE / 3;
    const float amplitude = 0.08f * 2147483647.0f;
    for (size_t offset = 0; offset < frames; offset += CHUNK_FRAMES) {
        for (size_t i = 0; i < CHUNK_FRAMES; i++) {
            int32_t v = (int32_t)(sinf(2.0f * M_PI * 880.0f * (offset + i) / AUDIO_RATE) * amplitude);
            chunk[i * 2] = v;
            chunk[i * 2 + 1] = v;
        }
        esp_err_t err = play(chunk, CHUNK_FRAMES);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

static void level(const int32_t *buf, size_t frames, int slot, float *rms_dbfs, float *peak_dbfs)
{
    double sum = 0;
    int64_t peak = 1;
    for (size_t i = 0; i < frames; i++) {
        int64_t v = buf[i * 2 + slot];
        sum += (double)v * v;
        if (llabs(v) > peak) {
            peak = llabs(v);
        }
    }
    *rms_dbfs = 20 * log10(sqrt(sum / frames) / 2147483648.0 + 1e-12);
    *peak_dbfs = 20 * log10(peak / 2147483648.0);
}

static int cmd_audio(int argc, char **argv)
{
    if (argc != 2 || strcmp(argv[1], "test") != 0) {
        cmd_printf("ERR audio test\n");
        return 1;
    }
    const size_t frames = AUDIO_RATE * 2;
    int32_t *buf = heap_caps_malloc(frames * 2 * sizeof(int32_t), MALLOC_CAP_SPIRAM);
    if (buf == NULL) {
        cmd_printf("ERR out of memory\n");
        return 1;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    gpio_set_level(AUDIO_AMP_SD_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    cmd_printf("Audio test: speaker tone\n");
    esp_err_t err = play_tone();
    if (err == ESP_OK) {
        cmd_printf("Speak now; recording 2 seconds\n");
        err = record(buf, frames);
    }
    if (err == ESP_OK) {
        float rms, peak;
        for (int slot = 0; slot < 2; slot++) {
            level(buf, frames, slot, &rms, &peak);
            cmd_printf("%s mic: rms %.1f dBFS, peak %.1f dBFS\n", slot ? "Right" : "Left", rms, peak);
        }
        /* Speaker plays the left slot: copy the louder mic there. */
        float rms_l, rms_r;
        level(buf, frames, 0, &rms_l, &peak);
        level(buf, frames, 1, &rms_r, &peak);
        int src = rms_r > rms_l ? 1 : 0;
        for (size_t i = 0; i < frames; i++) {
            int32_t v = buf[i * 2 + src];
            buf[i * 2] = v;
            buf[i * 2 + 1] = v;
        }
        cmd_printf("Playing recording\n");
        err = play(buf, frames);
    }
    gpio_set_level(AUDIO_AMP_SD_PIN, 0);
    xSemaphoreGive(s_lock);
    free(buf);
    if (err != ESP_OK) {
        cmd_printf("ERR audio %s\n", esp_err_to_name(err));
        return 1;
    }
    cmd_printf("Audio test complete\n");
    return 0;
}

void audio_register_command(void)
{
    const esp_console_cmd_t cmd = {
        .command = "audio",
        .help = "audio test   tone, record 2 s, play back",
        .func = cmd_audio,
    };
    cmd_register(&cmd);
}

static void put_le(uint8_t *p, uint32_t v, int bytes)
{
    for (int i = 0; i < bytes; i++) {
        p[i] = v >> (8 * i);
    }
}

static esp_err_t wav_handler(httpd_req_t *req)
{
    int seconds = 2;
    char query[16], value[4];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "s", value, sizeof(value)) == ESP_OK) {
        seconds = atoi(value);
    }
    if (seconds < 1 || seconds > MAX_SECONDS) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "s must be 1-10");
    }
    const size_t frames = AUDIO_RATE * seconds;
    int32_t *buf = heap_caps_malloc(frames * 2 * sizeof(int32_t), MALLOC_CAP_SPIRAM);
    if (buf == NULL) {
        return httpd_resp_send_500(req);
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = record(buf, frames);
    xSemaphoreGive(s_lock);
    if (err != ESP_OK) {
        free(buf);
        ESP_LOGE(TAG, "record failed: %s", esp_err_to_name(err));
        return httpd_resp_send_500(req);
    }

    /* Convert in place to 16-bit stereo; the output never outruns the input. */
    int16_t *pcm = (int16_t *)buf;
    for (size_t i = 0; i < frames * 2; i++) {
        int32_t v = buf[i] >> WAV_SHIFT;
        pcm[i] = v > INT16_MAX ? INT16_MAX : v < INT16_MIN ? INT16_MIN : v;
    }
    uint32_t data_bytes = frames * 2 * sizeof(int16_t);
    uint8_t header[44];
    memcpy(header, "RIFF", 4);
    put_le(header + 4, 36 + data_bytes, 4);
    memcpy(header + 8, "WAVEfmt ", 8);
    put_le(header + 16, 16, 4);
    put_le(header + 20, 1, 2);                    /* PCM */
    put_le(header + 22, 2, 2);                    /* channels */
    put_le(header + 24, AUDIO_RATE, 4);
    put_le(header + 28, AUDIO_RATE * 2 * 2, 4);   /* byte rate */
    put_le(header + 32, 4, 2);                    /* block align */
    put_le(header + 34, 16, 2);                   /* bits per sample */
    memcpy(header + 36, "data", 4);
    put_le(header + 40, data_bytes, 4);

    httpd_resp_set_type(req, "audio/wav");
    err = httpd_resp_send_chunk(req, (const char *)header, sizeof(header));
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, (const char *)pcm, data_bytes);
    }
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, NULL, 0);
    }
    free(buf);
    return err;
}

void audio_register_http(httpd_handle_t server)
{
    const httpd_uri_t uri = { .uri = "/audio.wav", .method = HTTP_GET, .handler = wav_handler };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uri));
}
