#include <Arduino.h>
#include <ctype.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <WiFi.h>
#include <esp_sleep.h>
#include <driver/uart.h>
#include "comms.h"
#include "battery.h"
#include "wifi_connection.h"
#include "clock.h"
#include "lcd.h"
#include "motor.h"
#include "gyro.h"
#include "config.h"
#include "faces.h"


#define COMMS_BAUD 115200
#define GPIO_UART_RX 13
#define GPIO_UART_TX 14
#define MAX_LINE_LEN 128
#define MOTOR_ROTATE_TIMEOUT_MS 15000UL
#define MOTOR_ROTATE_OVERSHOOT_DEGREES 2.0f

struct PortState {
  Stream *stream;
  uint8_t rxBuffer[128];
  uint16_t rxHead = 0;
  uint16_t rxTail = 0;
  char lineBuf[MAX_LINE_LEN];
  size_t lineLen = 0;
};

static PortState usbPort = { &Serial };
static PortState gpioPort = { &Serial2 };

static void rxPush(PortState *port, uint8_t b) {
  uint16_t next = (port->rxHead + 1) % sizeof(port->rxBuffer);
  if (next == port->rxTail) return;
  port->rxBuffer[port->rxHead] = b;
  port->rxHead = next;
}

static bool rxPop(PortState *port, uint8_t *b) {
  if (port->rxTail == port->rxHead) return false;
  *b = port->rxBuffer[port->rxTail];
  port->rxTail = (port->rxTail + 1) % sizeof(port->rxBuffer);
  return true;
}

static bool cmdBacklight(Print *output, const char *args) {
  (void)output;
  LCD_BacklightSet(atoi(args));
  return true;
}

static bool cmdBattery(Print *output, const char *args) {
  char subcmd[4];
  if (sscanf(args, "%3s", subcmd) != 1) {
    output->println("ERR batt <c|v>");
    return false;
  }

  if (strcasecmp(subcmd, "c") == 0) {
    output->printf("%d\n", BATT_GetPercent());
    return true;
  }
  if (strcasecmp(subcmd, "v") == 0) {
    output->printf("%.2f\n", BATT_GetVoltage());
    return true;
  }

  output->println("ERR batt <c|v>");
  return false;
}

static bool cmdSleep(Print *output, const char *args) {
  (void)output;
  (void)args;
  MOTOR_StopAll();
  if (!GYRO_PrepareForSleep()) {
    GYRO_RestoreAfterSleep(false);
    output->println("ERR cannot configure motion wake; sleep cancelled");
    return false;
  }
  Serial.println("Sleeping until multiple taps or UART activity...");
  Serial.flush();

  LCD_BacklightOff();
  WIFI_PrepareForSleep();
  uart_set_wakeup_threshold(UART_NUM_0, 3);
  esp_sleep_enable_uart_wakeup(UART_NUM_0);
  esp_err_t sleepResult;
  bool gyroRestored;
  for (;;) {
    sleepResult = esp_light_sleep_start();
    bool motionWake = sleepResult == ESP_OK &&
                      esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_GPIO;
    gyroRestored = GYRO_RestoreAfterSleep(sleepResult == ESP_OK && !motionWake);
    if (!gyroRestored || !motionWake || GYRO_ConfirmTapWake()) break;
    // An isolated tap is not a user-visible wake: leave LCD/Wi-Fi off.
    if (!GYRO_PrepareForSleep()) {
      GYRO_RestoreAfterSleep(false);
      gyroRestored = false;
      break;
    }
  }
  LCD_BacklightRestore();
  CLOCK_ResetSync();
  WIFI_RestoreAfterSleep();
  if (!gyroRestored) {
    output->println("ERR gyro restore/calibration failed after sleep");
    return false;
  }
  if (sleepResult != ESP_OK) {
    output->println("ERR failed to enter light sleep");
    return false;
  }
  return true;
}

static bool cmdMotorMove(Print *output, const char *args) {
  int motorId, pwmPercent;
  unsigned long durationMs;
  char dir[8];
  if (sscanf(args, "%d %7s %d %lu", &motorId, dir, &pwmPercent, &durationMs) != 4 ||
      motorId < 0 || motorId > 2) {
    output->println("ERR motor move <0|1|2> <FWD|BACK> <pwm> <ms>");
    return false;
  }
  bool forward = strcasecmp(dir, "FWD") == 0;
  if (strcasecmp(dir, "FWD") != 0 && strcasecmp(dir, "BACK") != 0) {
    output->println("ERR motor move <0|1|2> <FWD|BACK> <pwm> <ms>");
    return false;
  }
  if (motorId == 0) {
    MOTOR_Run(1, forward, pwmPercent, durationMs);
    MOTOR_Run(2, forward, pwmPercent, durationMs);
  } else {
    MOTOR_Run(motorId, forward, pwmPercent, durationMs);
  }
  return true;
}

