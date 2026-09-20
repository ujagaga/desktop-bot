#ifndef COMMS_H
#define COMMS_H

#include <Arduino.h>

// Starts the UART used for commands.
void COMMS_Init();

// Drains incoming UART bytes into the ring buffer and dispatches any
// complete commands found. Call from loop().
void COMMS_Poll();

// Execute one command (up to 127 bytes, without CR/LF). Responses, including
// OK/ERR, are written to output using the same dispatcher as both serial ports.
void COMMS_Execute(const char *command, Print &output);

#endif
