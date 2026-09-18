#ifndef COMMS_H
#define COMMS_H

// Starts the UART used for commands.
void initComms();

// Drains incoming UART bytes into the ring buffer and dispatches any
// complete commands found. Call from loop().
void commsPoll();

#endif
