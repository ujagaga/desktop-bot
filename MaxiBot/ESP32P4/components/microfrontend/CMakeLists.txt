# TFLite Micro audio frontend (spectrogram features) for microWakeWord.
# Copied from MiniBot/libraries/microfrontend (tensorflow/tflite-micro @ 3f787dc,
# Apache-2.0); kissfft comes from esp-tflite-micro.
file(GLOB srcs "tensorflow/lite/experimental/microfrontend/lib/*.c"
               "tensorflow/lite/experimental/microfrontend/lib/*.cc")
idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "."
                       REQUIRES espressif__esp-tflite-micro)
