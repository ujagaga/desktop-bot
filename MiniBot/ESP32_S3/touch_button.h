#ifndef MINIBOT_TOUCH_BUTTON_H
#define MINIBOT_TOUCH_BUTTON_H

#include <Arduino.h>

// Load saved calibration; no automatic startup measurement.
void TOUCH_Init();
bool TOUCH_Calibrate(Print &output);
// Wait for touch, then report the peak raw reading after debounced release.
bool TOUCH_Measure(Print &output);
void TOUCH_Process();
// Refuse sleep while the pad is held, and arm hardware touch wakeup.
bool TOUCH_PrepareForSleep();
void TOUCH_AfterWake();

#endif
