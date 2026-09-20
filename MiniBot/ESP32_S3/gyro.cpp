#include <Arduino.h>
#include <strings.h>
#include <Wire.h>
#include <Preferences.h>
#include <math.h>
#include <string.h>
#include <driver/gpio.h>
#include <esp_sleep.h>
#include "config.h"
#include "gyro.h"

#define GYRO_SDA_PIN 47
#define GYRO_SCL_PIN 48
#define GYRO_I2C_ADDR_1 0x6A
#define GYRO_I2C_ADDR_2 0x6B
#define GYRO_REG_WHO_AM_I 0x00
#define GYRO_REG_CTRL1 0x02
#define GYRO_REG_CTRL3 0x04
#define GYRO_REG_CTRL7 0x08
#define GYRO_REG_GYRO_X_L 0x3B
#define GYRO_WHO_AM_I 0x05
#define GYRO_SENSITIVITY 64.0f
#define GYRO_MAX_INTEGRATION_INTERVAL_MS 100
#define GYRO_CALIBRATION_SAMPLES 200

static bool detected = false;
static uint8_t deviceAddress = 0;
static float angles[3] = { 0.0f, 0.0f, 0.0f };
static float biasDps[3] = { 0.0f, 0.0f, 0.0f };
static unsigned long lastUpdateMs = 0;
static bool motionArmed = false;
static bool savedBiasValid = false;
static float savedBias[3] = {};

static_assert(GYRO_WAKE_THRESHOLD_MG > 0 && GYRO_WAKE_THRESHOLD_MG <= 246,
              "Motion threshold base must leave room for a 0-9 mg adjustment");
static uint8_t wakeThresholdMg = GYRO_WAKE_THRESHOLD_MG;

static void loadWakeThreshold() {
  wakeThresholdMg = GYRO_WAKE_THRESHOLD_MG;
  Preferences prefs;
  if (!prefs.begin("miniBotGyro", true)) return;
  uint8_t saved = prefs.getUChar("wake_mg", GYRO_WAKE_THRESHOLD_MG);
  prefs.end();
  if (saved != 0) wakeThresholdMg = saved;
}

uint8_t GYRO_GetWakeThreshold() {
  return wakeThresholdMg;
}

bool GYRO_SetWakeThreshold(uint8_t increment) {
  if (increment > 9) return false;
  uint8_t value = GYRO_WAKE_THRESHOLD_MG + increment;
  Preferences prefs;
  if (!prefs.begin("miniBotGyro", false)) return false;
  // Compare with NVS, so the first explicit setting is saved, even at default.
  bool ok = prefs.getUChar("wake_mg", 0) == value ||
            prefs.putUChar("wake_mg", value) == sizeof(value);
  prefs.end();
  if (ok) wakeThresholdMg = value;
  return ok;
}

static void loadBias() {
  Preferences prefs;
  if (!prefs.begin("miniBotGyro", true)) return;
  float values[3];
  bool valid = prefs.getBytesLength("bias_v1") == sizeof(values) &&
               prefs.getBytes("bias_v1", values, sizeof(values)) == sizeof(values);
  prefs.end();
  for (int i = 0; valid && i < 3; ++i) valid = isfinite(values[i]) && fabsf(values[i]) <= 512.0f;
  if (!valid) return;
  memcpy(biasDps, values, sizeof(values));
  memcpy(savedBias, values, sizeof(values));
  savedBiasValid = true;
}

static void saveBiasIfChanged() {
  bool changed = !savedBiasValid;
  for (int i = 0; i < 3; ++i)
    changed |= fabsf(biasDps[i] - savedBias[i]) >= GYRO_BIAS_SAVE_DELTA_DPS;
  if (!changed) return;
  Preferences prefs;
  if (!prefs.begin("miniBotGyro", false)) {
    Serial.println("GYRO: cannot open calibration preferences; using RAM bias");
    return;
  }
  bool ok = prefs.putBytes("bias_v1", biasDps, sizeof(biasDps)) == sizeof(biasDps);
  prefs.end();
  if (ok) {
    memcpy(savedBias, biasDps, sizeof(savedBias));
    savedBiasValid = true;
    Serial.println("GYRO: saved calibration");
  } else Serial.println("GYRO: calibration save failed; using RAM bias");
}

