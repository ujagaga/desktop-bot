/*
 * OV5647 CSI camera on WT9932P4-TINY -> MJPEG USB UVC webcam on the
 * High-Speed USB port (HUSB). Frames are captured and JPEG-encoded on
 * demand when the UVC host asks for the next one.
 */
#include <fcntl.h>
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
#include "esp_heap_caps.h"
#include "usb_device_uvc.h"
#include "config.h"


static const char *TAG = "uart_cam";

static int s_fd;
static uint8_t *s_buffers[BUF_COUNT];
static jpeg_encoder_handle_t s_jpeg_handle;
static jpeg_encode_cfg_t s_jpeg_cfg;
static uint8_t *s_jpeg_outbuf;
static size_t s_jpeg_outbuf_size;
static uvc_fb_t s_fb;

static esp_err_t uvc_start_cb(uvc_format_t format, int width, int height, int rate, void *cb_ctx)
{
    ESP_LOGI(TAG, "UVC start %dx%d@%d", width, height, rate);
    /* STREAMOFF drops all queued buffers, so queue them on every start. */
    for (int i = 0; i < BUF_COUNT; i++) {
        struct v4l2_buffer buf = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
            .index = i,
        };
        if (ioctl(s_fd, VIDIOC_QBUF, &buf) != 0) {
            return ESP_FAIL;
        }
    }
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    return ioctl(s_fd, VIDIOC_STREAMON, &type) == 0 ? ESP_OK : ESP_FAIL;
}

static uvc_fb_t *uvc_fb_get_cb(void *cb_ctx)
{
    struct v4l2_buffer buf = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
    };
    if (ioctl(s_fd, VIDIOC_DQBUF, &buf) != 0) {
        return NULL;
    }

    uint32_t jpeg_size = 0;
    esp_err_t ret = jpeg_encoder_process(s_jpeg_handle, &s_jpeg_cfg, s_buffers[buf.index], buf.bytesused,
                                         s_jpeg_outbuf, s_jpeg_outbuf_size, &jpeg_size);
    ioctl(s_fd, VIDIOC_QBUF, &buf);
    if (ret != ESP_OK) {
        return NULL;
    }

    s_fb.buf = s_jpeg_outbuf;
    s_fb.len = jpeg_size;
    s_fb.width = s_jpeg_cfg.width;
    s_fb.height = s_jpeg_cfg.height;
    s_fb.format = UVC_FORMAT_JPEG;
    gettimeofday(&s_fb.timestamp, NULL);
    return &s_fb;
}

static void uvc_fb_return_cb(uvc_fb_t *fb, void *cb_ctx)
{
}

static void uvc_stop_cb(void *cb_ctx)
{
    ESP_LOGI(TAG, "UVC stop");
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(s_fd, VIDIOC_STREAMOFF, &type);
}

void app_main(void)
{
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

    /* UVC copies each frame into this transfer buffer. */
    uint8_t *uvc_buffer = heap_caps_malloc(s_jpeg_outbuf_size, MALLOC_CAP_SPIRAM);
    assert(uvc_buffer != NULL);
    uvc_device_config_t uvc_cfg = {
        .uvc_buffer = uvc_buffer,
        .uvc_buffer_size = s_jpeg_outbuf_size,
        .start_cb = uvc_start_cb,
        .fb_get_cb = uvc_fb_get_cb,
        .fb_return_cb = uvc_fb_return_cb,
        .stop_cb = uvc_stop_cb,
    };
    ESP_ERROR_CHECK(uvc_device_config(0, &uvc_cfg));
    ESP_ERROR_CHECK(uvc_device_init());
    ESP_LOGI(TAG, "UVC ready, connect HUSB to host");
}
