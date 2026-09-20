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
on reboot and are also mirrored to Serial for development.

The camera mutex protects capture, settings and shutdown. A frame is returned
before its lock is released. HTTP handlers are stopped and joined before OTA
can deinitialize the camera.

## HTTP API

The device's `/api` page also lists these endpoints.

| Method | Path | Result |
| --- | --- | --- |
| GET | `/` | Camera/settings/Wi-Fi/log UI |
| GET | `/capture` | JPEG snapshot |
| GET | `:81/stream` | MJPEG stream |
| GET | `/status` | Camera availability and current settings as JSON |
| GET | `/config?var=quality&val=12` | Apply a validated camera setting in RAM |
| POST | `/api/camera/save` | Save current camera settings to Preferences |
| GET | `/api/logs` | Recent timestamped log lines |
| GET | `/api/info` | Firmware version, invalid target, OTA activity, free heap |
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
