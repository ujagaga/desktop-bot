// Top-level header so arduino-cli adds this library to the include path.
// Sources: tensorflow/tflite-micro @ 3f787dc, Apache-2.0. Only change: kissfft
// include paths in kiss_fft_int16.{h,cc} point at ESP_TF's kissfft/ copy.
#pragma once
#include "tensorflow/lite/experimental/microfrontend/lib/frontend.h"
#include "tensorflow/lite/experimental/microfrontend/lib/frontend_util.h"
