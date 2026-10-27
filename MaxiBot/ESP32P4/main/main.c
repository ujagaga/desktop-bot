/*
 * MaxiBot: OV5647 CSI camera on WT9932P4-TINY served over Wi-Fi (ESP32-WROOM
 * co-processor via ESP-Hosted). As on the ESP32-CAM, port 80 serves /jpg and
 * the Wi-Fi setup page, port 81 serves the MJPEG /stream, so a running stream
 * does not block port 80.
 */
#include <fcntl.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_video_init.h"
#include "esp_video_device.h"
#include "linux/videodev2.h"
#include "driver/jpeg_encode.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_console.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "wifi.h"
#include "lcd.h"
#include "cmd.h"
#include "gyro.h"
#include "head.h"
#include "audio.h"
#include "wake_word.h"
#include "config.h"


static const char *TAG = "maxibot";

static int s_fd;
static uint8_t *s_buffers[BUF_COUNT];
static jpeg_encoder_handle_t s_jpeg_handle;
static jpeg_encode_cfg_t s_jpeg_cfg;
static uint8_t *s_jpeg_outbuf;
static size_t s_jpeg_outbuf_size;
static SemaphoreHandle_t s_cam_lock;

/* Dequeues the next frame and JPEG-encodes it into s_jpeg_outbuf.
 * Caller holds s_cam_lock until it has sent the result. */
static esp_err_t capture_jpeg(int quality, uint32_t *jpeg_size)
{
    struct v4l2_buffer buf = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
    };
    if (ioctl(s_fd, VIDIOC_DQBUF, &buf) != 0) {
        return ESP_FAIL;
    }
    s_jpeg_cfg.image_quality = quality;
    esp_err_t ret = jpeg_encoder_process(s_jpeg_handle, &s_jpeg_cfg, s_buffers[buf.index], buf.bytesused,
                                         s_jpeg_outbuf, s_jpeg_outbuf_size, jpeg_size);
    ioctl(s_fd, VIDIOC_QBUF, &buf);
    return ret;
}

/* JPEG quality from the "q" query parameter (1-100), else JPEG_QUALITY. */
static int query_quality(httpd_req_t *req)
{
    char query[32];
    char value[8];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "q", value, sizeof(value)) == ESP_OK) {
        int q = atoi(value);
        if (q >= 1 && q <= 100) {
            return q;
        }
    }
    return JPEG_QUALITY;
}

static esp_err_t jpg_handler(httpd_req_t *req)
{
    int quality = query_quality(req);
    uint32_t jpeg_size = 0;
    xSemaphoreTake(s_cam_lock, portMAX_DELAY);
    /* Without a stream running the queued buffers hold old frames; drop them. */
    esp_err_t ret = ESP_OK;
    for (int i = 0; i <= BUF_COUNT && ret == ESP_OK; i++) {
        ret = capture_jpeg(quality, &jpeg_size);
    }
    if (ret == ESP_OK) {
        httpd_resp_set_type(req, "image/jpeg");
        ret = httpd_resp_send(req, (const char *)s_jpeg_outbuf, jpeg_size);
    } else {
        httpd_resp_send_500(req);
    }
    xSemaphoreGive(s_cam_lock);
    return ret;
}

#define BOUNDARY "frame"

static esp_err_t stream_handler(httpd_req_t *req)
{
    int quality = query_quality(req);
    httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=" BOUNDARY);
    char part[96];
    esp_err_t ret = ESP_OK;
    while (ret == ESP_OK) {
        uint32_t jpeg_size = 0;
        xSemaphoreTake(s_cam_lock, portMAX_DELAY);
        ret = capture_jpeg(quality, &jpeg_size);
        if (ret == ESP_OK) {
            int len = snprintf(part, sizeof(part),
                               "\r\n--" BOUNDARY "\r\nContent-Type: image/jpeg\r\nContent-Length: %lu\r\n\r\n",
                               (unsigned long)jpeg_size);
            ret = httpd_resp_send_chunk(req, part, len);
        }
        if (ret == ESP_OK) {
            ret = httpd_resp_send_chunk(req, (const char *)s_jpeg_outbuf, jpeg_size);
        }
        xSemaphoreGive(s_cam_lock);
    }
    ESP_LOGI(TAG, "stream closed");
    return ret;
}

