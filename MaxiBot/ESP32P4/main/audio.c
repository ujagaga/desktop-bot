/*
 * Speaker and four microphones, 16 kHz, 32-bit stereo slots, as on MiniBot.
 * I2S0 is master, full duplex: the MAX98357A plays the left slot (its SD pin
 * is driven to 3.3 V) and mic line A comes in. I2S1 is a clock slave on the
 * same BCLK/WS pins and reads mic line B. The INMP441 gives 24-bit samples
 * in the top bits; the L/R pin picks the slot.
 *
 * The two controllers start a whole number of samples apart. To find the
 * offset, I2S1 briefly reads line A too, and the two copies are matched.
 * One task reads both lines and hands out frames of four samples, mic1-mic4.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_console.h"
#include "esp_rom_gpio.h"
#include "soc/gpio_sig_map.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "config.h"
#include "cmd.h"
#include "audio.h"

#define MICS           4
#define CHUNK_FRAMES   256
#define MAX_SECONDS    10
/* 32-bit mic sample to 16-bit WAV: >> 12 gives 16x gain for speech level. */
#define WAV_SHIFT      12
/* Alignment: frames compared, largest offset searched, frames dropped after
 * switching I2S1 between lines (more than its DMA buffers hold). */
#define ALIGN_FRAMES   1024
#define MAX_OFFSET     256
#define SETTLE_FRAMES  (AUDIO_RATE / 5)

static const char *TAG = "audio";

static i2s_chan_handle_t s_tx;
static i2s_chan_handle_t s_rx_a;
static i2s_chan_handle_t s_rx_b;
static SemaphoreHandle_t s_lock;         /* one test or recording at a time */
static SemaphoreHandle_t s_rec_done;
static StreamBufferHandle_t s_wake;      /* AUDIO_WAKE_MIC samples */
static int32_t *volatile s_rec_buf;     /* recording in progress, filled by the task */
static volatile size_t s_rec_left;
static volatile bool s_realign = true;
static int s_offset;                     /* line A frames dropped (negative: line B) */
static bool s_aligned;
/* Stereo scratch for each line, used by the capture task. */
static int32_t s_a[ALIGN_FRAMES * 2];
static int32_t s_b[ALIGN_FRAMES * 2];
/* Scratch for play_tone(), used under s_lock; too big for the HTTP server
 * task's stack. */
static int32_t s_chunk[CHUNK_FRAMES * 2];

/* Reads the same number of frames from both lines, in chunks so neither
 * DMA queue overflows while the other is read. */
static bool read_both(int32_t *a, int32_t *b, size_t frames)
{
    for (size_t done = 0; done < frames; done += CHUNK_FRAMES) {
        size_t bytes = MIN(CHUNK_FRAMES, frames - done) * 2 * sizeof(int32_t), got;
        if (i2s_channel_read(s_rx_a, a + done * 2, bytes, &got, 1000) != ESP_OK ||
            i2s_channel_read(s_rx_b, b + done * 2, bytes, &got, 1000) != ESP_OK) {
            return false;
        }
    }
    return true;
}

static void skip(i2s_chan_handle_t chan, size_t frames)
{
    size_t got;
    while (frames > 0) {
        size_t n = MIN(frames, ALIGN_FRAMES);
        i2s_channel_read(chan, s_a, n * 2 * sizeof(int32_t), &got, 1000);
        frames -= n;
    }
}

static void skip_both(size_t frames)
{
    for (; frames >= CHUNK_FRAMES; frames -= CHUNK_FRAMES) {
        read_both(s_a, s_b, CHUNK_FRAMES);
    }
}

/* Offset d where line B frame i equals line A frame i + d, by normalized
 * cross-correlation of the sum of both slots. */
