#include "gameplay_hooks.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

REXCVAR_DEFINE_BOOL(gow3_60fps, true, "Game",
                   "Unlock the game's original 30 FPS cap; framerate_limit sets the ceiling")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(gow3_movie_pacing, true, "Game",
                   "Use millisecond idle-render polling for prerecorded movie pacing")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace {
bool supported_image = false;
bool enabled = false;
bool movie_pacing_enabled = false;

uint32_t ReadInstruction(const uint8_t* base, uint32_t address) {
  const auto* p = base + address;
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
}  // namespace

// Native equivalent of the Unlock FPS patch by illusion and boma, for module
// EF4AD5E9DD1982ED. Guest instruction writes alone cannot patch static C++ code.
// https://github.com/xenia-canary/game-patches/blob/main/patches/4D5308AB%20-%20Gears%20of%20War%203.patch.toml
void Gow3ConfigureFrameRate(const uint8_t* guest_base) {
  movie_pacing_enabled = REXCVAR_GET(gow3_movie_pacing) &&
      ReadInstruction(guest_base, 0x823F4148) == 0xC00A06EC &&
      ReadInstruction(guest_base, 0x823F414C) == 0xFF1F0000 &&
      ReadInstruction(guest_base, 0x820CDB5C) == 0x3CCCCCCD;
  REXLOG_INFO("Gears movie pacing: {}", movie_pacing_enabled ? "1 ms polling" : "original polling");
  supported_image = ReadInstruction(guest_base, 0x8253D9B0) == 0xC02304C8 &&
                    ReadInstruction(guest_base, 0x82AF66BC) == 0x39600002;
  enabled = supported_image && REXCVAR_GET(gow3_60fps);
  if (!supported_image) {
    REXLOG_ERROR("Gears frame-rate patch disabled: executable instructions do not match");
    return;
  }
  REXLOG_INFO("Gears frame-rate patch: {}", REXCVAR_GET(gow3_60fps) ? "unlocked" : "original 30 FPS");
}

bool Gow3UncapFrameRate() {
  return enabled;
}

void Gow3SetPresentInterval(rex::ppc::Register& interval) {
  if (Gow3UncapFrameRate()) {
    interval.s64 = 1;
  }
}

void Gow3MoviePollInterval(rex::ppc::Register& interval) {
  // Change only this idle tick threshold, never the shared 0.025 constant.
  // Bink retains its own frame deadlines and audio synchronization. This also
  // lets other idle-render tickables run more often; it does not add game frames.
  if (movie_pacing_enabled) interval.f64 = 0.001;
}