static void start_http(void)
{
    httpd_handle_t server;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    /* Drop the oldest connection instead of refusing new ones when clients
     * vanish without closing (e.g. a laptop switching networks). */
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 12;
    ESP_ERROR_CHECK(httpd_start(&server, &cfg));
    const httpd_uri_t jpg = { .uri = "/jpg", .method = HTTP_GET, .handler = jpg_handler };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &jpg));
    wifi_register_http(server);
    cmd_register_http(server);
    audio_register_http(server);

    httpd_handle_t stream_server;
    cfg.server_port = 81;
    cfg.ctrl_port += 1;
    ESP_ERROR_CHECK(httpd_start(&stream_server, &cfg));
    const httpd_uri_t stream = { .uri = "/stream", .method = HTTP_GET, .handler = stream_handler };
    ESP_ERROR_CHECK(httpd_register_uri_handler(stream_server, &stream));
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    lcd_init();
    head_start();
    gyro_start();
    audio_start();
    wake_word_start();
    wifi_start();

    gpio_config_t cam_en_cfg = {
        .pin_bit_mask = 1ULL << CAM_ENABLE_PIN,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&cam_en_cfg));
    gpio_set_level(CAM_ENABLE_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(100));

    esp_video_init_csi_config_t csi_config[] = {{
        .sccb_config = {
            .init_sccb = true,
            .i2c_config = {
                .port = CAM_I2C_PORT,
                .scl_pin = CAM_I2C_SCL_PIN,
                .sda_pin = CAM_I2C_SDA_PIN,
            },
            .freq = 100000,
        },
        .reset_pin = CAM_RESET_PIN,
        .pwdn_pin = CAM_PWDN_PIN,
    }};
    esp_video_init_config_t cam_config = { .csi = csi_config };
    ESP_ERROR_CHECK(esp_video_init(&cam_config));

    int fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDONLY);
    s_fd = fd;
    if (fd < 0) {
        ESP_LOGE(TAG, "open camera failed");
        return;
    }

    struct v4l2_format fmt = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
    ESP_ERROR_CHECK(ioctl(fd, VIDIOC_G_FMT, &fmt));

    /* Camera sensor outputs raw Bayer; ask the ISP pipeline to demosaic
     * it to RGB565 so the JPEG encoder has a format it accepts. */
    if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_RGB565) {
        struct v4l2_format rgb_fmt = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .fmt.pix.width = fmt.fmt.pix.width,
            .fmt.pix.height = fmt.fmt.pix.height,
            .fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565,
        };
        ESP_ERROR_CHECK(ioctl(fd, VIDIOC_S_FMT, &rgb_fmt));
        fmt = rgb_fmt;
    }
    uint16_t width = (uint16_t)fmt.fmt.pix.width;
    uint16_t height = (uint16_t)fmt.fmt.pix.height;
    ESP_LOGI(TAG, "camera format %ux%u", width, height);

    struct v4l2_requestbuffers req = {
        .count = BUF_COUNT,
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
    };
    ESP_ERROR_CHECK(ioctl(fd, VIDIOC_REQBUFS, &req));

    for (int i = 0; i < BUF_COUNT; i++) {
        struct v4l2_buffer buf = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
            .index = i,
        };
        ESP_ERROR_CHECK(ioctl(fd, VIDIOC_QUERYBUF, &buf));
        s_buffers[i] = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.offset);
    }

    jpeg_encode_engine_cfg_t jpeg_eng_cfg = { .timeout_ms = 200 };
    ESP_ERROR_CHECK(jpeg_new_encoder_engine(&jpeg_eng_cfg, &s_jpeg_handle));

    uint32_t raw_frame_size = (uint32_t)width * height * 2; /* RGB565 */
    jpeg_encode_memory_alloc_cfg_t jpeg_out_mem_cfg = { .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER };
    s_jpeg_outbuf = jpeg_alloc_encoder_mem(raw_frame_size / 4, &jpeg_out_mem_cfg, &s_jpeg_outbuf_size);
    assert(s_jpeg_outbuf != NULL);

    s_jpeg_cfg = (jpeg_encode_cfg_t) {
        .width = width,
        .height = height,
        .src_type = JPEG_ENCODE_IN_FORMAT_RGB565,
        .sub_sample = JPEG_DOWN_SAMPLING_YUV420,
        .image_quality = JPEG_QUALITY,
    };

    for (int i = 0; i < BUF_COUNT; i++) {
        struct v4l2_buffer buf = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
            .index = i,
        };
        ESP_ERROR_CHECK(ioctl(fd, VIDIOC_QBUF, &buf));
    }
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ESP_ERROR_CHECK(ioctl(fd, VIDIOC_STREAMON, &type));

    s_cam_lock = xSemaphoreCreateMutex();
    start_http();
    ESP_LOGI(TAG, "camera ready: http://<ip>/jpg, http://<ip>:81/stream");

    esp_console_repl_t *repl;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "maxibot>";
    esp_console_dev_usb_serial_jtag_config_t usj_cfg = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&usj_cfg, &repl_cfg, &repl));
    ESP_ERROR_CHECK(esp_console_register_help_command());
    wifi_register_command();
    gyro_register_command();
    head_register_command();
    audio_register_command();
    wake_word_register_command();
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
