#ifndef FACE_BITMAP_H
#define FACE_BITMAP_H

#include <stdint.h>
#include <stddef.h>

// Flash-resident RGB565 palette (16 entries) and row-major run-length data.
// Each byte: upper nibble = run length minus one, lower nibble = palette index.
// Runs never cross a row. Dimensions are native pixels, without scaling.
struct FaceBitmap {
  uint16_t width;
  uint16_t height;
  const uint16_t *palette;
  const uint8_t *runs;
  size_t runBytes;
};

#endif
