/*
 * TOF400C (VL53L1X) on the MPU6050 I2C bus, polled. Init sequence and
 * registers from ST's VL53L1X ULD API (default configuration 0x2D-0x87).
 *   tof [n]   n readings (default 1): distance mm and range status
 */
#include <stdlib.h>
#include "driver/i2c_master.h"
#include "esp_console.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "config.h"
#include "cmd.h"
#include "tof.h"

#define REG_GPIO_HV_MUX_CTRL   0x0030
#define REG_GPIO_TIO_HV_STATUS 0x0031
#define REG_INTERRUPT_CLEAR    0x0086
#define REG_MODE_START         0x0087
#define REG_RANGE_STATUS       0x0089
#define REG_RANGE_MM           0x0096
#define REG_BOOT_STATE         0x00E5
#define REG_MODEL_ID           0x010F

static const uint8_t DEFAULT_CONFIG[] = {
    0x00, 0x01, 0x01, 0x01, 0x02, 0x00, 0x02, 0x08, 0x00, 0x08, 0x10, 0x01, 0x01, 0x00, 0x00, 0x00,
    0x00, 0xff, 0x00, 0x0F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x0b, 0x00, 0x00, 0x02, 0x0a, 0x21,
    0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00, 0xc8, 0x00, 0x00, 0x38, 0xff, 0x01, 0x00, 0x08, 0x00,
    0x00, 0x01, 0xdb, 0x0f, 0x01, 0xf1, 0x0d, 0x01, 0x68, 0x00, 0x80, 0x08, 0xb8, 0x00, 0x00, 0x00,
    0x00, 0x0f, 0x89, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x0f, 0x0d, 0x0e, 0x0e, 0x00,
    0x00, 0x02, 0xc7, 0xff, 0x9B, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
};

static i2c_master_dev_handle_t s_dev;
static bool s_ready;

static esp_err_t wr(uint16_t reg, uint8_t val)
{
    uint8_t buf[3] = { reg >> 8, reg & 0xff, val };
    esp_err_t err = i2c_master_transmit(s_dev, buf, sizeof(buf), 50);
    if (err != ESP_OK) {
        cmd_printf("write 0x%04x failed\n", reg);
    }
    return err;
}

static esp_err_t rd(uint16_t reg, uint8_t *out, size_t len)
{
    uint8_t addr[2] = { reg >> 8, reg & 0xff };
    esp_err_t err = i2c_master_transmit_receive(s_dev, addr, sizeof(addr), out, len, 50);
    if (err != ESP_OK) {
        cmd_printf("read 0x%04x failed\n", reg);
    }
    return err;
}

/* Waits up to timeout_ms for a measurement. */
static esp_err_t wait_ready(int timeout_ms)
{
    uint8_t mux, status;
    esp_err_t err = rd(REG_GPIO_HV_MUX_CTRL, &mux, 1);
    uint8_t ready_level = !(mux & 0x10);
    /* The sensor may NACK while it is busy; keep polling until timeout. */
    for (int t = 0; err == ESP_OK && t < timeout_ms; t += 2) {
        if (rd(REG_GPIO_TIO_HV_STATUS, &status, 1) == ESP_OK && (status & 1) == ready_level) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    return err == ESP_OK ? ESP_ERR_TIMEOUT : err;
}

static esp_err_t sensor_init(void)
{
    i2c_master_bus_handle_t bus;
    esp_err_t err = i2c_master_get_bus_handle(IMU_I2C_PORT, &bus);
    if (err != ESP_OK) {
        return err;
    }
    if (s_dev == NULL) {
        const i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = TOF_I2C_ADDR,
            .scl_speed_hz = 100000,
        };
        ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dev_cfg, &s_dev));
    }
    uint8_t id[2], boot = 0;
    err = rd(REG_MODEL_ID, id, 2);
    if (err != ESP_OK) {
        return err;
    }
    cmd_printf("VL53L1X model id 0x%02x%02x (expect 0xeacc)\n", id[0], id[1]);
    for (int t = 0; t < 100 && boot == 0 && err == ESP_OK; t++) {
        err = rd(REG_BOOT_STATE, &boot, 1);
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    for (int i = 0; i < sizeof(DEFAULT_CONFIG) && err == ESP_OK; i++) {
        err = wr(0x2D + i, DEFAULT_CONFIG[i]);
    }
    /* One throwaway measurement, then VHV settings as in VL53L1X_SensorInit. */
    if (err == ESP_OK) err = wr(REG_INTERRUPT_CLEAR, 0x01);
    if (err == ESP_OK) err = wr(REG_MODE_START, 0x40);
    if (err == ESP_OK) err = wait_ready(300);
    if (err == ESP_OK) err = wr(REG_INTERRUPT_CLEAR, 0x01);
    if (err == ESP_OK) err = wr(REG_MODE_START, 0x00);
    if (err == ESP_OK) err = wr(0x0008, 0x09);
    if (err == ESP_OK) err = wr(0x000B, 0x00);
    /* Continuous ranging with the default 100 ms timing budget. */
    if (err == ESP_OK) err = wr(REG_INTERRUPT_CLEAR, 0x01);
    if (err == ESP_OK) err = wr(REG_MODE_START, 0x40);
    return err;
}

static int cmd_tof(int argc, char **argv)
{
    int count = argc > 1 ? atoi(argv[1]) : 1;
    esp_err_t err = ESP_OK;
    if (!s_ready) {
        err = sensor_init();
        s_ready = err == ESP_OK;
    }
    for (int i = 0; i < count && err == ESP_OK; i++) {
        uint8_t status, mm[2];
        err = wait_ready(500);
        if (err == ESP_OK) err = rd(REG_RANGE_STATUS, &status, 1);
        if (err == ESP_OK) err = rd(REG_RANGE_MM, mm, 2);
        if (err == ESP_OK) err = wr(REG_INTERRUPT_CLEAR, 0x01);
        if (err == ESP_OK) {
            /* Raw status 9 = valid range (ULD maps it to 0). */
            cmd_printf("%u mm (status %u%s)\n", (mm[0] << 8) | mm[1], status & 0x1f,
                       (status & 0x1f) == 9 ? " ok" : "");
        }
    }
    if (err != ESP_OK) {
        s_ready = false;
        cmd_printf("ERR tof %s\n", esp_err_to_name(err));
        return 1;
    }
    return 0;
}

void tof_register_command(void)
{
    const esp_console_cmd_t cmd = {
        .command = "tof",
        .help = "tof [n]   n VL53L1X readings in mm",
        .func = cmd_tof,
    };
    cmd_register(&cmd);
}
