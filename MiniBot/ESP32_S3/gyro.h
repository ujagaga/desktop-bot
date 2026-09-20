#ifndef GYRO_H
#define GYRO_H

#include <stdint.h>

void GYRO_Init();
void GYRO_Update();
uint8_t GYRO_GetWakeThreshold();
uint8_t GYRO_GetThresholdMultiplier();
uint8_t GYRO_PollTaps();
bool GYRO_ConfirmTapWake();
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
