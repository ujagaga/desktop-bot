#pragma once
#include <pgmspace.h>
const char api_html[] PROGMEM = R"HTML(<!doctype html><html><head><meta name="viewport" content="width=device-width"><title>MiniBot CAM API</title></head><body>
<h1>MiniBot CAM API</h1><a href="/">Camera console</a>
<ul><li>GET /capture — JPEG snapshot</li><li>GET :81/stream — MJPEG stream (one viewer)</li>
<li>GET /status — camera settings</li><li>GET /config?var=quality&amp;val=12 — change camera setting in RAM</li>
<li>POST /api/camera/save — persist current camera settings</li><li>GET /api/logs — recent timestamped log messages</li>
<li>GET /api/info — firmware version, invalid OTA target, free heap, OTA activity</li>
<li>GET /api/wifi — Wi-Fi mode, SSID and addresses (no password); s3_ip is the UART-reported S3 address, empty until discovered</li>
<li>POST /api/wifi — JSON {"mode":"station","ssid":"network","password":"password"}; mode "ap" disables station connection</li>
<li>POST /api/ota — schedule a check of the configured GitHub repository (empty body)</li></ul>
<p>Ports 80 and 81 use plain HTTP on your local network. No authentication is implemented. Motor, distance and WebSocket command endpoints have been removed.</p>
</body></html>)HTML";
