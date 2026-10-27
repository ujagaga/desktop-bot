/*
 * Console commands over HTTP, as on MiniBot:
 *   GET /command?cmd=gyro%20angle%20x  ->  plain text command output
 * The console REPL and the HTTP server run commands in different tasks, so
 * output is captured only for the task that runs the HTTP command.
 * esp_console_run() shares one line buffer between callers, so HTTP looks
 * commands up in its own table instead of calling it.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_console.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "cmd.h"

#define CMD_MAX_LEN 128
#define OUT_MAX_LEN 1024
#define MAX_COMMANDS 16
#define MAX_ARGS     8

static SemaphoreHandle_t s_lock;
static TaskHandle_t s_capture_task;
static char s_out[OUT_MAX_LEN];
static size_t s_out_len;
static esp_console_cmd_t s_commands[MAX_COMMANDS];
static int s_command_count;

void cmd_register(const esp_console_cmd_t *cmd)
{
    assert(s_command_count < MAX_COMMANDS);
    s_commands[s_command_count++] = *cmd;
    ESP_ERROR_CHECK(esp_console_cmd_register(cmd));
}

void cmd_printf(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    if (s_capture_task != NULL && s_capture_task == xTaskGetCurrentTaskHandle()) {
        int n = vsnprintf(s_out + s_out_len, sizeof(s_out) - s_out_len, fmt, args);
        if (n > 0) {
            s_out_len += n;
            if (s_out_len >= sizeof(s_out)) {
                s_out_len = sizeof(s_out) - 1;
            }
        }
    } else {
        vprintf(fmt, args);
    }
    va_end(args);
}

/* Decodes %XX and '+' in place. */
static void url_decode(char *s)
{
    char *out = s;
    while (*s) {
        if (*s == '%' && s[1] && s[2]) {
            char hex[3] = { s[1], s[2], 0 };
            *out++ = (char)strtol(hex, NULL, 16);
            s += 3;
        } else if (*s == '+') {
            *out++ = ' ';
            s++;
        } else {
            *out++ = *s++;
        }
    }
    *out = 0;
}

static esp_err_t command_handler(httpd_req_t *req)
{
    char query[CMD_MAX_LEN * 3];
    char line[CMD_MAX_LEN * 3];
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "cmd", line, sizeof(line)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "ERR missing cmd parameter\n");
    }
    url_decode(line);
    if (line[0] == 0 || strlen(line) >= CMD_MAX_LEN) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "ERR expected one command line, maximum 127 bytes\n");
    }

    char *argv[MAX_ARGS];
    int argc = esp_console_split_argv(line, argv, MAX_ARGS);
    const esp_console_cmd_t *cmd = NULL;
    for (int i = 0; i < s_command_count && argc > 0; i++) {
        if (strcmp(s_commands[i].command, argv[0]) == 0) {
            cmd = &s_commands[i];
        }
    }
    if (cmd == NULL) {
        return httpd_resp_sendstr(req, "ERR unknown command\n");
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_out_len = 0;
    s_out[0] = 0;
    s_capture_task = xTaskGetCurrentTaskHandle();
    cmd->func(argc, argv);
    s_capture_task = NULL;
    esp_err_t send_err = httpd_resp_sendstr(req, s_out);
    xSemaphoreGive(s_lock);
    return send_err;
}

void cmd_register_http(httpd_handle_t server)
{
    s_lock = xSemaphoreCreateMutex();
    const httpd_uri_t uri = { .uri = "/command", .method = HTTP_GET, .handler = command_handler };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uri));
}
