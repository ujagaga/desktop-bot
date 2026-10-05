#ifndef BATTERY_H
#define BATTERY_H

void BATT_Init();
void BATT_process();
void BATT_ShowStatus();
float BATT_GetVoltage();
int BATT_GetPercent();
// True while charger 5V is present (MOSFET pulls IO8 LOW).
bool BATT_IsCharging();
// Light-sleep GPIO wake on charger connect; armed only while not charging (level wake).
bool BATT_ArmChargerWake();
void BATT_DisarmChargerWake();

#endif
