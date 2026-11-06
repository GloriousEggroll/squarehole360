#pragma once
#include <cstdint>

namespace gow3::renderer {
// Internal resolution scale of the hardware render target path: every guest
// surface is drawn at this many host pixels per guest pixel in each
// direction, while guest state (EDRAM addresses, surface sizes, resolve
// rectangles, fetch constants) stays in guest pixels. 1 normally; 2 for the
// 1440p setting, which keeps the game's 720p layout
// (docs/resolution-scaling-plan.md). Set once before the renderer starts.
inline uint32_t native_render_scale=1;
inline uint32_t NativeRenderScale() {return native_render_scale;}
}
