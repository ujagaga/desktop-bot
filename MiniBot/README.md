# MiniBot

MiniBot is an ESP32-S3 robot controller with a 240x240 ST7789 LCD, QMI8658 IMU, two PWM motor channels, Wi-Fi, battery monitoring, and line-based UART commands.

## ESP32-CAM companion

The classic AI-Thinker camera module is a separate firmware project in
[`ESP32_CAM`](ESP32_CAM/README.md). It provides camera preview/settings, browser
logs, a Wi-Fi setup AP, Preferences storage and startup GitHub OTA checks.
Build it with `tools/build_cam.sh`; its pin map, version and OTA binary are
independent of the ESP32-S3 controller. See its README for first-time flashing,
Wi-Fi access and the HTTP API.

## Hardware

- Board: ESP32-S3
- Display: 1.3-inch ST7789, 240x240
- IMU: QMI8658 over I2C
- Motors: two independently controlled H-bridge channels
- Battery: single-cell Li-ion voltage monitor

### Pin map

| Function | GPIO |
| --- | ---: |
| IMU SDA | 47 |
| IMU SCL | 48 |
| IMU INT1 (motion wake) | 46 |
| IMU INT2 | 45, currently unused |
| Command UART RX | 14 |
| Command UART TX | 13 |
| CAM wake output (to CAM GPIO13) | 7 |
| Motor 1 | 9 / 10 |
| Motor 2 | 11 / 12 |
| Battery ADC | 6 |
| Charger detect (LOW while charging, MOSFET to GND, internal pull-up) | 8 |
| LCD SCLK | 40 |
| LCD MOSI | 41 |
| LCD CS | 39 |
| LCD DC | 38 |
| LCD RST | 42 |
| LCD backlight | 20 |

Once S3 has a station IP, its five-second timer sends `report ip <s3 ip addr>`
to CAM until it receives a nonzero CAM address. CAM saves the supplied S3 address
and replies with its own IP followed by `OK`, updating both modules in one exchange. The LCD status screen shows that address
on a `CAM` row (or `CAM waiting` before discovery). IPv4 replies and `OK`/`ERR`
on this port are consumed separately from incoming commands. The cached CAM
address is rediscovered after S3 wake/reboot or an S3 station address change.
CAM no longer polls S3. Use `lcd status` to return from custom displays.

Sleep is coordinated with the CAM: S3 shows `08_sleepy`, drives GPIO7 LOW,
sends `sleep`, and waits up to ten seconds for `CAM SLEEP READY`. It keeps the
face visible for at least three seconds before turning off the display and
entering light sleep. Missing/rejected acknowledgment cancels S3 sleep.
CAM refuses sleep while an HTTP/stream or setup-AP client is connected, with a
five-second grace period after HTTP disconnects. S3 shows `00_neutral` when CAM
refuses or its acknowledgment times out, keeping the screen on.
GPIO7 is held LOW during sleep and raised HIGH on confirmed wake or cancellation,
waking CAM GPIO13 from deep sleep. Connect common ground and a 10 kΩ pull-down
on CAM GPIO13. S3 then restores its status screen and rediscovers the CAM IP.

The command interface is available through USB `Serial` and the GPIO UART `Serial2` at `115200` baud. Commands are ASCII lines terminated by a newline and are case-insensitive. Keep
commands to 127 bytes, excluding the newline. Successful commands end with `OK`;
failures return `ERR ...`. Responses go to the originating console.

Use `ota check` to request an S3 firmware check while it is connected to Wi-Fi.
Use `cam ota check` to forward a check request to the connected CAM; check CAM
OTA progress in its web UI. These commands schedule checks and do not wake a
sleeping or disconnected module.

## Microphone and speaker demo

The INMP441 microphone and MAX98357A speaker amplifier share an I2S bus on the
ESP32-S3:

| Signal | GPIO |
| --- | ---: |
| I2S BCLK / microphone SCK | 2 |
| I2S WS / microphone WS | 3 |
| Amplifier shutdown/enable | 4 |
| Speaker I2S DIN | 5 |
| Microphone I2S SD | 1 |

