#include <Arduino.h>
#include <ctype.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <esp_sleep.h>
#include <driver/uart.h>
#include "comms.h"
#include "wifi_connection.h"
#include "LCD.h"
#include "motor.h"

#define COMMS_BAUD 115200
#define GPIO_UART_RX 13
#define GPIO_UART_TX 14

// Commands are ASCII lines terminated by '\n', case-insensitive, e.g.
// "bl 10" sets the backlight to 10%, "MOTOR 1 FWD 50 500" runs motor 1
// forward at 50% PWM for 500ms.
#define MAX_LINE_LEN 32

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
  if (next == port->rxTail) return;  // buffer full, drop byte
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

// UART wakeup only works from light sleep (deep sleep powers the UART off
// entirely, so bytes sent while asleep would just be lost). The wakeup
// threshold is the minimum number of RX edges to trigger it; 3 is the
// hardware minimum on the S3.
static bool cmdSleep(PortState *port, const char *args) {
  (void)port;
  (void)args;
  Serial.println("Sleeping until UART activity...");
  Serial.flush();

  LCD_BacklightOff();
  uart_set_wakeup_threshold(UART_NUM_0, 3);
  esp_sleep_enable_uart_wakeup(UART_NUM_0);
  esp_light_sleep_start();
  LCD_BacklightRestore();
  return true;
}

// "MOTOR <1|2> <FWD|BACK> <pwm%> <ms>"
static bool cmdMotor(PortState *port, const char *args) {
  int motorId, pwmPercent;
  unsigned long durationMs;
  char dir[8];
  if (sscanf(args, "%d %7s %d %lu", &motorId, dir, &pwmPercent, &durationMs) != 4) {
    port->stream->println("ERR motor <id> <FWD|BACK> <pwm> <ms>");
    return false;
  }
  MOTOR_Run(motorId, strcasecmp(dir, "FWD") == 0, pwmPercent, durationMs);
  return true;
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

static bool cmdHelp(PortState *port, const char *args) {
  (void)args;
  port->stream->println("Commands:");
  port->stream->println("  bl <0-100>");
  port->stream->println("  sleep");
  port->stream->println("  motor <id> <FWD|BACK> <pwm> <ms>");
  port->stream->println("  lcdrotate <0|1|2|3>");
  port->stream->println("  wifi <on|off|clear>");
  port->stream->println("  help");
  return true;
}

static bool cmdWifi(PortState *port, const char *args) {
  char subcmd[16];
  if (sscanf(args, "%15s", subcmd) != 1) {
    port->stream->println("ERR wifi <on|off|clear>");
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

  port->stream->println("ERR wifi <on|off|clear>");
  return false;
}

typedef bool (*CommandHandler)(PortState *port, const char *args);

struct CommandEntry {
  const char *name;
  CommandHandler handler;
};

static const CommandEntry commandMap[] = {
  { "bl", cmdBacklight },
  { "sleep", cmdSleep },
  { "motor", cmdMotor },
  { "lcdrotate", cmdLCDRotate },
  { "wifi", cmdWifi },
  { "help", cmdHelp },
};

static void dispatch(PortState *port, char *line) {
  char *args = line;
  while (*args && !isspace(*args)) args++;
  if (*args) *args++ = '\0';
  while (isspace(*args)) args++;

  for (size_t i = 0; i < sizeof(commandMap) / sizeof(commandMap[0]); i++) {
    if (strcasecmp(commandMap[i].name, line) == 0) {
      if (commandMap[i].handler(port, args)) {
        port->stream->println("OK");
      }
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
      port->lineLen = 0;  // line too long, drop it
    }
  }
}

void COMMS_Init() {
  Serial.begin(COMMS_BAUD);
  Serial2.begin(COMMS_BAUD, SERIAL_8N1, GPIO_UART_RX, GPIO_UART_TX);
}

void COMMS_Poll() {
  pollPort(&usbPort);
  pollPort(&gpioPort);
}
