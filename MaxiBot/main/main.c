/*
 * OV5647 CSI camera on WT9932P4-TINY -> JPEG frames over the
 * USB-Serial-JTAG port (FUSB), one frame at a time.
 *
 * Frame wire format (little-endian):
 *   u32 magic (0x55AA55AA), u16 width, u16 height, u32 length, then
 *   `length` bytes of JPEG data.
 */
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_video_init.h"
#include "esp_video_device.h"
#include "linux/videodev2.h"
#include "driver/usb_serial_jtag.h"
#include "driver/jpeg_encode.h"

/* SCCB I2C wiring and CSI reset/pwdn per WT9932P4-TINY schematic V1.3. */
#define CAM_I2C_PORT     0
#define CAM_I2C_SCL_PIN  8
#define CAM_I2C_SDA_PIN  7
#define CAM_RESET_PIN    -1
#define CAM_PWDN_PIN     -1
#define BUF_COUNT        2
#define FRAME_MAGIC      0x55AA55AA
#define JPEG_QUALITY     80

static const char *TAG = "uart_cam";

static void usb_write_all(const uint8_t *data, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        int n = usb_serial_jtag_write_bytes(data + sent, len - sent, portMAX_DELAY);
        if (n > 0) {
            sent += (size_t)n;
        }
    }
}

static void put_u32le(uint8_t *p, uint32_t v)
{
    p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}

static void put_u16le(uint8_t *p, uint16_t v)
{
    p[0] = v; p[1] = v >> 8;
}

void app_main(void)
{
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

    uint8_t *buffers[BUF_COUNT];
    for (int i = 0; i < BUF_COUNT; i++) {
        struct v4l2_buffer buf = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
            .index = i,
        };
        ESP_ERROR_CHECK(ioctl(fd, VIDIOC_QUERYBUF, &buf));
        buffers[i] = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.offset);
        ESP_ERROR_CHECK(ioctl(fd, VIDIOC_QBUF, &buf));
    }

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ESP_ERROR_CHECK(ioctl(fd, VIDIOC_STREAMON, &type));

    jpeg_encoder_handle_t jpeg_handle;
    jpeg_encode_engine_cfg_t jpeg_eng_cfg = { .timeout_ms = 200 };
    ESP_ERROR_CHECK(jpeg_new_encoder_engine(&jpeg_eng_cfg, &jpeg_handle));

    uint32_t raw_frame_size = (uint32_t)width * height * 2; /* RGB565 */
    jpeg_encode_memory_alloc_cfg_t jpeg_out_mem_cfg = { .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER };
    size_t jpeg_outbuf_size = 0;
    uint8_t *jpeg_outbuf = jpeg_alloc_encoder_mem(raw_frame_size / 4, &jpeg_out_mem_cfg, &jpeg_outbuf_size);
    assert(jpeg_outbuf != NULL);

    jpeg_encode_cfg_t jpeg_cfg = {
        .width = width,
        .height = height,
        .src_type = JPEG_ENCODE_IN_FORMAT_RGB565,
        .sub_sample = JPEG_DOWN_SAMPLING_YUV420,
        .image_quality = JPEG_QUALITY,
    };

    usb_serial_jtag_driver_config_t usb_cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb_cfg));

    uint8_t header[12];
    put_u32le(&header[0], FRAME_MAGIC);
    put_u16le(&header[4], width);
    put_u16le(&header[6], height);

    while (1) {
        struct v4l2_buffer buf = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
        };
        ESP_ERROR_CHECK(ioctl(fd, VIDIOC_DQBUF, &buf));

        uint32_t jpeg_size = 0;
        ESP_ERROR_CHECK(jpeg_encoder_process(jpeg_handle, &jpeg_cfg, buffers[buf.index], buf.bytesused,
                                              jpeg_outbuf, jpeg_outbuf_size, &jpeg_size));

        put_u32le(&header[8], jpeg_size);
        usb_write_all(header, sizeof(header));
        usb_write_all(jpeg_outbuf, jpeg_size);

        ESP_ERROR_CHECK(ioctl(fd, VIDIOC_QBUF, &buf));
    }
}