The microphone is powered from 3V3, with its L/R pin grounded for the left I2S
slot. Run `audio test` from either command port: the speaker plays a short tone,
the microphone records two seconds, then the recording is played through the
speaker. This is an on-device bring-up test; it does not upload audio to the
server or Gemini.

## Wake word

While the S3 is awake it listens for **"Hey Jarvis"** with
[microWakeWord](https://github.com/kahrendt/microWakeWord) (ESPHome `hey_jarvis`
v2 model, `ESP32_S3/wake_word_model.h`). A detection shows face 04 on the LCD.
The listener runs as a FreeRTOS task on core 0: 10 ms I2S reads, a TFLite Micro
audio frontend (40 mel features, 30 ms window), and one inference every 30 ms.
Frontend settings, feature scaling, cutoff (0.97) and 5-inference sliding
window match ESPHome's `micro_wake_word`; a detection, like boot, is followed by
one second that cannot trigger again. `audio test` pauses the listener while it
uses the I2S bus.

```text
ww
```

Prints detections and inferences since boot, plus the maximum probability,
slowest inference, peak 16-bit microphone level, and tensor arena use since the
previous `ww`. Say the wake word, then run `ww`, to see how close it came to the
247/255 cutoff.

## Voice conversation

A detection plays a quiet three-tone chime and opens a session with the Gemini
gateway on the Pi (`server/conversation.py`), normally through
`wss://face.ujagaga.in.rs/conversation`. The wake word task hands its microphone
audio to `ESP32_S3/voice.cpp` from the moment of detection, so speech during
the TLS connect is buffered (2 s), then sent as 100 ms frames of 16 kHz PCM.
Gemini decides when you have finished speaking and replies with audio, which a
playback task plays through the amplifier (60 s PSRAM buffer). There is no
echo cancellation: while anything plays, and for 300 ms after, microphone audio
is discarded and `audio_end` is sent. After a reply the session stays open for
8 seconds of follow-up; it closes after 8 seconds without speech or replies,
on errors, or after 3 minutes. Face 04 shows during the session; the status
screen returns when it ends. If the session cannot start or fails (no key, no
Wi-Fi, connect or gateway error), face 02 shows for two seconds and `voice`
reports the reason. Wake word detection is paused during a session.

```text
voice
voice url <wss://host[:port]/path>
voice key <api-key>
voice name [<voice>|default]
voice start
voice stop
```

- `voice` prints the state, URL, whether a key is set, the last health check
  result and HTTP code, the session count, audio
  seconds sent and received, dropped reply bytes, and the last error.
- `voice url` and `voice key` save the gateway URL and the device API key
  (`API_KEY` in the Pi's `server/appsettings.py`) in Preferences
  (`miniBotVoice`). Without a saved URL, `VOICE_DEFAULT_URL` from
  `ESP32_S3/config.h` is used; there is no key fallback. The key is never printed. Do not compile it into the
  firmware: the repository and OTA binaries are public.
- `voice name` prints the gateway's current Gemini voice and the 30 choices;
  `voice name <voice>` (any letter case) or `voice name default` saves it on the
  Pi (`server/voice.json`, overriding `GEMINI_VOICE`) for all new sessions.
- `voice start` opens a session without the wake word; `voice stop` ends it.

A gateway command (`server/commands/`) can send `{"type":"display","face":N,"text":"...","seconds":S}`:
the robot shows face N (0–15) with the text as a footer (about 20 characters), or the text full
screen without a face; `"clock":"14:32"` instead shows big time text. It stays until the session
ends, or exactly S seconds (max 600) when given; then the normal screen (neutral face) returns.

When a session is ready, the S3 sends `snapshot <session>` to a discovered CAM, which POSTs one
frame to the gateway's `/snapshot`; Gemini can ask who is present when needed.
When the recognition result arrives, the robot shows 01_happy, winks (05_wink) for
1 s and returns to 01_happy if anyone is recognized; otherwise it shows 00_neutral.
A `{"type":"sleep"}` message (from a command such as `sleep.py`) makes the robot wait for
Gemini's reply to finish, end the session and run `sleep` (see the CAM README for its
gateway settings).

## Sleep and tap wake

