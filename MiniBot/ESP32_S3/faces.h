#ifndef FACES_H
#define FACES_H

#include "face_bitmap.h"

class Adafruit_GFX;

// Draw exactly width x height pixels; reject images outside the display.
// Does not clear or modify pixels outside the image rectangle.
bool FACE_DrawBitmap(Adafruit_GFX *display, const FaceBitmap &bitmap,
                     int16_t x, int16_t y);

bool FACE_Show(int faceId);

#endif
