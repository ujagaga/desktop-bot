#include "wake_word.h"

#include <ESP_TF.h>
#include <microfrontend.h>
#include "tensorflow/lite/micro/micro_allocator.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_resource_variable.h"
#include "tensorflow/lite/schema/schema_generated.h"

#include "audio.h"
#include "wake_word_model.h"

// Inference pipeline follows ESPHome micro_wake_word; the frontend settings
// must match the ones every microWakeWord model was trained with.
namespace {
constexpr int FEATURE_SIZE = 40;
constexpr int FEATURE_STEP_MS = 10;
constexpr int SAMPLES_PER_STEP = 16000 * FEATURE_STEP_MS / 1000;
constexpr uint8_t PROBABILITY_CUTOFF = 247;  // 0.97 * 255
constexpr int SLIDING_WINDOW = 5;
constexpr int MIN_SLICES_BEFORE_DETECTION = 100;
constexpr size_t TENSOR_ARENA_SIZE = 40 * 1024;  // manifest says 22860; spare for ESP-NN scratch
constexpr size_t VARIABLE_ARENA_SIZE = 1024;

FrontendState frontendState;
tflite::MicroMutableOpResolver<20> opResolver;
tflite::MicroInterpreter *interpreter = nullptr;
alignas(16) uint8_t tensorArena[TENSOR_ARENA_SIZE];
alignas(16) uint8_t variableArena[VARIABLE_ARENA_SIZE];

uint8_t probabilities[SLIDING_WINDOW];
int probabilityIndex = 0;
int strideStep = 0;
int ignoreSlices = -MIN_SLICES_BEFORE_DETECTION;

const char *initError = "not started";
volatile bool detectionPending = false;
volatile uint32_t detectionCount = 0;
volatile uint32_t inferenceCount = 0;
volatile uint8_t maxProbability = 0;
volatile uint32_t maxInferenceUs = 0;
volatile int16_t peakSample = 0;

bool setupFrontend() {
  FrontendConfig config;
  config.window.size_ms = 30;
  config.window.step_size_ms = FEATURE_STEP_MS;
  config.filterbank.num_channels = FEATURE_SIZE;
  config.filterbank.lower_band_limit = 125.0f;
  config.filterbank.upper_band_limit = 7500.0f;
  config.noise_reduction.smoothing_bits = 10;
  config.noise_reduction.even_smoothing = 0.025f;
  config.noise_reduction.odd_smoothing = 0.06f;
  config.noise_reduction.min_signal_remaining = 0.05f;
  config.pcan_gain_control.enable_pcan = 1;
  config.pcan_gain_control.strength = 0.95f;
  config.pcan_gain_control.offset = 80.0f;
  config.pcan_gain_control.gain_bits = 21;
  config.log_scale.enable_log = 1;
  config.log_scale.scale_shift = 6;
  return FrontendPopulateState(&config, &frontendState, 16000);
}

bool setupModel() {
  const tflite::Model *model = tflite::GetModel(WAKE_WORD_MODEL);
  if (model->version() != TFLITE_SCHEMA_VERSION) return false;

  opResolver.AddCallOnce();
  opResolver.AddVarHandle();
  opResolver.AddReshape();
  opResolver.AddReadVariable();
  opResolver.AddStridedSlice();
  opResolver.AddConcatenation();
  opResolver.AddAssignVariable();
  opResolver.AddConv2D();
  opResolver.AddMul();
  opResolver.AddAdd();
  opResolver.AddMean();
  opResolver.AddFullyConnected();
  opResolver.AddLogistic();
  opResolver.AddQuantize();
  opResolver.AddDepthwiseConv2D();
  opResolver.AddAveragePool2D();
  opResolver.AddMaxPool2D();
  opResolver.AddPad();
  opResolver.AddPack();
  opResolver.AddSplitV();

  tflite::MicroAllocator *allocator = tflite::MicroAllocator::Create(variableArena, VARIABLE_ARENA_SIZE);
  tflite::MicroResourceVariables *variables = tflite::MicroResourceVariables::Create(allocator, 20);
  static tflite::MicroInterpreter staticInterpreter(model, opResolver, tensorArena, TENSOR_ARENA_SIZE, variables);
  if (staticInterpreter.AllocateTensors() != kTfLiteOk) return false;

  TfLiteTensor *input = staticInterpreter.input(0);
  TfLiteTensor *output = staticInterpreter.output(0);
  if (input->type != kTfLiteInt8 || input->dims->size != 3 || input->dims->data[2] != FEATURE_SIZE ||
      output->type != kTfLiteUInt8) {
    return false;
  }
  interpreter = &staticInterpreter;
  return true;
}

void processFeatures(const FrontendOutput &frontendOutput) {
  TfLiteTensor *input = interpreter->input(0);
  const int stride = input->dims->data[1];
  int8_t *slot = input->data.int8 + FEATURE_SIZE * strideStep;
  for (size_t i = 0; i < frontendOutput.size; ++i) {
    // input = (feature * 256) / (25.6 * 26.0) - 128, as in ESPHome.
    int32_t value = ((frontendOutput.values[i] * 256) + 333) / 666 - 128;
    slot[i] = (int8_t)constrain(value, -128, 127);
  }
  if (++strideStep < stride) return;
  strideStep = 0;

  uint32_t startUs = micros();
  if (interpreter->Invoke() != kTfLiteOk) return;
  uint32_t elapsedUs = micros() - startUs;
  if (elapsedUs > maxInferenceUs) maxInferenceUs = elapsedUs;
  inferenceCount++;

  uint8_t probability = interpreter->output(0)->data.uint8[0];
  if (probability > maxProbability) maxProbability = probability;
  probabilityIndex = (probabilityIndex + 1) % SLIDING_WINDOW;
  probabilities[probabilityIndex] = probability;
}

// Per 10 ms feature slice, like ESPHome: require a quiet second after start or detection.
void checkDetection() {
  if (probabilities[probabilityIndex] < PROBABILITY_CUTOFF && ignoreSlices < 0) ignoreSlices++;
  if (ignoreSlices < 0) return;

  uint32_t sum = 0;
  for (uint8_t probability : probabilities) sum += probability;
  if (sum <= (uint32_t)PROBABILITY_CUTOFF * SLIDING_WINDOW) return;

  detectionCount++;
  detectionPending = true;
  memset(probabilities, 0, sizeof(probabilities));
  ignoreSlices = -MIN_SLICES_BEFORE_DETECTION;
}

void listenTask(void *) {
  int32_t raw[SAMPLES_PER_STEP];
  int16_t samples[SAMPLES_PER_STEP];
  for (;;) {
    size_t count = AUDIO_Read(raw, SAMPLES_PER_STEP);
    if (!count) {
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }
    for (size_t i = 0; i < count; ++i) {
      samples[i] = (int16_t)(raw[i] >> 16);  // INMP441: 24-bit data, MSB-aligned in 32 bits
      int16_t level = abs(samples[i]);
      if (level > peakSample) peakSample = level;
    }

    const int16_t *pending = samples;
    size_t remaining = count;
    while (remaining) {
      size_t processed = 0;
      FrontendOutput frontendOutput = FrontendProcessSamples(&frontendState, pending, remaining, &processed);
      pending += processed;
      remaining -= processed;
      if (frontendOutput.size == FEATURE_SIZE) {
        processFeatures(frontendOutput);
        checkDetection();
      }
    }
  }
}
}

