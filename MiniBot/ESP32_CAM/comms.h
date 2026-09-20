#pragma once
#include <Arduino.h>
#include <atomic>

// Packed IPv4 address (first octet in bits 31..24), zero until discovered.
extern std::atomic<uint32_t> COMMS_peerWifiIP;

// Starts independent UART RX and periodic S3 Wi-Fi IP discovery.
bool COMMS_Init();
// Thread-safe snapshot for HTTP/UI consumers; empty until discovered.
String COMMS_GetPeerWifiIP();
