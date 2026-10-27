#pragma once
#include "esp_http_server.h"
#include "esp_console.h"

/* printf for console commands: goes into the HTTP reply when the command
 * came from /command, to the console otherwise. */
void cmd_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Registers a console command, also reachable through /command. */
void cmd_register(const esp_console_cmd_t *cmd);

/* Registers GET /command?cmd=<console command line> on the port 80 server. */
void cmd_register_http(httpd_handle_t server);
