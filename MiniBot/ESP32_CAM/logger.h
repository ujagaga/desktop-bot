#pragma once
#include <stddef.h>
#define LOG_CAPACITY 4096
void LOG_init();
void LOG_append(const char *message);
void LOG_printf(const char *format, ...) __attribute__((format(printf, 1, 2)));
size_t LOG_get(char *out, size_t capacity);