GPIO7 capacitive touch support and its commands have been removed. The IMU
now groups acceleration impulses into single, double, triple, or longer tap
sequences. Allow 120–500 ms between taps, and then pause for over 500 ms:

- Awake: taps no longer cause sleep (it was too sensitive). Use the `sleep`
  command, ask in a voice session ("go to sleep"; `server/commands/sleep.py`),
  or lay the robot on any side: once it has been upright (vertical X axis within
  30° of gravity, by accelerometer), a tilt above 70° held for 1 s sleeps, after
  any voice session ends.
- After a tilt sleep, standing the robot upright again for 0.5 s wakes it (the
  lift triggers motion wake), and `gyro calibrate` runs 3 s later. Taps still wake it.
- Sleeping: a first motion event briefly wakes the CPU with LCD/Wi-Fi off.
  A sequence at or above the saved wake count completes the wake (default 1);
  fewer taps return the CPU to sleep.
- UART wake remains available.

Set the wake count with `gyro tap wake <1-3>`. The comparison is inclusive:
setting 2 means two or more taps. Omit the number to read the current setting.
The count survives restarts in Preferences; repeating an unchanged saved setting
avoids a flash write.
A sequence is evaluated after the quiet interval, not immediately on the nth tap.

The first sleeping impulse is detected by hardware WoM; subsequent impulses
are checked in software. Short pulses must return to quiet within 100 ms.
This is a heuristic and needs tuning/testing on the assembled robot; vibration
can resemble taps. Long blocking commands and OTA can interrupt awake tap
sampling. The firmware logs the observed tap count over USB serial.

## Commands

### Help

```text
help
```

### Battery

```text
batt c
batt v
batt chg
```

- `batt c` returns the battery percentage as an integer.
- `batt v` returns the measured battery voltage in volts.
- `batt chg` returns `1` while charger 5V is present (a MOSFET pulls GPIO8 LOW), else `0`.
- `bat` is also accepted as an alias for `batt`.

Voltage averages eight ADC readings with the onboard 2:1 divider correction.
Percentage is a linear, clamped estimate from 3.3 V (0%) to 4.2 V (100%).

### LCD

```text
lcd bl <0-100>
lcd clear
lcd color bg <4 hex digits>
lcd color fg <4 hex digits>
lcd face <0-15>
lcd rotate <0-3>
lcd status
lcd text <text>
lcd time
time
```

- `lcd bl` sets the backlight percentage. It is restored after light sleep but is
  not saved across a reboot.
