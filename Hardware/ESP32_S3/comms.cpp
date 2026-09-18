#include <Arduino.h>
#include <ctype.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <esp_sleep.h>
#include <driver/uart.h>
#include "comms.h"
#include "LCD.h"
#include "motor.h"

#define COMMS_BAUD 115200

// Commands are ASCII lines terminated by '\n', case-insensitive, e.g.
// "bl 10" sets the backlight to 10%, "MOTOR 1 FWD 50 500" runs motor 1
// forward at 50% PWM for 500ms.
#define MAX_LINE_LEN 32

static uint8_t rxBuffer[128];
static uint16_t rxHead = 0;
static uint16_t rxTail = 0;

static void rxPush(uint8_t b) {
  uint16_t next = (rxHead + 1) % sizeof(rxBuffer);
  if (next == rxTail) return;  // buffer full, drop byte
  rxBuffer[rxHead] = b;
  rxHead = next;
}

static bool rxPop(uint8_t *b) {
  if (rxTail == rxHead) return false;
  *b = rxBuffer[rxTail];
  rxTail = (rxTail + 1) % sizeof(rxBuffer);
  return true;
}

static void cmdBacklight(const char *args) {
  setBacklightPercent(atoi(args));
}

// UART wakeup only works from light sleep (deep sleep powers the UART off
// entirely, so bytes sent while asleep would just be lost). The wakeup
// threshold is the minimum number of RX edges to trigger it; 3 is the
// hardware minimum on the S3.
static void cmdSleep(const char *args) {
  (void)args;
  Serial.println("Sleeping until UART activity...");
  Serial.flush();

  turnOffBacklight();
  uart_set_wakeup_threshold(UART_NUM_0, 3);
  esp_sleep_enable_uart_wakeup(UART_NUM_0);
  esp_light_sleep_start();
  restoreBacklight();
}

// "MOTOR <1|2> <FWD|BACK> <pwm%> <ms>"
static void cmdMotor(const char *args) {
  int motorId, pwmPercent;
  unsigned long durationMs;
  char dir[8];
  if (sscanf(args, "%d %7s %d %lu", &motorId, dir, &pwmPercent, &durationMs) != 4) return;
  motorRun(motorId, strcasecmp(dir, "FWD") == 0, pwmPercent, durationMs);
}

typedef void (*CommandHandler)(const char *args);

struct CommandEntry {
  const char *name;
  CommandHandler handler;
};

static const CommandEntry commandMap[] = {
  { "bl", cmdBacklight },
  { "sleep", cmdSleep },
  { "motor", cmdMotor },
};

static void dispatch(char *line) {
  char *args = line;
  while (*args && !isspace(*args)) args++;
  if (*args) *args++ = '\0';
  while (isspace(*args)) args++;

  for (size_t i = 0; i < sizeof(commandMap) / sizeof(commandMap[0]); i++) {
    if (strcasecmp(commandMap[i].name, line) == 0) {
      commandMap[i].handler(args);
      Serial.println("OK");
      return;
    }
  }
  Serial.print("ERR unknown command: ");
  Serial.println(line);
}

static char lineBuf[MAX_LINE_LEN];
static size_t lineLen = 0;

void initComms() {
  Serial.begin(COMMS_BAUD);
}

void commsPoll() {
  while (Serial.available()) {
    rxPush(Serial.read());
  }

  uint8_t b;
  while (rxPop(&b)) {
    if (b == '\n') {
      lineBuf[lineLen] = '\0';
      if (lineLen > 0) dispatch(lineBuf);
      lineLen = 0;
    } else if (lineLen < MAX_LINE_LEN - 1) {
      lineBuf[lineLen++] = (char)b;
    } else {
      lineLen = 0;  // line too long, drop it
    }
  }
}
