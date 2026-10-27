/*
 * Wi-Fi over ESP-Hosted (esp_wifi_remote). Credentials live in the P4's NVS,
 * not on the WROOM, so reflashing the WROOM keeps them.
 *
 * With a saved network the robot joins it as a station. When there is no
 * saved network, or it cannot be joined for STA_TIMEOUT_S, the robot also
 * starts a setup hotspot (AP_SSID_PREFIX + MAC, AP_PASSWORD, 192.168.4.1)
 * with the setup page on http://192.168.4.1/. While nobody is on the hotspot
 * it scans for the saved network every SCAN_INTERVAL_S and rejoins it; once
 * the station has an IP the hotspot is turned off.
 *
 * The robot announces itself over mDNS as <hostname>.local (also used as the
 * DHCP name), DEFAULT_HOSTNAME unless changed with `wifi host <name>`.
 */
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_console.h"
#include "nvs.h"
#include "mdns.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "config.h"
#include "lcd.h"
#include "cmd.h"
#include "wifi.h"

#define NVS_NAMESPACE "wifi"
#define SCAN_MAX      20
#define HOSTNAME_MAX  32

static const char *TAG = "wifi";

static esp_netif_t *s_sta_netif;
static wifi_config_t s_sta_cfg;
static wifi_config_t s_ap_cfg;
static char s_hostname[HOSTNAME_MAX] = DEFAULT_HOSTNAME;
static bool s_has_creds;
static volatile bool s_connected;
static volatile bool s_ap_mode;
static volatile int s_ap_clients;
static volatile int64_t s_last_ok_us;
static volatile bool s_redraw;
/* Hotspot kept on from the setup page, even while connected. */
static volatile bool s_ap_forced;

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_has_creds) {
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        /* With the hotspot on, the task decides when to retry. */
        if (s_has_creds && !s_ap_mode) {
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        s_ap_clients++;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        if (s_ap_clients > 0) {
            s_ap_clients--;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = data;
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&event->ip_info.ip));
        s_connected = true;
    }
}

/* Returns the number of records written to recs, or -1 if the scan failed. */
static int scan(wifi_ap_record_t *recs)
{
    wifi_scan_config_t scan_cfg = {0};
    if (esp_wifi_scan_start(&scan_cfg, true) != ESP_OK) {
        return -1;
    }
    uint16_t n = SCAN_MAX;
    if (esp_wifi_scan_get_ap_records(&n, recs) != ESP_OK) {
        return -1;
    }
    return n;
}

static bool saved_network_visible(void)
{
    wifi_ap_record_t *recs = malloc(SCAN_MAX * sizeof(*recs));
    if (recs == NULL) {
        return false;
    }
    bool found = false;
    int n = scan(recs);
    for (int i = 0; i < n && !found; i++) {
        found = strcmp((char *)recs[i].ssid, (char *)s_sta_cfg.sta.ssid) == 0;
    }
    free(recs);
    return found;
}

/* A failed call is retried on the next task tick. */
static void start_ap(void)
{
    if (ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_mode(WIFI_MODE_APSTA)) != ESP_OK ||
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_config(WIFI_IF_AP, &s_ap_cfg)) != ESP_OK) {
        return;
    }
    s_ap_clients = 0;
    s_ap_mode = true;
    ESP_LOGW(TAG, "setup hotspot \"%s\" password \"%s\", open http://192.168.4.1/",
             (char *)s_ap_cfg.ap.ssid, AP_PASSWORD);
}

static void stop_ap(void)
{
    if (ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK) {
        return;
    }
    s_ap_mode = false;
    ESP_LOGI(TAG, "setup hotspot off");
}

static void get_ip(char *out, size_t len);

static void show_status(void)
{
    char l1[48], l2[48], l3[48];
    if (s_ap_mode) {
        snprintf(l1, sizeof(l1), "AP: %s", (char *)s_ap_cfg.ap.ssid);
        snprintf(l2, sizeof(l2), "Pass: %s", AP_PASSWORD[0] ? AP_PASSWORD : "none");
        lcd_text(s_ap_forced ? "Hotspot" : "Wi-Fi setup", l1, l2, "http://192.168.4.1");
    } else if (s_connected) {
        char ip[20];
        get_ip(ip, sizeof(ip));
        snprintf(l1, sizeof(l1), "Wi-Fi: %s", (char *)s_sta_cfg.sta.ssid);
        snprintf(l2, sizeof(l2), "IP: %s", ip);
        snprintf(l3, sizeof(l3), "http://%s.local", s_hostname);
        lcd_text("Connected", l1, l2, l3);
    } else {
        lcd_text("Connecting to", (char *)s_sta_cfg.sta.ssid, NULL, NULL);
    }
}

