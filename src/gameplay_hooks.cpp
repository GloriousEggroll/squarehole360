#include "gameplay_hooks.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

#include <algorithm>
#include <utility>

REXCVAR_DEFINE_BOOL(gow3_60fps, true, "Game",
                   "Unlock the game's original 30 FPS cap; framerate_limit sets the ceiling")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(gow3_movie_pacing, true, "Game",
                   "Use millisecond idle-render polling for prerecorded movie pacing")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_INT32(gow3_render_height, 720, "Game",
                    "Experimental: the game's own render height (16:9 width); 720 is the original")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace {
bool supported_image = false;
uint8_t* screen_size_base = nullptr;
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
  // lwz r11,0(r30) loads the stored screen width into CreateDevice's
  // presentation parameters.
  if (REXCVAR_GET(gow3_render_height) != 720 &&
      ReadInstruction(guest_base, 0x82960D04) == 0x817E0000)
    screen_size_base = const_cast<uint8_t*>(guest_base);
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

// The engine stores its screen size (height fixed at 720, width from the
// aspect ratio) at 0x83392428/0x8339242C before CreateDevice; every later
// reader, including the back buffer parameters, takes it from there.
namespace {
// A multiple of 8, the GPU's resolve block size.
uint32_t RenderHeight() { return uint32_t(std::clamp(REXCVAR_GET(gow3_render_height), 360, 2160)) & ~7u; }
uint32_t RenderWidth() { return (RenderHeight() * 16 / 9 + 4) & ~7u; }
}  // namespace

// Viewport rectangles some engine objects initialize to a literal 1280x720.
void Gow3ScreenWidthConstant(rex::ppc::Register& value) {
  if (screen_size_base) value.u64 = RenderWidth();
}
void Gow3ScreenHeightConstant(rex::ppc::Register& value) {
  if (screen_size_base) value.u64 = RenderHeight();
}

// Scaleform Stage.scaleMode setter: the menus ask for noScale (0), which draws
// the 1280x720 stage at 1:1 from the top-left; showAll (1) scales it to fit.
void Gow3ScaleformScaleMode(rex::ppc::Register& mode) {
  if (screen_size_base && mode.u32 == 0) mode.u64 = 1;
}

void Gow3ScreenWidthFloat(rex::ppc::Register& value) {
  if (screen_size_base) value.f64 = RenderWidth();
}
void Gow3ScreenHeightFloat(rex::ppc::Register& value) {
  if (screen_size_base) value.f64 = RenderHeight();
}

void Gow3OverrideScreenSize() {
  if (!screen_size_base) return;
  const uint32_t height = RenderHeight(), width = RenderWidth();
  const auto write = [](uint32_t address, uint32_t value) {
    auto* p = screen_size_base + address;
    p[0] = uint8_t(value >> 24); p[1] = uint8_t(value >> 16); p[2] = uint8_t(value >> 8); p[3] = uint8_t(value);
  };
  write(0x83392428, width);
  write(0x8339242C, height);
  // The system settings' ResX/ResY and the client's display size, which the
  // UI lays out in; written only while they still hold the original 1280x720.
  for (const uint32_t address : {0x8343FC24u, 0x83416C60u}) {
    if (ReadInstruction(screen_size_base, address) == 1280 &&
        ReadInstruction(screen_size_base, address + 4) == 720) {
      write(address, width);
      write(address + 4, height);
    } else {
      REXLOG_WARN("Gears render size: no 1280x720 at {:08X}", address);
    }
  }
  REXLOG_INFO("Gears render size: {}x{} (experimental)", width, height);
}
