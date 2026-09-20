#include "camera.h"
#include "logger.h"
#include <Preferences.h>
#include <ArduinoJson.h>
#include <freertos/semphr.h>
static SemaphoreHandle_t mutex = nullptr;
static bool initialized = false;
struct Setting { const char *name; int minimum, maximum; int (*set)(sensor_t *, int); int (*get)(sensor_t *); };
static const Setting settings[] = {
  {"framesize", FRAMESIZE_96X96, FRAMESIZE_UXGA, [](sensor_t *s, int v) { return s->set_framesize(s, (framesize_t)v); }, [](sensor_t *s) { return (int)s->status.framesize; }},
  {"quality", 4, 63, [](sensor_t *s, int v) { return s->set_quality(s, v); }, [](sensor_t *s) { return (int)s->status.quality; }},
  {"contrast", -2, 2, [](sensor_t *s, int v) { return s->set_contrast(s, v); }, [](sensor_t *s) { return (int)s->status.contrast; }},
  {"brightness", -2, 2, [](sensor_t *s, int v) { return s->set_brightness(s, v); }, [](sensor_t *s) { return (int)s->status.brightness; }},
  {"saturation", -2, 2, [](sensor_t *s, int v) { return s->set_saturation(s, v); }, [](sensor_t *s) { return (int)s->status.saturation; }},
  {"gainceiling", 0, 6, [](sensor_t *s, int v) { return s->set_gainceiling(s, (gainceiling_t)v); }, [](sensor_t *s) { return (int)s->status.gainceiling; }},
  {"colorbar", 0, 1, [](sensor_t *s, int v) { return s->set_colorbar(s, v); }, [](sensor_t *s) { return (int)s->status.colorbar; }},
  {"awb", 0, 1, [](sensor_t *s, int v) { return s->set_whitebal(s, v); }, [](sensor_t *s) { return (int)s->status.awb; }},
  {"agc", 0, 1, [](sensor_t *s, int v) { return s->set_gain_ctrl(s, v); }, [](sensor_t *s) { return (int)s->status.agc; }},
  {"aec", 0, 1, [](sensor_t *s, int v) { return s->set_exposure_ctrl(s, v); }, [](sensor_t *s) { return (int)s->status.aec; }},
  {"hmirror", 0, 1, [](sensor_t *s, int v) { return s->set_hmirror(s, v); }, [](sensor_t *s) { return (int)s->status.hmirror; }},
  {"vflip", 0, 1, [](sensor_t *s, int v) { return s->set_vflip(s, v); }, [](sensor_t *s) { return (int)s->status.vflip; }},
  {"awb_gain", 0, 1, [](sensor_t *s, int v) { return s->set_awb_gain(s, v); }, [](sensor_t *s) { return (int)s->status.awb_gain; }},
  {"agc_gain", 0, 30, [](sensor_t *s, int v) { return s->set_agc_gain(s, v); }, [](sensor_t *s) { return (int)s->status.agc_gain; }},
  {"aec_value", 0, 1200, [](sensor_t *s, int v) { return s->set_aec_value(s, v); }, [](sensor_t *s) { return (int)s->status.aec_value; }},
  {"aec2", 0, 1, [](sensor_t *s, int v) { return s->set_aec2(s, v); }, [](sensor_t *s) { return (int)s->status.aec2; }},
  {"dcw", 0, 1, [](sensor_t *s, int v) { return s->set_dcw(s, v); }, [](sensor_t *s) { return (int)s->status.dcw; }},
  {"bpc", 0, 1, [](sensor_t *s, int v) { return s->set_bpc(s, v); }, [](sensor_t *s) { return (int)s->status.bpc; }},
  {"wpc", 0, 1, [](sensor_t *s, int v) { return s->set_wpc(s, v); }, [](sensor_t *s) { return (int)s->status.wpc; }},
  {"raw_gma", 0, 1, [](sensor_t *s, int v) { return s->set_raw_gma(s, v); }, [](sensor_t *s) { return (int)s->status.raw_gma; }},
  {"lenc", 0, 1, [](sensor_t *s, int v) { return s->set_lenc(s, v); }, [](sensor_t *s) { return (int)s->status.lenc; }},
  {"special_effect", 0, 6, [](sensor_t *s, int v) { return s->set_special_effect(s, v); }, [](sensor_t *s) { return (int)s->status.special_effect; }},
  {"wb_mode", 0, 4, [](sensor_t *s, int v) { return s->set_wb_mode(s, v); }, [](sensor_t *s) { return (int)s->status.wb_mode; }},
  {"ae_level", -2, 2, [](sensor_t *s, int v) { return s->set_ae_level(s, v); }, [](sensor_t *s) { return (int)s->status.ae_level; }},
};
static constexpr size_t SETTING_COUNT = sizeof(settings) / sizeof(settings[0]);
static bool setValue(sensor_t *sensor, size_t index, int value) {
  const Setting &setting = settings[index];
  if (value < setting.minimum || value > setting.maximum) return false;
  if (index == 0 && !psramFound() && value > FRAMESIZE_VGA) return false;
  return setting.set(sensor, value) == 0;
}
bool CAM_Init() {
  if (!mutex) mutex = xSemaphoreCreateMutex();
  if (!mutex) return false;
  xSemaphoreTake(mutex, portMAX_DELAY);
  if (initialized) { xSemaphoreGive(mutex); return true; }
  camera_config_t config = {};
  // Classic AI-Thinker ESP32-CAM, OV2640. Not the ESP32-S3 camera pin map.
  config.pin_pwdn = 32; config.pin_reset = -1; config.pin_xclk = 0;
  config.pin_sccb_sda = 26; config.pin_sccb_scl = 27;
  config.pin_d0 = 5; config.pin_d1 = 18; config.pin_d2 = 19; config.pin_d3 = 21;
  config.pin_d4 = 36; config.pin_d5 = 39; config.pin_d6 = 34; config.pin_d7 = 35;
  config.pin_vsync = 25; config.pin_href = 23; config.pin_pclk = 22;
  config.ledc_channel = LEDC_CHANNEL_0; config.ledc_timer = LEDC_TIMER_0;
  config.xclk_freq_hz = 20000000; config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = psramFound() ? FRAMESIZE_UXGA : FRAMESIZE_VGA;
  config.fb_location = psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
  config.fb_count = psramFound() ? 2 : 1;
  config.grab_mode = CAMERA_GRAB_LATEST; config.jpeg_quality = 12;
  esp_err_t err = esp_camera_init(&config);
  initialized = err == ESP_OK;
  if (initialized) {
    sensor_t *sensor = esp_camera_sensor_get();
    sensor->set_framesize(sensor, FRAMESIZE_VGA);
    Preferences prefs;
    if (prefs.begin("camCamera", true)) {
      int32_t values[SETTING_COUNT];
      if (prefs.getBytesLength("settings_v1") == sizeof(values) &&
          prefs.getBytes("settings_v1", values, sizeof(values)) == sizeof(values)) {
        for (size_t i = 0; i < SETTING_COUNT; ++i)
          if (!setValue(sensor, i, values[i])) LOG_printf("Camera: ignored saved %s", settings[i].name);
      }
      prefs.end();
    }
  }
  xSemaphoreGive(mutex);
  if (initialized) LOG_append("Camera ready");
  else LOG_printf("ERR camera initialization: 0x%x; Wi-Fi UI remains available", (unsigned)err);
  return initialized;
}
bool CAM_isInitialized() {
  if (!mutex) return false;
  xSemaphoreTake(mutex, portMAX_DELAY); bool result = initialized; xSemaphoreGive(mutex);
  return result;
}
camera_fb_t *CAM_Capture() {
  if (!mutex || xSemaphoreTake(mutex, pdMS_TO_TICKS(2000)) != pdTRUE) return nullptr;
  camera_fb_t *frame = initialized ? esp_camera_fb_get() : nullptr;
  if (!frame) xSemaphoreGive(mutex);
  return frame; // Lock held until Dispose, so settings/deinit cannot race the frame.
}
void CAM_Dispose(camera_fb_t *frame) {
  if (frame) { esp_camera_fb_return(frame); xSemaphoreGive(mutex); }
}
void CAM_Stop() {
  if (!mutex) return;
  xSemaphoreTake(mutex, portMAX_DELAY);
  if (initialized) { esp_camera_deinit(); initialized = false; }
  xSemaphoreGive(mutex);
}
bool CAM_Set(const char *name, int value) {
  if (!mutex) return false;
  xSemaphoreTake(mutex, portMAX_DELAY);
  bool ok = false;
  sensor_t *sensor = initialized ? esp_camera_sensor_get() : nullptr;
  if (sensor) for (size_t i = 0; i < SETTING_COUNT; ++i)
    if (!strcmp(name, settings[i].name)) { ok = setValue(sensor, i, value); break; }
  xSemaphoreGive(mutex);
  LOG_printf("Camera %s=%d: %s", name, value, ok ? "OK" : "ERR");
  return ok;
}
bool CAM_Save() {
  if (!mutex) return false;
  xSemaphoreTake(mutex, portMAX_DELAY);
  bool ok = false;
  if (initialized) {
    int32_t values[SETTING_COUNT], saved[SETTING_COUNT];
    sensor_t *sensor = esp_camera_sensor_get();
    for (size_t i = 0; i < SETTING_COUNT; ++i) values[i] = settings[i].get(sensor);
    Preferences prefs;
    if (prefs.begin("camCamera", false)) {
      ok = (prefs.getBytesLength("settings_v1") == sizeof(saved) &&
            prefs.getBytes("settings_v1", saved, sizeof(saved)) == sizeof(saved) &&
            !memcmp(saved, values, sizeof(saved))) ||
            prefs.putBytes("settings_v1", values, sizeof(values)) == sizeof(values);
      prefs.end();
    }
  }
  xSemaphoreGive(mutex);
  LOG_append(ok ? "Camera settings saved" : "ERR saving camera settings");
  return ok;
}
String CAM_Status() {
  JsonDocument doc;
  if (mutex) xSemaphoreTake(mutex, portMAX_DELAY);
  doc["initialized"] = initialized;
  if (initialized) {
    sensor_t *sensor = esp_camera_sensor_get();
    for (const Setting &setting : settings) doc[setting.name] = setting.get(sensor);
  }
  if (mutex) xSemaphoreGive(mutex);
  String result; serializeJson(doc, result); return result;
}
