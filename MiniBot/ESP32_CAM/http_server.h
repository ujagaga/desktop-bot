#pragma once
void HTTPSRV_init();
void HTTPSRV_stop();
// Refuse while clients are present; otherwise reserve shutdown and reject new
// HTTP connections until the server is initialized again.
bool HTTPSRV_PrepareSleep();
