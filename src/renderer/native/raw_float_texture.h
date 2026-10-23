#pragma once
#include "texture_data.h"
#include <cstddef>
namespace gow3::renderer {
// This experiment covers only the observed linear RGBA32F row. No texel
// normalization, float instruction, padding read, tiling or mip interpretation
// is authorized by this policy. The exact sampled footprint is one 64 KiB run.
inline constexpr uint32_t kRawFloatCpuTextureBytes=65536;
inline bool RawFloatCpuTextureLayout(const TextureLayout& t) noexcept {
    if(t.format!=38||t.dimension!=1||t.width!=4096||t.height!=1||t.depth!=1||t.layers!=1||
        t.tiled||t.endian!=2||t.min_mip||t.max_mip||!t.base_address||t.mip_address||
        (t.base_address&3)||t.block.block_width!=1||t.block.block_height!=1||
        t.block.bytes!=16||t.block.components!=4||t.levels.size()!=1)return false;
    const auto& l=t.levels.front();
    return !l.mip&&!l.layer&&l.width==4096&&l.height==1&&l.depth==1&&l.pitch_blocks==4096&&
        l.slice_rows==32&&!l.x&&!l.y&&!l.z&&!l.offset&&!l.mips;
}
inline bool RawFloatCpuTexturePayload(size_t bytes) noexcept {return bytes>=kRawFloatCpuTextureBytes;}
}
