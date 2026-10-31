#pragma once

/* Checks GitHub for newer firmware after each Wi-Fi connection, as MiniBot
 * does, and confirms a freshly installed image after it has run for 60 s. */
void ota_start(void);

/* Registers `ota check`: schedule a check now. */
void ota_register_command(void);
