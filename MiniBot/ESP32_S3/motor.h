#ifndef MOTOR_H
#define MOTOR_H

// Attaches PWM to the H-bridge input pins.
void MOTOR_Init();

// Runs the given motor (1 or 2) forward (or backward) at the given PWM
// percent (0-100) for the given duration, then stops it.
void MOTOR_Run(int motor, bool forward, int pwmPercent, unsigned long durationMs);

// Stops any motor whose duration has elapsed. Call from loop().
void MOTOR_Process();

#endif
