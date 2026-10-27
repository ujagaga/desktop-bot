/*
 * ST7789 LCD text screen: four lines of 28 characters in landscape.
 */
#include <string.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "config.h"
#include "font.h"
#include "lcd.h"

/* RGB565, byte-swapped because the panel takes the high byte first. */
#define SWAP(c)       ((uint16_t)(((c) >> 8) | ((c) << 8)))
#define COLOR_BG      SWAP(LCD_COLOR_BG)
#define COLOR_TEXT    SWAP(LCD_COLOR_TEXT)
#define COLOR_ACCENT  SWAP(LCD_COLOR_ACCENT)

static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_frame;
static SemaphoreHandle_t s_frame_free;
static SemaphoreHandle_t s_lock;

static bool on_trans_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_frame_free, &woken);
    return woken == pdTRUE;
}

void lcd_init(void)
{
    gpio_config_t bl_cfg = {
        .pin_bit_mask = 1ULL << LCD_BL_PIN,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&bl_cfg));
    gpio_set_level(LCD_BL_PIN, !LCD_BL_ON);

    const spi_bus_config_t bus_cfg = {
        .sclk_io_num = LCD_SCL_PIN,
        .mosi_io_num = LCD_SDA_PIN,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_W * LCD_H * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    s_frame_free = xSemaphoreCreateBinary();
    xSemaphoreGive(s_frame_free);
    s_lock = xSemaphoreCreateMutex();

    esp_lcd_panel_io_handle_t io;
    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = LCD_CS_PIN,
        .dc_gpio_num = LCD_DC_PIN,
        .spi_mode = 0,
        .pclk_hz = 20 * 1000 * 1000,
        .trans_queue_depth = 4,
        .on_color_trans_done = on_trans_done,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(LCD_SPI_HOST, &io_cfg, &io));

    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_RST_PIN,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &panel_cfg, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, false));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(s_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, false, true));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(s_panel, LCD_X_GAP, LCD_Y_GAP));

    s_frame = heap_caps_malloc(LCD_W * LCD_H * sizeof(uint16_t), MALLOC_CAP_DMA);
    assert(s_frame != NULL);
    lcd_text(NULL, NULL, NULL, NULL);
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));
    gpio_set_level(LCD_BL_PIN, LCD_BL_ON);
}

static void draw_line(int row, const char *text, uint16_t color)
{
    int max_chars = LCD_W / FONT_W;
    for (int i = 0; text[i] != 0 && i < max_chars; i++) {
        unsigned char c = text[i];
        if (c < 32 || c > 126) {
            c = '?';
        }
        const uint16_t *glyph = FONT[c - 32];
        for (int y = 0; y < FONT_H; y++) {
            uint16_t *px = s_frame + (row * FONT_H + y) * LCD_W + i * FONT_W;
            for (int x = 0; x < FONT_W; x++) {
                if (glyph[y] & (1 << (FONT_W - 1 - x))) {
                    px[x] = color;
                }
            }
        }
    }
}

void lcd_text(const char *l0, const char *l1, const char *l2, const char *l3)
{
    const char *lines[] = { l0, l1, l2, l3 };
    xSemaphoreTake(s_lock, portMAX_DELAY);
    /* Wait until the previous frame has been sent out of the buffer. */
    xSemaphoreTake(s_frame_free, portMAX_DELAY);
    for (int i = 0; i < LCD_W * LCD_H; i++) {
        s_frame[i] = COLOR_BG;
    }
    for (int row = 0; row < 4; row++) {
        if (lines[row] != NULL) {
            draw_line(row, lines[row], row == 0 ? COLOR_ACCENT : COLOR_TEXT);
        }
    }
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(s_panel, 0, 0, LCD_W, LCD_H, s_frame));
    xSemaphoreGive(s_lock);
}
