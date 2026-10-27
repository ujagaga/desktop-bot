/*
 * MPU6050 angles. A task samples gyro and accelerometer every 10 ms.
 * X and Y use a complementary filter (gyro, pulled slowly toward the
 * accelerometer's tilt), so they do not drift. Z has no reference and is
 * gyro integration only, so it drifts slowly; recalibrate to reset it.
 * Axes are the sensor's own; their meaning depends on how it is mounted.
 */
#include <math.h>
#include <string.h>
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_console.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "config.h"
#include "cmd.h"
#include "gyro.h"

#define REG_SMPLRT_DIV    0x19
#define REG_CONFIG        0x1A
#define REG_GYRO_CONFIG   0x1B
#define REG_ACCEL_CONFIG  0x1C
#define REG_ACCEL_XOUT_H  0x3B
#define REG_PWR_MGMT_1    0x6B
#define REG_WHO_AM_I      0x75

#define GYRO_LSB_PER_DPS  131.0f    /* +-250 dps range */
#define PERIOD_MS         10
#define CALIB_SAMPLES     200
#define NVS_NAMESPACE     "gyro"

static const char *TAG = "gyro";

static i2c_master_dev_handle_t s_dev;
static bool s_detected;
static float s_bias[3];
static volatile float s_rate[3];
static volatile float s_angle[3];
static volatile bool s_calib_request;
static SemaphoreHandle_t s_calib_done;

static esp_err_t write_reg(uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = { reg, value };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), 50);
}

/* Reads accelerometer (g) and gyro (dps, without bias) in one burst. */
static esp_err_t read_sample(float accel[3], float gyro[3])
{
    uint8_t reg = REG_ACCEL_XOUT_H;
    uint8_t raw[14];
    esp_err_t err = i2c_master_transmit_receive(s_dev, &reg, 1, raw, sizeof(raw), 50);
    if (err != ESP_OK) {
        return err;
    }
    for (int i = 0; i < 3; i++) {
        accel[i] = (int16_t)(raw[i * 2] << 8 | raw[i * 2 + 1]) / 16384.0f;
        gyro[i] = (int16_t)(raw[8 + i * 2] << 8 | raw[8 + i * 2 + 1]) / GYRO_LSB_PER_DPS;
    }
    return ESP_OK;
}

static bool load_bias(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }
    size_t len = sizeof(s_bias);
    bool ok = nvs_get_blob(nvs, "bias", s_bias, &len) == ESP_OK && len == sizeof(s_bias);
    nvs_close(nvs);
    return ok;
}

static void save_bias(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }
    nvs_set_blob(nvs, "bias", s_bias, sizeof(s_bias));
    nvs_commit(nvs);
    nvs_close(nvs);
}

/* Tilt around X and Y from gravity, in degrees. */
static void accel_angles(const float a[3], float out[2])
{
    out[0] = atan2f(a[1], a[2]) * 180.0f / M_PI;
    out[1] = atan2f(-a[0], sqrtf(a[1] * a[1] + a[2] * a[2])) * 180.0f / M_PI;
}

static void gyro_task(void *arg)
{
    float sum[3] = { 0 };
    int samples = 0;
    float accel[3], gyro[3], tilt[2];

    /* Start X/Y from the accelerometer so they do not have to converge. */
    if (read_sample(accel, gyro) == ESP_OK) {
        accel_angles(accel, tilt);
        s_angle[0] = tilt[0];
        s_angle[1] = tilt[1];
    }

    int64_t last_us = esp_timer_get_time();
    TickType_t wake = xTaskGetTickCount();
    while (true) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(PERIOD_MS));
        int64_t now_us = esp_timer_get_time();
        float dt = (now_us - last_us) / 1e6f;
        last_us = now_us;
        if (read_sample(accel, gyro) != ESP_OK) {
            continue;
        }

        if (s_calib_request) {
            for (int i = 0; i < 3; i++) {
                sum[i] += gyro[i];
            }
            if (++samples == CALIB_SAMPLES) {
                for (int i = 0; i < 3; i++) {
                    s_bias[i] = sum[i] / CALIB_SAMPLES;
                    sum[i] = 0;
                }
                samples = 0;
                accel_angles(accel, tilt);
                s_angle[0] = tilt[0];
                s_angle[1] = tilt[1];
                s_angle[2] = 0;
                save_bias();
                s_calib_request = false;
                xSemaphoreGive(s_calib_done);
            }
            continue;
        }

        for (int i = 0; i < 3; i++) {
            s_rate[i] = gyro[i] - s_bias[i];
        }
        accel_angles(accel, tilt);
        for (int i = 0; i < 2; i++) {
            s_angle[i] = IMU_GYRO_WEIGHT * (s_angle[i] + s_rate[i] * dt) + (1.0f - IMU_GYRO_WEIGHT) * tilt[i];
        }
        s_angle[2] += s_rate[2] * dt;
    }
}

