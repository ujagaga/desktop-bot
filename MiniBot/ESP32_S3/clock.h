#ifndef CLOCK_H
#define CLOCK_H

#include <time.h>
#include <stddef.h>

// Starts NTP synchronization when Wi-Fi is connected.
void CLOCK_Process();

// Forces a fresh NTP synchronization attempt after waking from sleep.
void CLOCK_ResetSync();

// Gets the current local time in the Belgrade timezone.
bool CLOCK_GetLocalTime(struct tm &localTime);
bool CLOCK_GetTimeText(char *buffer, size_t bufferSize);

#endif