- `lcd color bg` and `lcd color fg` set and save RGB565 background/text colors;
  see [Saved LCD colors](#saved-lcd-colors).
- `lcd clear` clears the screen and returns to normal status updates.
- `lcd rotate` selects display rotation `0` through `3`.
- `lcd status` shows battery percentage, voltage, current time, Wi-Fi SSID, IP address, and firmware version.
- `lcd text` clears the screen and displays wrapped text. It remains visible until another LCD content command is used.
- `lcd time` shows the synchronized local time as `HH:MM` and the weekday/date below it, for example `Saturday 19.09.`. Seconds are not displayed.
- `time` prints the current `HH:MM` and weekday/date to the command console.
- `lcd face` displays a bitmap face and remains visible until another LCD content command is used.

Face source images live in `tools/faces/`, named `00_neutral.png` through
`15_shocked.png`. They were split from the original sheet without frames or captions.
Only the first two filename characters determine the ID (00–15); the rest of the
name is arbitrary. Provide exactly one PNG per ID. Missing or duplicate IDs are rejected. The default image bounds are 240×240 pixels, centered on the display without
stretching. Each asset declares its own width and height; the renderer writes only
that rectangle, after clearing the previous screen once. Margins use the configured
LCD background; the artwork retains its own colors and black background.

The 16 RGB565 palettes and run-length encoded images occupy 77,937 bytes (about
76 KiB) in flash, plus a small descriptor table. No full-image RAM buffer is needed.
Preview the individual source PNGs directly in `tools/faces/`.

To regenerate smaller images, install Python Pillow if needed, then run:

```sh
python3 tools/import_faces.py --size 120
tools/build_s3.sh
```

`--size` is the maximum dimension in pixels (16–240), preserving aspect ratio.
Use `--face-size 00=96` to override one face independently; repeat this option
for other faces. Running without arguments restores the default 240-pixel size.
The importer writes `ESP32_S3/face_assets.h`; PNG previews are optional.
Changes take effect after rebuilding and installing the firmware. The importer reads the individual PNGs, never the original sheet, and leaves them
unchanged. It preserves aspect ratio, only shrinks oversized images, and composites
transparent pixels onto black. Use `--input-dir /path/to/faces` for a different
source directory. No preview folder is created by default. To optionally generate
rendered previews, pass `--previews /tmp/face-previews`.

Default artwork by face ID (replacement artwork may differ):

| ID | Expression |
| ---: | --- |
| 0 | Neutral |
| 1 | Happy |
| 2 | Sad |
| 3 | Excited |
| 4 | Surprised |
| 5 | Wink |
| 6 | Angry |
| 7 | Confused |
| 8 | Sleepy |
| 9 | Laughing |
| 10 | Blushing |
| 11 | Worried |
| 12 | Skeptical |
| 13 | Playful |
| 14 | Cool |
| 15 | Shocked |

Examples:

```text
lcd text MiniBot ready
lcd face 1
lcd status
```

### Gyro

```text
gyro angle <x|y|z|0|1|2>
gyro rate <x|y|z|0|1|2>
gyro calibrate
gyro threshold [0-10]
gyro tap wake [1-3]
```

- `gyro rate` reports angular velocity in degrees per second (`dps`).
- `gyro angle` reports integrated relative rotation in degrees from the last calibration or startup.
- `gyro calibrate` averages the stationary gyro bias for about one second and resets the integrated angles.
- `gyro threshold` reads or saves the acceleration sensitivity multiplier; see
  [Motion wake sensitivity](#motion-wake-sensitivity).
- `gyro tap wake` reads or saves the minimum wake tap count. Values 1–3 mean
  that many taps or more, not strictly more.

Gyro bias is loaded from Preferences namespace `miniBotGyro`, key `bias_v1`,
at startup. If no valid saved bias exists, startup calibrates once. The
`gyro calibrate` command and non-motion wakes recalibrate using stationary
samples. Fresh offsets always take effect in RAM; NVS is written only when
any axis differs by at least `GYRO_BIAS_SAVE_DELTA_DPS` (default 0.25 degrees/s)
from its last saved value. Storage failures retain the RAM calibration and
are logged on USB serial. Wi-Fi uses a separate namespace.

A bias is an angular-rate correction, not an angle: 0.25 degrees/s corresponds
to 5 degrees of drift over 20 seconds. It does not guarantee 5-degree accuracy
over arbitrary durations. Keep the robot still during calibration.

### Motors

```text
motor move <0|1|2> <FWD|BACK> <pwm> <ms>
motor rotate <pwm> <angle>
drive <f|b|l|r> <pwm>
drive s
motor calibrate <1|2> <percent>
motor calibrate 0 <duration in seconds>
```

`motor calibrate` sets a per-motor PWM multiplier from `0` to `100`, saved in
Preferences namespace `motorCal` (keys `motor1` and `motor2`). Both default to
100%. For example, `motor calibrate 1 80` makes a subsequent 100% request use
80% PWM on motor 1; a 50% request uses 40%. Motor 2 keeps its own multiplier.
Calibration applies to forward/reverse moves and rotations, starting with the
next motor command. Set 100 to restore full output or 0 to suppress that motor's
output. Invalid arguments or a failed save leave the previous setting unchanged.
`motor calibrate 0 <seconds>` performs an automatic straight-line calibration.
It runs both motors forward at 30% for the requested 1–60 seconds while
integrating the gyro X angle. Motor 1 is treated as the left motor and motor 2
as the right motor; with this robot's convention, positive X rotation means
motor 1 is stronger and negative X rotation means motor 2 is stronger. The
identified stronger motor is reduced while the weaker motor remains unchanged.
The correction is proportional to the measured X rotation rate, capped at a
50% reduction per run, and is saved to Preferences. Run the command again if
the first correction is not sufficient. Stop the robot before starting the
command and keep it on a surface where both wheels can move freely.
This trims PWM output; actual wheel speed and straightness depend on the motors
and load. Tune the faster motor downward to match the slower one.

ID `1` or `2` selects one motor. ID `0` runs both motors. `pwm` is clamped to `0-100`. The motor stops automatically after the requested duration.
`drive` runs both motors for one 500 ms pulse: forward, back, or rotate left/right
(left is the positive `motor rotate` direction); `drive s` stops them now. The
server's drive page sends it through the CAM while a button is held, and
`drive s` on release.

`motor rotate` drives both motors in opposite directions until the requested relative gyro angle is reached on the configured axis. The default axis is Z. The current convention is motor 1 forward and motor 2 backward for positive rotation. The axis and direction settings are defined in `ESP32_S3/motor.h` so they can be changed if testing shows they are wrong. The command reverses at reduced power to correct a small overshoot and has a 15-second safety timeout.

Examples:

```text
motor move 0 FWD 50 500
motor move 2 BACK 35 1000
motor rotate 40 90
```

### HTTP console

Once Wi-Fi is connected, open `http://<device-ip>/` for the web console on port 80.
Use `wifi ip` over serial to find the address. Enter `help` in the page to list
commands. `http_server.cpp` uses `COMMS_Execute()` from `comms.h`, so the web
console and both serial ports share all command handlers and `OK`/`ERR` replies.
The page displays command replies; background serial logs are not streamed.

The browser saves the last 10 submitted commands in localStorage. Arrow Up
recalls the newest command, then progressively older commands; Arrow Down moves
toward newer ones and restores the unfinished input after the newest entry.
History survives page reloads for that browser/site address. If browser storage
is unavailable, history still works for the current page session.

Commands can also be sent directly with GET:

```sh
curl --get --data-urlencode 'cmd=batt v' http://<device-ip>/command
curl --get --data-urlencode 'cmd=lcd text Hello MiniBot' http://<device-ip>/command
```

The endpoint accepts one command of up to 127 bytes without line endings and
returns plain text. Missing or malformed input returns HTTP 400; executed
commands return HTTP 200 with the normal command response, including `ERR` for
command failures. Long-running commands finish before their response is sent.
`sleep` responds with `OK sleep scheduled` before invoking the shared sleep
handler, with a 500 ms grace period for delivery before Wi-Fi turns off. This
acknowledges scheduling; it does not confirm that sleep has completed.
`wifi off`, `wifi clear`, or changing Wi-Fi may disconnect the request after
executing. The server resumes when Wi-Fi reconnects. The console has no
authentication and is intended for a trusted local network.

### Wi-Fi

```text
wifi on <ssid> <pass>
wifi off
wifi clear
wifi ip
```

`wifi on` connects to the network and stores the credentials. SSID and password are whitespace-delimited, so they currently cannot contain spaces.
`wifi ip` returns the current IP address, or `0.0.0.0` when disconnected.
`wifi off` disconnects without deleting credentials; a reboot can reconnect using
them. `wifi clear` disconnects and deletes the saved SSID/password. Startup
connects asynchronously using saved credentials; `wifi on` waits up to 15 seconds.

When Wi-Fi is connected, the firmware synchronizes time from NTP using the
Belgrade timezone, including Central European daylight-saving time rules. If
NTP has not completed, `lcd time` returns `ERR time unavailable`.

### Sleep

```text
sleep
```

Stops both motors, enters light sleep, and waits for the configured IMU tap
count, charger connect, or UART0 activity. Charger connect (GPIO8 pulled LOW)
is armed only when the robot is not already charging, because GPIO wake is
level-triggered; it completes the wake without a tap count. UART0 wake is distinct from the `Serial2` command port;
GPIO13/14 are not configured as the sleep wake source. INT1 on
GPIO46 provides the first motion interrupt. The LCD and Wi-Fi remain off while
software confirms the configured tap count. IMU wakes retain gyro bias; UART wakes
recalibrate and save only significant changes. Sleep time is excluded from
angle integration. If the IMU cannot be configured, sleep is cancelled.

Wi-Fi disconnects and the radio turns off before sleep. If Wi-Fi was enabled,
the device reconnects to the same network asynchronously after waking, which
also triggers a new firmware check. Wi-Fi that was explicitly turned off stays
off. Sleep does not erase saved credentials.

## Firmware behavior

- After boot and after waking from sleep, the live status screen shows until two
  seconds after the S3 has a Wi-Fi IP address (immediately if Wi-Fi is turned off).
- Then face 00 (neutral) shows. While a problem lasts, face 02 (sad) shows instead,
  with one line below it: `LOW BATTERY <V>` (below 15%, clears at 20% or when
  charging), `NO WI-FI` (Wi-Fi enabled but disconnected), or `NO SERVER` (the
  gateway `/health` check, run every 60 s, fails or reports Gemini unconfigured).
  The screen is redrawn only when this state changes, so `lcd text`, `lcd face`, and
  `lcd status` content stays until the next change.
- The status screen refreshes about every two seconds while visible.
- `lcd status` returns the display to the live battery and Wi-Fi status screen.
- The LCD, gyro, and face renderer are separated into dedicated modules.

### LCD row layout

`ESP32_S3/lcd.h` centralizes `LCD_DEFAULT_TEXT_SIZE` (2), `LCD_ROW_TOP` (20),
`LCD_ROW_LEFT` (10), and `LCD_ROW_GAP` (12). The default row pitch is 28 pixels:
`8 * textSize + LCD_ROW_GAP`. `LCD_SetRow(row, textSize)` positions a zero-based
row; `LCD_ClearRow(row, textSize)` clears its full width using the current
background. The size argument is optional and defaults to `LCD_DEFAULT_TEXT_SIZE`.
Use the same size for both helpers; invalid sizes or off-screen rows return false.

Status rows are fixed: battery, time, firmware version, reserved invalid-target
message, Wi-Fi SSID, and IP address. Only changed rows are cleared and redrawn,
reducing flicker. Long status strings are clipped rather than wrapped into the
next row. The optional invalid-target row stays reserved even when empty.

### Persistent settings

Preferences manages NVS storage by namespace and key; no manual address
allocation is needed between these modules.

| Namespace | Keys | Saved data |
| --- | --- | --- |
| `miniBotWiFi` | `ssid`, `pass` | Wi-Fi credentials |
| `miniBotGyro` | `bias_v1` | Three gyro bias values |
| `miniBotGyro` | `tap_mult` | Acceleration threshold multiplier |
| `miniBotGyro` | `wake_taps` | Minimum wake tap count |
| `miniBotLCD` | `bg`, `fg` | LCD RGB565 colors |
| `miniBotOTA` | `target` | Advertised OTA target for restart validation |
| `miniBotVoice` | `url`, `key` | Gemini gateway URL and device API key |

These settings survive normal restarts and application OTA updates. `wifi clear`
only clears Wi-Fi credentials. Browser command history lives in the browser,
not in device Preferences.

## Build and upload

Install `arduino-cli`, the ESP32 Arduino core, and the Arduino libraries
`Adafruit GFX Library`, `Adafruit ST7735 and ST7789 Library`, `ArduinoJson`,
and `ESP_TF@2.1.1` (TFLite Micro with ESP-NN, for the wake word), with their
dependencies. The build script also passes `--libraries libraries`, which holds
`microfrontend`, the TFLite Micro audio frontend copied from tflite-micro.
`ESP32_S3/build_opt.h` enables ESP-NN's ESP32-S3 assembly kernels for the
whole build. Python 3 is needed for IntelliSense generation; Pillow
is needed only when regenerating face assets. The project uses the `esp32:esp32:esp32s3` board target with
`FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=opi`: two 3 MiB OTA firmware
slots and approximately 10 MiB of FAT filesystem space on the 16 MB flash, plus
the 8 MB octal PSRAM, which holds the voice conversation audio buffers.
The build script and VS Code Arduino settings select this layout by default.

Compile only:

```bash
tools/build_s3.sh
```

Compile and upload to the default port `/dev/ttyACM0`:

```bash
tools/build_s3.sh upload
```

Override the board or serial port with environment variables:

```bash
FQBN='esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=opi' PORT=/dev/ttyUSB0 tools/build_s3.sh upload
```

Close the serial monitor before uploading so it does not hold the serial port open.

Devices using the old 4 MB layout need one USB upload with this configuration
to install the new partition table and bootloader settings. Application-only
OTA cannot migrate the partition layout. Do not select a full-chip erase:
NVS remains at offset `0x9000`, size `0x5000`, preserving existing
Preferences during a normal upload. A bare FQBN override without these flash
options reverts to the Arduino core defaults.

### Firmware version and OTA artifact

Set `FIRMWARE_VERSION` in `ESP32_S3/config.h` before building a release. The
version is a positive integer, starting at `1`, and the LCD status screen displays
it as `Firmware V1`. Increase it for each release (`2`, `3`, etc.).

After running `tools/build_s3.sh`, commit the source changes together with
`ESP32_S3/build/ESP32_S3.ino.bin`. This application binary is the only build
artifact tracked by Git and is the image to use for application OTA updates.
Bootloader, partition table, merged flash images, and other build outputs remain
ignored. The application binary must fit within the 3,145,728-byte OTA
slot, and the device must already have a compatible partition layout.

After each Wi-Fi connection, `http_client.cpp` checks the latest commit on
`ujagaga/desktop-bot`, branch `main`. It reads `MiniBot/ESP32_S3/config.h` from
that commit and, only when its integer version is higher than the running
version, downloads `MiniBot/ESP32_S3/build/ESP32_S3.ino.bin` from the same commit.
The repository, branch, and firmware directory are configured in `config.h`.
The repository must be public; no GitHub credentials are stored on the device.

HTTPS uses the ESP32 core's trusted root certificate bundle. The check waits
up to three minutes for NTP time so certificates can be validated. Failed checks
or downloads get up to three attempts, one minute apart; a new Wi-Fi connection
starts a fresh check. An equal or older version is skipped. Serial output reports
the versions and any errors. There is no periodic check while continuously
connected after the attempts finish.

Motors stop before the blocking HTTP requests. Commands and display refreshes
pause during the check/download. When a newer version is found, the LCD clears
and shows a dedicated firmware update screen with the target version, percentage,
and progress bar. It shows connection, download, verification, and restart status.
If the update fails, the failure message stays visible until another LCD command
or update attempt replaces it; details are logged to Serial.
The updater validates the image and available
OTA slot capacity, then reboots after a successful installation. Failed downloads
leave the current firmware selected. This does not provide automatic rollback
if a successfully installed firmware later fails at runtime.

For each release, increase the version, run `tools/build_s3.sh`, and commit and
push the matching config, source, and application binary together. Do not publish
a new version with an old binary. Devices running firmware without this OTA
client need one initial USB upload using `tools/build_s3.sh upload`.

## Project structure

- `ESP32_S3/ESP32_S3.ino`: firmware setup and main loop
- `ESP32_S3/config.h`: firmware version, OTA repository, and IMU wake/bias settings
- `ESP32_S3/http_server.cpp`: browser console, command history, and GET commands
- `ESP32_S3/http_client.cpp`: HTTPS GitHub version check and OTA download
- `ESP32_S3/firmware_version.cpp`: strict integer version parser
- `ESP32_S3/comms.cpp`: UART buffering and command dispatch
- `ESP32_S3/lcd.cpp`: display, backlight, text, and status rendering
- `ESP32_S3/faces.cpp`: native-resolution bitmap face renderer
- `ESP32_S3/face_assets.h`: generated numeric face palettes, RLE data, and dimensions
- `ESP32_S3/face_bitmap.h`: bitmap descriptor format
- `ESP32_S3/tap_sequence.h`: tap grouping and quiet-interval detection
- `ESP32_S3/gyro.cpp`: QMI8658 driver, calibration, rates, and angle integration
- `ESP32_S3/clock.cpp`: Belgrade timezone and NTP synchronization
- `ESP32_S3/battery.cpp`: battery measurement, status refresh, and charger detect/wake
- `ESP32_S3/audio.cpp`: I2S microphone/speaker, shared with the wake word task
- `ESP32_S3/wake_word.cpp`: microWakeWord listener task
- `ESP32_S3/wake_word_model.h`: embedded `hey_jarvis` model
- `ESP32_S3/voice.cpp`: Gemini gateway WebSocket session, chime, reply playback, and health check
- `ESP32_S3/screen.cpp`: status screen, idle face, and error display policy
- `libraries/microfrontend/`: TFLite Micro audio frontend library
- `ESP32_S3/motor.cpp`: timed motor control
- `ESP32_S3/wifi_connection.cpp`: stored Wi-Fi credentials and connection management
- `tools/build_s3.sh`: build and upload script
- `tools/import_faces.py`: convert numbered PNGs to firmware face assets
- `tools/faces/`: replaceable source PNGs and face-specific instructions
- `tools/update_intellisense.py`: generate VS Code configuration from the build

### VS Code IntelliSense

`tools/build_s3.sh` refreshes the C/C++ include paths, defines, and compiler from
the actual Arduino build using `tools/update_intellisense.py` (requires Python 3).
The generated compilation database maps Arduino's cached sketch copies back to
the editable files under `ESP32_S3/`. To refresh settings without rebuilding,
run `python3 tools/update_intellisense.py` after a successful build.

If old include errors remain, run **C/C++: Reset IntelliSense Database** from
the Command Palette, then **Developer: Reload Window**. Edit the files directly
in `ESP32_S3/`, not the generated `.cache/sketch/` copies.

### Motion wake sensitivity

`gyro threshold` reports the threshold and multiplier. `gyro threshold 3`
sets `GYRO_WAKE_THRESHOLD_MG * 3` (75 mg with the current 25 mg base).
The command parser currently accepts 0–10. The underlying setter permits 0–12,
but rejects products above the sensor maximum of 255 mg; with the current base,
10 gives 250 mg and 11–12 would exceed that maximum. The runtime `help` output
still lists 0–12, so follow the parser's 0–10 range.
Zero disables tap wake, leaving UART wake available. Higher values
require stronger acceleration impulses; this is not a rotation-angle threshold.

The multiplier is saved under `miniBotGyro` / `tap_mult` and restored at boot.
Repeating a saved value avoids another flash write. Previous additive
`wake_mg` settings are ignored because their meaning differs; the initial
multiplier is 1. Removed touch calibration data is no longer used.

### OTA rollback

A new image stays pending verification for its first 60 seconds
(`verifyRollbackLater()` in `ESP32_S3.ino`); the loop then marks it valid. A
crash or reset before that makes the bootloader boot the previous image, and
the invalid-target protection below stops the same version being re-downloaded.

### Invalid OTA target protection

After a successful download, OTA saves its advertised target version in
Preferences (`miniBotOTA` / `target`) before restarting. If the next boot still
runs an older version, that target is marked invalid and will not be downloaded
again, including after power cycles or Wi-Fi reconnections. The status LCD
shows `max fw V<n> - invalid` below the installed firmware version. Successful
installation of the target or a newer version clears the saved target.

To recover, publish a correctly built firmware under a **higher version** or
install it over USB; replacing a binary under the same invalid version will
not retry it. Devices need this protection installed before it can detect a
failed target. A failure to save the target cancels the planned restart and
attempts to restore the running firmware as the boot partition.

Display background and text colors are configured centrally as RGB565 values
in `ESP32_S3/lcd.h`: `LCD_BACKGROUND_COLOR` and `LCD_TEXT_COLOR`.

### Saved LCD colors

Use `lcd color bg 0000` for a black background and `lcd color fg ffff` for
white text. Values are exactly four hexadecimal RGB565 digits, case-insensitive
(no `0x` prefix). Examples: `f800` red, `07e0` green, `001f` blue.
Colors apply immediately to status, text, and clock screens and are also used
by OTA screens. They are stored independently in Preferences namespace
`miniBotLCD`, keys `bg` and `fg`, and loaded at startup. The defaults in
`lcd.h` apply when no saved value exists. Repeating a saved color avoids a
flash write. Face artwork and the cyan progress fill retain their own colors.
