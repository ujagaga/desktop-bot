#ifndef MINIBOT_TAP_SEQUENCE_H
#define MINIBOT_TAP_SEQUENCE_H
#include <stdint.h>

// Group separate, debounced impulses. Wait for quiet before reporting, so a
// double tap is not reported before a possible third tap.
class TapSequence {
 public:
  void reset() { count = 0; last = 0; }
  void tap(uint32_t now) {
    if (count && now - last < 120) return;
    if (count && now - last > 500) count = 0;
    if (count < 4) ++count;
    last = now;
  }
  uint8_t finish(uint32_t now) {
    if (!count || now - last <= 500) return 0;
    uint8_t result = count;
    reset();
    return result;
  }
 private:
  uint8_t count = 0;
  uint32_t last = 0;
};
#endif
