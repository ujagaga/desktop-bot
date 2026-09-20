# MiniBot ESP32-CAM

Camera firmware for the **classic AI-Thinker ESP32-CAM with OV2640**, separate
from the ESP32-S3 robot controller. The pasted S3 camera pin map has been replaced
with the classic board's wiring. Motor, steering/claw, distance-sensor and legacy
WebSocket commands have been removed.

## First connection

1. Flash once over USB/UART using `tools/build_cam.sh upload` from the repository
   root. GPIO0 must be held low during reset to enter the classic board's download
   mode; release it and reset again to run the firmware.
2. Join Wi-Fi **MiniBot-CAM**, password **MiniBotCAM123**, then open
   **http://192.168.4.1/**. The SSID/password are configurable in `config.h`.
3. Enter the station SSID/password on the page and save. The setup AP remains
   available if those credentials are wrong or the station network disappears.
   Use **Access point only** to disable station connection.
4. Start the preview and change camera settings. **Save camera settings** makes
   them survive reboot; ordinary camera changes are only in RAM.

For compatibility with the pasted project, a fresh device also attempts the
`BallBot` / `BallBot123` station network. Change the defaults in `config.h` or
save your network from the page. Saved settings take precedence. SSIDs and
passwords may contain spaces. An empty password selects an open network; otherwise
use 8–63 bytes. SSIDs are limited to 32 bytes. Passwords are not returned by the
HTTP API or written to logs. There is no UART setup requirement after flashing.

### S3 UART link

The CAM uses UART0 (`Serial`) at 115200 baud (8N1), with U0R/RX GPIO3
connected to S3 TX GPIO13 and U0T/TX GPIO1 connected to S3 RX GPIO14, plus
common ground. This matches the schematic.
The CAM UART pins are configurable in `config.h`. The five-second discovery
interval is `CAM_IP_REPORT_INTERVAL_MS` in `ESP32_S3/config.h`.
These are also the programming UART pins; disconnect the S3 UART wires when
flashing through a USB-to-UART adapter. Application logs stay in the web UI,
and runtime SDK serial logging is disabled to keep the command link clean.
ROM boot output can still appear before the application starts.

An independent FreeRTOS task starts before camera initialization and drains
UART input even while capture/streaming or the main loop is busy. CAM does not
initiate IP discovery. Once S3 has a station IP, its cooperative five-second
timer sends `report ip <s3 ip addr>` until CAM returns a nonzero station IP.
CAM stores the reported address in the atomic global `COMMS_peerWifiIP`, then
replies with its own IPv4 address followed by `OK` (`0.0.0.0` if disconnected).
The S3 shows this address on the LCD's `CAM` row; zero/error replies keep the
five-second timer running. Discovery restarts after S3 wake, reboot or a change
in its own station address. CAM-only address changes after discovery are not
actively monitored.

`report ip` is case-insensitive and requires exactly one valid dotted-decimal
IPv4 argument. Every valid report replaces the stored S3 address; `0.0.0.0`
clears it. Invalid requests return `ERR report ip <s3 ip addr>` without changing
the cache. `COMMS_GetPeerWifiIP()` provides a thread-safe snapshot exposed as
`s3_ip` by `GET /api/wifi` and refreshed in the UI. CAM does not handle `wifi ip`; IP exchange uses only `report ip`.
Indented S3 help entries are ignored by the CAM parser, including `sleep`.
Other incoming lines remain console replies and are not answered, preventing
command/reply feedback loops. Both modules need the matching firmware changes.

Open **S3 serial console** in the CAM UI to send commands such as `help`,
`wifi ip` or `batt v`. Up/Down recalls the last ten commands stored in the
browser. The console displays the last 4096 characters of UART traffic;
`>` marks a transmitted command, `[reply]` marks a CAM command response, and
other text is received from the S3 (nonprintable bytes appear as `\xNN`).
No reply is fabricated when the S3 is silent. Output is shared by all browsers
and is cleared on CAM reboot.

Opening the console does not change S3 discovery; reports and responses remain
visible in the transcript.
The console polls `GET /api/console` every half second while open and visible.
`POST /api/console` accepts a plain-text ASCII command up to 127 bytes without
CR/LF. HTTP 202 means queued, not acknowledged by the S3. The UART task owns
all command writes and continues receiving independently of HTTP/camera work.

