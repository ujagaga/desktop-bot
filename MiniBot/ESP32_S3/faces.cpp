#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include "faces.h"
#include "LCD.h"

static const int16_t FACE_CX = 120;
static const int16_t FACE_EYE_Y = 104;
static const int16_t FACE_LEFT_X = 64;
static const int16_t FACE_RIGHT_X = 176;
static const uint16_t CYAN = 0x07FF;
static const uint16_t PINK = 0xF81F;

static void drawEye(Adafruit_GFX *display, int16_t x, int16_t y, bool wink = false) {
  if (wink) {
    display->drawLine(x - 28, y, x + 28, y, CYAN);
    display->drawLine(x - 23, y + 4, x + 23, y + 4, CYAN);
    return;
  }
  display->fillCircle(x, y, 32, CYAN);
  display->fillCircle(x, y, 19, ST77XX_BLACK);
  display->fillCircle(x + 8, y - 9, 8, ST77XX_WHITE);
}

static void drawBrows(Adafruit_GFX *display, int16_t y, int16_t tilt) {
  display->drawLine(FACE_LEFT_X - 28, y + tilt, FACE_LEFT_X + 28, y - tilt, CYAN);
  display->drawLine(FACE_RIGHT_X - 28, y - tilt, FACE_RIGHT_X + 28, y + tilt, CYAN);
}

static void drawFace(Adafruit_GFX *display, int faceId) {
  drawEye(display, FACE_LEFT_X, FACE_EYE_Y);
  drawEye(display, FACE_RIGHT_X, FACE_EYE_Y);

  switch (faceId) {
    case 0:
      display->drawLine(98, 165, 142, 165, CYAN);
      break;
    case 1:
      display->fillRoundRect(80, 153, 80, 34, 17, CYAN);
      break;
    case 2:
      drawBrows(display, 55, 8);
      display->drawLine(98, 178, 142, 178, CYAN);
      display->drawLine(105, 174, 120, 165, CYAN);
      display->drawLine(135, 174, 120, 165, CYAN);
      break;
    case 3:
      display->fillRoundRect(80, 143, 80, 50, 24, CYAN);
      display->fillCircle(120, 198, 15, CYAN);
      break;
    case 4:
      display->fillCircle(FACE_CX, 178, 18, CYAN);
      break;
    case 5:
      drawEye(display, FACE_LEFT_X, FACE_EYE_Y, true);
      display->drawLine(148, 168, 196, 168, CYAN);
      break;
    case 6:
      drawBrows(display, 52, 12);
      display->drawLine(94, 178, 146, 178, CYAN);
      break;
    case 7:
      drawBrows(display, 52, 7);
      display->drawLine(96, 177, 112, 168, CYAN);
      display->drawLine(112, 168, 128, 177, CYAN);
      display->drawLine(128, 177, 144, 168, CYAN);
      break;
    case 8:
      drawEye(display, FACE_LEFT_X, FACE_EYE_Y, true);
      drawEye(display, FACE_RIGHT_X, FACE_EYE_Y, true);
      display->drawLine(96, 180, 144, 180, CYAN);
      break;
    case 9:
      display->fillRoundRect(80, 148, 80, 46, 22, CYAN);
      display->fillCircle(34, 166, 7, PINK);
      display->fillCircle(206, 166, 7, PINK);
      break;
    case 10:
      display->drawLine(98, 170, 112, 179, CYAN);
      display->drawLine(112, 179, 128, 170, CYAN);
      display->drawLine(128, 170, 142, 179, CYAN);
      display->fillCircle(42, 170, 6, PINK);
      display->fillCircle(198, 170, 6, PINK);
      break;
    case 11:
      display->drawLine(96, 176, 110, 183, CYAN);
      display->drawLine(110, 183, 124, 176, CYAN);
      display->drawLine(124, 176, 138, 183, CYAN);
      display->drawLine(138, 183, 152, 176, CYAN);
      break;
    case 12:
      drawBrows(display, 48, 0);
      display->drawLine(96, 183, 144, 183, CYAN);
      break;
    case 13:
      display->drawLine(102, 160, 112, 182, PINK);
      display->drawLine(112, 182, 128, 160, PINK);
      display->drawLine(128, 160, 140, 182, PINK);
      break;
    case 14:
      display->drawRoundRect(24, 66, 84, 64, 10, 0x001F);
      display->drawRoundRect(132, 66, 84, 64, 10, 0x001F);
      display->drawLine(108, 94, 132, 94, 0x001F);
      display->drawLine(42, 116, 82, 80, ST77XX_WHITE);
      display->drawLine(158, 116, 198, 80, ST77XX_WHITE);
      display->drawLine(96, 165, 144, 165, CYAN);
      break;
    case 15:
      display->fillRoundRect(102, 143, 36, 60, 18, CYAN);
      break;
  }
}

bool FACE_Show(int faceId) {
  if (faceId < 0 || faceId > 15) return false;
  LCD_ShowText("");
  Adafruit_GFX *display = LCD_GetGraphics();
  if (display == nullptr) return false;
  drawFace(display, faceId);
  return true;
}
