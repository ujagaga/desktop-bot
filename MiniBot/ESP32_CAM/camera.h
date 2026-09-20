#pragma once
#include <Arduino.h>
#include <esp_camera.h>
bool CAM_Init();
camera_fb_t *CAM_Capture();
void CAM_Dispose(camera_fb_t *frame);
bool CAM_isInitialized();
void CAM_Stop();
bool CAM_Set(const char *name, int value);
bool CAM_Save();
String CAM_Status();