static int find_offset(float *best_score)
{
    static float a[ALIGN_FRAMES], b[ALIGN_FRAMES];
    float mean_a = 0, mean_b = 0;
    for (int i = 0; i < ALIGN_FRAMES; i++) {
        a[i] = (s_a[i * 2] >> 8) + (s_a[i * 2 + 1] >> 8);
        b[i] = (s_b[i * 2] >> 8) + (s_b[i * 2 + 1] >> 8);
        mean_a += a[i];
        mean_b += b[i];
    }
    for (int i = 0; i < ALIGN_FRAMES; i++) {
        a[i] -= mean_a / ALIGN_FRAMES;
        b[i] -= mean_b / ALIGN_FRAMES;
    }
    int best = 0;
    *best_score = 0;
    for (int d = -MAX_OFFSET; d <= MAX_OFFSET; d++) {
        double ab = 0, aa = 0, bb = 0;
        for (int i = MAX_OFFSET; i < ALIGN_FRAMES - MAX_OFFSET; i++) {
            ab += a[i + d] * b[i];
            aa += a[i + d] * a[i + d];
            bb += b[i] * b[i];
        }
        float score = aa > 0 && bb > 0 ? ab / sqrt(aa * bb) : 0;
        if (score > *best_score) {
            *best_score = score;
            best = d;
        }
    }
    return best;
}

static void align(void)
{
    s_realign = false;
    esp_rom_gpio_connect_in_signal(AUDIO_MIC_A_PIN, I2S1_I_SD_PAD_IN_IDX, false);
    skip_both(SETTLE_FRAMES);
    read_both(s_a, s_b, ALIGN_FRAMES);
    esp_rom_gpio_connect_in_signal(AUDIO_MIC_B_PIN, I2S1_I_SD_PAD_IN_IDX, false);

    float score;
    int offset = find_offset(&score);
    /* A match below this means line A is silent or I2S1 gets no clock. */
    s_aligned = score > 0.9f;
    if (s_aligned) {
        s_offset = offset;
        if (offset > 0) {
            skip(s_rx_a, offset);
        } else if (offset < 0) {
            skip(s_rx_b, -offset);
        }
        ESP_LOGI(TAG, "mic lines aligned, offset %d frames", offset);
    } else {
        ESP_LOGE(TAG, "mic lines not aligned (match %.2f)", score);
    }
    skip_both(SETTLE_FRAMES);
}

static bool IRAM_ATTR on_overflow(i2s_chan_handle_t chan, i2s_event_data_t *event, void *ctx)
{
    s_realign = true;
    return false;
}

static void capture_task(void *arg)
{
    static int32_t frames[CHUNK_FRAMES * MICS];
    static int32_t wake[CHUNK_FRAMES];
    while (true) {
        if (s_realign) {
            align();
        }
        if (!read_both(s_a, s_b, CHUNK_FRAMES)) {
            continue;
        }
        for (int i = 0; i < CHUNK_FRAMES; i++) {
            int32_t *f = frames + i * MICS;
            f[0] = s_b[i * 2];
            f[1] = s_b[i * 2 + 1];
            f[2] = s_a[i * 2];
            f[3] = s_a[i * 2 + 1];
            wake[i] = f[AUDIO_WAKE_MIC - 1];
        }
        if (xStreamBufferSpacesAvailable(s_wake) >= sizeof(wake)) {
            xStreamBufferSend(s_wake, wake, sizeof(wake), 0);
        }
        int32_t *rec = s_rec_buf;
        if (rec != NULL) {
            size_t n = MIN(CHUNK_FRAMES, s_rec_left);
            memcpy(rec, frames, n * MICS * sizeof(int32_t));
            s_rec_left -= n;
            if (s_rec_left == 0) {
                s_rec_buf = NULL;
                xSemaphoreGive(s_rec_done);
            } else {
                s_rec_buf = rec + n * MICS;
            }
        }
    }
}

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
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_tx, &s_rx_a));
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = AUDIO_BCLK_PIN,
            .ws = AUDIO_WS_PIN,
            .dout = AUDIO_DOUT_PIN,
            .din = AUDIO_MIC_A_PIN,
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx_a, &std_cfg));

    /* After I2S0, so its BCLK/WS outputs stay on the pins: the slave only
     * adds the pins as inputs. */
    i2s_chan_config_t slave_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_SLAVE);
    ESP_ERROR_CHECK(i2s_new_channel(&slave_cfg, NULL, &s_rx_b));
    std_cfg.gpio_cfg.dout = I2S_GPIO_UNUSED;
    std_cfg.gpio_cfg.din = AUDIO_MIC_B_PIN;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx_b, &std_cfg));

    const i2s_event_callbacks_t cbs = { .on_recv_q_ovf = on_overflow };
    ESP_ERROR_CHECK(i2s_channel_register_event_callback(s_rx_a, &cbs, NULL));
    ESP_ERROR_CHECK(i2s_channel_register_event_callback(s_rx_b, &cbs, NULL));
    ESP_ERROR_CHECK(i2s_channel_enable(s_rx_b));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx));
    ESP_ERROR_CHECK(i2s_channel_enable(s_rx_a));

    s_lock = xSemaphoreCreateMutex();
    s_rec_done = xSemaphoreCreateBinary();
    s_wake = xStreamBufferCreate(AUDIO_RATE / 4 * sizeof(int32_t), sizeof(int32_t));
    xTaskCreate(capture_task, "audio", 4096, NULL, 6, NULL);
}

