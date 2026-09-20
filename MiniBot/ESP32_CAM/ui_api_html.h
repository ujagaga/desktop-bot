#pragma once
#include <pgmspace.h>
const char api_html[] PROGMEM = R"HTML(<!doctype html><html><head><meta name="viewport" content="width=device-width"><title>MiniBot CAM API</title></head><body>
<h1>MiniBot CAM API</h1><a href="/">Camera console</a>
<ul><li>GET /capture — JPEG snapshot</li><li>GET :81/stream — MJPEG stream (one viewer)</li>
<li>GET /status — camera settings</li><li>GET /config?var=quality&amp;val=12 — change camera setting in RAM</li>
<li>POST /api/camera/save — persist current camera settings</li><li>GET /api/logs — recent timestamped log messages</li>
<li>GET /api/info — firmware version, invalid OTA target, free heap, ota_busy, ota_pending, ota_found_version (0 until found), and ota_status</li>
<li>GET /api/console — JSON with UART ready state and recent TX/RX transcript; pauses automatic IP polling for 30 seconds</li>
<li>POST /api/console — text/plain body containing one printable ASCII S3 command (1–127 bytes, no CR/LF); 202 means queued, 409 means queue full, 503 means UART unavailable</li>
<li>GET /api/wifi — Wi-Fi mode, SSID and addresses (no password); s3_ip is the UART-reported S3 address, empty until discovered</li>
<li>POST /api/wifi — JSON {"mode":"station","ssid":"network","password":"password"}; mode "ap" disables station connection</li>
<li>POST /api/ota — schedule a check of the configured GitHub repository (empty body)</li></ul>
<p>Ports 80 and 81 use plain HTTP on your local network. No authentication is implemented. Motor, distance and WebSocket command endpoints have been removed.</p>
</body></html>)HTML";
