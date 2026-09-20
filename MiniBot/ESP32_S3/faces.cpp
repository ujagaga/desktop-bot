#include <Adafruit_GFX.h>
#include <pgmspace.h>
#include "faces.h"
#include "lcd.h"

#include "face_assets.h"
static_assert(sizeof(FACE_BITMAPS) / sizeof(FACE_BITMAPS[0]) == 16,
              "Expected all 16 face IDs");

bool FACE_DrawBitmap(Adafruit_GFX *display, const FaceBitmap &bitmap,
                     int16_t x, int16_t y) {
  if (!display || !bitmap.width || !bitmap.height || !bitmap.palette ||
      !bitmap.runs || x < 0 || y < 0 ||
      (int32_t)x + bitmap.width > display->width() ||
      (int32_t)y + bitmap.height > display->height()) return false;

  // Validate before writing anything; reject incomplete data or overflowing rows.
  uint32_t pixels = 0;
  const uint32_t total = (uint32_t)bitmap.width * bitmap.height;
  if (bitmap.runBytes > total) return false;
  for (size_t i = 0; i < bitmap.runBytes; ++i) {
    uint8_t length = (pgm_read_byte(bitmap.runs + i) >> 4) + 1;
    if (length > bitmap.width - pixels % bitmap.width || pixels + length > total)
      return false;
    pixels += length;
  }
  if (pixels != total) return false;

  display->startWrite();
  uint16_t column = 0, row = 0;
  for (size_t i = 0; i < bitmap.runBytes; ++i) {
    uint8_t run = pgm_read_byte(bitmap.runs + i);
    uint8_t length = (run >> 4) + 1;
    uint16_t color = pgm_read_word(bitmap.palette + (run & 0x0F));
    display->writeFastHLine(x + column, y + row, length, color);
    column += length;
    if (column == bitmap.width) {
      column = 0;
      ++row;
    }
  }
  display->endWrite();
  return true;
}

bool FACE_Show(int faceId) {
  if (faceId < 0 || faceId > 15) return false;
  Adafruit_GFX *display = LCD_GetGraphics();
  if (!display) return false;
  const FaceBitmap &bitmap = FACE_BITMAPS[faceId];
  if (bitmap.width > display->width() || bitmap.height > display->height()) return false;
  // Clear the previous content once, including margins outside smaller images.
  LCD_ShowText("");
  return FACE_DrawBitmap(display, bitmap, (display->width() - bitmap.width) / 2,
                         (display->height() - bitmap.height) / 2);
}