static bool cmdMotorRotate(Print *output, const char *args) {
  int pwmPercent;
  float targetDegrees;
  if (sscanf(args, "%d %f", &pwmPercent, &targetDegrees) != 2 ||
      pwmPercent < 0 || pwmPercent > 100 || targetDegrees <= 0.0f) {
    output->println("ERR motor rotate <pwm> <angle>");
    return false;
  }

  int axis = MOTOR_ROTATE_AXIS_INDEX;
  if (axis < 0 || axis > 2) {
    output->println("ERR motor rotate axis configuration");
    return false;
  }
  if (!GYRO_IsDetected()) {
    output->println("ERR gyro not detected");
    return false;
  }

  float startDegrees = GYRO_GetAngleDegrees(axis);
  unsigned long startMs = millis();
  MOTOR_Set(1, MOTOR_ROTATE_MOTOR1_FORWARD, pwmPercent);
  MOTOR_Set(2, MOTOR_ROTATE_MOTOR2_FORWARD, pwmPercent);

  bool correctingOvershoot = false;
  while (millis() - startMs < MOTOR_ROTATE_TIMEOUT_MS) {
    GYRO_Update();
    float deltaDegrees = GYRO_GetAngleDegrees(axis) - startDegrees;

    if (!correctingOvershoot && deltaDegrees >= targetDegrees) {
      if (deltaDegrees <= targetDegrees + MOTOR_ROTATE_OVERSHOOT_DEGREES) break;
      correctingOvershoot = true;
      MOTOR_Set(1, !MOTOR_ROTATE_MOTOR1_FORWARD, pwmPercent / 2);
      MOTOR_Set(2, !MOTOR_ROTATE_MOTOR2_FORWARD, pwmPercent / 2);
    } else if (correctingOvershoot && deltaDegrees <= targetDegrees) {
      break;
    }
    delay(5);
  }

  MOTOR_StopAll();
  if (millis() - startMs >= MOTOR_ROTATE_TIMEOUT_MS) {
    output->println("ERR motor rotate timeout");
    return false;
  }
  return true;
}

static bool cmdMotor(Print *output, const char *args) {
  char subcmd[16];
  const char *subargs = args;
  if (sscanf(args, "%15s", subcmd) != 1) {
    output->println("ERR motor <move>");
    return false;
  }
  while (*subargs && !isspace(*subargs)) subargs++;
  while (isspace(*subargs)) subargs++;

  if (strcasecmp(subcmd, "move") == 0) return cmdMotorMove(output, subargs);
  if (strcasecmp(subcmd, "rotate") == 0) return cmdMotorRotate(output, subargs);

  output->println("ERR motor <move|rotate>");
  return false;
}

static bool cmdLCDRotate(Print *output, const char *args) {
  int rotation;
  if (sscanf(args, "%d", &rotation) != 1 || rotation < 0 || rotation > 3) {
    output->println("ERR lcdrotate <0|1|2|3>");
    return false;
  }
  LCD_SetRotation(rotation);
  return true;
}

static bool cmdAngle(Print *output, const char *args) {
  char axisText[8];
  if (sscanf(args, "%7s", axisText) != 1) {
    output->println("ERR angle <x|y|z|0|1|2>");
    return false;
  }

  int axis = GYRO_AxisIndexFromText(axisText);
  if (axis < 0) {
    output->println("ERR angle <x|y|z|0|1|2>");
    return false;
  }
  if (!GYRO_IsDetected()) {
    output->println("ERR gyro not detected");
    return false;
  }

  output->printf("%.2f\n", GYRO_GetAngleDegrees(axis));
  return true;
}

static bool cmdRate(Print *output, const char *args) {
  char axisText[8];
  if (sscanf(args, "%7s", axisText) != 1) {
    output->println("ERR rate <x|y|z|0|1|2>");
    return false;
  }

  int axis = GYRO_AxisIndexFromText(axisText);
  if (axis < 0) {
    output->println("ERR rate <x|y|z|0|1|2>");
    return false;
  }
  if (!GYRO_IsDetected()) {
    output->println("ERR gyro not detected");
    return false;
  }

  output->printf("RATE %c = %.2f dps\n", "XYZ"[axis], GYRO_GetRateDps(axis));
  return true;
}

