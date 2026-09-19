#include <Arduino.h>
#include <strings.h>
#include <Wire.h>
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
  Wire.begin(GYRO_SDA_PIN, GYRO_SCL_PIN);
  Wire.setClock(400000);

  for (int i = 0; i < 2; i++) {
    uint8_t address = (i == 0) ? GYRO_I2C_ADDR_1 : GYRO_I2C_ADDR_2;
    uint8_t whoami = 0;
    if (!readRegister(address, GYRO_REG_WHO_AM_I, &whoami)) continue;

    if (whoami == GYRO_WHO_AM_I) {
      if (!writeRegister(address, GYRO_REG_CTRL1, 0x60) ||
          !writeRegister(address, GYRO_REG_CTRL3, 0x53) ||
          !writeRegister(address, GYRO_REG_CTRL7, 0x03)) {
        continue;
      }
      deviceAddress = address;
      detected = true;
      lastUpdateMs = millis();
      float startupBiasDps[3];
      if (GYRO_Calibrate(startupBiasDps)) {
        Serial.printf("GYRO: calibrated bias X=%.2f Y=%.2f Z=%.2f dps\n",
                      startupBiasDps[0], startupBiasDps[1], startupBiasDps[2]);
      } else {
        Serial.println("GYRO: calibration failed");
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

  if (validSamples == 0) return false;

  for (int axis = 0; axis < 3; axis++) {
    biasDps[axis] = biasSum[axis] / validSamples;
    angles[axis] = 0.0f;
    outputBiasDps[axis] = biasDps[axis];
  }
  lastUpdateMs = millis();
  return true;
}
