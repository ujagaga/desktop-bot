#ifndef BATTERY_H
#define BATTERY_H

void BATT_Init();
void BATT_process();
void BATT_ShowStatus();
float BATT_GetVoltage();
int BATT_GetPercent();
// True while charger 5V is present (68K/100K divider from header 5V pin to IO8).
bool BATT_IsCharging();

#endif