static bool cmdCalibrate(Print *output, const char *args) {
  (void)args;
  if (!GYRO_IsDetected()) {
    output->println("ERR gyro not detected");
    return false;
  }

  output->println("CALIBRATING keep gyro still...");
  float biasDps[3];
  if (!GYRO_Calibrate(biasDps)) {
    output->println("ERR gyro read failed during calibration");
    return false;
  }
  output->printf("BIAS X=%.2f Y=%.2f Z=%.2f dps\n",
                       biasDps[0], biasDps[1], biasDps[2]);
  return true;
}

static bool cmdGyroThreshold(Print *output, const char *args) {
  if (*args) {
    const char *end = args;
    unsigned int increment = 0;
    while (*end >= '0' && *end <= '9') {
      increment = increment * 10 + (*end++ - '0');
      if (increment > 12) break; // Bound accumulation before it can overflow.
    }
    bool hasDigits = end != args;
    while (isspace((unsigned char)*end)) ++end;
    if (!hasDigits || increment > 10 || *end) {
      output->println("ERR gyro threshold [0-10]");
      return false;
    }
    if ((unsigned)GYRO_WAKE_THRESHOLD_MG * increment > 255) {
      output->println("ERR threshold exceeds sensor maximum 255 mg");
      return false;
    }
    if (!GYRO_SetWakeThreshold(increment)) {
      output->println("ERR cannot save gyro threshold; unchanged");
      return false;
    }
  }
  output->printf("GYRO THRESHOLD %u mg (multiplier %u)\n",
                 (unsigned)GYRO_GetWakeThreshold(), (unsigned)GYRO_GetThresholdMultiplier());
  return true;
}

static bool cmdGyroTap(Print *output, const char *args) {
  char action[16];
  if (sscanf(args, "%15s", action) != 1 ||
      (strcasecmp(action, "wake") != 0 && strcasecmp(action, "sleep") != 0)) {
    output->println("ERR gyro tap <wake|sleep> [1-3]");
    return false;
  }
  bool wake = strcasecmp(action, "wake") == 0;
  while (*args && !isspace((unsigned char)*args)) ++args;
  while (isspace((unsigned char)*args)) ++args;
  if (*args) {
    char digit = *args++;
    while (isspace((unsigned char)*args)) ++args;
    if (digit < '1' || digit > '3' || *args) {
      output->println("ERR gyro tap <wake|sleep> [1-3]");
      return false;
    }
    if (!(wake ? GYRO_SetWakeTapThreshold(digit - '0')
               : GYRO_SetSleepTapThreshold(digit - '0'))) {
      output->println("ERR cannot save gyro tap count; unchanged");
      return false;
    }
  }
  unsigned value = wake ? GYRO_GetWakeTapThreshold() : GYRO_GetSleepTapThreshold();
  output->printf("GYRO TAP %s %u (requires %u or more taps)\n",
                 wake ? "WAKE" : "SLEEP", value, value);
  return true;
}

static bool cmdGyro(Print *output, const char *args) {
  char subcmd[16];
  const char *subargs = args;
  if (sscanf(args, "%15s", subcmd) != 1) {
    output->println("ERR gyro <angle|rate|calibrate|threshold|tap>");
    return false;
  }

  while (*subargs && !isspace(*subargs)) subargs++;
  while (isspace(*subargs)) subargs++;

  if (strcasecmp(subcmd, "tap") == 0) return cmdGyroTap(output, subargs);
  if (strcasecmp(subcmd, "threshold") == 0) return cmdGyroThreshold(output, subargs);
  if (strcasecmp(subcmd, "angle") == 0) return cmdAngle(output, subargs);
  if (strcasecmp(subcmd, "rate") == 0) return cmdRate(output, subargs);
  if (strcasecmp(subcmd, "calibrate") == 0) return cmdCalibrate(output, subargs);

  output->println("ERR gyro <angle|rate|calibrate|threshold|tap>");
  return false;
}