static bool writeRegister(uint8_t address, uint8_t reg, uint8_t value) {
  Wire.beginTransmission(address);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

static bool readRegister(uint8_t address, uint8_t reg, uint8_t *value) {
  Wire.beginTransmission(address);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)address, 1, true) != 1) return false;
  *value = Wire.read();
  return true;
}

// QMI8658A CTRL9 protocol: wait for command completion, ACK, then wait clear.
static bool waitCommand(bool done) {
  uint32_t start = millis();
  do {
    uint8_t status;
    if (!readRegister(deviceAddress, 0x2D, &status)) return false;
    if (!!(status & 0x80) == done) return true;
    delay(1);
  } while (millis() - start < 500);
  return false;
}

static bool setMotionThreshold(uint8_t threshold) {
  return writeRegister(deviceAddress, 0x0A, 0) && waitCommand(false) &&
         writeRegister(deviceAddress, 0x0B, threshold) &&
         // INT1 initially low; ignore first 8 accelerometer samples.
         writeRegister(deviceAddress, 0x0C, 8) &&
         writeRegister(deviceAddress, 0x0A, 0x08) && waitCommand(true) &&
         writeRegister(deviceAddress, 0x0A, 0) && waitCommand(false);
}

static bool configureNormal() {
  return writeRegister(deviceAddress, GYRO_REG_CTRL7, 0) &&
         // Auto-increment, little-endian data to match readAxisRaw().
         writeRegister(deviceAddress, GYRO_REG_CTRL1, 0x40) &&
         writeRegister(deviceAddress, 0x09, 0x80) &&
         setMotionThreshold(0) &&
         writeRegister(deviceAddress, 0x03, 0x03) &&
         writeRegister(deviceAddress, GYRO_REG_CTRL3, 0x53) &&
         writeRegister(deviceAddress, GYRO_REG_CTRL7, 0x03);
}

bool GYRO_PrepareForSleep() {
  if (!detected) return false;
  motionArmed = true; // Also enables recovery after a partially completed setup.
  pinMode(GYRO_MOTION_WAKE_GPIO, INPUT);
  uint8_t status;
  return writeRegister(deviceAddress, GYRO_REG_CTRL7, 0) &&
         writeRegister(deviceAddress, GYRO_REG_CTRL1, 0x48) &&
         writeRegister(deviceAddress, 0x09, 0x80) &&
         // +/-2g, 128 Hz low-power accelerometer; gyro is disabled.
         writeRegister(deviceAddress, 0x03, 0x0C) &&
         setMotionThreshold(wakeThresholdMg) &&
         readRegister(deviceAddress, 0x2F, &status) &&
         writeRegister(deviceAddress, GYRO_REG_CTRL7, 0x01) &&
         gpio_wakeup_enable((gpio_num_t)GYRO_MOTION_WAKE_GPIO, GPIO_INTR_HIGH_LEVEL) == ESP_OK &&
         esp_sleep_enable_gpio_wakeup() == ESP_OK;
}

bool GYRO_RestoreAfterSleep(bool recalibrate) {
  if (!motionArmed) return false;
  // Read before clearing WoM: also catch motion coincident with touch/UART wake.
  uint8_t status = 0;
  bool statusRead = readRegister(deviceAddress, 0x2F, &status);
  bool moved = esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_GPIO ||
               digitalRead(GYRO_MOTION_WAKE_GPIO) == HIGH || (status & 0x04);
  gpio_wakeup_disable((gpio_num_t)GYRO_MOTION_WAKE_GPIO);
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
  motionArmed = false;
  if (!configureNormal()) {
    detected = false;
    Serial.println("GYRO: failed to restore normal mode");
    return false;
  }
  delay(100); // Gyro startup settling after disabling it for WoM.
  lastUpdateMs = millis(); // Never integrate elapsed sleep time.
  if (recalibrate && statusRead && !moved) {
    float values[3];
    return GYRO_Calibrate(values);
  }
  Serial.println("GYRO: retained calibration after motion/uncertain wake");
  return true;
}

