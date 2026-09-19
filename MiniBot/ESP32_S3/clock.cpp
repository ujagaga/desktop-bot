#include <Arduino.h>
#include <WiFi.h>
#include <time.h>
#include "clock.h"

#define CLOCK_TZ "CET-1CEST,M3.5.0,M10.5.0"

static bool timeSyncStarted = false;

void CLOCK_Process() {
  if (WiFi.status() == WL_CONNECTED && !timeSyncStarted) {
    configTzTime(CLOCK_TZ, "pool.ntp.org", "time.nist.gov");
    timeSyncStarted = true;
  }
}

void CLOCK_ResetSync() {
  timeSyncStarted = false;
}

bool CLOCK_GetLocalTime(struct tm &localTime) {
  CLOCK_Process();
  if (!getLocalTime(&localTime, 100)) return false;
  return localTime.tm_year >= 124;
}

bool CLOCK_GetTimeText(char *buffer, size_t bufferSize) {
  struct tm localTime;
  if (buffer == nullptr || bufferSize < 6 || !CLOCK_GetLocalTime(localTime)) return false;
  snprintf(buffer, bufferSize, "%02d:%02d", localTime.tm_hour, localTime.tm_min);
  return true;
}