/* Records frames of four 32-bit samples, mic1-mic4, starting now. */
static esp_err_t record(int32_t *buf, size_t frames)
{
    s_rec_left = frames;
    s_rec_buf = buf;
    if (xSemaphoreTake(s_rec_done, pdMS_TO_TICKS(1000 + frames * 1000 / AUDIO_RATE)) != pdTRUE) {
        s_rec_buf = NULL;
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

size_t audio_read(int32_t *samples, size_t count)
{
    return xStreamBufferReceive(s_wake, samples, count * sizeof(int32_t), pdMS_TO_TICKS(100)) / sizeof(int32_t);
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

static void level(const int32_t *buf, size_t frames, int mic, float *rms_dbfs, float *peak_dbfs)
{
    double sum = 0;
    int64_t peak = 1;
    for (size_t i = 0; i < frames; i++) {
        int64_t v = buf[i * MICS + mic];
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
    int32_t *buf = heap_caps_malloc(frames * MICS * sizeof(int32_t), MALLOC_CAP_SPIRAM);
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
        if (s_aligned) {
            cmd_printf("Mic lines aligned, offset %d frames\n", s_offset);
        } else {
            cmd_printf("Mic lines NOT aligned\n");
        }
        float rms, peak, best = -INFINITY;
        int src = 0;
        for (int mic = 0; mic < MICS; mic++) {
            level(buf, frames, mic, &rms, &peak);
            cmd_printf("Mic %d: rms %.1f dBFS, peak %.1f dBFS\n", mic + 1, rms, peak);
            if (rms > best) {
                best = rms;
                src = mic;
            }
        }
        /* Play the loudest mic on both slots; stereo frames fit in place. */
        for (size_t i = 0; i < frames; i++) {
            int32_t v = buf[i * MICS + src];
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
        .help = "audio test   tone, record 2 s, mic levels, play back",
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
    int32_t *buf = heap_caps_malloc(frames * MICS * sizeof(int32_t), MALLOC_CAP_SPIRAM);
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

    /* Convert in place to 16-bit, one channel per mic; the output never
     * outruns the input. */
    int16_t *pcm = (int16_t *)buf;
    for (size_t i = 0; i < frames * MICS; i++) {
        int32_t v = buf[i] >> WAV_SHIFT;
        pcm[i] = v > INT16_MAX ? INT16_MAX : v < INT16_MIN ? INT16_MIN : v;
    }
    uint32_t data_bytes = frames * MICS * sizeof(int16_t);
    uint8_t header[44];
    memcpy(header, "RIFF", 4);
    put_le(header + 4, 36 + data_bytes, 4);
    memcpy(header + 8, "WAVEfmt ", 8);
    put_le(header + 16, 16, 4);
    put_le(header + 20, 1, 2);                    /* PCM */
    put_le(header + 22, MICS, 2);                 /* channels */
    put_le(header + 24, AUDIO_RATE, 4);
    put_le(header + 28, AUDIO_RATE * MICS * 2, 4); /* byte rate */
    put_le(header + 32, MICS * 2, 2);             /* block align */
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
