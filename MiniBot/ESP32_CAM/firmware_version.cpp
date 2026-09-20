#include "firmware_version.h"

#include <stdio.h>
#include <string.h>

bool parseFirmwareVersion(const char *config, uint32_t &version) {
  bool found = false;
  while (*config) {
    const char *end = strchr(config, '\n');
    size_t length = end ? (size_t)(end - config) : strlen(config);
    if (length >= 256) return false;
    char line[256], name[64], value[64];
    memcpy(line, config, length);
    line[length] = '\0';
    int consumed = 0;
    if (sscanf(line, " # define %63s %63s %n", name, value, &consumed) == 2 &&
        strcmp(name, "FIRMWARE_VERSION") == 0) {
      if (found || (line[consumed] && strncmp(line + consumed, "//", 2) != 0)) return false;
      uint32_t parsed = 0;
      for (const char *p = value; *p; ++p) {
        if (*p < '0' || *p > '9' || parsed > (UINT32_MAX - (*p - '0')) / 10) return false;
        parsed = parsed * 10 + (*p - '0');
      }
      if (!parsed) return false;
      version = parsed;
      found = true;
    }
    if (!end) break;
    config = end + 1;
  }
  return found;
}
