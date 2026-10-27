/*
 * Head pan and tilt servos on LEDC PWM. A task moves each servo toward its
 * target by at most HEAD_SPEED_DPS, every 20 ms (one servo period).
 */
#include <stdlib.h>
#include <string.h>
#include "driver/ledc.h"
#include "esp_console.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "config.h"
#include "cmd.h"
#include "head.h"

#define PERIOD_MS   20
#define DUTY_BITS   LEDC_TIMER_14_BIT

enum { PAN, TILT };

static const ledc_channel_t CHANNEL[2] = { LEDC_CHANNEL_0, LEDC_CHANNEL_1 };
static const int PIN[2] = { HEAD_PAN_PIN, HEAD_TILT_PIN };
static volatile float s_target[2] = { HEAD_CENTER_DEG, HEAD_CENTER_DEG };
static float s_current[2] = { HEAD_CENTER_DEG, HEAD_CENTER_DEG };

static void set_angle(int servo, float deg)
{
    float us = HEAD_MIN_US + (HEAD_MAX_US - HEAD_MIN_US) * deg / 180.0f;
    uint32_t duty = (uint32_t)(us / (1000.0f * PERIOD_MS) * (1 << DUTY_BITS));
    ledc_set_duty(LEDC_LOW_SPEED_MODE, CHANNEL[servo], duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, CHANNEL[servo]);
}

static void head_task(void *arg)
{
    const float max_step = HEAD_SPEED_DPS * PERIOD_MS / 1000.0f;
    TickType_t wake = xTaskGetTickCount();
    while (true) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(PERIOD_MS));
        for (int i = 0; i < 2; i++) {
            float diff = s_target[i] - s_current[i];
            if (diff == 0) {
                continue;
            }
            if (diff > max_step) {
                diff = max_step;
            } else if (diff < -max_step) {
                diff = -max_step;
            }
            s_current[i] += diff;
            set_angle(i, s_current[i]);
        }
    }
}

void head_start(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = DUTY_BITS,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 1000 / PERIOD_MS,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));
    for (int i = 0; i < 2; i++) {
        const ledc_channel_config_t channel = {
            .gpio_num = PIN[i],
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = CHANNEL[i],
            .timer_sel = LEDC_TIMER_0,
        };
        ESP_ERROR_CHECK(ledc_channel_config(&channel));
        set_angle(i, s_current[i]);
    }
    xTaskCreate(head_task, "head", 2048, NULL, 5, NULL);
}

static int cmd_head(int argc, char **argv)
{
    if (argc == 3) {
        char *end_pan, *end_tilt;
        float pan = strtof(argv[1], &end_pan);
        float tilt = strtof(argv[2], &end_tilt);
        if (*end_pan || *end_tilt || pan < 0 || pan > 180 || tilt < 0 || tilt > 180) {
            cmd_printf("ERR head <pan 0-180> <tilt 0-180>\n");
            return 1;
        }
        s_target[PAN] = pan;
        s_target[TILT] = tilt;
    } else if (argc == 2 && strcmp(argv[1], "center") == 0) {
        s_target[PAN] = HEAD_CENTER_DEG;
        s_target[TILT] = HEAD_CENTER_DEG;
    } else if (argc != 1) {
        cmd_printf("ERR head [<pan> <tilt> | center]\n");
        return 1;
    }
    cmd_printf("HEAD pan %.1f tilt %.1f (target %.1f %.1f)\n",
               s_current[PAN], s_current[TILT], s_target[PAN], s_target[TILT]);
    return 0;
}

void head_register_command(void)
{
    const esp_console_cmd_t cmd = {
        .command = "head",
        .help = "head | head <pan> <tilt> | head center   (degrees 0-180)",
        .func = cmd_head,
    };
    cmd_register(&cmd);
}
