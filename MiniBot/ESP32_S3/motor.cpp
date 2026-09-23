#include <Arduino.h>
#include <Preferences.h>
#include "motor.h"

// H-bridge inputs: motor 1 = GP9/GP10, motor 2 = GP11/GP12. Each pin is
// PWM'd directly (no separate enable pin): driving pin A gives forward,
// driving pin B gives reverse, with the other pin held at 0.
#define MOTOR_PWM_FREQ 5000
#define MOTOR_PWM_RES_BITS 8

struct MotorPins {
  uint8_t pinA;
  uint8_t pinB;
};

static const MotorPins motorPins[2] = {
  { 9, 10 },
  { 12, 11 },
};

struct MotorState {
  unsigned long startAt;
  unsigned long durationMs;
  bool running;
};

static MotorState motors[2];
static uint8_t calibrationPercent[2] = {100, 100};
static const char *calibrationKeys[2] = {"motor1", "motor2"};

static uint32_t calibratedDuty(int motor, int requestedPercent) {
  requestedPercent = constrain(requestedPercent, 0, 100);
  // Scale before dividing to preserve the available 8-bit PWM resolution.
  return ((1U << MOTOR_PWM_RES_BITS) - 1) * requestedPercent *
         calibrationPercent[motor - 1] / 10000U;
}

bool MOTOR_Calibrate(int motor, int percent) {
  if (motor < 1 || motor > 2 || percent < 0 || percent > 100) return false;
  Preferences prefs;
  if (!prefs.begin("motorCal", false)) return false;
  const char *key = calibrationKeys[motor - 1];
  bool saved = prefs.getUChar(key, 100) == percent ||
               prefs.putUChar(key, (uint8_t)percent) == sizeof(uint8_t);
  prefs.end();
  if (saved) calibrationPercent[motor - 1] = (uint8_t)percent;
  return saved;
}

int MOTOR_GetCalibration(int motor) {
  if (motor < 1 || motor > 2) return -1;
  return calibrationPercent[motor - 1];
}

void MOTOR_Init() {
  Preferences prefs;
  calibrationPercent[0] = calibrationPercent[1] = 100;
  if (prefs.begin("motorCal", true)) {
    for (int i = 0; i < 2; ++i) {
      uint8_t saved = prefs.getUChar(calibrationKeys[i], 100);
      if (saved <= 100) calibrationPercent[i] = saved;
    }
    prefs.end();
  }
  for (int i = 0; i < 2; i++) {
    ledcAttach(motorPins[i].pinA, MOTOR_PWM_FREQ, MOTOR_PWM_RES_BITS);
    ledcAttach(motorPins[i].pinB, MOTOR_PWM_FREQ, MOTOR_PWM_RES_BITS);
    motors[i].running = false;
  }
}

void MOTOR_Run(int motor, bool forward, int pwmPercent, unsigned long durationMs) {
  if (motor < 1 || motor > 2) return;
  if (pwmPercent < 0) pwmPercent = 0;
  if (pwmPercent > 100) pwmPercent = 100;

  const MotorPins &pins = motorPins[motor - 1];
  uint32_t duty = calibratedDuty(motor, pwmPercent);
  ledcWrite(pins.pinA, forward ? duty : 0);
  ledcWrite(pins.pinB, forward ? 0 : duty);

  motors[motor - 1].startAt = millis();
  motors[motor - 1].durationMs = durationMs;
  motors[motor - 1].running = true;
}

void MOTOR_Set(int motor, bool forward, int pwmPercent) {
  if (motor < 1 || motor > 2) return;
  if (pwmPercent < 0) pwmPercent = 0;
  if (pwmPercent > 100) pwmPercent = 100;

  const MotorPins &pins = motorPins[motor - 1];
  uint32_t duty = calibratedDuty(motor, pwmPercent);
  ledcWrite(pins.pinA, forward ? duty : 0);
  ledcWrite(pins.pinB, forward ? 0 : duty);
  motors[motor - 1].running = false;
}

void MOTOR_StopAll() {
  for (int i = 0; i < 2; i++) {
    ledcWrite(motorPins[i].pinA, 0);
    ledcWrite(motorPins[i].pinB, 0);
    motors[i].running = false;
  }
}

void MOTOR_Process() {
  for (int i = 0; i < 2; i++) {
    if (motors[i].running && millis() - motors[i].startAt >= motors[i].durationMs) {
      ledcWrite(motorPins[i].pinA, 0);
      ledcWrite(motorPins[i].pinB, 0);
      motors[i].running = false;
    }
  }
}
