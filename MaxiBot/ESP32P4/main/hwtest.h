#pragma once

/* Switches the camera IR LEDs off. */
void hwtest_start(void);

/* Registers `wiretest`, `i2c scan`, `irled on|off`, `lcdprobe` and `gpio`. */
void hwtest_register_command(void);
