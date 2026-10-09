# MaxiBot plan

MaxiBot is a desk robot built on the WT9932P4-TINY (ESP32-P4) with an
ESP32-WROOM-32D as its Wi-Fi co-processor. It reuses the MiniBot voice
assistant (wake word, then a Gemini Live conversation through the gateway in
`server/conversation.py`). It differs from MiniBot in these ways:

| | MiniBot | MaxiBot |
| --- | --- | --- |
| MCU | ESP32-S3 (Arduino) | ESP32-P4 (ESP-IDF 6.0) |
| Wi-Fi | built in | ESP32-WROOM over SPI (ESP-Hosted) |
| Power | Li-ion battery, charger, docking | continuous 5 V supply |
| Sleep | light sleep, tap / tilt / charger wake | none (idle state only) |
| Movement | two wheel motors | head: one pan servo, one tilt servo |
| LCD | 240x240, faces | 2.25" ST7789 76x284, sound graphics and text |
| IMU | QMI8658, taps, tilt sleep | MPU6050, precise angles (navigation later) |
| Camera | separate ESP32-CAM, UART link | OV5647 on the P4 itself |
| Wake word | "Hey Jarvis" | "Hey Mycroft" |

## Hardware

Wiring follows [Schematic.png](Schematic.png).

| Function | P4 pin | Other side |
| --- | ---: | --- |
| I2S BCLK | IO16 | all four INMP441 SCK, MAX98357A BCLK |
| I2S WS | IO17 | all four INMP441 WS, MAX98357A LRC |
| Mic data in A | IO18 | mic3 SD (L/R to GND, left) + mic4 SD (L/R to 3.3 V, right) |
| Mic data in B | IO21 | mic1 SD (L/R to GND, left) + mic2 SD (L/R to 3.3 V, right) |
| Speaker data out | IO19 | MAX98357A DIN |
| Amp enable | IO20 | MAX98357A SD |
| MPU6050 SCL / SDA / INT | IO15 / IO14 / IO13 | GY-521, AD0 to GND (0x68) |
| LCD SCL / SDA / RES / DC / CS / BL | IO12 / IO11 / IO10 / IO9 / IO6 / IO5 | ST7789 |
| Tilt servo / Pan servo | IO28 / IO29 | SG90 signal |
| WROOM EN (reset) | IO22 | EN |
| SPI MOSI / MISO / SCLK / CS | IO23 / IO34 / IO33 / IO32 | GPIO23 / 19 / 18 / 5 |
| Handshake / Data ready | IO26 / IO27 | GPIO17 / GPIO16 |
| Camera | CSI connector | OV5647 (already working as USB UVC webcam) |

Power: MAX98357A, both servos, the LCD and the WROOM run from the common 5 V
supply. The four INMP441 and the MPU6050 use 3.3 V. Servo current peaks are the
largest load, so the supply should give at least 2 A, with a bulk capacitor
near the servos.

Microphones: four INMP441 in a square about 4-6 cm across, facing outward,
away from the speaker and servos. Each mic has its own 100 nF decoupling
capacitor. Two mics share each data line, one in the left slot and one in the
right. Mics 1 and 2 (IO21) can be left unpopulated and the robot works with
mics 3 and 4 on IO18 (one stereo line, left/right direction only).

The WROOM firmware (`ESP32WROOM/`, ESP-Hosted SPI co-processor) already uses
handshake GPIO17 and data ready GPIO16, matching the schematic.

## What is copied from MiniBot

Each part is ported from Arduino to ESP-IDF, not copied file by file.

| MiniBot source | MaxiBot module | Change |
| --- | --- | --- |
| `audio.cpp` | `audio` | IDF `i2s_std`, 16 kHz, new pins: I2S0 master full duplex (speaker + stereo mic line A), I2S1 slave RX on the same BCLK/WS (stereo mic line B) |
| `wake_word.cpp`, `libraries/microfrontend` | `wake_word` | microWakeWord "hey_mycroft" model; esp-tflite-micro has P4 support |
| `voice.cpp` | `voice` | `esp_websocket_client` instead of arduinoWebSockets; same protocol, chime, mute during playback, 8 s follow-up |
| `comms.cpp` command console | `console` | same line commands over USB-Serial-JTAG and HTTP |
| `wifi_connection.cpp`, ESP32-CAM setup AP | `wifi` | `esp_wifi_remote` through ESP-Hosted; credentials in NVS; setup AP as on the ESP32-CAM |
| `http_server.cpp` | `http` | web console, `/command` endpoint |
| OTA check from GitHub | `ota` | P4 app only at first; WROOM updates stay manual |
| CAM snapshot at session start | `voice` | P4 sends its own JPEG frame (`{"type":"snapshot"}` + binary frame) |

