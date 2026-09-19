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
#include "LCD.h"
#include "motor.h"
#include "gyro.h"
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

static bool cmdBacklight(PortState *port, const char *args) {
  (void)port;
  LCD_BacklightSet(atoi(args));
  return true;
}

static bool cmdBattery(PortState *port, const char *args) {
  char subcmd[4];
  if (sscanf(args, "%3s", subcmd) != 1) {
    port->stream->println("ERR batt <c|v>");
    return false;
  }

  if (strcasecmp(subcmd, "c") == 0) {
    port->stream->printf("%d\n", BATT_GetPercent());
    return true;
  }
  if (strcasecmp(subcmd, "v") == 0) {
    port->stream->printf("%.2f\n", BATT_GetVoltage());
    return true;
  }

  port->stream->println("ERR batt <c|v>");
  return false;
}

static bool cmdSleep(PortState *port, const char *args) {
  (void)port;
  (void)args;
  Serial.println("Sleeping until UART activity...");
  Serial.flush();

  LCD_BacklightOff();
  WIFI_PrepareForSleep();
  uart_set_wakeup_threshold(UART_NUM_0, 3);
  esp_sleep_enable_uart_wakeup(UART_NUM_0);
  esp_light_sleep_start();
  LCD_BacklightRestore();
  CLOCK_ResetSync();
  GYRO_Init();
  WIFI_RestoreAfterSleep();
  return true;
}

