#pragma once
#include "esp_http_server.h"

/* Starts Wi-Fi (through the ESP32-WROOM co-processor): joins the saved
 * network, or runs the setup hotspot when there is none or it is not found. */
void wifi_start(void);

/* Registers the `wifi` console command. */
void wifi_register_command(void);

/* Registers the Wi-Fi setup page (/) and its endpoints on the port 80 server. */
void wifi_register_http(httpd_handle_t server);
