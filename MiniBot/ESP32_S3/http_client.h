#ifndef MINIBOT_HTTP_CLIENT_H
#define MINIBOT_HTTP_CLIENT_H

#include <stdint.h>

// Register the Wi-Fi connection callback before starting Wi-Fi.
void HTTP_CLIENT_Init();
// Check GitHub after connecting, once time is synchronized for HTTPS.
void HTTP_CLIENT_Process();
// Failed OTA target retained across restarts; zero when none.
uint32_t HTTP_CLIENT_GetInvalidVersion();

#endif
