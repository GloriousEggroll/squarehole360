#include "gameplay_hooks.h"
#include "gow3_renderer_mode.h"
#include "gow3_tmap_find.h"
#include "generated/default/gow3_init.h"

#include <rex/cvar.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/input/sdl/sdl_input_driver.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/network.h>
#include <rex/system/xmemory.h>
#include <rex/ui/flags.h>
#include <rex/ui/window.h>

#include <algorithm>
#include <cmath>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

REXCVAR_DEFINE_BOOL(gow3_60fps, true, "Game",
                   "Unlock the game's original 30 FPS cap; framerate_limit sets the ceiling")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(gow3_movie_pacing, true, "Game",
                   "Use millisecond idle-render polling for prerecorded movie pacing")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_INT32(gow3_fov, 65, "Game",
                    "Field of view in degrees, horizontal at 16:9; the game's is 65. Scales every camera, so aiming still zooms in");

REXCVAR_DEFINE_STRING(gow3_aspect_ratio, "auto", "Game",
                     "Render aspect ratio: auto (the window's), 16:9, 16:10, 21:9 or 32:9. Wider than 16:9 shows more at the sides")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_INT32(gow3_render_height, 1080, "Game",
                    "Internal render height (width from gow3_aspect_ratio): 720 (the console's), 1080, or 1440 "
                    "(the game's 720p layout drawn at twice the size by the native renderer)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_INT32(gow3_texture_pool_mb, 0, "Game",
                     "Texture streaming pool in MB, from the game's guest memory (0: the game's 145)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gow3_texture_pool_stats, false, "Game",
                    "Log the texture pool and guest physical memory every second")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

int Gow3GuestRenderHeight() {
  const int height = REXCVAR_GET(gow3_render_height);
  return height >= 1440 ? 720 : std::clamp(height, 360, 1080);
}