bool WAKEWORD_Init() {
  if (!setupFrontend()) {
    initError = "frontend allocation failed";
    return false;
  }
  if (!setupModel()) {
    initError = "model setup failed";
    return false;
  }
  // Core 0: keeps inference bursts out of loop() timing (motor rotate, UI).
  if (xTaskCreatePinnedToCore(listenTask, "wakeword", 8192, nullptr, 2, nullptr, 0) != pdPASS) {
    initError = "task start failed";
    return false;
  }
  initError = nullptr;
  return true;
}

bool WAKEWORD_TakeDetection() {
  if (!detectionPending) return false;
  detectionPending = false;
  return true;
}

bool WAKEWORD_PrintStatus(Print &output) {
  if (initError) {
    output.printf("ERR wake word %s\n", initError);
    return false;
  }
  output.printf("WAKEWORD detections=%lu inferences=%lu max_prob=%u/255 max_infer=%luus peak=%d arena=%u\n",
                (unsigned long)detectionCount, (unsigned long)inferenceCount, maxProbability,
                (unsigned long)maxInferenceUs, peakSample, (unsigned)interpreter->arena_used_bytes());
  // Report maxima since the previous status call.
  maxProbability = 0;
  maxInferenceUs = 0;
  peakSample = 0;
  return true;
}