static void wifi_task(void *arg)
{
    int64_t last_scan_us = 0;
    int shown = -1;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        int64_t now = esp_timer_get_time();
        if (s_connected) {
            s_last_ok_us = now;
            if (s_ap_mode && !s_ap_forced) {
                stop_ap();
            } else if (!s_ap_mode && s_ap_forced) {
                start_ap();
            }
        } else if (!s_ap_mode) {
            if (s_ap_forced || !s_has_creds || now - s_last_ok_us > STA_TIMEOUT_S * 1000000LL) {
                start_ap();
            }
        } else if (s_has_creds && s_ap_clients == 0 && now - last_scan_us > SCAN_INTERVAL_S * 1000000LL) {
            last_scan_us = now;
            if (saved_network_visible()) {
                ESP_LOGI(TAG, "found \"%s\", connecting", (char *)s_sta_cfg.sta.ssid);
                esp_wifi_connect();
            }
        }

        int state = s_ap_mode ? 1 : s_connected ? 2 : 0;
        if (state != shown || s_redraw) {
            shown = state;
            s_redraw = false;
            show_status();
        }
    }
}

static void load_creds(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    size_t ssid_len = sizeof(s_sta_cfg.sta.ssid);
    size_t pass_len = sizeof(s_sta_cfg.sta.password);
    if (nvs_get_str(nvs, "ssid", (char *)s_sta_cfg.sta.ssid, &ssid_len) == ESP_OK &&
        nvs_get_str(nvs, "pass", (char *)s_sta_cfg.sta.password, &pass_len) == ESP_OK) {
        s_has_creds = true;
    }
    size_t host_len = sizeof(s_hostname);
    nvs_get_str(nvs, "host", s_hostname, &host_len);
    nvs_close(nvs);
}

void wifi_start(void)
{
    load_creds();
    s_sta_netif = esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();
    ESP_ERROR_CHECK(esp_netif_set_hostname(s_sta_netif, s_hostname));
    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set(s_hostname));
    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    if (s_has_creds) {
        ESP_LOGI(TAG, "connecting to \"%s\"", (char *)s_sta_cfg.sta.ssid);
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &s_sta_cfg));
    }
    ESP_ERROR_CHECK(esp_wifi_start());
    /* Mains powered: modem sleep only adds latency and cuts throughput. */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_AP, mac));
    s_ap_cfg.ap.ssid_len = snprintf((char *)s_ap_cfg.ap.ssid, sizeof(s_ap_cfg.ap.ssid),
                                    AP_SSID_PREFIX "%02X%02X%02X", mac[3], mac[4], mac[5]);
    strcpy((char *)s_ap_cfg.ap.password, AP_PASSWORD);
    /* An empty AP_PASSWORD gives an open hotspot. */
    s_ap_cfg.ap.authmode = AP_PASSWORD[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    s_ap_cfg.ap.max_connection = 2;

    s_last_ok_us = esp_timer_get_time();
    xTaskCreate(wifi_task, "wifi", 4096, NULL, 5, NULL);
}

static esp_err_t wifi_save(const char *ssid, const char *pass)
{
    if (strlen(ssid) >= sizeof(s_sta_cfg.sta.ssid) || strlen(pass) >= sizeof(s_sta_cfg.sta.password)) {
        return ESP_ERR_INVALID_SIZE;
    }
    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs));
    nvs_set_str(nvs, "ssid", ssid);
    nvs_set_str(nvs, "pass", pass);
    nvs_commit(nvs);
    nvs_close(nvs);

    memset(&s_sta_cfg, 0, sizeof(s_sta_cfg));
    strcpy((char *)s_sta_cfg.sta.ssid, ssid);
    strcpy((char *)s_sta_cfg.sta.password, pass);
    s_has_creds = true;
    s_last_ok_us = esp_timer_get_time();
    esp_wifi_disconnect();
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &s_sta_cfg));
    esp_wifi_connect();
    s_redraw = true;
    ESP_LOGI(TAG, "saved, connecting to \"%s\"", ssid);
    return ESP_OK;
}

/* Forgets the network; the task then starts the setup hotspot. */
static void wifi_clear(void)
{
    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs));
    nvs_erase_all(nvs);
    nvs_commit(nvs);
    nvs_close(nvs);

    s_has_creds = false;
    esp_wifi_disconnect();
    ESP_LOGI(TAG, "credentials cleared");
}

/* Lowercase letters, digits and '-', as allowed in a .local name. */
static esp_err_t set_hostname(const char *name)
{
    size_t len = strlen(name);
    if (len == 0 || len >= HOSTNAME_MAX || name[0] == '-') {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < len; i++) {
        if (!(islower((unsigned char)name[i]) || isdigit((unsigned char)name[i]) || name[i] == '-')) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs));
    nvs_set_str(nvs, "host", name);
    nvs_commit(nvs);
    nvs_close(nvs);

    strcpy(s_hostname, name);
    mdns_hostname_set(s_hostname);
    /* The DHCP name is sent on the next lease. */
    esp_netif_set_hostname(s_sta_netif, s_hostname);
    s_redraw = true;
    ESP_LOGI(TAG, "hostname %s.local", s_hostname);
    return ESP_OK;
}

