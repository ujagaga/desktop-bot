#include <Arduino.h>
#include <stdarg.h>
#include "logger.h"
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static char lines[20][192];
static unsigned head = 0, count = 0;
void LOG_init() {
  portENTER_CRITICAL(&mux);
  head = count = 0;
  portEXIT_CRITICAL(&mux);
}
void LOG_append(const char *message) {
  if (!message) return;
  char line[192];
  snprintf(line, sizeof(line), "[%lu] %s", millis() / 1000UL, message);
  portENTER_CRITICAL(&mux);
  strlcpy(lines[head], line, sizeof(lines[head]));
  head = (head + 1) % 20;
  if (count < 20) ++count;
  portEXIT_CRITICAL(&mux);
  // Keep logs in the HTTP ring buffer; Serial is the S3 command link.
}
void LOG_printf(const char *format, ...) {
  char message[160];
  va_list args;
  va_start(args, format);
  vsnprintf(message, sizeof(message), format, args);
  va_end(args);
  LOG_append(message);
}
size_t LOG_get(char *out, size_t capacity) {
  if (!out || !capacity) return 0;
  size_t used = 0;
  portENTER_CRITICAL(&mux);
  for (unsigned i = 0; i < count; ++i) {
    const char *line = lines[(head + 20 - count + i) % 20];
    size_t length = strlen(line);
    if (used + length + 1 >= capacity) break;
    memcpy(out + used, line, length);
    used += length;
    out[used++] = '\n';
  }
  out[used] = 0;
  portEXIT_CRITICAL(&mux);
  return used;
}
