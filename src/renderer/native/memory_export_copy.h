#pragma once
#include <array>
#include <cstdint>
#include <span>

namespace gow3::renderer {
struct MemoryExportCopy {
    uint32_t source=0,destination=0,bytes=0,source_binding=0,source_offset=0;
};
// Exact intrinsic for Gears' 64-byte GPU memcpy kernel. Its eight uint16x4
// fetches and two groups of four integer memory exports preserve every byte.
// Different shaders, export formats, stream bounds or raster states throw.
MemoryExportCopy DecodeMemoryExportCopy(std::span<const uint8_t> container,
    std::span<const uint8_t> device_state,const std::array<uint32_t,8>& args);
// The same decode for a container the caller already passed through
// DecodeMemoryExportCopy (byte-identical), without hashing it again.
MemoryExportCopy DecodeVerifiedMemoryExportCopy(std::span<const uint8_t> device_state,
    const std::array<uint32_t,8>& args);
// CP-time validation against actual active shader/registers, after all PM4
// state writes and before skipping its reference draw. Words are host-endian.
MemoryExportCopy DecodeMemoryExportPacket(std::span<const uint32_t> shader_words,
    const std::array<uint32_t,2>& fetch95,const std::array<uint32_t,8>& constants,
    uint32_t clip_control,uint32_t vte_control,uint32_t first,uint32_t count);
}
