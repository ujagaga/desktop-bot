/*
 * Firmware update from GitHub, ported from MiniBot/ESP32_S3/http_client.cpp.
 * After each Wi-Fi connection (or `ota check`) the robot reads the latest
 * commit of OTA_GITHUB_BRANCH, parses FIRMWARE_VERSION from main/config.h at
 * that commit, and if it is higher installs build/maxibot.bin from the same
 * commit into the inactive OTA slot.
 *
 * A target version newer than the running firmware means the last installed
 * image did not deliver the advertised version; it is kept in NVS and never
 * installed again, so a wrong binary cannot cause an update loop.
 */
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_console.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "config.h"
#include "cmd.h"
#include "lcd.h"
#include "ota.h"

#define NVS_NAMESPACE   "ota"
#define CONFIRM_US      (60 * 1000000LL)
#define RETRY_MS        60000
#define MAX_ATTEMPTS    3
#define CONFIG_MAX      16384

static const char *TAG = "ota";

static volatile bool s_pending;
static volatile bool s_connected;
static uint32_t s_invalid_version;

static void load_target_version(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }
    uint32_t target = 0;
    nvs_get_u32(nvs, "target", &target);
    if (target > FIRMWARE_VERSION) {
        s_invalid_version = target;
        ESP_LOGW(TAG, "target %lu was not installed; marking invalid", (unsigned long)target);
    } else if (target) {
        /* The target was reached (or superseded by a USB flash). */
        nvs_erase_key(nvs, "target");
        nvs_commit(nvs);
    }
    nvs_close(nvs);
}

static bool save_target_version(uint32_t version)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return false;
    }
    bool ok = nvs_set_u32(nvs, "target", version) == ESP_OK && nvs_commit(nvs) == ESP_OK;
    nvs_close(nvs);
    return ok;
}

/* Accepts exactly one positive decimal "#define FIRMWARE_VERSION <n>",
 * optionally followed by a // comment, as MiniBot's parseFirmwareVersion. */
static bool parse_firmware_version(const char *config, uint32_t *version)
{
    bool found = false;
    while (*config) {
        const char *end = strchr(config, '\n');
        size_t length = end ? (size_t)(end - config) : strlen(config);
        if (length >= 256) {
            return false;
        }
        char line[256], name[64], value[64];
        memcpy(line, config, length);
        line[length] = 0;
        int consumed = 0;
        if (sscanf(line, " # define %63s %63s %n", name, value, &consumed) == 2 &&
            strcmp(name, "FIRMWARE_VERSION") == 0) {
            if (found || (line[consumed] && strncmp(line + consumed, "//", 2) != 0)) {
                return false;
            }
            uint32_t parsed = 0;
            for (const char *p = value; *p; ++p) {
                if (*p < '0' || *p > '9' || parsed > (UINT32_MAX - (*p - '0')) / 10) {
                    return false;
                }
                parsed = parsed * 10 + (*p - '0');
            }
            if (!parsed) {
                return false;
            }
            *version = parsed;
            found = true;
        }
        if (!end) {
            break;
        }
        config = end + 1;
    }
    return found;
}

/* GETs url into body (NUL-terminated). Fails on non-200 or a body that does
 * not fit in limit - 1 bytes. */
static bool get_text(const char *url, const char *accept, char *body, size_t limit)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_http_client_set_header(client, "User-Agent", "MaxiBot-OTA");
    esp_http_client_set_header(client, "Accept", accept);
    bool ok = false;
    int status = 0;
    int64_t size = -1;
    if (esp_http_client_open(client, 0) == ESP_OK) {
        size = esp_http_client_fetch_headers(client);
        status = esp_http_client_get_status_code(client);
        if (status == 200 && size > 0 && size < (int64_t)limit) {
            int len = 0;
            while (len < size) {
                int r = esp_http_client_read(client, body + len, size - len);
                if (r <= 0) {
                    break;
                }
                len += r;
            }
            body[len] = 0;
            ok = len == size;
        }
    }
    if (!ok) {
        ESP_LOGW(TAG, "HTTP request failed (status %d, size %lld): %s", status, (long long)size, url);
    }
    esp_http_client_cleanup(client);
    return ok;
}

