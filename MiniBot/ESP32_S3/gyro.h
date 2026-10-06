#ifndef GYRO_H
#define GYRO_H

#include <stdint.h>

void GYRO_Init();
void GYRO_Update();
// Same integration as GYRO_Update(), but only the given axis (fewer I2C reads, tighter loop timing).
void GYRO_UpdateAxis(int axis);
uint8_t GYRO_GetWakeThreshold();
uint8_t GYRO_GetThresholdMultiplier();
// Wake/sleep require at least the saved tap count (1-3).
uint8_t GYRO_GetWakeTapThreshold();
bool GYRO_SetWakeTapThreshold(uint8_t value);
uint8_t GYRO_PollTaps();
// While sleeping: true for enough taps, or (after a tilt sleep) when the robot stands upright again.
bool GYRO_ConfirmTapWake();
// Awake: true once the robot, upright since waking, lies on a side (> 70 deg) for 1 s.
bool GYRO_PollTiltSleep();
// After waking: whether the sleep was a tilt sleep (clears it).
bool GYRO_TakeTiltWake();
// Set config base * multiplier (0-12); reject products above 255 mg.
bool GYRO_SetWakeThreshold(uint8_t multiplier);
// Arm IMU INT1 for light-sleep motion wake.
bool GYRO_PrepareForSleep();
// Restore normal mode; retain bias on motion wake or when recalibrate is false.
bool GYRO_RestoreAfterSleep(bool recalibrate);
bool GYRO_IsDetected();
int GYRO_AxisIndexFromText(const char *axisText);
float GYRO_GetRateDps(int axis);
float GYRO_GetAngleDegrees(int axis);
bool GYRO_Calibrate(float biasDps[3]);

#endif
