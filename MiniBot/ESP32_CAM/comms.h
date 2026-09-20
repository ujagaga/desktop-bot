#pragma once
#include <Arduino.h>
#include <atomic>

// Packed IPv4 address (first octet in bits 31..24), zero until discovered.
extern std::atomic<uint32_t> COMMS_peerWifiIP;

// Starts independent UART RX and the CAM command handlers.
// S3 initiates discovery with report ip <s3 address>.
bool COMMS_Init();
// Thread-safe snapshot for HTTP/UI consumers; empty until discovered.
String COMMS_GetPeerWifiIP();

// Queue one printable ASCII command (1..127 bytes), without CR/LF.
// The RX task owns normal UART writes; the main loop sends the final sleep
// acknowledgment only after RX has paused.
bool COMMS_SendCommand(const char *command);
// Recent TX/RX transcript.
String COMMS_ConsoleRead();
bool COMMS_IsReady();

// Called from loop() to prepare deep sleep and send the final acknowledgment.
void COMMS_ProcessSleep();
