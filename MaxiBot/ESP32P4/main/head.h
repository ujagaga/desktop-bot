#pragma once

/* Starts the pan/tilt servos at center. */
void head_start(void);

/* Registers the `head` console command:
 *   head                 print current and target angles
 *   head <pan> <tilt>    move to angles in degrees (0-180)
 *   head center */
void head_register_command(void);