static void get_ip(char *out, size_t len)
{
    esp_netif_ip_info_t ip;
    esp_netif_get_ip_info(s_sta_netif, &ip);
    if (s_connected) {
        snprintf(out, len, IPSTR, IP2STR(&ip.ip));
    } else {
        snprintf(out, len, "not connected");
    }
}

static int cmd_wifi(int argc, char **argv)
{
    if (argc >= 3 && argc <= 4 && strcmp(argv[1], "set") == 0) {
        if (wifi_save(argv[2], argc == 4 ? argv[3] : "") != ESP_OK) {
            cmd_printf("ssid or password too long\n");
            return 1;
        }
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "clear") == 0) {
        wifi_clear();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "ip") == 0) {
        char ip[20];
        get_ip(ip, sizeof(ip));
        cmd_printf("%s\n", ip);
        cmd_printf("%s.local\n", s_hostname);
        if (s_ap_mode) {
            cmd_printf("setup hotspot \"%s\" on 192.168.4.1\n", (char *)s_ap_cfg.ap.ssid);
        }
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "host") == 0) {
        if (set_hostname(argv[2]) != ESP_OK) {
            cmd_printf("hostname: 1-31 chars, a-z 0-9 -\n");
            return 1;
        }
        return 0;
    }
    cmd_printf("usage: wifi set <ssid> [password] | wifi clear | wifi ip | wifi host <name>\n");
    return 1;
}

void wifi_register_command(void)
{
    const esp_console_cmd_t cmd = {
        .command = "wifi",
        .help = "wifi set <ssid> [password] | wifi clear | wifi ip | wifi host <name>",
        .func = cmd_wifi,
    };
    cmd_register(&cmd);
}

static const char SETUP_HTML[] =
    "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>MaxiBot</title><style>"
    "body{font-family:Arial,sans-serif;max-width:400px;margin:0 auto;padding:8px}"
    "input,button{width:100%;box-sizing:border-box;padding:8px;margin:4px 0;font-size:1em}"
    "button{border:0;border-radius:4px;color:#fff;background:#1fa3ec}"
    "#list a{display:block;padding:4px 0}"
    "</style></head><body>"
    "<h2>MaxiBot</h2><pre id='status'></pre>"
    "<p><a href='/jpg'>Snapshot</a> | <a id='stream'>Stream</a></p>"
    "<h3>Networks</h3><div id='list'>Scanning...</div>"
    "<button onclick='scan()'>Rescan</button>"
    "<input id='s' placeholder='SSID (blank: hotspot only)'>"
    "<input id='p' type='password' placeholder='password'>"
    "<button onclick='save()'>Save</button>"
    "<h3>Name</h3><input id='h' placeholder='hostname (a-z 0-9 -)'>"
    "<button onclick='host()'>Set name</button>"
    "<h3>Hotspot</h3><button onclick='hotspot(\"on\")'>Hotspot on</button>"
    "<button onclick='hotspot(\"off\")'>Hotspot off (automatic)</button>"
    "<script>"
    "document.getElementById('stream').href='http://'+location.hostname+':81/stream';"
    "function status(){fetch('/status').then(r=>r.text()).then(t=>{document.getElementById('status').textContent=t;});}"
    "function scan(){var l=document.getElementById('list');l.textContent='Scanning...';"
    "fetch('/scan').then(r=>r.text()).then(t=>{l.textContent='';"
    "t.split('\\n').filter(x=>x).forEach(x=>{var a=document.createElement('a');a.href='#';a.textContent=x;"
    "a.onclick=()=>{document.getElementById('s').value=x;document.getElementById('p').focus();return false;};l.appendChild(a);});"
    "if(!l.childNodes.length)l.textContent='No networks found.';});}"
    "function save(){fetch('/wifisave',{method:'POST',body:document.getElementById('s').value+'\\n'+document.getElementById('p').value})"
    ".then(r=>r.text()).then(t=>{document.getElementById('status').textContent=t;});}"
    "function host(){fetch('/host',{method:'POST',body:document.getElementById('h').value})"
    ".then(r=>r.text()).then(t=>{document.getElementById('status').textContent=t;});}"
    "function hotspot(v){fetch('/hotspot',{method:'POST',body:v})"
    ".then(r=>r.text()).then(t=>{document.getElementById('status').textContent=t;setTimeout(status,3000);});}"
    "status();scan();"
    "</script></body></html>";

