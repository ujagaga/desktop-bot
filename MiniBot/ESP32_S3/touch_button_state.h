#ifndef MINIBOT_TOUCH_BUTTON_STATE_H
#define MINIBOT_TOUCH_BUTTON_STATE_H
#include <stdint.h>

// No hardware dependencies: debounce, long hold, then release to request sleep.
class TouchButtonState {
 public:
  void reset() {
    blocked = true;
    candidate = stable = false;
    candidateSince = pressedAt = 0;
    armed = false;
    initialized = false;
  }

  bool update(bool pressed, uint32_t now, uint32_t holdMs) {
    if (!initialized || pressed != candidate) {
      initialized = true;
      candidate = pressed;
      candidateSince = now;
    }
    if (now - candidateSince < 60) return false;
    if (blocked) {
      if (!candidate) blocked = false;
      return false;
    }
    if (candidate != stable) {
      stable = candidate;
      if (stable) {
        pressedAt = candidateSince;
      } else {
        bool sleep = armed || candidateSince - pressedAt >= holdMs;
        armed = false;
        return sleep;
      }
    }
    if (stable && now - pressedAt >= holdMs) armed = true;
    return false;
  }

 private:
  bool blocked = true, candidate = false, stable = false;
  bool armed = false, initialized = false;
  uint32_t candidateSince = 0, pressedAt = 0;
};
#endif
