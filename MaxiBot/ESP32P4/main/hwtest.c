/*
 * Hardware bring-up commands:
 *   wiretest [s] [drive]  read the WROOM SPI wires as inputs and print
 *                     changes, or drive them high one at a time
 *   i2c scan          list devices on the MPU6050 / ToF bus
 *   irled on|off      camera IR LEDs enable (IO47)
 *   gpio <pin> 0|1|in drive a pin, or read it with pull-down (takes the pin
 *                     from its peripheral until reboot)
 *   lcdprobe          bit-bang the ST7789: read its ID, then fill it red
 *                     (takes the LCD pins until reboot)
 */
#include <stdlib.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_console.h"
#include "esp_ldo_regulator.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "config.h"
#include "cmd.h"
#include "hwtest.h"

/* P4 pin and the WROOM pin it should be wired to. */
static const struct { int p4; int wroom; const char *name; } WIRES[] = {
    { 23, 23, "MOSI" }, { 34, 19, "MISO" }, { 33, 18, "SCLK" },
    { 32, 5, "CS" },    { 26, 17, "HS" },   { 27, 16, "DR" },
};
#define WIRE_COUNT (sizeof(WIRES) / sizeof(WIRES[0]))

/* P4 drives each wire high alone for 1 s, then 2 s all low. */
static void wiretest_drive(int seconds)
{
    gpio_config_t cfg = { .mode = GPIO_MODE_OUTPUT };
    for (int i = 0; i < WIRE_COUNT; i++) {
        cfg.pin_bit_mask |= 1ULL << WIRES[i].p4;
    }
    gpio_config(&cfg);
    int64_t end = esp_timer_get_time() + seconds * 1000000LL;
    while (esp_timer_get_time() < end) {
        for (int i = 0; i < WIRE_COUNT; i++) {
            gpio_set_level(WIRES[i].p4, 0);
        }
        cmd_printf("all low\n");
        vTaskDelay(pdMS_TO_TICKS(2000));
        for (int i = 0; i < WIRE_COUNT; i++) {
            for (int j = 0; j < WIRE_COUNT; j++) {
                gpio_set_level(WIRES[j].p4, i == j);
            }
            cmd_printf("IO%d (%s) high\n", WIRES[i].p4, WIRES[i].name);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    for (int i = 0; i < WIRE_COUNT; i++) {
        gpio_set_level(WIRES[i].p4, 0);
    }
}

static int cmd_wiretest(int argc, char **argv)
{
    int seconds = argc > 1 ? atoi(argv[1]) : 10;
    if (argc > 2 && strcmp(argv[2], "drive") == 0) {
        wiretest_drive(seconds);
        cmd_printf("Reboot to give the pins back to ESP-Hosted\n");
        return 0;
    }
    gpio_config_t cfg = { .mode = GPIO_MODE_INPUT, .pull_down_en = GPIO_PULLDOWN_ENABLE };
    for (int i = 0; i < WIRE_COUNT; i++) {
        cfg.pin_bit_mask |= 1ULL << WIRES[i].p4;
    }
    gpio_config(&cfg);
    cmd_printf("Expected order: ");
    for (int i = 0; i < WIRE_COUNT; i++) {
        cmd_printf("%s(WROOM%d=IO%d) ", WIRES[i].name, WIRES[i].wroom, WIRES[i].p4);
    }
    cmd_printf("\n");
    /* Also watch every other pin (input enable only, outputs untouched) to
     * find wires that landed on the wrong pin. Skips the USB console pins. */
    uint64_t others = 0;
    for (int pin = 0; pin < GPIO_NUM_MAX; pin++) {
        if (pin != 24 && pin != 25 && !(cfg.pin_bit_mask & (1ULL << pin))) {
            gpio_input_enable(pin);
            others |= 1ULL << pin;
        }
    }
    uint64_t last_other = 0;
    for (int pin = 0; pin < GPIO_NUM_MAX; pin++) {
        if ((others & (1ULL << pin)) && gpio_get_level(pin)) {
            last_other |= 1ULL << pin;
        }
    }
    int last = -1;
    int64_t end = esp_timer_get_time() + seconds * 1000000LL;
    while (esp_timer_get_time() < end) {
        int state = 0;
        for (int i = 0; i < WIRE_COUNT; i++) {
            state |= gpio_get_level(WIRES[i].p4) << i;
        }
        if (state != last) {
            cmd_printf("%6lld ms high:", esp_timer_get_time() / 1000);
            for (int i = 0; i < WIRE_COUNT; i++) {
                if (state & (1 << i)) {
                    cmd_printf(" IO%d(%s)", WIRES[i].p4, WIRES[i].name);
                }
            }
            cmd_printf(state ? "\n" : " none\n");
            last = state;
        }
        uint64_t other = 0;
        for (int pin = 0; pin < GPIO_NUM_MAX; pin++) {
            if ((others & (1ULL << pin)) && gpio_get_level(pin)) {
                other |= 1ULL << pin;
            }
        }
        if (other != last_other) {
            cmd_printf("%6lld ms other pins changed:", esp_timer_get_time() / 1000);
            for (int pin = 0; pin < GPIO_NUM_MAX; pin++) {
                if ((other ^ last_other) & (1ULL << pin)) {
                    cmd_printf(" IO%d=%d", pin, (int)((other >> pin) & 1));
                }
            }
            cmd_printf("\n");
            last_other = other;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    cmd_printf("Reboot to give the pins back to ESP-Hosted\n");
    return 0;
}

static int cmd_i2c(int argc, char **argv)
{
    i2c_master_bus_handle_t bus;
    if (argc != 2 || strcmp(argv[1], "scan") != 0) {
        cmd_printf("ERR i2c scan\n");
        return 1;
    }
    if (i2c_master_get_bus_handle(IMU_I2C_PORT, &bus) != ESP_OK) {
        cmd_printf("ERR I2C bus not started\n");
        return 1;
    }
    int found = 0;
    for (int addr = 0x08; addr < 0x78; addr++) {
        if (i2c_master_probe(bus, addr, 50) == ESP_OK) {
            cmd_printf("found 0x%02x\n", addr);
            found++;
        }
    }
    cmd_printf("%d device(s)\n", found);
    return 0;
}

static int cmd_irled(int argc, char **argv)
{
    if (argc != 2 || (strcmp(argv[1], "on") != 0 && strcmp(argv[1], "off") != 0)) {
        cmd_printf("ERR irled on|off\n");
        return 1;
    }
    bool on = strcmp(argv[1], "on") == 0;
    gpio_set_level(IR_CAM_LED_PIN, on ? IR_CAM_LED_ON : !IR_CAM_LED_ON);
    cmd_printf("IR LEDs %s\n", argv[1]);
    return 0;
}

static int cmd_gpio(int argc, char **argv)
{
    int pin = argc == 3 ? atoi(argv[1]) : -1;
    if (pin < 0 || pin >= GPIO_NUM_MAX) {
        cmd_printf("ERR gpio <pin> 0|1|in\n");
        return 1;
    }
    if (strcmp(argv[2], "in") == 0) {
        gpio_config_t cfg = { .pin_bit_mask = 1ULL << pin, .mode = GPIO_MODE_INPUT,
                              .pull_down_en = GPIO_PULLDOWN_ENABLE };
        gpio_config(&cfg);
        vTaskDelay(pdMS_TO_TICKS(10));
        cmd_printf("IO%d = %d\n", pin, gpio_get_level(pin));
        return 0;
    }
    gpio_config_t cfg = { .pin_bit_mask = 1ULL << pin, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&cfg);
    gpio_set_level(pin, atoi(argv[2]) != 0);
    cmd_printf("IO%d set %d\n", pin, atoi(argv[2]) != 0);
    return 0;
}

/* Slow software SPI so long or bad wires still work. */
static void bb_byte(uint8_t b)
{
    for (int i = 7; i >= 0; i--) {
        gpio_set_level(LCD_SDA_PIN, (b >> i) & 1);
        esp_rom_delay_us(5);
        gpio_set_level(LCD_SCL_PIN, 1);
        esp_rom_delay_us(5);
        gpio_set_level(LCD_SCL_PIN, 0);
    }
}

static void bb_cmd(uint8_t cmd, const uint8_t *data, int len)
{
    gpio_set_level(LCD_CS_PIN, 0);
    gpio_set_level(LCD_DC_PIN, 0);
    bb_byte(cmd);
    gpio_set_level(LCD_DC_PIN, 1);
    for (int i = 0; i < len; i++) {
        bb_byte(data[i]);
    }
    gpio_set_level(LCD_CS_PIN, 1);
}

static int cmd_lcdprobe(int argc, char **argv)
{
    gpio_config_t cfg = { .mode = GPIO_MODE_OUTPUT,
                          .pin_bit_mask = (1ULL << LCD_SCL_PIN) | (1ULL << LCD_SDA_PIN) |
                                          (1ULL << LCD_RST_PIN) | (1ULL << LCD_DC_PIN) |
                                          (1ULL << LCD_CS_PIN) | (1ULL << LCD_BL_PIN) };
    gpio_config(&cfg);
    gpio_set_level(LCD_BL_PIN, LCD_BL_ON);
    gpio_set_level(LCD_CS_PIN, 1);
    gpio_set_level(LCD_SCL_PIN, 0);
    gpio_set_level(LCD_RST_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(LCD_RST_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(150));

    /* RDDID (0x04): one dummy clock, then 24 bits on SDA. */
    gpio_set_level(LCD_CS_PIN, 0);
    gpio_set_level(LCD_DC_PIN, 0);
    bb_byte(0x04);
    gpio_set_direction(LCD_SDA_PIN, GPIO_MODE_INPUT);
    gpio_pulldown_en(LCD_SDA_PIN);
    uint32_t id = 0;
    for (int i = 0; i < 25; i++) {
        gpio_set_level(LCD_SCL_PIN, 1);
        esp_rom_delay_us(5);
        id = (id << 1) | gpio_get_level(LCD_SDA_PIN);
        gpio_set_level(LCD_SCL_PIN, 0);
        esp_rom_delay_us(5);
    }
    gpio_set_level(LCD_CS_PIN, 1);
    gpio_pulldown_dis(LCD_SDA_PIN);
    gpio_set_direction(LCD_SDA_PIN, GPIO_MODE_OUTPUT);
    cmd_printf("LCD id 0x%06lx (ST7789: 0x8581xx or 0x8552xx; 0 or 0xffffff = no answer)\n",
               (unsigned long)(id & 0xffffff));

    /* Wake, 16-bit colour, display on, fill the whole 240x320 RAM red. */
    const uint8_t colmod = 0x55, caset[] = { 0, 0, 0, 239 }, raset[] = { 0, 0, 0x01, 0x3f };
    bb_cmd(0x11, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(120));
    bb_cmd(0x3a, &colmod, 1);
    bb_cmd(0x29, NULL, 0);
    bb_cmd(0x2a, caset, 4);
    bb_cmd(0x2b, raset, 4);
    gpio_set_level(LCD_CS_PIN, 0);
    gpio_set_level(LCD_DC_PIN, 0);
    bb_byte(0x2c);
    gpio_set_level(LCD_DC_PIN, 1);
    for (int i = 0; i < 240 * 320; i++) {
        bb_byte(0xf8);
        bb_byte(0x00);
    }
    gpio_set_level(LCD_CS_PIN, 1);
    cmd_printf("filled red; reboot to give the pins back to the LCD driver\n");
    return 0;
}

void hwtest_start(void)
{
    /* IO39-IO48 (VDDPST_5) are powered from LDO channel 4 on this board;
     * unset, a high output on IO47 only reaches ~1.3 V. */
    static esp_ldo_channel_handle_t ldo;
    const esp_ldo_channel_config_t ldo_cfg = { .chan_id = 4, .voltage_mv = 3300 };
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_cfg, &ldo));
    gpio_config_t cfg = { .pin_bit_mask = 1ULL << IR_CAM_LED_PIN, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&cfg);
    gpio_set_level(IR_CAM_LED_PIN, !IR_CAM_LED_ON);
}

void hwtest_register_command(void)
{
    const esp_console_cmd_t cmds[] = {
        { .command = "wiretest", .help = "wiretest [s] [drive]   read (or drive) the WROOM SPI wires", .func = cmd_wiretest },
        { .command = "i2c", .help = "i2c scan   list devices on the IMU/ToF bus", .func = cmd_i2c },
        { .command = "irled", .help = "irled on|off   camera IR LEDs (IO47)", .func = cmd_irled },
        { .command = "lcdprobe", .help = "lcdprobe   bit-bang the LCD: read ID, fill red", .func = cmd_lcdprobe },
        { .command = "gpio", .help = "gpio <pin> 0|1|in   drive or read a pin (until reboot)", .func = cmd_gpio },
    };
    for (int i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        cmd_register(&cmds[i]);
    }
}
