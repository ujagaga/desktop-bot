#ifndef GYRO_H
#define GYRO_H

#include <stdint.h>

void GYRO_Init();
void GYRO_Update();
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
