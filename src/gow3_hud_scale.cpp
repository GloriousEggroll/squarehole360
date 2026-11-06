// HUD size (gow3_hud_size).
//
// Gears draws parts of its HUD (the weapon panel, ammo, prompts) at fixed
// pixel sizes and the rest in proportion to the screen, so the same layout
// looks smaller at 1080p than at the console's 720p. With 1440p (the 720p
// layout drawn at twice the size) the HUD would keep its 720p size.
//
// The HUD instead lays out on a virtual screen: UCanvas::Update sets the
// canvas clip size to the virtual size, and the canvas transform (pixel to
// clip space) is scaled to match, so everything drawn through the canvas
// fits the real screen. UCanvas::Project and DeProject convert with the clip
// size, so markers over the world follow. Scaleform menus are separate.
// Addresses are for the Title Update 6 executable.
#include "gameplay_hooks.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

REXCVAR_DEFINE_INT32(gow3_hud_size, 0, "Game",
                     "HUD size as a percentage of the Xbox 360's (25-200); 0 = automatic, as the console's "
                     "HUD would look at the internal resolution");

REXCVAR_DECLARE(int32_t, gow3_render_height);
REX_EXTERN(__imp__sub_827483C0);

namespace {
uint32_t Load32(const uint8_t* base, uint32_t address) {
  const auto* p = base + address;
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
void Store32(uint8_t* base, uint32_t address, uint32_t value) {
  for (int i = 0; i < 4; ++i) base[address + i] = uint8_t(value >> (24 - 8 * i));
}
float LoadFloat(const uint8_t* base, uint32_t address) {
  const uint32_t bits = Load32(base, address);
  float value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}
void StoreFloat(uint8_t* base, uint32_t address, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  Store32(base, address, bits);
}

// UCanvas: OrgX/OrgY +64/+68, ClipX/ClipY +72/+76, SizeX/SizeY +104/+108,
// FCanvas* +112. FCanvas: TransformStack data/count +36/+40, 80-byte entries
// (FMatrix, then its CRC at +64), the top entry is the one draws use.
constexpr uint32_t kClipX = 72, kClipY = 76, kSizeX = 104, kSizeY = 108, kFCanvas = 112;
constexpr uint32_t kStackData = 36, kStackCount = 40, kEntryBytes = 80, kEntryCrc = 64;
// The screen canvas and its transform's CRC before scaling.
std::atomic<uint32_t> original_canvas{0}, original_crc{0};

// Virtual screen height / real canvas height.
float HudFactor(uint32_t height) {
  const int size = REXCVAR_GET(gow3_hud_size);
  if (size <= 0) {
    // Automatic: as the HUD would be laid out at the internal resolution.
    const int internal = REXCVAR_GET(gow3_render_height), guest = Gow3GuestRenderHeight();
    return guest > 0 && internal > guest ? float(internal) / float(guest) : 1.0f;
  }
  // The console lays out at 720 lines; a fixed-pixel element of the HUD then
  // covers size% of its console share when the virtual height is 720 / size%.
  return 720.0f * 100.0f / float(std::clamp(size, 25, 200)) / float(height);
}
}  // namespace

// UCanvas::Update: eventReset, then ClipX/ClipY = SizeX/SizeY.
REX_EXTERN(sub_827483C0) {
  const uint32_t canvas = ctx.r3.u32;
  __imp__sub_827483C0(ctx, base);
  const int32_t width = int32_t(Load32(base, canvas + kSizeX)), height = int32_t(Load32(base, canvas + kSizeY));
  const uint32_t fcanvas = Load32(base, canvas + kFCanvas);
  if (width <= 0 || height <= 0 || !fcanvas) return;
  const uint32_t count = Load32(base, fcanvas + kStackCount), data = Load32(base, fcanvas + kStackData);
  if (!count || count > 16 || !data) return;
  const uint32_t top = data + (count - 1) * kEntryBytes;
  // An orthographic pixel-to-clip transform: x * 2 / width, y * -2 / height.
  const float sx = LoadFloat(base, top), sy = LoadFloat(base, top + 20);
  for (const uint32_t zero : {1u, 2u, 3u, 4u, 6u, 7u})
    if (LoadFloat(base, top + zero * 4) != 0.0f) return;
  if (!(sx > 0.0f) || !(sy < 0.0f)) return;
  // The canvas keeps this transform between frames, while the engine resets
  // the canvas size to the real one every frame before this call.
  if (height < 360 || width < 360) return;  // Only the screen's canvas.
  const float base_sx = 2.0f / float(width), base_sy = -2.0f / float(height);
  const float ratio_x = base_sx / sx, ratio_y = base_sy / sy;
  // The engine's transform (ratio 1) or one scaled here (the same ratio on
  // both axes, within the setting's range).
  if (std::abs(ratio_x / ratio_y - 1.0f) > 1e-3f || ratio_x < 0.2f || ratio_x > 5.0f) return;
  const bool original = std::abs(ratio_x - 1.0f) < 1e-4f;
  const uint32_t crc = Load32(base, top + kEntryCrc);
  if (original) {
    original_canvas = canvas;
    original_crc = crc;
  } else if (original_canvas.load() != canvas) {
    return;
  }
  const float factor = HudFactor(uint32_t(height));
  // Scripts lay out with both the canvas size and the clip size.
  const int32_t virtual_width = int32_t(std::lround(float(width) * factor));
  const int32_t virtual_height = int32_t(std::lround(float(height) * factor));
  Store32(base, canvas + kSizeX, uint32_t(virtual_width));
  Store32(base, canvas + kSizeY, uint32_t(virtual_height));
  StoreFloat(base, canvas + kClipX, float(virtual_width));
  StoreFloat(base, canvas + kClipY, float(virtual_height));
  if (std::abs(ratio_x / factor - 1.0f) < 1e-4f) return;
  StoreFloat(base, top, base_sx / factor);
  StoreFloat(base, top + 20, base_sy / factor);
  // The CRC tells batches with different transforms apart: the engine's
  // value for its own transform, another for each scale.
  uint32_t bits = 0;
  if (std::abs(factor - 1.0f) >= 1e-4f) std::memcpy(&bits, &factor, sizeof(bits));
  Store32(base, top + kEntryCrc, original_crc.load() ^ bits);
}