namespace {
bool supported_image = false;
uint8_t* screen_size_base = nullptr;
bool enabled = false;
bool movie_pacing_enabled = false;
bool departed_connection_hook = false;
bool texture_pool_hooks = false;
bool mouse_look_enabled = false;
bool projection_hooks = false;
uint8_t* guest_memory = nullptr;
rex::ui::Window* display_window = nullptr;

// The engine's MouseX and MouseY key names (FName, index and number), set up
// at startup from the UTF-16 names at 0x821471D0 and 0x821471E0.
constexpr uint32_t kKeyMouseX = 0x83454898;
constexpr uint32_t kKeyMouseY = 0x834548A0;

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
// EF4AD5E9DD1982ED (the disc executable; the sites are moved to Title Update 6).
// Guest instruction writes alone cannot patch static C++ code.
// https://github.com/xenia-canary/game-patches/blob/main/patches/4D5308AB%20-%20Gears%20of%20War%203.patch.toml
void Gow3ConfigureFrameRate(const uint8_t* guest_base) {
  // lwz r11,0(r30) loads the stored screen width into CreateDevice's
  // presentation parameters.
  // Only the native renderer models EDRAM beyond the console's 10 MB.
  guest_memory = const_cast<uint8_t*>(guest_base);
  if ((Gow3GuestRenderHeight() != 720 || REXCVAR_GET(gow3_aspect_ratio) != "16:9") &&
      ParseGow3RendererMode(REXCVAR_QUERY(std::string, gow3_renderer)) == Gow3RendererMode::Native &&
      ReadInstruction(guest_base, 0x82971CDC) == 0x817E0000) {
    screen_size_base = const_cast<uint8_t*>(guest_base);
    // The video mode stays 1280x720; show the frame at the render aspect.
    REXCVAR_SET(present_frontbuffer_aspect, true);
  }
  movie_pacing_enabled = REXCVAR_GET(gow3_movie_pacing) &&
      ReadInstruction(guest_base, 0x824042E0) == 0xC00A06EC &&
      ReadInstruction(guest_base, 0x824042E4) == 0xFF1F0000 &&
      ReadInstruction(guest_base, 0x820CDBCC) == 0x3CCCCCCD;
  REXLOG_INFO("Gears movie pacing: {}", movie_pacing_enabled ? "1 ms polling" : "original polling");
  mouse_look_enabled = ReadInstruction(guest_base, 0x8297EE10) == 0x7D8802A6 &&
      HasUtf16Name(guest_base, 0x821471D0, "MouseX") && HasUtf16Name(guest_base, 0x821471E0, "MouseY");
  if (!mouse_look_enabled) REXLOG_WARN("Gears mouse look disabled: executable does not match");
  Gow3ConfigureInputPrompts(guest_base);
  // bl FPerspectiveMatrix at the projection hook site.
  projection_hooks = ReadInstruction(guest_base, 0x8262CE30) == 0x48004299 &&
      ReadInstruction(guest_base, 0x826310C8) == 0x7D8802A6 &&
      ReadInstruction(guest_base, 0x8262CF14) == 0x480040B5;
  if (!projection_hooks) REXLOG_WARN("Gears field of view disabled: executable does not match");
  // lwz r11,276(r31) in UNetConnection::Tick; lwz r3,20420(r3) in the
  // UIpConnection accessor for the remote address.
  departed_connection_hook = ReadInstruction(guest_base, 0x828CE13C) == 0x817F0114 &&
      ReadInstruction(guest_base, 0x829088C8) == 0x80634FC4;
  if (!departed_connection_hook) REXLOG_WARN("Gears network drop disabled: executable does not match");
  // lis r11 / lis r4, right after bl GConfig::GetInt with the PoolSize key.
  texture_pool_hooks = ReadInstruction(guest_base, 0x82978858) == 0x4B93F1B1 &&
      ReadInstruction(guest_base, 0x8297885C) == 0x3D60821D &&
      ReadInstruction(guest_base, 0x824A96F8) == 0x4BE0E311 &&
      ReadInstruction(guest_base, 0x824A96FC) == 0x3C808213 && HasUtf16Name(guest_base, 0x82130048, "PoolSize");
  if (!texture_pool_hooks) REXLOG_WARN("Gears texture pool setting disabled: executable does not match");
  supported_image = ReadInstruction(guest_base, 0x8254E4A8) == 0xC02304C8 &&
                    ReadInstruction(guest_base, 0x82B09E5C) == 0x39600002;
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

// The title only learns a remote game is gone through its connection timeout.
// Make that test true for the address the network layer knows has left; the
// title then closes the connection the normal way.
void Gow3ExpireDepartedConnection(rex::ppc::Register& connection) {
  if (!departed_connection_hook) return;
  auto* object = guest_memory + connection.u32;
  const uint32_t ip = uint32_t(object[20420]) << 24 | uint32_t(object[20421]) << 16 |
                      uint32_t(object[20422]) << 8 | object[20423];
  if (!rex::system::xam::Network::Get().PeerGone(ip)) return;
  REXLOG_INFO("Network: closing the connection to {}.{}.{}.{}; that player has left", object[20420],
              object[20421], object[20422], object[20423]);
  const uint64_t long_ago = std::bit_cast<uint64_t>(-1.0e9);
  for (int i = 0; i < 8; ++i) object[276 + i] = uint8_t(long_ago >> (56 - 8 * i));
}

namespace {
uint32_t ReadBigEndian32(const uint8_t* at) {
  return uint32_t(at[0]) << 24 | uint32_t(at[1]) << 16 | uint32_t(at[2]) << 8 | at[3];
}

void OverrideTexturePool(uint32_t address, const char* use) {
  const int megabytes = REXCVAR_GET(gow3_texture_pool_mb);
  if (!texture_pool_hooks || megabytes <= 0) return;
  auto* value = guest_memory + address;
  REXLOG_INFO("Texture streaming {}: {} MB (game's setting: {} MB)", use, megabytes, ReadBigEndian32(value));
  for (int i = 0; i < 4; ++i) value[i] = uint8_t(uint32_t(megabytes) >> (24 - 8 * i));
}

void LogTexturePool(uint32_t pool) {
  std::thread([pool] {
    const rex::memory::BaseHeap* heaps[3] = {REX_KERNEL_MEMORY()->LookupHeapByType(true, 0x1000),
                                             REX_KERNEL_MEMORY()->LookupHeapByType(true, 0x10000),
                                             REX_KERNEL_MEMORY()->LookupHeapByType(true, 0x1000000)};
    for (;;) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      uint32_t unreserved = 0, reserved = 0, used = 0, reserved_bytes = 0;
      REX_KERNEL_MEMORY()->GetHeapsPageStatsSummary(heaps, std::size(heaps), unreserved, reserved, used,
                                                    reserved_bytes);
      const auto* object = guest_memory + pool;
      REXLOG_INFO("Texture pool: size={} +40={} +44={} +48={} physical used={} MB of 512",
                  ReadBigEndian32(object + 276), ReadBigEndian32(object + 40), ReadBigEndian32(object + 44),
                  ReadBigEndian32(object + 48), used / 256);
    }
  }).detach();
}
}  // namespace

// Where the pool is allocated (sub_829787F0): the read value is on the stack.
void Gow3TexturePoolSize(rex::ppc::Register& stack, rex::ppc::Register& pool) {
  OverrideTexturePool(stack.u32 + 80, "pool");
  static std::once_flag stats;
  if (REXCVAR_GET(gow3_texture_pool_stats)) std::call_once(stats, LogTexturePool, pool.u32);
}

// The streaming manager's budget (sub_824A9338): its own copy of the value.
void Gow3TextureStreamingBudget(rex::ppc::Register& value) { OverrideTexturePool(value.u32, "budget"); }

void Gow3MoviePollInterval(rex::ppc::Register& interval) {
  // Change only this idle tick threshold, never the shared 0.025 constant.
  // Bink retains its own frame deadlines and audio synchronization. This also
  // lets other idle-render tickables run more often; it does not add game frames.
  if (movie_pacing_enabled) interval.f64 = 0.001;
}

// The engine stores its screen size (height fixed at 720, width from the
// aspect ratio) at 0x833A2BD4/0x833A2BD8 before CreateDevice; every later
// reader, including the back buffer parameters, takes it from there.
namespace {
constexpr double kGameAspect = 16.0 / 9.0;

double DisplayAspect() {
  const auto& choice = REXCVAR_GET(gow3_aspect_ratio);
  if (choice == "16:10") return 1.6;
  if (choice == "21:9") return 64.0 / 27.0;  // 2560x1080, 5120x2160; 3440x1440 is close
  if (choice == "32:9") return 32.0 / 9.0;
  if (choice == "auto" && display_window) {
    const double width = display_window->GetActualPhysicalWidth();
    const double height = display_window->GetActualPhysicalHeight();
    if (width > 0 && height > 0) return std::clamp(width / height, 1.25, 4.0);
  }
  return kGameAspect;
}

struct RenderSize { uint32_t width, height; };
// Height from gow3_render_height, width from the aspect ratio, both multiples
// of 8 (the GPU's resolve block size). Wider sizes keep at most 1920x1080's
// pixels, the largest the EDRAM model has been validated for; the presenter
// scales the image to the window. Fixed on first use. 1440p is the 720p
// layout scaled by the renderer (Gow3GuestRenderHeight).
RenderSize RenderSizeFor() {
  static const RenderSize size = [] {
    const double aspect = DisplayAspect();
    double height = Gow3GuestRenderHeight();
    height = std::min(height, std::sqrt(1920.0 * 1080.0 / std::max(aspect, kGameAspect)));
    const uint32_t h = uint32_t(height) & ~7u;
    return RenderSize{std::max(8u, uint32_t(h * aspect + 4) & ~7u), h};
  }();
  return size;
}
uint32_t RenderHeight() { return RenderSizeFor().height; }
uint32_t RenderWidth() { return RenderSizeFor().width; }
}  // namespace

void Gow3SetDisplayWindow(rex::ui::Window* window) { display_window = window; }

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

// GFxMovieRoot::SetViewport. The menus and HUD lay out a 16:9 stage, with
// some items anchored to the visible edges and backgrounds the stage's width,
// so on a screen wider than 16:9 they are drawn in a centered 16:9 area of
// the full-width view. GViewport: BufferWidth, BufferHeight, Left, Top,
// Width, Height, ...
void Gow3ScaleformViewport(rex::ppc::Register& viewport) {
  if (!screen_size_base || !viewport.u32 || RenderWidth() * 9 <= RenderHeight() * 16 + 72) return;
  const uint32_t left = ReadInstruction(guest_memory, viewport.u32 + 8);
  const uint32_t width = ReadInstruction(guest_memory, viewport.u32 + 16);
  const uint32_t height = ReadInstruction(guest_memory, viewport.u32 + 20);
  if (left != 0 || width != RenderWidth() || height != RenderHeight()) return;
  const uint32_t stage = (height * 16 / 9) & ~1u;
  const auto write = [&](uint32_t offset, uint32_t value) {
    uint8_t* p = guest_memory + viewport.u32 + offset;
    p[0] = uint8_t(value >> 24); p[1] = uint8_t(value >> 16); p[2] = uint8_t(value >> 8); p[3] = uint8_t(value);
  };
  write(8, (width - stage) / 2);
  write(16, stage);
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
  write(0x833A2BD4, width);
  write(0x833A2BD8, height);
  // The system settings' ResX/ResY and the client's display size, which the
  // UI lays out in; written only while they still hold the original 1280x720.
  for (const uint32_t address : {0x834503F4u, 0x83427420u}) {
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

// ULocalPlayer::CalcSceneView (0x8262C988) builds the view projection with
// FPerspectiveMatrix(FOV * pi / 360, PlayerCamera.ConstrainedAspectRatio, 1,
// NearPlane), then letterboxes the view to that aspect: Gears always
// constrains it, to 16:9. Before the call (r9 = PlayerCamera), widen a 16:9
// camera to the render aspect, keeping the vertical field of view (Hor+), and
// apply gow3_fov. Cinematic cameras with their own aspect keep it.
// gow3_fov as a zoom factor: tan(half angle) times tan(fov / 2) / tan(65 / 2),
// the game's own 65 degrees. Every camera keeps its zoom relative to the
// default one (aiming, cover, scopes), and wide cameras do not approach 180
// degrees as multiplying the angle did (cover at 90 became 166 at fov 120).
// At most 140 degrees in total.
static double ScaleHalfFov(double half) {
  const int fov = std::clamp(REXCVAR_GET(gow3_fov), 40, 120);
  if (fov == 65) return half;
  constexpr double kPi = 3.14159265358979323846;
  const double factor = std::tan(fov * kPi / 360.0) / std::tan(65.0 * kPi / 360.0);
  return std::min(std::atan(std::tan(half) * factor), 70.0 * kPi / 180.0);
}

void Gow3ConstrainedProjection(rex::ppc::Register& half_fov, rex::ppc::Register& aspect,
                               rex::ppc::Register&, rex::ppc::Register& camera) {
  if (!projection_hooks || !camera.u32) return;
  uint8_t* field = guest_memory + camera.u32 + 476;
  const auto read = [&] { return std::bit_cast<float>(uint32_t(ReadInstruction(guest_memory, camera.u32 + 476))); };
  const float target = screen_size_base ? float(double(RenderWidth()) / RenderHeight()) : float(kGameAspect);
  float current = read();
  const bool game_aspect = std::abs(current - float(kGameAspect)) < 0.005f;
  if (std::abs(target - float(kGameAspect)) > 0.005f && (game_aspect || current == target)) {
    const uint32_t bits = std::bit_cast<uint32_t>(target);
    field[0] = uint8_t(bits >> 24); field[1] = uint8_t(bits >> 16); field[2] = uint8_t(bits >> 8); field[3] = uint8_t(bits);
    aspect.f64 = target;
    current = target;
  }
  double half = ScaleHalfFov(half_fov.f64);
  // Wider than 16:9: the 16:9 vertical field of view; narrower keeps the
  // horizontal one and so shows more above and below.
  if (current == target && target > float(kGameAspect)) half = std::atan(std::tan(half) * target / kGameAspect);
  half_fov.f64 = std::min(half, 1.5);
}

// The other projection in CalcSceneView, FPerspectiveMatrix(HalfFOVX,
// HalfFOVY, MultFOVX, MultFOVY, MinZ) at 0x82630FC8 for cameras without a
// constrained aspect: both half angles are FOV * pi / 360 and the multipliers
// (from the viewport and AspectRatioAxisConstraint) set the aspect. gow3_fov
// scales both angles; scaling only X squashed the picture horizontally.
void Gow3UnconstrainedProjection(rex::ppc::Register& half_fov, rex::ppc::Register& half_fov_y,
                                 rex::ppc::Register& local_player) {
  if (!projection_hooks || !local_player.u32) return;
  // Only while the player controls a pawn: the main menu's backdrop is a 3D
  // scene framed for the game's own field of view (wider showed past its
  // edges). LocalPlayer.Actor (+64) is the PlayerController; its Pawn is at
  // +0x1C8 (zero in the menu, set from the first gameplay frame).
  const uint32_t controller = ReadInstruction(guest_memory, local_player.u32 + 64);
  if (!controller || !ReadInstruction(guest_memory, controller + 0x1C8)) return;
  half_fov.f64 = ScaleHalfFov(half_fov.f64);
  half_fov_y.f64 = ScaleHalfFov(half_fov_y.f64);
}

// FXenonViewport::ProcessInput (0x8297EE10, r3 = viewport, f1 = DeltaTime)
// passes each controller's sticks and triggers to the viewport client's
// InputAxis. Afterwards pass mouse motion as the MouseX and MouseY keys, as the
// engine's PC viewports do; the game binds them to its PlayerInput mouse look.
void Gow3RefreshInputPrompts(PPCContext& ctx, uint8_t* base);

REX_EXTERN(__imp__sub_8297EE10);
REX_EXTERN(sub_8297EE10) {
  const uint32_t viewport = ctx.r3.u32;
  const double delta_time = ctx.f1.f64;
  __imp__sub_8297EE10(ctx, base);
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

// TMap<int32,...> element lookup (0x82A3F238, r3 = result, r4 = map, r5 =
// key). The texture pool's chunk map hashes aligned addresses into one bucket,
// so texture streaming walks chains of 2000 elements thousands of times a
// frame. A chain longer than kTMapWalkSteps is answered by TMapIntFindByScan
// instead; every gow3_tmap_find_verify-th scan is checked against the full
// walk, and a disagreement turns the scan off for the session.
REXCVAR_DEFINE_INT32(gow3_tmap_find_verify, 64, "Game",
                     "Check every Nth fast map lookup against the game's own walk (0 = never)");

namespace {
constexpr uint64_t kTMapWalkSteps = 32;
std::atomic<bool> tmap_scan_enabled{true};
std::atomic<uint32_t> tmap_scan_lookups{0};
}  // namespace

REX_EXTERN(sub_82A3F238) {
  const auto raw = [base](uint32_t address) {
    uint32_t value;
    std::memcpy(&value, REX_RAW_ADDR(address), sizeof(value));
    return value;
  };
  const uint32_t map = ctx.r4.u32, key = ctx.r5.u32;
  const bool scan = tmap_scan_enabled.load(std::memory_order_relaxed);
  uint64_t index = gow3::TMapIntFind(raw, map, key, scan ? kTMapWalkSteps : UINT64_MAX);
  if (index == gow3::kTMapUndecided) {
    index = gow3::TMapIntFindByScan(raw, map, key);
    const int32_t verify = REXCVAR_GET(gow3_tmap_find_verify);
    if (index != gow3::kTMapUndecided && verify > 0 &&
        tmap_scan_lookups.fetch_add(1, std::memory_order_relaxed) % uint32_t(verify) == 0) {
      const uint64_t walked = gow3::TMapIntFind(raw, map, key);
      if (walked != index) {
        if (tmap_scan_enabled.exchange(false))
          REXLOG_ERROR("Map lookup scan disagreed with the game's walk (map {:08X} key {:08X}: {} vs {}); "
                       "using the walk from now on", map, key, index, walked);
        index = walked;
      }
    }
    if (index == gow3::kTMapUndecided) index = gow3::TMapIntFind(raw, map, key);
  }
  REX_STORE_U32(ctx.r3.u32, uint32_t(index));
  ctx.r11.u64 = uint32_t(index);
}
