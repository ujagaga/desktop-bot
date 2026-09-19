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
| IMU INT1 | 46, currently unused |
| IMU INT2 | 45, currently unused |
| Command UART RX | 13 |
| Command UART TX | 14 |
| Motor 1 | 9 / 10 |
| Motor 2 | 11 / 12 |
| Battery ADC | 6 |
| LCD SCLK | 40 |
| LCD MOSI | 41 |
| LCD CS | 39 |
| LCD DC | 38 |
| LCD RST | 42 |
| LCD backlight | 20 |

The command interface is available through USB `Serial` and the GPIO UART `Serial2` at `115200` baud. Commands are ASCII lines terminated by a newline and are case-insensitive.

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

The gyro is also initialized and calibrated automatically at startup and after waking from light sleep. Keep the robot still during calibration.

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

Enters light sleep and wakes on activity on UART0. The LCD backlight is disabled during sleep and restored after wake. The gyro is reinitialized and calibrated after waking.

## Firmware behavior

- Battery status is sampled and refreshed approximately every two seconds in normal status mode.
- Custom text and face screens suppress automatic battery status redraws.
- `lcd status` returns the display to the live battery and Wi-Fi status screen.
- The LCD, gyro, and face renderer are separated into dedicated modules.

## Build and upload

Install `arduino-cli` and the ESP32 Arduino core first. The project uses the `esp32:esp32:esp32s3` board target.

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
FQBN=esp32:esp32:esp32s3 PORT=/dev/ttyUSB0 tools/build_s3.sh upload
```

Close the serial monitor before uploading so it does not hold the serial port open.

### Firmware version and OTA artifact

Set `FIRMWARE_VERSION` in `ESP32_S3/config.h` before building a release. The
version is a positive integer, starting at `1`, and the LCD status screen displays
it as `FW 1`. Increase it for each release (`2`, `3`, etc.).

After running `tools/build_s3.sh`, commit the source changes together with
`ESP32_S3/build/ESP32_S3.ino.bin`. This application binary is the only build
artifact tracked by Git and is the image to use for application OTA updates.
Bootloader, partition table, merged flash images, and other build outputs remain
ignored. The application binary must fit within the current 1,310,720-byte OTA
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
pause during the check/download. The updater validates the image and available
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
