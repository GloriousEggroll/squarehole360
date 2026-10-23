#pragma once
// L3+R3 toggles once until both sticks are released. Any stick held while
// unfocused consumes the chord, so refocusing cannot complete a stale press.
struct MenuStickChordLatch {
  bool armed = true;
  bool Update(bool left_pressed, bool right_pressed, bool focused) {
    if (!left_pressed && !right_pressed) {
      armed = true;
      return false;
    }
    if (!focused) {
      armed = false;
      return false;
    }
    if (left_pressed && right_pressed && armed) {
      armed = false;
      return true;
    }
    return false;
  }
};
