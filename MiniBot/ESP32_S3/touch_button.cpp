#include <Arduino.h>
#include <Preferences.h>
#include "motor.h"
#include "gyro.h"
#include <esp_sleep.h>
#include <esp_idf_version.h>
#include "touch_button.h"
#include "touch_button_state.h"
#include "config.h"
#include "comms.h"

static TouchButtonState button;
static bool ready = false;
static bool pressed = false;
static uint32_t baseline = 0;
static uint32_t lastSample = 0;

static uint32_t touchDelta() {
  return max(1UL, (unsigned long)((uint64_t)baseline * TOUCH_THRESHOLD_PERCENT / 100));
}

static bool readPressed(bool &value) {
  uint32_t reading = touchRead(TOUCH_GPIO);
  if (!reading) return false;
  // Release at half the activation delta to avoid chatter near the threshold.
  uint32_t delta = touchDelta();
  pressed = reading > baseline + (pressed ? delta / 2 : delta);
  value = pressed;
  return true;
}

static void resetTracking() {
  pressed = false;
  button.reset();
  lastSample = millis();
}

void TOUCH_Init() {
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
  touchSetDefaultThreshold(TOUCH_THRESHOLD_PERCENT);
#endif
  ready = false;
  resetTracking();
  Preferences prefs;
  uint64_t saved = 0;
  if (prefs.begin("miniBotTouch", true)) {
    saved = prefs.getULong64("calibration", 0);
    prefs.end();
  }
  // Store pin and baseline in a single NVS value so they cannot get out of sync.
  uint32_t savedBaseline = (uint32_t)saved;
  if ((saved >> 32) != TOUCH_GPIO || savedBaseline == 0 ||
      savedBaseline > UINT32_MAX / 2) {
    Serial.println("Touch: no saved calibration; run touch calibrate with pad untouched");
    return;
  }
  baseline = savedBaseline;
  ready = true;
  Serial.printf("Touch: loaded GPIO%d baseline %lu\n", TOUCH_GPIO, (unsigned long)baseline);
}

bool TOUCH_Calibrate(Print &output) {
  // Sampling blocks loop(), including timed motor stopping.
  MOTOR_StopAll();
  output.println("Touch: leave pad untouched; calibrating in 2 seconds...");
  delay(2000);
  touchRead(TOUCH_GPIO);
  delay(100);
  uint64_t total = 0;
  uint32_t lowest = UINT32_MAX, highest = 0;
  for (int i = 0; i < 50; ++i) {
    uint32_t value = touchRead(TOUCH_GPIO);
    if (!value || value > UINT32_MAX / 2) {
      resetTracking();
      output.println("ERR touch read failed; calibration unchanged");
      return false;
    }
    if (value < lowest) lowest = value;
    if (value > highest) highest = value;
    total += value;
    delay(10);
  }
  uint32_t measured = total / 50;
  if ((uint64_t)(highest - lowest) * 100 > (uint64_t)measured * 10) {
    resetTracking();
    output.println("ERR touch readings unstable; calibration unchanged");
    return false;
  }
  Preferences prefs;
  if (!prefs.begin("miniBotTouch", false)) {
    resetTracking();
    output.println("ERR cannot open touch preferences; calibration unchanged");
    return false;
  }
  uint64_t saved = ((uint64_t)TOUCH_GPIO << 32) | measured;
  bool stored = prefs.putULong64("calibration", saved) == sizeof(saved);
  prefs.end();
  resetTracking();
  if (!stored) {
    output.println("ERR cannot save touch calibration; calibration unchanged");
    return false;
  }
  baseline = measured;
  ready = true;
  output.printf("Touch: saved GPIO%d baseline %lu, activation %lu\n",
                TOUCH_GPIO, (unsigned long)baseline,
                (unsigned long)(baseline + touchDelta()));
  return true;
}

bool TOUCH_Measure(Print &output) {
  if (!ready) {
    output.println("ERR run touch calibrate first");
    return false;
  }
  // This command owns the pad until release; a long measurement must not sleep.
  // Stop motors because command dispatch blocks the main loop.
  MOTOR_StopAll();
  resetTracking();
  const uint32_t startedAt = millis();
  uint32_t peak = 0, releasedAt = 0;
  bool touched = false, releasing = false;
  for (;;) {
    uint32_t reading = touchRead(TOUCH_GPIO);
    uint32_t now = millis();
    if (!reading) {
      resetTracking();
      output.println("ERR touch read failed");
      return false;
    }
    if (!touched) {
      if (reading > baseline + touchDelta()) {
        touched = true;
        peak = reading;
      } else if (now - startedAt >= 30000UL) {
        resetTracking();
        output.println("ERR touch measure timed out waiting for touch");
        return false;
      }
    } else {
      if (reading > peak) peak = reading;
      if (reading <= baseline + touchDelta() / 2) {
        if (!releasing) {
          releasing = true;
          releasedAt = now;
        }
        if (now - releasedAt >= 60) {
          resetTracking();
          output.printf("TOUCH MAX %lu\n", (unsigned long)peak);
          return true;
        }
      } else {
        releasing = false;
      }
    }
    GYRO_Update();
    delay(10);
  }
}

void TOUCH_Process() {
  uint32_t now = millis();
  if (!ready || now - lastSample < 20) return;
  // A blocked main loop cannot establish a continuous hold. Start over after
  // long commands/OTA instead of treating an observation gap as a long press.
  if (now - lastSample > 250) button.reset();
  lastSample = now;
  bool value;
  if (!readPressed(value)) {
    button.reset();
    return;
  }
  if (button.update(value, now, TOUCH_HOLD_MS)) {
    button.reset();
    COMMS_Execute("sleep", Serial);
  }
}

bool TOUCH_PrepareForSleep() {
  if (!ready) return false;
  bool value;
  if (!readPressed(value) || value) return false;
  // S3 hardware thresholds are deltas above its benchmark, not raw readings.
  touchSleepWakeUpEnable(TOUCH_GPIO, touchDelta());
  return esp_sleep_enable_touchpad_wakeup() == ESP_OK;
}

void TOUCH_AfterWake() {
  button.reset();
  lastSample = millis();
}
