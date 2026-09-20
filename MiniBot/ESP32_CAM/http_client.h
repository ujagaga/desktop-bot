#pragma once
#include <stdint.h>
void HTTPC_init();
void HTTPC_process();
bool HTTPC_requestCheck();
bool HTTPC_fwUpdateInProgress();
uint32_t HTTPC_invalidVersion();
uint32_t HTTPC_foundVersion();
const char *HTTPC_updateStatus();
bool HTTPC_checkPending();
