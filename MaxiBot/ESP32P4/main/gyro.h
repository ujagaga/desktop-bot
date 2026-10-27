#pragma once

/* Starts the MPU6050 task. Loads the saved gyro bias, or calibrates once if
 * none is saved (keep the robot still at first boot). */
void gyro_start(void);

/* Registers the `gyro` console command:
 *   gyro angle <x|y|z>   degrees; x/y corrected by the accelerometer, z drifts
 *   gyro rate <x|y|z>    degrees per second
 *   gyro calibrate       measure gyro bias (keep still ~2 s), reset angles */
void gyro_register_command(void);
