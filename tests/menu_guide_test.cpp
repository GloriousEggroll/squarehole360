#include <catch2/catch_test_macros.hpp>
#include "../src/menu_guide_latch.h"

TEST_CASE("Settings chord needs both stick buttons in either press order", "[guide-menu]") {
  MenuStickChordLatch chord;
  CHECK_FALSE(chord.Update(false, false, true));
  CHECK_FALSE(chord.Update(true, false, true));
  CHECK_FALSE(chord.Update(true, false, true));
  CHECK(chord.Update(true, true, true));
  CHECK_FALSE(chord.Update(false, false, true));
  CHECK_FALSE(chord.Update(false, true, true));
  CHECK_FALSE(chord.Update(false, true, true));
  CHECK(chord.Update(true, true, true));
}

TEST_CASE("Settings chord toggles once and rearms only after both releases", "[guide-menu]") {
  MenuStickChordLatch chord;
  CHECK(chord.Update(true, true, true));
  CHECK_FALSE(chord.Update(true, true, true));
  CHECK_FALSE(chord.Update(false, true, true));
  CHECK_FALSE(chord.Update(true, true, true));
  CHECK_FALSE(chord.Update(true, false, true));
  CHECK_FALSE(chord.Update(true, true, true));
  CHECK_FALSE(chord.Update(false, false, true));
  CHECK(chord.Update(true, true, true));
}

TEST_CASE("Held settings chord cannot toggle on refocus", "[guide-menu]") {
  MenuStickChordLatch chord;
  CHECK_FALSE(chord.Update(true, true, false));
  CHECK_FALSE(chord.Update(true, true, true));
  CHECK_FALSE(chord.Update(false, true, true));
  CHECK_FALSE(chord.Update(true, true, true));
  CHECK_FALSE(chord.Update(false, false, true));
  CHECK(chord.Update(true, true, true));
  CHECK_FALSE(chord.Update(true, true, false));
  CHECK_FALSE(chord.Update(true, true, true));
}

TEST_CASE("Unfocused partial chord is consumed until both buttons release", "[guide-menu]") {
  MenuStickChordLatch chord;
  CHECK_FALSE(chord.Update(true, false, false));
  CHECK_FALSE(chord.Update(true, false, true));
  CHECK_FALSE(chord.Update(true, true, true));
  CHECK_FALSE(chord.Update(false, false, false));
  CHECK_FALSE(chord.Update(false, true, false));
  CHECK_FALSE(chord.Update(true, true, true));
  CHECK_FALSE(chord.Update(false, false, true));
  CHECK(chord.Update(true, true, true));
}

TEST_CASE("An idle focus change or cleared input does not leave the chord stuck", "[guide-menu]") {
  MenuStickChordLatch chord;
  CHECK_FALSE(chord.Update(false, false, false));
  CHECK_FALSE(chord.Update(false, false, true));
  CHECK(chord.Update(true, true, true));
  // PollMenu supplies both false when a controller disconnects.
  CHECK_FALSE(chord.Update(false, false, true));
  CHECK_FALSE(chord.Update(true, false, true));
  CHECK(chord.Update(true, true, true));
}