static bool cmdMotorMove(PortState *port, const char *args) {
  int motorId, pwmPercent;
  unsigned long durationMs;
  char dir[8];
  if (sscanf(args, "%d %7s %d %lu", &motorId, dir, &pwmPercent, &durationMs) != 4 ||
      motorId < 0 || motorId > 2) {
    port->stream->println("ERR motor move <0|1|2> <FWD|BACK> <pwm> <ms>");
    return false;
  }
  bool forward = strcasecmp(dir, "FWD") == 0;
  if (strcasecmp(dir, "FWD") != 0 && strcasecmp(dir, "BACK") != 0) {
    port->stream->println("ERR motor move <0|1|2> <FWD|BACK> <pwm> <ms>");
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

static bool cmdMotorRotate(PortState *port, const char *args) {
  int pwmPercent;
  float targetDegrees;
  if (sscanf(args, "%d %f", &pwmPercent, &targetDegrees) != 2 ||
      pwmPercent < 0 || pwmPercent > 100 || targetDegrees <= 0.0f) {
    port->stream->println("ERR motor rotate <pwm> <angle>");
    return false;
  }

  int axis = MOTOR_ROTATE_AXIS_INDEX;
  if (axis < 0 || axis > 2) {
    port->stream->println("ERR motor rotate axis configuration");
    return false;
  }
  if (!GYRO_IsDetected()) {
    port->stream->println("ERR gyro not detected");
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
    port->stream->println("ERR motor rotate timeout");
    return false;
  }
  return true;
}

static bool cmdMotor(PortState *port, const char *args) {
  char subcmd[16];
  const char *subargs = args;
  if (sscanf(args, "%15s", subcmd) != 1) {
    port->stream->println("ERR motor <move>");
    return false;
  }
  while (*subargs && !isspace(*subargs)) subargs++;
  while (isspace(*subargs)) subargs++;

  if (strcasecmp(subcmd, "move") == 0) return cmdMotorMove(port, subargs);
  if (strcasecmp(subcmd, "rotate") == 0) return cmdMotorRotate(port, subargs);

  port->stream->println("ERR motor <move|rotate>");
  return false;
}

static bool cmdLCDRotate(PortState *port, const char *args) {
  int rotation;
  if (sscanf(args, "%d", &rotation) != 1 || rotation < 0 || rotation > 3) {
    port->stream->println("ERR lcdrotate <0|1|2|3>");
    return false;
  }
  LCD_SetRotation(rotation);
  return true;
}

static bool cmdAngle(PortState *port, const char *args) {
  char axisText[8];
  if (sscanf(args, "%7s", axisText) != 1) {
    port->stream->println("ERR angle <x|y|z|0|1|2>");
    return false;
  }

  int axis = GYRO_AxisIndexFromText(axisText);
  if (axis < 0) {
    port->stream->println("ERR angle <x|y|z|0|1|2>");
    return false;
  }
  if (!GYRO_IsDetected()) {
    port->stream->println("ERR gyro not detected");
    return false;
  }

  port->stream->printf("%.2f\n", GYRO_GetAngleDegrees(axis));
  return true;
}

static bool cmdRate(PortState *port, const char *args) {
  char axisText[8];
  if (sscanf(args, "%7s", axisText) != 1) {
    port->stream->println("ERR rate <x|y|z|0|1|2>");
    return false;
  }

  int axis = GYRO_AxisIndexFromText(axisText);
  if (axis < 0) {
    port->stream->println("ERR rate <x|y|z|0|1|2>");
    return false;
  }
  if (!GYRO_IsDetected()) {
    port->stream->println("ERR gyro not detected");
    return false;
  }

  port->stream->printf("RATE %c = %.2f dps\n", "XYZ"[axis], GYRO_GetRateDps(axis));
  return true;
}

static bool cmdCalibrate(PortState *port, const char *args) {
  (void)args;
  if (!GYRO_IsDetected()) {
    port->stream->println("ERR gyro not detected");
    return false;
  }

  port->stream->println("CALIBRATING keep gyro still...");
  float biasDps[3];
  if (!GYRO_Calibrate(biasDps)) {
    port->stream->println("ERR gyro read failed during calibration");
    return false;
  }
  port->stream->printf("BIAS X=%.2f Y=%.2f Z=%.2f dps\n",
                       biasDps[0], biasDps[1], biasDps[2]);
  return true;
}

static bool cmdGyro(PortState *port, const char *args) {
  char subcmd[16];
  const char *subargs = args;
  if (sscanf(args, "%15s", subcmd) != 1) {
    port->stream->println("ERR gyro <angle|rate|calibrate>");
    return false;
  }

  while (*subargs && !isspace(*subargs)) subargs++;
  while (isspace(*subargs)) subargs++;

  if (strcasecmp(subcmd, "angle") == 0) return cmdAngle(port, subargs);
  if (strcasecmp(subcmd, "rate") == 0) return cmdRate(port, subargs);
  if (strcasecmp(subcmd, "calibrate") == 0) return cmdCalibrate(port, subargs);

  port->stream->println("ERR gyro <angle|rate|calibrate>");
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

static bool cmdTime(PortState *port, const char *args) {
  (void)args;
  char timeText[6];
  char dateText[32];
  if (!getTimeText(timeText, sizeof(timeText), dateText, sizeof(dateText))) {
    port->stream->println("ERR time unavailable");
    return false;
  }
  port->stream->println(timeText);
  port->stream->println(dateText);
  return true;
}

static bool cmdLCDTime(PortState *port) {
  char timeText[6];
  char dateText[32];
  if (!getTimeText(timeText, sizeof(timeText), dateText, sizeof(dateText))) {
    port->stream->println("ERR time unavailable");
    return false;
  }
  LCD_ShowTime(timeText, dateText);
  return true;
}

static bool cmdHelp(PortState *port, const char *args) {
  (void)args;
  port->stream->println("Commands:");
  port->stream->println("  help");
  port->stream->println("  batt c");
  port->stream->println("  batt v");
  port->stream->println("  gyro angle <x|y|z|0|1|2>");
  port->stream->println("  gyro calibrate");
  port->stream->println("  gyro rate <x|y|z|0|1|2>");
  port->stream->println("  lcd bl <0-100>");
  port->stream->println("  lcd clear");
  port->stream->println("  lcd face <0-15>");
  port->stream->println("  lcd rotate <0|1|2|3>");
  port->stream->println("  lcd status");
  port->stream->println("  lcd text <text>");
  port->stream->println("  lcd time");
  port->stream->println("  motor move <0|1|2> <FWD|BACK> <pwm> <ms>");
  port->stream->println("  motor rotate <pwm> <angle>");
  port->stream->println("  sleep");
  port->stream->println("  time");
  port->stream->println("  wifi clear");
  port->stream->println("  wifi ip");
  port->stream->println("  wifi off");
  port->stream->println("  wifi on <ssid> <pass>");
  return true;
}

static bool cmdLCDClear(PortState *port, const char *args) {
  (void)args;
  LCD_Clear();
  return true;
}

static bool cmdLCD(PortState *port, const char *args) {
  char subcmd[16];
  const char *subargs = args;
  if (sscanf(args, "%15s", subcmd) != 1) {
    port->stream->println("ERR lcd <bl|clear|face|rotate|status|text|time>");
    return false;
  }

  while (*subargs && !isspace(*subargs)) subargs++;
  while (isspace(*subargs)) subargs++;

  if (strcasecmp(subcmd, "bl") == 0) return cmdBacklight(port, subargs);
  if (strcasecmp(subcmd, "rotate") == 0) return cmdLCDRotate(port, subargs);
  if (strcasecmp(subcmd, "clear") == 0) return cmdLCDClear(port, subargs);
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
    return cmdLCDTime(port);
  }
  if (strcasecmp(subcmd, "face") == 0) {
    int faceId;
    if (sscanf(subargs, "%d", &faceId) != 1 || !FACE_Show(faceId)) {
      port->stream->println("ERR lcd face <0-15>");
      return false;
    }
    return true;
  }

  port->stream->println("ERR lcd <bl|clear|face|rotate|status|text|time>");
  return false;
}

static bool cmdWifi(PortState *port, const char *args) {
  char subcmd[16];
  if (sscanf(args, "%15s", subcmd) != 1) {
    port->stream->println("ERR wifi <clear|ip|off|on>");
    return false;
  }

  if (strcasecmp(subcmd, "on") == 0) {
    char ssid[33];
    char pass[64];
    if (sscanf(args, "%*s %32s %63s", ssid, pass) != 2) {
      port->stream->println("ERR wifi on <ssid> <pass>");
      return false;
    }
    if (!WIFI_Connect(ssid, pass, true)) {
      port->stream->println("ERR wifi connect failed");
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
    port->stream->println(WiFi.status() == WL_CONNECTED ? WiFi.localIP() : IPAddress(0, 0, 0, 0));
    return true;
  }

  port->stream->println("ERR wifi <clear|ip|off|on>");
  return false;
}

typedef bool (*CommandHandler)(PortState *port, const char *args);

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

static void dispatch(PortState *port, char *line) {
  char *args = line;
  while (*args && !isspace(*args)) args++;
  if (*args) *args++ = '\0';
  while (isspace(*args)) args++;

  for (size_t i = 0; i < sizeof(commandMap) / sizeof(commandMap[0]); i++) {
    if (strcasecmp(commandMap[i].name, line) == 0) {
      if (commandMap[i].handler(port, args)) port->stream->println("OK");
      return;
    }
  }
  port->stream->print("ERR unknown command: ");
  port->stream->println(line);
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
      if (port->lineLen > 0) dispatch(port, port->lineBuf);
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