static bool install(const char *url, uint32_t version)
{
    char line[32];
    snprintf(line, sizeof(line), "to version %lu", (unsigned long)version);
    lcd_text("Firmware update", line, "Downloading...", NULL);

    esp_http_client_config_t http_cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t ota_cfg = { .http_config = &http_cfg };
    esp_https_ota_handle_t handle;
    esp_err_t err = esp_https_ota_begin(&ota_cfg, &handle);
    int shown = -1;
    while (err == ESP_OK) {
        err = esp_https_ota_perform(handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }
        int total = esp_https_ota_get_image_size(handle);
        int percent = total > 0 ? (int)((int64_t)esp_https_ota_get_image_len_read(handle) * 100 / total) : 0;
        if (percent / 5 != shown) {
            shown = percent / 5;
            char progress[16];
            snprintf(progress, sizeof(progress), "%d%%", percent);
            lcd_text("Firmware update", line, progress, NULL);
        }
        err = ESP_OK;
    }
    /* esp_https_ota_finish validates the image and selects it for boot. */
    if (err == ESP_OK && esp_https_ota_is_complete_data_received(handle)) {
        err = esp_https_ota_finish(handle);
    } else {
        esp_https_ota_abort(handle);
        if (err == ESP_OK) {
            err = ESP_FAIL;
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "update failed: %s", esp_err_to_name(err));
        lcd_text("Firmware update", line, "Update failed", NULL);
        return false;
    }
    if (!save_target_version(version)) {
        /* The new slot is already selected. Undo that if the guard cannot be
         * saved, so a later reset cannot start an update loop. */
        esp_err_t restored = esp_ota_set_boot_partition(esp_ota_get_running_partition());
        ESP_LOGE(TAG, "target save failed; boot restore status %d", (int)restored);
        lcd_text("Firmware update", line, "Target save failed", NULL);
        return false;
    }
    ESP_LOGI(TAG, "installed; restarting");
    lcd_text("Firmware update", line, "Done. Restarting...", NULL);
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return true;
}

/* Returns true when the check is finished (up to date, skipped or installed),
 * false when it should be retried. */
static bool check_repository(void)
{
    static char text[CONFIG_MAX];
    if (!get_text("https://api.github.com/repos/" OTA_GITHUB_REPOSITORY "/commits/" OTA_GITHUB_BRANCH,
                  "application/vnd.github.sha", text, 128)) {
        return false;
    }
    char commit[41];
    if (strlen(text) < 40) {
        return false;
    }
    memcpy(commit, text, 40);
    commit[40] = 0;
    for (int i = 0; i < 40; i++) {
        if (!isxdigit((unsigned char)commit[i])) {
            return false;
        }
    }

    char url[192];
    snprintf(url, sizeof(url), "https://raw.githubusercontent.com/%s/%s/%s/main/config.h",
             OTA_GITHUB_REPOSITORY, commit, OTA_FIRMWARE_DIRECTORY);
    uint32_t remote_version = 0;
    if (!get_text(url, "text/plain", text, sizeof(text))) {
        return false;
    }
    if (!parse_firmware_version(text, &remote_version)) {
        ESP_LOGW(TAG, "invalid remote FIRMWARE_VERSION; skipping");
        return false;
    }
    ESP_LOGI(TAG, "device %d, GitHub %lu", FIRMWARE_VERSION, (unsigned long)remote_version);
    if (remote_version <= FIRMWARE_VERSION) {
        return true;
    }
    if (remote_version == s_invalid_version) {
        ESP_LOGW(TAG, "version %lu is invalid; skipping repeated update", (unsigned long)remote_version);
        return true;
    }
    ESP_LOGI(TAG, "downloading newer firmware...");
    snprintf(url, sizeof(url), "https://raw.githubusercontent.com/%s/%s/%s/build/maxibot.bin",
             OTA_GITHUB_REPOSITORY, commit, OTA_FIRMWARE_DIRECTORY);
    return install(url, remote_version);
}

static void ota_task(void *arg)
{
    int attempts = 0;
    while (true) {
        if (!s_pending || !s_connected) {
            attempts = 0;
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (check_repository() || ++attempts >= MAX_ATTEMPTS) {
            s_pending = false;
        } else {
            vTaskDelay(pdMS_TO_TICKS(RETRY_MS));
        }
    }
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_connected = true;
        s_pending = true;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
    }
}

static void confirm_app(void *arg)
{
    esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGI(TAG, "firmware %d confirmed", FIRMWARE_VERSION);
}

void ota_start(void)
{
    ESP_LOGI(TAG, "firmware version %d", FIRMWARE_VERSION);
    load_target_version();

    /* Keep a freshly installed image pending until it has run for a minute;
     * a crash or reset before that makes the bootloader roll back. */
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        const esp_timer_create_args_t timer_args = { .callback = confirm_app, .name = "ota_confirm" };
        esp_timer_handle_t timer;
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &timer));
        ESP_ERROR_CHECK(esp_timer_start_once(timer, CONFIRM_US));
    }

    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_event, NULL));
    xTaskCreate(ota_task, "ota", 8192, NULL, 3, NULL);
}

static int cmd_ota(int argc, char **argv)
{
    if (argc != 2 || strcmp(argv[1], "check") != 0) {
        cmd_printf("ERR ota check\n");
        return 1;
    }
    if (!s_connected || s_pending) {
        cmd_printf("ERR OTA check unavailable (Wi-Fi disconnected or check already pending)\n");
        return 1;
    }
    s_pending = true;
    cmd_printf("OTA check scheduled (firmware %d)\n", FIRMWARE_VERSION);
    return 0;
}

void ota_register_command(void)
{
    const esp_console_cmd_t cmd = {
        .command = "ota",
        .help = "ota check   look for newer firmware on GitHub",
        .func = cmd_ota,
    };
    cmd_register(&cmd);
}