void gyro_start(void)
{
    i2c_master_bus_handle_t bus;
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = IMU_I2C_PORT,
        .sda_io_num = IMU_SDA_PIN,
        .scl_io_num = IMU_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));
    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = IMU_I2C_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dev_cfg, &s_dev));

    uint8_t reg = REG_WHO_AM_I;
    uint8_t who = 0;
    if (i2c_master_transmit_receive(s_dev, &reg, 1, &who, 1, 50) != ESP_OK) {
        ESP_LOGE(TAG, "MPU6050 not found at 0x%02x", IMU_I2C_ADDR);
        return;
    }
    ESP_LOGI(TAG, "WHO_AM_I 0x%02x", who);
    if (write_reg(REG_PWR_MGMT_1, 0x01) != ESP_OK ||   /* wake, gyro X PLL clock */
        write_reg(REG_SMPLRT_DIV, 9) != ESP_OK ||      /* 1 kHz / 10 = 100 Hz */
        write_reg(REG_CONFIG, 3) != ESP_OK ||          /* 44 Hz low-pass */
        write_reg(REG_GYRO_CONFIG, 0x00) != ESP_OK ||  /* +-250 dps */
        write_reg(REG_ACCEL_CONFIG, 0x00) != ESP_OK) { /* +-2 g */
        ESP_LOGE(TAG, "MPU6050 configuration failed");
        return;
    }
    s_detected = true;

    s_calib_done = xSemaphoreCreateBinary();
    if (load_bias()) {
        ESP_LOGI(TAG, "loaded saved calibration");
    } else {
        ESP_LOGW(TAG, "no saved calibration, calibrating: keep the robot still");
        s_calib_request = true;
    }
    xTaskCreate(gyro_task, "gyro", 4096, NULL, 6, NULL);
}

static int axis_from_text(const char *text)
{
    if (strcasecmp(text, "x") == 0 || strcmp(text, "0") == 0) return 0;
    if (strcasecmp(text, "y") == 0 || strcmp(text, "1") == 0) return 1;
    if (strcasecmp(text, "z") == 0 || strcmp(text, "2") == 0) return 2;
    return -1;
}

static int cmd_gyro(int argc, char **argv)
{
    if (!s_detected) {
        cmd_printf("ERR gyro not detected\n");
        return 1;
    }
    if (argc == 3 && (strcasecmp(argv[1], "angle") == 0 || strcasecmp(argv[1], "rate") == 0)) {
        int axis = axis_from_text(argv[2]);
        if (axis < 0) {
            cmd_printf("ERR %s <x|y|z|0|1|2>\n", argv[1]);
            return 1;
        }
        if (strcasecmp(argv[1], "angle") == 0) {
            cmd_printf("%.2f\n", s_angle[axis]);
        } else {
            cmd_printf("RATE %c = %.2f dps\n", "XYZ"[axis], s_rate[axis]);
        }
        return 0;
    }
    if (argc == 2 && strcasecmp(argv[1], "calibrate") == 0) {
        cmd_printf("CALIBRATING keep gyro still...\n");
        xSemaphoreTake(s_calib_done, 0);
        s_calib_request = true;
        if (xSemaphoreTake(s_calib_done, pdMS_TO_TICKS(CALIB_SAMPLES * PERIOD_MS * 3)) != pdTRUE) {
            cmd_printf("ERR gyro read failed during calibration\n");
            return 1;
        }
        cmd_printf("BIAS X=%.2f Y=%.2f Z=%.2f dps\n", s_bias[0], s_bias[1], s_bias[2]);
        return 0;
    }
    cmd_printf("ERR gyro <angle|rate|calibrate>\n");
    return 1;
}

void gyro_register_command(void)
{
    const esp_console_cmd_t cmd = {
        .command = "gyro",
        .help = "gyro angle <x|y|z> | gyro rate <x|y|z> | gyro calibrate",
        .func = cmd_gyro,
    };
    cmd_register(&cmd);
}