Not copied: battery and charger code, sleep and wake logic, tap detection,
tilt sleep, wheel motors and drive page, docking, face bitmaps, CAM UART link.

## Display

The LCD does not show faces. Planned screens:

- **Idle**: a calm, slow animation (for example a flat line or a breathing dot).
- **Listening**: graphics follow the microphone level after the wake word.
- **Speaking**: graphics follow the reply audio. The playback task computes
  the level of each 20 ms block (RMS), and the screen draws bars or a
  waveform from the last few levels, at about 25-30 fps.
- **Text**: shown instead of graphics when there is real text to read:
  Wi-Fi setup (AP name, password, IP to open), the station IP after connect,
  errors (NO WI-FI, NO SERVER), and gateway `display` messages that carry text
  or a clock.

Gateway `display` messages with a `face` number have no face to show. MaxiBot
ignores the face and shows only the text, if any.

## Head

Two SG90 servos on LEDC PWM (50 Hz, 500-2500 us pulses): tilt on IO28, pan on
IO29. The full servo range is safe. The firmware moves smoothly toward a target
angle instead of jumping to it.

The head moves on its own:

- **Face the speaker.** The four microphones give the direction of a voice
  (time difference between mics, GCC-PHAT). After the wake word, the head pans
  toward the voice. Then face detection (esp-dl) on camera frames takes over
  and keeps the nearest face centered. With no face in view, the head stays
  pointed at the voice.
- **Nod.** Small tilt movements while the user speaks (acknowledging) and
  while the robot replies.

The only spoken control is **follow an object**: a gateway tool (for example
"follow the cup") sends the object name to the robot, which then tracks that
object instead of faces until told to stop or the object is lost for a few
seconds. On-device detection only knows the classes its model was trained on
(for example the 80 COCO classes of esp-dl's YOLO11n), so the tool should
reject names outside that list.

Debug commands:

```text
head <pan> <tilt>       move to angles in degrees
head center
head auto <on|off>      enable or disable autonomous movement
```

## IMU

MPU6050 on I2C. Gyro bias calibration and NVS storage as in MiniBot, plus a
complementary filter (gyro with accelerometer correction) for pitch and roll.
Yaw comes from gyro integration only, so it drifts slowly. Navigation is not
planned yet. This module is only the angle source for it.

```text
gyro angle <x|y|z>
gyro rate <x|y|z>
gyro calibrate
```

## Gateway changes

- First iteration: MaxiBot uses the existing gateway as is (same
  `/conversation` WebSocket and API key as MiniBot).
- Later: separate MaxiBot endpoints, so MaxiBot-only tools (follow object)
  and settings (voice, system instruction) do not affect MiniBot.
- The `sleep` tool has no meaning for MaxiBot, since it does not sleep.

## Build phases

Each phase ends with something testable on the robot.

1. **Console and Wi-Fi.** ESP-Hosted host side on the P4, Wi-Fi station with
   NVS credentials, `wifi on/off/clear/ip`, USB-Serial-JTAG console, HTTP
   console. Test: `wifi ip` returns an address, the web console works.
2. **LCD.** ST7789 driver (`esp_lcd`), text screen, status screen with IP.
   Wi-Fi setup AP with its credentials on screen when no network is saved.
3. **Audio.** I2S full duplex, `audio test` (tone, record 2 s, play back).
   First check that the I2S1 slave receiver can share the BCLK/WS pins
   driven by I2S0 (connected inside the GPIO matrix with
   `esp_rom_gpio_connect_out_signal` / `esp_rom_gpio_connect_in_signal`).
   If not, use mics 3 and 4 only.
   The two controllers do not start recording on the same sample. Because
   they share one clock, the offset is a whole number of samples and stays
   fixed until the next start. At each start the robot plays a short chirp,
   compares when it reaches line A and line B against the delay expected
   from the speaker position, and shifts line B by the difference. The two
   mics on the same line are always aligned.
   Then `audio dir` prints the voice direction.
4. **Wake word.** microWakeWord with the new model, `ww` statistics command.
5. **Voice.** Gateway session, chime, playback, follow-up window, `voice ...`
   commands. Test: a full spoken conversation.
6. **Sound graphics.** Level-following screens for listening and speaking.
7. **Head servos.** `head` commands, smooth motion, nodding.
8. **IMU.** MPU6050 angles and calibration.
9. **Camera.** Camera running alongside Wi-Fi (the USB UVC stream can stay as
   a debug option), snapshot at session start for face recognition, face
   detection driving the head, then object following with a gateway tool.
10. **OTA.** GitHub firmware check for the P4.
