#include <Arduino.h>
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
  { 11, 12 },
};

struct MotorState {
  unsigned long startAt;
  unsigned long durationMs;
  bool running;
};

static MotorState motors[2];

void MOTOR_Init() {
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
  uint32_t duty = ((1 << MOTOR_PWM_RES_BITS) - 1) * pwmPercent / 100;
  ledcWrite(pins.pinA, forward ? duty : 0);
  ledcWrite(pins.pinB, forward ? 0 : duty);

  motors[motor - 1].startAt = millis();
  motors[motor - 1].durationMs = durationMs;
  motors[motor - 1].running = true;
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
