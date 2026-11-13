#pragma once

/* Registers `tof [n]`: VL53L1X (TOF400C) distance readings. The sensor is
 * set up on first use, on the MPU6050 I2C bus. */
void tof_register_command(void);
