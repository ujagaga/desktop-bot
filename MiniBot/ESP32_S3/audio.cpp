#include "audio.h"

#include <math.h>
#include <stdlib.h>

#include "ESP_I2S.h"

namespace {
constexpr int8_t AUDIO_BCLK_GPIO = 2;
constexpr int8_t AUDIO_WS_GPIO = 3;
constexpr int8_t AUDIO_AMP_SD_GPIO = 4;
constexpr int8_t AUDIO_SPEAKER_DATA_GPIO = 5;
constexpr int8_t AUDIO_MIC_DATA_GPIO = 1;
constexpr uint32_t AUDIO_SAMPLE_RATE = 16000;
constexpr size_t AUDIO_RECORD_SECONDS = 2;
constexpr size_t AUDIO_TONE_CHUNK_SAMPLES = 256;

I2SClass audioI2S;
bool audioReady = false;

bool playTestTone() {
  int32_t samples[AUDIO_TONE_CHUNK_SAMPLES];
  constexpr size_t toneSamples = AUDIO_SAMPLE_RATE / 3;
  constexpr float amplitude = 0.08f * 2147483647.0f;

  for (size_t offset = 0; offset < toneSamples; offset += AUDIO_TONE_CHUNK_SAMPLES) {
    size_t count = toneSamples - offset;
    if (count > AUDIO_TONE_CHUNK_SAMPLES) count = AUDIO_TONE_CHUNK_SAMPLES;
    for (size_t i = 0; i < count; ++i) {
      float phase = 2.0f * PI * 880.0f * (offset + i) / AUDIO_SAMPLE_RATE;
      samples[i] = (int32_t)(sinf(phase) * amplitude);
    }
    size_t bytes = count * sizeof(samples[0]);
    if (audioI2S.write((const uint8_t *)samples, bytes) != bytes) return false;
  }

  int32_t silence[AUDIO_TONE_CHUNK_SAMPLES] = {};
  return audioI2S.write((const uint8_t *)silence, sizeof(silence)) == sizeof(silence);
}
}

bool AUDIO_Init() {
  pinMode(AUDIO_AMP_SD_GPIO, OUTPUT);
  digitalWrite(AUDIO_AMP_SD_GPIO, LOW);

  audioI2S.setPins(AUDIO_BCLK_GPIO, AUDIO_WS_GPIO, AUDIO_SPEAKER_DATA_GPIO, AUDIO_MIC_DATA_GPIO);
  audioReady = audioI2S.begin(I2S_MODE_STD, AUDIO_SAMPLE_RATE, I2S_DATA_BIT_WIDTH_32BIT,
                              I2S_SLOT_MODE_MONO, I2S_STD_SLOT_LEFT);
  return audioReady;
}

bool AUDIO_Test(Print &output) {
  if (!audioReady) {
    output.println("ERR audio I2S unavailable");
    return false;
  }

  digitalWrite(AUDIO_AMP_SD_GPIO, HIGH);
  delay(10);
  output.println("Audio test: speaker tone");
  if (!playTestTone()) {
    digitalWrite(AUDIO_AMP_SD_GPIO, LOW);
    output.println("ERR audio speaker write failed");
    return false;
  }

  output.println("Speak now; recording 2 seconds");
  output.flush();
  size_t wavSize = 0;
  uint8_t *wav = audioI2S.recordWAV(AUDIO_RECORD_SECONDS, &wavSize);
  if (!wav) {
    digitalWrite(AUDIO_AMP_SD_GPIO, LOW);
    output.println("ERR audio recording failed");
    return false;
  }
  if (!wavSize) {
    free(wav);
    digitalWrite(AUDIO_AMP_SD_GPIO, LOW);
    output.println("ERR audio recording failed");
    return false;
  }

  output.println("Playing recording");
  audioI2S.playWAV(wav, wavSize);
  free(wav);
  digitalWrite(AUDIO_AMP_SD_GPIO, LOW);
  output.println("Audio test complete");
  return true;
}
