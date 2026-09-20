# Copilot instructions for MiniBot

## Build, upload, and test

Run commands from the `MiniBot/` repository root. The project uses `arduino-cli`
and the Espressif Arduino core; `tools/setup_env.sh` is the documented
one-time setup helper.

- Build the ESP32-S3 controller:
  `tools/build_s3.sh`
- Build and upload the S3 controller:
  `tools/build_s3.sh upload`
- Override the board target or serial port with `FQBN=... PORT=/dev/ttyUSB0`.
  The default S3 target is
  `esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=app3M_fat9M_16MB`.
- Build the ESP32-CAM companion:
  `tools/build_cam.sh`
- Build and upload the CAM:
  `PORT=/dev/ttyUSB0 tools/build_cam.sh upload`
  (`UPLOAD_BAUD` can be overridden when needed.) Keep GPIO0 low while flashing
  the classic AI-Thinker board.
- Regenerate face firmware assets, then rebuild:
  `python3 tools/import_faces.py && tools/build_s3.sh`
- Refresh VS Code IntelliSense after a successful build:
  `python3 tools/update_intellisense.py`

There is no unified test runner or configured lint command. The host-side tests
are standalone C++ programs and should be compiled and run individually:

```sh
g++ -std=c++11 tests/test_firmware_version.cpp ESP32_S3/firmware_version.cpp \
  -o /tmp/test_firmware_version && /tmp/test_firmware_version
g++ -std=c++11 -Itests/gyro_stubs tests/test_gyro_sleep.cpp \
  -o /tmp/test_gyro_sleep && /tmp/test_gyro_sleep
g++ -std=c++11 tests/test_tap_sequence.cpp \
  -o /tmp/test_tap_sequence && /tmp/test_tap_sequence
```

To run one test, use the corresponding command above and omit the other
commands. The gyro test uses the mocks in `tests/gyro_stubs`; do not compile it
as an Arduino sketch.

## Architecture

MiniBot contains two independently flashed ESP32 applications:

- `ESP32_S3/` is the robot controller: ST7789 LCD, QMI8658 IMU, battery ADC,
  two motor channels, Wi-Fi, NTP clock, HTTP console, UART command handling,
  and GitHub-based HTTPS OTA updates.
- `ESP32_CAM/` is a classic AI-Thinker ESP32-CAM companion: OV2640 capture,
  setup/station Wi-Fi, browser camera UI, MJPEG streaming, local logs, camera
  settings persistence, console forwarding, and its own GitHub OTA flow.

Each sketch's `.ino` file is a small coordinator. Hardware and behavior are
split into modules (`lcd`, `gyro`, `motor`, `wifi_connection`, `comms`,
`http_*`, etc.), with `setup()` initializing modules and `loop()` making
cooperative `*_Process`, `*_Poll`, or `*_Process` calls. Preserve this
non-blocking main-loop design when adding behavior; long-running work must
coordinate with sleep, HTTP, motor, and OTA state.

The S3 and CAM communicate over a 115200-baud UART. The S3 sends commands and
periodic `report ip <address>` discovery messages; the CAM reports its address
back. Sleep is a coordinated protocol: S3 drives its wake line low, sends
`sleep`, waits for the explicit CAM acknowledgment, then sleeps; CAM rejects
sleep while HTTP/AP clients are active and uses the wake line to resume.
Changes to this protocol normally require matching changes in both firmware
directories.

The S3 browser console and physical serial ports share the same command
dispatcher in `ESP32_S3/comms.cpp`. Commands are case-insensitive ASCII lines,
limited to 127 bytes excluding the newline, and use `OK`/`ERR ...` responses.
The CAM web console queues commands to the S3 rather than implementing a second
command language.

Preferences/NVS is the persistence layer. Modules own their namespaces and
keys; use the existing namespace/key patterns instead of manual flash
allocation. Wi-Fi, gyro calibration/tap settings, LCD colors, camera settings,
and OTA target guards are persisted independently.

## Repository-specific conventions

- Edit source files under `ESP32_S3/` and `ESP32_CAM/`, never generated
  `.cache/sketch/` copies. Arduino build output is generated into each module's
  `.cache/` and `build/` directories.
- `ESP32_S3/face_assets.h` is generated from the numbered PNGs in
  `tools/faces/`; change the PNGs or importer options, then regenerate it.
  Face IDs come from the first two filename characters, and exactly one source
  image must exist for every ID 00–15.
- Firmware versions are independent: update `FIRMWARE_VERSION` in the relevant
  module's `config.h`. For a release, commit the matching source/config and
  tracked application binary (`ESP32_S3/build/ESP32_S3.ino.bin` or
  `ESP32_CAM/build/ESP32_CAM.ino.bin`) together. Do not publish a new version
  with an old binary.
- OTA checks read the version and download the application image from the same
  GitHub commit. Keep the repository, branch, firmware directory, version
  header, and artifact path consistent when changing OTA behavior.
- The S3 partition layout is part of the firmware contract: use the default
  16 MB `app3M_fat9M_16MB` target. A bare board-target override can select an
  incompatible layout and prevent application OTA migration.
- The CAM uses UART0 for the S3 link, so application logs belong in its
  in-memory web logger; do not add normal Serial logging that can corrupt the
  command link. Its HTTP server is unauthenticated and intended for a trusted
  local network.
- Hardware pin assignments, UART directions, wake-line wiring, OTA paths, and
  persistence keys are documented in the root and `ESP32_CAM/` READMEs and in
  the module `config.h` files. Keep those sources aligned when changing wiring
  or protocol behavior.
- Generated IntelliSense metadata and compiled outputs are implementation
  artifacts. Review the editable module sources, not cached Arduino copies,
  when diagnosing or changing firmware.
