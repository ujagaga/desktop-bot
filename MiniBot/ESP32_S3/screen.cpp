#include "screen.h"

#include <WiFi.h>
#include <stdio.h>

#include "battery.h"
#include "faces.h"
#include "lcd.h"
#include "voice.h"

namespace {
constexpr unsigned long REFRESH_MS = 2000;
constexpr unsigned long STATUS_AFTER_IP_MS = 2000;
constexpr int LOW_BATTERY_PERCENT = 15;
constexpr int LOW_BATTERY_CLEAR_PERCENT = 20;

enum Problem { NONE, LOW_BATTERY, NO_WIFI, NO_SERVER };

bool wakePhase = true;
unsigned long ipSinceMs = 0;
unsigned long lastRefreshMs = 0;
bool lowBattery = false;
int shownProblem = -1;

int currentProblem() {
  int percent = BATT_GetPercent();
  if (BATT_IsCharging()) lowBattery = false;
  else lowBattery = percent < (lowBattery ? LOW_BATTERY_CLEAR_PERCENT : LOW_BATTERY_PERCENT);
  if (lowBattery) return LOW_BATTERY;
  // An explicitly disabled radio (`wifi off`) is a choice, not an error.
  if (!(WiFi.getMode() & WIFI_STA)) return NONE;
  if (WiFi.status() != WL_CONNECTED) return NO_WIFI;
  return VOICE_ServerOk() ? NONE : NO_SERVER;
}

void show(int problem) {
  if (problem == NONE) {
    FACE_Show(0);  // 00_neutral
    return;
  }
  FACE_Show(2);  // 02_sad
  char text[24];
  if (problem == LOW_BATTERY) snprintf(text, sizeof(text), "LOW BATTERY %.2fV", BATT_GetVoltage());
  else snprintf(text, sizeof(text), "%s", problem == NO_WIFI ? "NO WI-FI" : "NO SERVER");
  LCD_ShowFooter(text);
}
}

void SCREEN_StartWake() {
  wakePhase = true;
  ipSinceMs = 0;
  LCD_Clear();
  BATT_ShowStatus();
}

void SCREEN_Process() {
  unsigned long now = millis();
  if (now - lastRefreshMs < REFRESH_MS) return;
  lastRefreshMs = now;
  // Keep the status screen live while it is visible (wake phase or `lcd status`).
  if (!LCD_IsTextMode()) BATT_ShowStatus();
  if (VOICE_IsBusy()) return;

  if (wakePhase) {
    if ((WiFi.getMode() & WIFI_STA) && WiFi.status() != WL_CONNECTED) {
      ipSinceMs = 0;
      return;
    }
    if (!ipSinceMs) ipSinceMs = now;
    if (now - ipSinceMs < STATUS_AFTER_IP_MS) return;
    wakePhase = false;
    shownProblem = -1;
  }
  int problem = currentProblem();
  if (problem != shownProblem) {
    shownProblem = problem;
    show(problem);
  }
}

void SCREEN_Refresh() {
  if (wakePhase) {
    LCD_Clear();
    BATT_ShowStatus();
  } else {
    show(shownProblem < 0 ? NONE : shownProblem);
  }
}
