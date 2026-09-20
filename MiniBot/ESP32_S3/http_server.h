#ifndef MINIBOT_HTTP_SERVER_H
#define MINIBOT_HTTP_SERVER_H

// Register the web console routes. Call once during setup().
void HTTP_SERVER_Init();
// Start/stop with Wi-Fi and service requests from the main loop.
void HTTP_SERVER_Process();

#endif
