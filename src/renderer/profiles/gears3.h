#pragma once
// Gears of War 3 (title 4D5308AB), default.xex SHA-256
// 4e5d00b8f584f42951344b26a2ae8292bb30c7634b1a75cec0c70a80b3f8f790.
//
// Every executable-specific address and Direct3D device offset the native
// renderer reads is named here, so another game needs a new profile rather
// than edits across the renderer. tools/porting/xdk_signatures.py finds the
// d3d:: functions in another XEX by signature and reports how the device::
// offsets moved (Judgment: +0x80 above 0x2F00; Gears 2: several ranges).
//
// Kept in their own per-game files, because they are hook data, not constants:
//   config/renderer-hooks.toml, config/gameplay-hooks.toml,
//   config/function-boundaries.toml (recompiler hooks and boundaries);
//   native/packet_commit_sites.h, native/up_commit_sites.h,
//   native/shader_discovery.h, native/query_poll_yield.h,
//   native/getdata_inline_selection.h and the getdata_*.inc bodies
//   (instruction-word witnesses checked against the loaded XEX);
//   the sub_XXXXXXXX overrides in gears_bridge.cpp (the address is the symbol).
#include <cstdint>

namespace gow3::renderer::profile {
inline constexpr char kName[] = "Gears of War 3";

// XDK Direct3D library entry points (findable by signature in other titles).
namespace d3d {
inline constexpr uint32_t kDrawVertices = 0x82AEFF00;         // DrawPrimitive
inline constexpr uint32_t kDrawIndexedVertices = 0x82AF0318;  // DrawIndexedPrimitive
inline constexpr uint32_t kBeginVertices = 0x82AEF500;        // DrawPrimitiveUP begin
inline constexpr uint32_t kBeginIndexedVertices = 0x82AEF9C0; // DrawIndexedPrimitiveUP begin
inline constexpr uint32_t kSpecializedDraw = 0x82AF0790;      // ABI under investigation
inline constexpr uint32_t kClearRect = 0x82AF16D8;
inline constexpr uint32_t kResolve = 0x82AF34A8;
inline constexpr uint32_t kPresent = 0x82AF6B10;              // Swap
inline constexpr uint32_t kOcclusionQueryReport = 0x82AFF740; // record 1 begins, 0 ends
inline constexpr uint32_t kSetRenderTarget = 0x82AECC98;
inline constexpr uint32_t kSetDepthStencilSurface = 0x82AED028;
inline constexpr uint32_t kInitAuxiliaryCommandBuffer = 0x82AF99B0;
}  // namespace d3d

// Offsets into the XDK D3D device. Values from 0x480 to 0x2A5F shadow Xenos
// registers and constants; the rest are XDK software fields that move between
// XDK versions.
namespace device {
inline constexpr uint32_t kCaptureBytes = 0x3600;     // captured prefix (device is 0x5F80)
inline constexpr uint32_t kFetchConstants = 0x480;    // 96 x 8 bytes / 32 textures x 24 bytes
inline constexpr uint32_t kSurfaces = 0x3190;         // 4 render targets + depth, 4 bytes each
inline constexpr uint32_t kIndexBuffer = 0x318C;
inline constexpr uint32_t kShaderMetadata = 0x2F58;   // 0x200-byte block captured as kind 13
inline constexpr uint32_t kPixelShader = 0x328C;
inline constexpr uint32_t kVertexShader = 0x3290;
inline constexpr uint32_t kUpVertexPointer = 0x3598;
inline constexpr uint32_t kUpIndexPointer = 0x359C;
inline constexpr uint32_t kUpVertexWords = 0x35A0;
inline constexpr uint32_t kUpIndexWords = 0x35A4;
inline constexpr uint32_t kClearViewport = 0x3260;    // 4 floats
inline constexpr uint32_t kScissorEnable = 0x2F80;
inline constexpr uint32_t kScissorRect = 0x327C;      // 4 words
}  // namespace device

// Game code (UE3/Gears), not XDK: located by hand per title.
namespace game {
inline constexpr uint32_t kShaderCacheDecoderReturn = 0x823F702C; // LR into the shader-cache loader
}  // namespace game
}  // namespace gow3::renderer::profile