static bool getTimeText(char *timeText, size_t timeTextSize,
                        char *dateText, size_t dateTextSize) {
  static const char *weekdayNames[] = {
    "Sunday", "Monday", "Tuesday", "Wednesday",
    "Thursday", "Friday", "Saturday"
  };
  struct tm localTime;
  if (!CLOCK_GetLocalTime(localTime)) return false;

  snprintf(timeText, timeTextSize, "%02d:%02d", localTime.tm_hour, localTime.tm_min);
  snprintf(dateText, dateTextSize, "%s %02d.%02d.",
           weekdayNames[localTime.tm_wday], localTime.tm_mday, localTime.tm_mon + 1);
  return true;
}

static bool cmdTime(Print *output, const char *args) {
  (void)args;
  char timeText[6];
  char dateText[32];
  if (!getTimeText(timeText, sizeof(timeText), dateText, sizeof(dateText))) {
    output->println("ERR time unavailable");
    return false;
  }
  output->println(timeText);
  output->println(dateText);
  return true;
}

static bool cmdLCDTime(Print *output) {
  char timeText[6];
  char dateText[32];
  if (!getTimeText(timeText, sizeof(timeText), dateText, sizeof(dateText))) {
    output->println("ERR time unavailable");
    return false;
  }
  LCD_ShowTime(timeText, dateText);
  return true;
}

static bool cmdHelp(Print *output, const char *args) {
  (void)args;
  output->println("Commands:");
  output->println("  help");
  output->println("  batt c");
  output->println("  batt v");
  output->println("  gyro angle <x|y|z|0|1|2>");
  output->println("  gyro calibrate");
  output->println("  gyro tap sleep [1-3] (sleep at or above selected count)");
  output->println("  gyro tap wake [1-3] (wake at or above selected count)");
  output->println("  gyro threshold [0-12]");
  output->println("  gyro rate <x|y|z|0|1|2>");
  output->println("  lcd bl <0-100>");
  output->println("  lcd clear");
  output->println("  lcd color <bg|fg> <4 hex digits>");
  output->println("  lcd face <0-15>");
  output->println("  lcd rotate <0|1|2|3>");
  output->println("  lcd status");
  output->println("  lcd text <text>");
  output->println("  lcd time");
  output->println("  motor move <0|1|2> <FWD|BACK> <pwm> <ms>");
  output->println("  motor rotate <pwm> <angle>");
  output->println("  sleep");
  output->println("  time");
  output->println("  wifi clear");
  output->println("  wifi ip");
  output->println("  wifi off");
  output->println("  wifi on <ssid> <pass>");
  return true;
}

static bool cmdLCDClear(Print *output, const char *args) {
  (void)args;
  LCD_Clear();
  return true;
}

static bool cmdLCDColor(Print *output, const char *args) {
  char target[3], hex[5], extra[2];
  if (sscanf(args, "%2s %4s %1s", target, hex, extra) != 2 || strlen(hex) != 4 ||
      (strcasecmp(target, "bg") != 0 && strcasecmp(target, "fg") != 0)) {
    output->println("ERR lcd color <bg|fg> <4 hex digits>");
    return false;
  }
  uint16_t color = 0;
  for (int i = 0; i < 4; ++i) {
    unsigned char c = (unsigned char)tolower((unsigned char)hex[i]);
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
      output->println("ERR lcd color <bg|fg> <4 hex digits>");
      return false;
    }
    color = (color << 4) | (c <= '9' ? c - '0' : c - 'a' + 10);
  }
  if (!LCD_SetColor(strcasecmp(target, "bg") == 0, color)) {
    output->println("ERR cannot save LCD color; unchanged");
    return false;
  }
  if (!LCD_IsTextMode()) BATT_ShowStatus();
  return true;
}

static bool cmdLCD(Print *output, const char *args) {
  char subcmd[16];
  const char *subargs = args;
  if (sscanf(args, "%15s", subcmd) != 1) {
    output->println("ERR lcd <bl|clear|color|face|rotate|status|text|time>");
    return false;
  }

  while (*subargs && !isspace(*subargs)) subargs++;
  while (isspace(*subargs)) subargs++;

  if (strcasecmp(subcmd, "color") == 0) return cmdLCDColor(output, subargs);
  if (strcasecmp(subcmd, "bl") == 0) return cmdBacklight(output, subargs);
  if (strcasecmp(subcmd, "rotate") == 0) return cmdLCDRotate(output, subargs);
  if (strcasecmp(subcmd, "clear") == 0) return cmdLCDClear(output, subargs);
  if (strcasecmp(subcmd, "status") == 0) {
    LCD_Clear();
    BATT_ShowStatus();
    return true;
  }
  if (strcasecmp(subcmd, "text") == 0) {
    LCD_ShowText(subargs);
    return true;
  }
  if (strcasecmp(subcmd, "time") == 0) {
    return cmdLCDTime(output);
  }
  if (strcasecmp(subcmd, "face") == 0) {
    int faceId;
    if (sscanf(subargs, "%d", &faceId) != 1 || !FACE_Show(faceId)) {
      output->println("ERR lcd face <0-15>");
      return false;
    }
    return true;
  }

  output->println("ERR lcd <bl|clear|color|face|rotate|status|text|time>");
  return false;
}