static bool readAxisRaw(int axis, int16_t *rawValue) {
  if (axis < 0 || axis > 2) return false;

  uint8_t reg = GYRO_REG_GYRO_X_L + (uint8_t)axis * 2;
  uint8_t low = 0;
  uint8_t high = 0;
  if (!readRegister(deviceAddress, reg, &low)) return false;
  if (!readRegister(deviceAddress, reg + 1, &high)) return false;
  *rawValue = (int16_t)(((uint16_t)high << 8) | low);
  return true;
}

void GYRO_Init() {
  loadWakeThreshold();
  loadBias();
  Wire.begin(GYRO_SDA_PIN, GYRO_SCL_PIN);
  Wire.setClock(400000);

  for (int i = 0; i < 2; i++) {
    uint8_t address = (i == 0) ? GYRO_I2C_ADDR_1 : GYRO_I2C_ADDR_2;
    uint8_t whoami = 0;
    if (!readRegister(address, GYRO_REG_WHO_AM_I, &whoami)) continue;

    if (whoami == GYRO_WHO_AM_I) {
      deviceAddress = address;
      if (!configureNormal()) continue;
      detected = true;
      delay(100);
      lastUpdateMs = millis();
      if (savedBiasValid) {
        Serial.println("GYRO: loaded saved calibration");
      } else {
        float startupBiasDps[3];
        if (!GYRO_Calibrate(startupBiasDps)) Serial.println("GYRO: calibration failed");
      }
      return;
    }

    Serial.printf("GYRO: address 0x%02X responded with WHO_AM_I 0x%02X\n",
                  address, whoami);
  }

  detected = false;
  Serial.println("GYRO: QMI8658 not detected at 0x6A or 0x6B");
}

void GYRO_Update() {
  if (!detected) return;

  unsigned long now = millis();
  unsigned long elapsedMs = now - lastUpdateMs;
  lastUpdateMs = now;
  if (elapsedMs == 0 || elapsedMs > GYRO_MAX_INTEGRATION_INTERVAL_MS) return;

  float elapsedSeconds = elapsedMs / 1000.0f;
  for (int axis = 0; axis < 3; axis++) {
    angles[axis] += GYRO_GetRateDps(axis) * elapsedSeconds;
  }
}

bool GYRO_IsDetected() {
  return detected;
}

int GYRO_AxisIndexFromText(const char *axisText) {
  if (axisText == nullptr) return -1;
  if (strcasecmp(axisText, "x") == 0 || strcasecmp(axisText, "0") == 0) return 0;
  if (strcasecmp(axisText, "y") == 0 || strcasecmp(axisText, "1") == 0) return 1;
  if (strcasecmp(axisText, "z") == 0 || strcasecmp(axisText, "2") == 0) return 2;
  return -1;
}

float GYRO_GetRateDps(int axis) {
  if (!detected || axis < 0 || axis > 2) return 0.0f;

  int16_t raw = 0;
  if (!readAxisRaw(axis, &raw)) return 0.0f;
  return raw / GYRO_SENSITIVITY - biasDps[axis];
}

float GYRO_GetAngleDegrees(int axis) {
  if (axis < 0 || axis > 2) return 0.0f;
  return angles[axis];
}

bool GYRO_Calibrate(float outputBiasDps[3]) {
  if (!detected) return false;

  float biasSum[3] = { 0.0f, 0.0f, 0.0f };
  int validSamples = 0;
  for (int sample = 0; sample < GYRO_CALIBRATION_SAMPLES; sample++) {
    bool valid = true;
    float sampleDps[3];
    for (int axis = 0; axis < 3; axis++) {
      int16_t raw = 0;
      if (!readAxisRaw(axis, &raw)) {
        valid = false;
        break;
      }
      sampleDps[axis] = raw / GYRO_SENSITIVITY;
    }
    if (valid) {
      for (int axis = 0; axis < 3; axis++) biasSum[axis] += sampleDps[axis];
      validSamples++;
    }
    delay(5);
  }

  if (validSamples < GYRO_CALIBRATION_SAMPLES * 9 / 10) return false;

  for (int axis = 0; axis < 3; axis++) {
    biasDps[axis] = biasSum[axis] / validSamples;
    angles[axis] = 0.0f;
    outputBiasDps[axis] = biasDps[axis];
  }
  lastUpdateMs = millis();
  saveBiasIfChanged();
  return true;
}
