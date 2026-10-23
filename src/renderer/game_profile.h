#pragma once
// Selects the executable profile this build targets. Each recompiled game is
// its own build, so the profile is compile-time and costs nothing at runtime.
#if !defined(GOW3_GAME_PROFILE) || GOW3_GAME_PROFILE == 3
#include "renderer/profiles/gears3.h"
#else
#error "Unknown GOW3_GAME_PROFILE"
#endif
