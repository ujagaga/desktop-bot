# MiniBot

MiniBot is an ESP32-S3 robot controller with a 240x240 ST7789 LCD, QMI8658 IMU, two PWM motor channels, Wi-Fi, battery monitoring, and line-based UART commands.

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
| Command UART RX | 13 |
| Command UART TX | 14 |
| Motor 1 | 9 / 10 |
| Motor 2 | 11 / 12 |
| Battery ADC | 6 |
| Touch sleep/wake pad | 7 |
| LCD SCLK | 40 |
| LCD MOSI | 41 |
| LCD CS | 39 |
| LCD DC | 38 |
| LCD RST | 42 |
| LCD backlight | 20 |

The command interface is available through USB `Serial` and the GPIO UART `Serial2` at `115200` baud. Commands are ASCII lines terminated by a newline and are case-insensitive.

## Touch sleep and wake

Connect a capacitive pad to GPIO7. With the pad untouched, run `touch calibrate`
from serial or the HTTP console. After a two-second settling period, calibration
is measured and saved in Preferences (NVS), surviving restarts. Startup loads
the saved baseline without recalibrating. Until calibration is saved, touch
sleep/wake and the sleep command are disabled. Recalibrate after changing the
pad or wiring. The command stops motors before sampling; failed or unstable
measurements leave the previous calibration unchanged. Keep the pad untouched
for the entire command, including when using HTTP, which buffers the reply.
Hold it for at least three seconds, then release to enter light sleep. A short
touch wakes the robot; release the pad before starting another long press.
The display and Wi-Fi restore through the same handler used by the `sleep`
command. Motors stop before sleeping. Serial and HTTP sleep also enable touch
wake; sleep is refused if touch calibration failed or the pad is still held.

Run `touch measure`, then touch and release the pad to obtain its peak raw
reading. The command sends `TOUCH MAX <value>` followed by `OK` only after the
pad has been released for 60 ms. It also accepts a touch already in progress.
Saved calibration is required to detect touch and release; measurement does
not change it. If no touch starts within 30 seconds, an error is returned.
Once touched, measurement waits for release without a hold timeout. Motors
are stopped, long-press sleep is suspended, and other commands wait until
measurement finishes. Serial and HTTP use the same behavior.

`TOUCH_HOLD_MS` and `TOUCH_THRESHOLD_PERCENT` in `ESP32_S3/config.h` control
hold duration and sensitivity. The default activation level is 20% above the
saved baseline, with a lower release threshold and 60 ms debounce. Baseline
and activation readings are returned by `touch calibrate`. Adjust sensitivity for
the actual pad and wiring; lower percentages detect smaller changes. Long
blocking commands/OTA interrupt hold tracking, so release and try again after
they finish. Wake detection runs in hardware while the CPU sleeps.

## Commands

### Help

```text
help
```

### Battery

```text
batt c
batt v
```

- `batt c` returns the battery percentage as an integer.
- `batt v` returns the measured battery voltage in volts.

### LCD

```text
lcd bl <0-100>
lcd clear
lcd face <0-15>
lcd rotate <0-3>
lcd status
lcd text <text>
lcd time
time
```

- `lcd bl` sets the backlight percentage.
- `lcd clear` clears the screen and returns to normal status updates.
- `lcd rotate` selects display rotation `0` through `3`.
- `lcd status` shows battery percentage, voltage, current time, Wi-Fi SSID, IP address, and firmware version.
- `lcd text` clears the screen and displays wrapped text. It remains visible until another LCD content command is used.
- `lcd time` shows the synchronized local time as `HH:MM` and the weekday/date below it, for example `Saturday 19.09.`. Seconds are not displayed.
- `time` prints the current `HH:MM` and weekday/date to the command console.
- `lcd face` displays a geometric face and remains visible until another LCD content command is used.

