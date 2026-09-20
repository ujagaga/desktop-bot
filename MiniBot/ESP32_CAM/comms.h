#pragma once
#include <Arduino.h>
#include <atomic>

// Packed IPv4 address (first octet in bits 31..24), zero until discovered.
extern std::atomic<uint32_t> COMMS_peerWifiIP;

// Starts independent UART RX and periodic S3 Wi-Fi IP discovery.
bool COMMS_Init();
// Thread-safe snapshot for HTTP/UI consumers; empty until discovered.
String COMMS_GetPeerWifiIP();

// Queue one printable ASCII command (1..127 bytes), without CR/LF.
// Only the RX task writes to UART, so console and discovery cannot interleave.
bool COMMS_SendCommand(const char *command);
// Recent TX/RX transcript. Reading pauses automatic IP requests for 30 seconds.
String COMMS_ConsoleRead();
bool COMMS_IsReady();