static bool cmdWifi(Print *output, const char *args) {
  char subcmd[16];
  if (sscanf(args, "%15s", subcmd) != 1) {
    output->println("ERR wifi <clear|ip|off|on>");
    return false;
  }

  if (strcasecmp(subcmd, "on") == 0) {
    char ssid[33];
    char pass[64];
    if (sscanf(args, "%*s %32s %63s", ssid, pass) != 2) {
      output->println("ERR wifi on <ssid> <pass>");
      return false;
    }
    if (!WIFI_Connect(ssid, pass, true)) {
      output->println("ERR wifi connect failed");
      return false;
    }
    return true;
  }

  if (strcasecmp(subcmd, "off") == 0) {
    WIFI_Disconnect();
    return true;
  }

  if (strcasecmp(subcmd, "clear") == 0) {
    WIFI_Disconnect();
    WIFI_ClearStoredCredentials();
    return true;
  }

  if (strcasecmp(subcmd, "ip") == 0) {
    output->println(WiFi.status() == WL_CONNECTED ? WiFi.localIP() : IPAddress(0, 0, 0, 0));
    return true;
  }

  output->println("ERR wifi <clear|ip|off|on>");
  return false;
}

typedef bool (*CommandHandler)(Print *output, const char *args);

struct CommandEntry {
  const char *name;
  CommandHandler handler;
};

static const CommandEntry commandMap[] = {
  { "help", cmdHelp },
  { "bat", cmdBattery },
  { "batt", cmdBattery },
  { "gyro", cmdGyro },
  { "lcd", cmdLCD },
  { "motor", cmdMotor },
  { "sleep", cmdSleep },
  { "time", cmdTime },
  { "wifi", cmdWifi },
};

static void dispatch(Print *output, char *line) {
  char *args = line;
  while (*args && !isspace(*args)) args++;
  if (*args) *args++ = '\0';
  while (isspace(*args)) args++;

  for (size_t i = 0; i < sizeof(commandMap) / sizeof(commandMap[0]); i++) {
    if (strcasecmp(commandMap[i].name, line) == 0) {
      if (commandMap[i].handler(output, args)) output->println("OK");
      return;
    }
  }
  output->print("ERR unknown command: ");
  output->println(line);
}

void COMMS_Execute(const char *command, Print &output) {
  if (!command || strlen(command) >= MAX_LINE_LEN) {
    output.println("ERR command too long (maximum 127 bytes)");
    return;
  }
  if (strchr(command, '\r') || strchr(command, '\n')) {
    output.println("ERR expected one command line");
    return;
  }
  char line[MAX_LINE_LEN];
  strcpy(line, command);
  if (*line) dispatch(&output, line);
}

static void pollPort(PortState *port) {
  while (port->stream->available()) {
    uint8_t b = (uint8_t)port->stream->read();
    if (b == '\r') continue;
    rxPush(port, b);
  }

  uint8_t b;
  while (rxPop(port, &b)) {
    if (b == '\n') {
      port->lineBuf[port->lineLen] = '\0';
      if (port->lineLen > 0) COMMS_Execute(port->lineBuf, *port->stream);
      port->lineLen = 0;
    } else if (port->lineLen < MAX_LINE_LEN - 1) {
      port->lineBuf[port->lineLen++] = (char)b;
    } else {
      port->lineLen = 0;
    }
  }
}

void COMMS_Init() {
  Serial.begin(COMMS_BAUD);
  Serial2.begin(COMMS_BAUD, SERIAL_8N1, GPIO_UART_RX, GPIO_UART_TX);
  GYRO_Init();
}

void COMMS_Poll() {
  GYRO_Update();
  pollPort(&usbPort);
  pollPort(&gpioPort);
}
