#ifndef MOTOR_H
#define MOTOR_H

#define MOTOR_ROTATE_MOTOR1_FORWARD true
#define MOTOR_ROTATE_MOTOR2_FORWARD false
#define MOTOR_ROTATE_AXIS_INDEX 2

// Attaches PWM to the H-bridge input pins.
void MOTOR_Init();

// Runs the given motor (1 or 2) forward (or backward) at the given PWM
// percent (0-100) for the given duration, then stops it.
void MOTOR_Run(int motor, bool forward, int pwmPercent, unsigned long durationMs);

// Drives a motor continuously until MOTOR_StopAll() is called.
void MOTOR_Set(int motor, bool forward, int pwmPercent);

// Stops both motors immediately.
void MOTOR_StopAll();

// Stops any motor whose duration has elapsed. Call from loop().
void MOTOR_Process();

#endif