static esp_err_t setup_page_handler(httpd_req_t *req)
{
    return httpd_resp_send(req, SETUP_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t status_handler(httpd_req_t *req)
{
    char ip[20];
    char text[200];
    get_ip(ip, sizeof(ip));
    snprintf(text, sizeof(text), "network: %s\nip: %s\nname: %s.local\nhotspot: %s",
             s_has_creds ? (char *)s_sta_cfg.sta.ssid : "(none)", ip, s_hostname,
             s_ap_mode ? (char *)s_ap_cfg.ap.ssid : "off");
    if (s_ap_forced) {
        strlcat(text, " (kept on)", sizeof(text));
    }
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, text, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t scan_handler(httpd_req_t *req)
{
    wifi_ap_record_t *recs = malloc(SCAN_MAX * sizeof(*recs));
    if (recs == NULL) {
        return httpd_resp_send_500(req);
    }
    int n = scan(recs);
    httpd_resp_set_type(req, "text/plain");
    for (int i = 0; i < n; i++) {
        if (recs[i].ssid[0] != 0) {
            httpd_resp_sendstr_chunk(req, (char *)recs[i].ssid);
            httpd_resp_sendstr_chunk(req, "\n");
        }
    }
    free(recs);
    return httpd_resp_sendstr_chunk(req, NULL);
}

/* Body is "<ssid>\n<password>". An empty SSID forgets the network. */
static esp_err_t wifisave_handler(httpd_req_t *req)
{
    char body[128];
    int len = 0;
    while (len < req->content_len && len < (int)sizeof(body) - 1) {
        int r = httpd_req_recv(req, body + len, sizeof(body) - 1 - len);
        if (r <= 0) {
            return ESP_FAIL;
        }
        len += r;
    }
    body[len] = 0;
    char *pass = strchr(body, '\n');
    if (pass == NULL) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad request");
    }
    *pass++ = 0;

    httpd_resp_set_type(req, "text/plain");
    if (body[0] == 0) {
        httpd_resp_sendstr(req, "Network forgotten, hotspot only.");
        wifi_clear();
        return ESP_OK;
    }
    if (strlen(body) >= sizeof(s_sta_cfg.sta.ssid) || strlen(pass) >= sizeof(s_sta_cfg.sta.password)) {
        return httpd_resp_sendstr(req, "SSID or password too long.");
    }
    char text[256];
    snprintf(text, sizeof(text), "Saved. Connecting to %s.\nThe hotspot turns off once connected; "
             "find the new IP with `wifi ip`.", body);
    /* Reply first: joining may move the hotspot channel and drop this client. */
    httpd_resp_sendstr(req, text);
    wifi_save(body, pass);
    return ESP_OK;
}

static esp_err_t host_handler(httpd_req_t *req)
{
    char name[HOSTNAME_MAX];
    int len = httpd_req_recv(req, name, req->content_len < sizeof(name) ? req->content_len : sizeof(name) - 1);
    if (len <= 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad request");
    }
    name[len] = 0;
    httpd_resp_set_type(req, "text/plain");
    if (set_hostname(name) != ESP_OK) {
        return httpd_resp_sendstr(req, "Hostname: 1-31 chars, a-z 0-9 -");
    }
    char text[80];
    snprintf(text, sizeof(text), "Name set: http://%s.local/", s_hostname);
    return httpd_resp_sendstr(req, text);
}

/* Body "on" keeps the hotspot running (not saved across reboots), "off"
 * returns to automatic: off while connected to the saved network. */
static esp_err_t hotspot_handler(httpd_req_t *req)
{
    char body[4];
    int len = httpd_req_recv(req, body, req->content_len < sizeof(body) ? req->content_len : sizeof(body) - 1);
    if (len <= 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad request");
    }
    body[len] = 0;
    s_ap_forced = strcmp(body, "on") == 0;
    httpd_resp_set_type(req, "text/plain");
    if (s_ap_forced) {
        char text[96];
        snprintf(text, sizeof(text), "Hotspot on: %s, password %s, http://192.168.4.1/",
                 (char *)s_ap_cfg.ap.ssid, AP_PASSWORD[0] ? AP_PASSWORD : "none");
        return httpd_resp_sendstr(req, text);
    }
    return httpd_resp_sendstr(req, "Hotspot automatic.");
}

void wifi_register_http(httpd_handle_t server)
{
    const httpd_uri_t uris[] = {
        { .uri = "/", .method = HTTP_GET, .handler = setup_page_handler },
        { .uri = "/status", .method = HTTP_GET, .handler = status_handler },
        { .uri = "/scan", .method = HTTP_GET, .handler = scan_handler },
        { .uri = "/wifisave", .method = HTTP_POST, .handler = wifisave_handler },
        { .uri = "/host", .method = HTTP_POST, .handler = host_handler },
        { .uri = "/hotspot", .method = HTTP_POST, .handler = hotspot_handler },
    };
    for (int i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uris[i]));
    }
}
