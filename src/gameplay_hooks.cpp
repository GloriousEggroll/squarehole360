#include "gameplay_hooks.h"
#include "gow3_renderer_mode.h"

#include <rex/cvar.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/input/sdl/sdl_input_driver.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

#include <algorithm>
#include <string>
#include <utility>

REXCVAR_DEFINE_BOOL(gow3_60fps, true, "Game",
                   "Unlock the game's original 30 FPS cap; framerate_limit sets the ceiling")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(gow3_movie_pacing, true, "Game",
                   "Use millisecond idle-render polling for prerecorded movie pacing")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_INT32(gow3_render_height, 1080, "Game",
                    "The game's own render height (16:9 width); 720 is the console's, 1080 the maximum")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace {
bool supported_image = false;
uint8_t* screen_size_base = nullptr;
bool enabled = false;
bool movie_pacing_enabled = false;
bool mouse_look_enabled = false;

// The engine's MouseX and MouseY key names (FName, index and number), set up
// at startup from the UTF-16 names at 0x82146AA8 and 0x82146AB8.
constexpr uint32_t kKeyMouseX = 0x834440C8;
constexpr uint32_t kKeyMouseY = 0x834440D0;

bool HasUtf16Name(const uint8_t* base, uint32_t address, const char* name) {
  for (;; ++name, address += 2) {
    if (base[address] != 0 || base[address + 1] != uint8_t(*name)) return false;
    if (!*name) return true;
  }
}

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
  // Only the native renderer models EDRAM beyond the console's 10 MB.
  if (REXCVAR_GET(gow3_render_height) != 720 &&
      ParseGow3RendererMode(REXCVAR_QUERY(std::string, gow3_renderer)) == Gow3RendererMode::Native &&
      ReadInstruction(guest_base, 0x82960D04) == 0x817E0000)
    screen_size_base = const_cast<uint8_t*>(guest_base);
  movie_pacing_enabled = REXCVAR_GET(gow3_movie_pacing) &&
      ReadInstruction(guest_base, 0x823F4148) == 0xC00A06EC &&
      ReadInstruction(guest_base, 0x823F414C) == 0xFF1F0000 &&
      ReadInstruction(guest_base, 0x820CDB5C) == 0x3CCCCCCD;
  REXLOG_INFO("Gears movie pacing: {}", movie_pacing_enabled ? "1 ms polling" : "original polling");
  mouse_look_enabled = ReadInstruction(guest_base, 0x8296DCB8) == 0x7D8802A6 &&
      HasUtf16Name(guest_base, 0x82146AA8, "MouseX") && HasUtf16Name(guest_base, 0x82146AB8, "MouseY");
  if (!mouse_look_enabled) REXLOG_WARN("Gears mouse look disabled: executable does not match");
  Gow3ConfigureInputPrompts(guest_base);
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
  REXLOG_INFO("Gears render size: {}x{}", width, height);
}

// FXenonViewport::ProcessInput (0x8296DCB8, r3 = viewport, f1 = DeltaTime)
// passes each controller's sticks and triggers to the viewport client's
// InputAxis. Afterwards pass mouse motion as the MouseX and MouseY keys, as the
// engine's PC viewports do; the game binds them to its PlayerInput mouse look.
void Gow3RefreshInputPrompts(PPCContext& ctx, uint8_t* base);

REX_EXTERN(__imp__sub_8296DCB8);
REX_EXTERN(sub_8296DCB8) {
  const uint32_t viewport = ctx.r3.u32;
  const double delta_time = ctx.f1.f64;
  __imp__sub_8296DCB8(ctx, base);
  Gow3RefreshInputPrompts(ctx, base);
  if (!mouse_look_enabled) return;
  for (uint32_t controller = 0; controller < 4; ++controller) {
    double dx = 0.0, dy = 0.0, gyro_dx = 0.0, gyro_dy = 0.0;
    const bool mouse = rex::input::mnk::TakeMouseLook(controller, dx, dy);
    if (rex::input::sdl::TakeGyroLook(controller, gyro_dx, gyro_dy)) {
      // PlayerInput turns MouseSensitivity (60) rotation units, 1/65536 of a
      // turn, per mouse count.
      constexpr double kCountsPerDegree = 65536.0 / 360.0 / 60.0;
      dx += gyro_dx * kCountsPerDegree;
      dy += gyro_dy * kCountsPerDegree;
    } else if (!mouse) {
      continue;
    }
    const uint32_t client = ReadInstruction(base, viewport + 52);
    if (!client) return;
    // UGameViewportClient::InputAxis(Viewport, ControllerId, Key, Delta,
    // DeltaTime, bGamepad), the client's vtable slot 7; mouse Y is up-positive.
    const uint32_t input_axis = ReadInstruction(base, ReadInstruction(base, client) + 28);
    for (const auto& [key, delta] : {std::pair{kKeyMouseX, dx}, std::pair{kKeyMouseY, -dy}}) {
      if (delta == 0.0) continue;
      ctx.r3.u64 = client;
      ctx.r4.u64 = viewport == 4 ? 0 : viewport;
      ctx.r5.u64 = controller;
      ctx.r6.u64 = uint64_t(ReadInstruction(base, key)) << 32 | ReadInstruction(base, key + 4);
      ctx.f1.f64 = float(delta);
      ctx.f2.f64 = delta_time;
      ctx.r9.u64 = 0;
      rex::runtime::ResolveIndirectFunction(input_axis)(ctx, base);
    }
  }
}
