idf_component_register(SRCS "main.c" "wifi.c" "lcd.c" PRIV_REQUIRES driver esp_driver_jpeg esp_wifi esp_netif nvs_flash esp_http_server console mdns esp_lcd)