Face IDs:

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
```

- `gyro rate` reports angular velocity in degrees per second (`dps`).
- `gyro angle` reports integrated relative rotation in degrees from the last calibration or startup.
- `gyro calibrate` averages the stationary gyro bias for about one second and resets the integrated angles.

Gyro bias is loaded from Preferences namespace `miniBotGyro`, key `bias_v1`,
at startup. If no valid saved bias exists, startup calibrates once. The
`gyro calibrate` command and non-motion wakes recalibrate using stationary
samples. Fresh offsets always take effect in RAM; NVS is written only when
any axis differs by at least `GYRO_BIAS_SAVE_DELTA_DPS` (default 0.25 degrees/s)
from its last saved value. Storage failures retain the RAM calibration and
are logged on USB serial. Wi-Fi and touch use separate namespaces.

A bias is an angular-rate correction, not an angle: 0.25 degrees/s corresponds
to 5 degrees of drift over 20 seconds. It does not guarantee 5-degree accuracy
over arbitrary durations. Keep the robot still during calibration.

### Motors

```text
motor move <0|1|2> <FWD|BACK> <pwm> <ms>
motor rotate <pwm> <angle>
```

ID `1` or `2` selects one motor. ID `0` runs both motors. `pwm` is clamped to `0-100`. The motor stops automatically after the requested duration.
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

When Wi-Fi is connected, the firmware synchronizes time from NTP using the
Belgrade timezone, including Central European daylight-saving time rules. If
NTP has not completed, `lcd time` returns `ERR time unavailable`.

### Sleep

```text
sleep
```

Enters light sleep and wakes on GPIO7 touch, UART0 activity, or QMI8658 motion
via INT1/GPIO46. The LCD backlight is disabled during sleep and restored after
wake. The IMU uses its low-power accelerometer for motion detection, with
`GYRO_WAKE_THRESHOLD_MG` (default 20 mg) sensitivity and an initial eight-sample
blanking period. This detects acceleration changes, not a five-degree angle;
very slow rotation, especially around gravity, may not trigger it.

Normal gyro sensing resumes after wake. Motion wakes retain the RAM bias;
touch/UART wakes recalibrate and save only significant bias changes. A motion
flag coincident with another wake also suppresses calibration. Sleep time is
excluded from angle integration; movement during sleep is not reconstructed.
If motion wake cannot be configured, sleep is cancelled. Hardware sensitivity
and interrupt behavior must be verified on the actual board.

Wi-Fi disconnects and the radio turns off before sleep. If Wi-Fi was enabled,
the device reconnects to the same network asynchronously after waking, which
also triggers a new firmware check. Wi-Fi that was explicitly turned off stays
off. Sleep does not erase saved credentials.

## Firmware behavior

- Battery status is sampled and refreshed approximately every two seconds in normal status mode.
- Custom text and face screens suppress automatic battery status redraws.
- `lcd status` returns the display to the live battery and Wi-Fi status screen.
- The LCD, gyro, and face renderer are separated into dedicated modules.

## Build and upload

Install `arduino-cli` and the ESP32 Arduino core first. The project uses the `esp32:esp32:esp32s3` board target with
`FlashSize=16M,PartitionScheme=app3M_fat9M_16MB`: two 3 MiB OTA firmware
slots and approximately 10 MiB of FAT filesystem space on the 16 MB flash.
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
FQBN='esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=app3M_fat9M_16MB' PORT=/dev/ttyUSB0 tools/build_s3.sh upload
```

Close the serial monitor before uploading so it does not hold the serial port open.

Devices using the old 4 MB layout need one USB upload with this configuration
to install the new partition table and bootloader settings. Application-only
OTA cannot migrate the partition layout. Do not select a full-chip erase:
NVS remains at offset `0x9000`, size `0x5000`, preserving Wi-Fi and touch
Preferences during a normal upload. A bare FQBN override without these flash
options reverts to the Arduino core defaults.

### Firmware version and OTA artifact

Set `FIRMWARE_VERSION` in `ESP32_S3/config.h` before building a release. The
version is a positive integer, starting at `1`, and the LCD status screen displays
it as `FW 1`. Increase it for each release (`2`, `3`, etc.).

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
- `ESP32_S3/config.h`: firmware version
- `ESP32_S3/http_client.cpp`: HTTPS GitHub version check and OTA download
- `ESP32_S3/firmware_version.cpp`: strict integer version parser
- `ESP32_S3/comms.cpp`: UART buffering and command dispatch
- `ESP32_S3/LCD.cpp`: display, backlight, text, and status rendering
- `ESP32_S3/faces.cpp`: geometric face renderer
- `ESP32_S3/gyro.cpp`: QMI8658 driver, calibration, rates, and angle integration
- `ESP32_S3/clock.cpp`: Belgrade timezone and NTP synchronization
- `ESP32_S3/battery.cpp`: battery measurement and status refresh
- `ESP32_S3/motor.cpp`: timed motor control
- `ESP32_S3/wifi_connection.cpp`: stored Wi-Fi credentials and connection management
- `tools/build_s3.sh`: build and upload script

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

`gyro threshold` reports the current motion wake threshold in mg.
`gyro threshold 3` sets it to `GYRO_WAKE_THRESHOLD_MG + 3` (23 mg with the
current 20 mg base). The accepted adjustment is one digit, 0 through 9; it is
always relative to config, not the previous setting. Higher values require
more movement to wake.

The resulting threshold is saved under `miniBotGyro` / `wake_mg` in Preferences,
loaded on restart, and applied when entering the next sleep. Repeating the
same value avoids another flash write. Touch sensitivity is unchanged.