### Coordinated sleep and wake

Connect **S3 GPIO7 to CAM GPIO13**, with common ground and a **10 kΩ pull-down**
from CAM GPIO13 to ground. This is a 3.3 V signal; GPIO13 must not also be used
for the SD card. S3 holds the line HIGH while awake, drives it LOW before the
sleep request, and retains LOW through its light sleep. It raises the line on
confirmed wake, on cancelled sleep, and at startup.

S3 displays face `08_sleepy`, sends `sleep`, and waits up to ten seconds for
the explicit `CAM SLEEP READY` reply. A plain `OK` is not a sleep acknowledgment.
CAM rejects sleep during firmware installation or while the wake line is HIGH.
It also refuses with `ERR CAM SLEEP client connected` while a client is attached
to the setup AP, either HTTP server has an open connection (including streaming),
or fewer than five seconds have passed since the last HTTP connection closed.
The grace period covers gaps between UI polls. Once sleep is accepted, new HTTP
connections are rejected so they cannot race shutdown. On refusal S3 stays awake
and replaces the sleepy face with `00_neutral`.
Its RX task queues the request; the main loop stops HTTP, the camera and Wi-Fi,
holds the sensor power-down pin HIGH, then acknowledges and enters deep sleep.
GPIO13 HIGH wakes CAM through EXT0 and restarts the firmware from setup.

S3 leaves the sleepy face visible for at least three seconds before switching
off its backlight. If CAM rejects the request or fails to acknowledge, S3 cancels
sleep and raises the wake line, so even a late CAM sleep transition wakes again.
Isolated motion taps below the S3 wake threshold do not wake CAM. After a real
wake, S3 returns to its status screen and discovers the CAM address again.
CAM UI/streaming are unavailable during sleep. Both firmware images must be
updated for this handshake to work; an older CAM will cause S3 sleep to time out.

The AP chooses a quiet channel among 1, 6 and 11 at startup. When the station
connects, the ESP32's shared radio follows the station network's channel; AP
clients may need to reconnect. Power saving is disabled for camera streaming.
The setup AP is always enabled; the station reconnects automatically.

## Camera and logger

The UI and commands use port **80**; MJPEG streaming uses port **81** and supports
one streaming viewer at a time. Still captures are available at `/capture`.
Camera settings cover resolution, JPEG quality, brightness, contrast, saturation,
effects, gain/exposure, white balance, mirroring, flipping and pixel/lens correction.
The default is VGA JPEG at quality 12; smaller quality numbers mean larger,
higher-quality images. With PSRAM, resolutions up to UXGA (1600×1200) are supported.
Without PSRAM the frame count is one and resolution is capped at VGA.

Camera initialization failure leaves the setup UI and logs accessible. The
in-memory logger retains the last **20 messages**, each up to 191 bytes including
seconds since boot. The page polls every two seconds and renders messages as
plain text. Future handlers can call `LOG_append()` or `LOG_printf()` to report
status. Logging is synchronized between the HTTP tasks and main loop. Logs reset
on reboot. They are not mirrored to Serial because UART0 is the S3 command link.

The camera mutex protects capture, settings and shutdown. A frame is returned
before its lock is released. HTTP handlers are stopped and joined before OTA
can deinitialize the camera.

## HTTP API

Only endpoints used by the home page are registered. There is no separate
API documentation page.

| Method | Path | Result |
| --- | --- | --- |
| GET | `/` | Camera/settings/Wi-Fi/log UI |
| GET | `/capture` | JPEG snapshot |
| GET | `:81/stream` | MJPEG stream |
| GET | `/status` | Camera availability and current settings as JSON |
| GET | `/config?var=quality&val=12` | Apply a validated camera setting in RAM |
| POST | `/api/camera/save` | Save current camera settings to Preferences |
| GET | `/api/logs` | Recent timestamped log lines |
| GET | `/api/console` | UART readiness and recent TX/RX transcript |
| POST | `/api/console` | Queue a plain-text S3 command from the home-page console |
| GET | `/api/info` | Firmware version, invalid target, OTA activity/state, discovered version, free heap |
| GET | `/api/wifi` | Wi-Fi mode, configured SSID and addresses; no password |
| POST | `/api/wifi` | Save/apply JSON Wi-Fi configuration |
| POST | `/api/ota` | Queue a GitHub firmware check; empty body |

