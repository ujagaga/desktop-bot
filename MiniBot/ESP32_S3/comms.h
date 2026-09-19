#ifndef COMMS_H
#define COMMS_H

// Starts the UART used for commands.
void COMMS_Init();

// Drains incoming UART bytes into the ring buffer and dispatches any
// complete commands found. Call from loop().
void COMMS_Poll();

#endif