Wi-Fi POST example:

```json
{"mode":"station","ssid":"My network","password":"my-password"}
```

Use `"mode":"ap"` for AP-only operation (still supply the `ssid` and `password`
strings). Credentials are saved before a one-second delay to apply the change,
allowing the HTTP reply to leave first. Re-enter the password when saving; an
empty password does not mean “keep the existing password.”

HTTP is unauthenticated and intended for a trusted local network. The old arbitrary
URL OTA upload and log callback URL are replaced by a repository check and local
browser logs. `/command`, `/ws`, motor and distance routes no longer exist.

## Preferences

No EEPROM dependency or EEPROM layout was present in the supplied files, so
there is no legacy data migration. Preferences/NVS manages addresses automatically:

| Namespace | Key | Contents |
| --- | --- | --- |
| `camWiFi` | `config` | One JSON record with station mode, SSID and password |
| `camCamera` | `settings_v1` | Validated camera setting values |
| `camOTA` | `target` | Advertised firmware version installed before restart |

Unchanged Wi-Fi/camera settings avoid another flash write. Camera settings save
only when requested, not on each UI adjustment. These namespaces are independent
of the S3 settings and survive normal application OTA. Erasing flash removes them.

## Build and firmware updates

The UI shows the discovered firmware version and update state. Before stopping
HTTP for installation, the updater leaves a five-second window for the browser
to read that version. A separate JavaScript status poll uses a two-second request
timeout, displays elapsed time and a retry countdown while offline, and retries
every two seconds after a failed request. When the CAM returns, the entire page
reloads, including after recovery from a failed download. Up-to-date, invalid,
retry and failure states are also displayed. This requires installing the updated
CAM firmware and loading its new UI first.

Use the installed ESP32 Arduino core (tested with **3.3.10**) and `ArduinoJson`.
The camera, Wi-Fi, HTTP, TLS and Preferences libraries are provided by the core.

```sh
tools/build_cam.sh
PORT=/dev/ttyUSB0 tools/build_cam.sh upload
```

Uploads default to 230400 baud. Override
with `UPLOAD_BAUD=115200 tools/build_cam.sh upload` if communication fails, or
select a higher speed once the connection is reliable. A failure after chip
detection or a baud-rate change can be a serial-link or power issue, before the
application starts. Check the USB cable, UART connections and common ground,
and use a stable board power supply. Keep GPIO0 low through the upload, then
release it and reset to run.

The build uses `esp32:esp32:esp32cam` and overrides its usual **no-OTA** partition
layout with `min_spiffs`: 4 MB flash, two **1,966,080-byte** application slots.
The first install must include this partition table over USB/UART; an application
OTA cannot convert a board still using the old `huge_app` layout. Only
`build/ESP32_CAM.ino.bin` is retained as a Git-tracked build artifact.

Increase this module's own `FIRMWARE_VERSION` in `ESP32_CAM/config.h`, build, then
commit/push the matching source and application binary together. The CAM version
is independent of `ESP32_S3/config.h`.

On station connection (including startup), the client checks the configured public
GitHub repository/branch. It resolves a commit, reads
`MiniBot/ESP32_CAM/config.h`, and downloads `build/ESP32_CAM.ino.bin` from that
same commit only when the advertised version is newer. **Check firmware** requests
another check; internet-connected station Wi-Fi is required. The AP-only mode
cannot reach GitHub.

HTTPS uses the ESP32 certificate bundle, waiting up to three minutes for NTP time.
Failures get up to three attempts, one minute apart. There is no continuous polling
after these attempts; another station connection or manual check starts a new cycle.

During installation, the HTTP servers and camera stop to release resources. The
browser temporarily shows “reconnecting”; download progress is logged but cannot
be polled while the servers are stopped. On download failure, the camera and HTTP
servers restart, and the error remains in the RAM log. Unsaved camera changes
are lost when the camera is reinitialized. Successful installation saves its target
version in Preferences before reboot. If the installed image still advertises an
older version, that exact target is blocked from repeated download and shown on
the page. Publish a higher version or flash over USB to recover. This guard is not
a runtime crash rollback mechanism.

Physical preview, radio recovery, persistence and OTA require testing on the board;
a successful compile does not verify the camera cable or power supply.
