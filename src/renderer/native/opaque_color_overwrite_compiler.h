#pragma once
#include "shader_compiler.h"
#include "opaque_color_overwrite.h"
#include "fsi_state.h"
#include <array>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace gow3::renderer {
inline constexpr std::string_view kOpaqueColorOverwritePixel =
    "49f26283e1436f4cce68ac4e75695c6375176768b1dbfbf6c1f79d9bef434ced";
inline constexpr std::string_view kOpaqueColorOverwriteVertex =
    "f7dba9374d4cba41785f4e2810d881f559a2f3b0cbc40fc8a1f58ee49f4eb969";
// Broaden only after revalidating the actual compiler output and GPU fixture.
inline constexpr std::string_view kOpaqueColorOverwriteBaselineCompiler =
    "1c697af73d13496ae2424cfd4e65de2856f16bd612bd715b92160403c8bd762b";
inline bool OpaqueColorOverwriteRasterKey(const std::array<uint32_t,6>& key) noexcept {
    return key==std::array<uint32_t,6>{3,1,1,4,0,0}||
           key==std::array<uint32_t,6>{3,2,1,4,0,0};
}
// Live selection is deliberately narrower than the shader-side algebraic
// proof. Keep every existing state/layout validation in the caller as well.
inline bool OpaqueColorOverwriteAllowed(const NativeFsiConstants& raster,
    uint32_t width,uint32_t height,uint32_t surface_info,uint32_t raw_color_mask,
    bool queried,std::string_view vertex,std::string_view pixel,
    const std::array<uint32_t,6>& host_raster) noexcept {
    if(queried||vertex!=kOpaqueColorOverwriteVertex||pixel!=kOpaqueColorOverwritePixel||
       !OpaqueColorOverwriteRasterKey(host_raster)||raw_color_mask!=15u||
       !width||!height||width>8192||height>8192||
       (surface_info&0x30000u)||raster.pitch<width||raster.pitch>16383||
       raster.color[0].base>=2048||raster.depth_base>=2048||
       raster.color[0].format!=3||raster.color[0].mask!=15||raster.blend_enabled!=0||
       raster.depth_format!=1||raster.depth_control!=0x700766u)return false;
    const uint64_t columns=(uint64_t(width)+79)/80,rows=(uint64_t(height)+15)/16;
    const uint64_t pitch_tiles=(uint64_t(raster.pitch)+79)/80;
    return pitch_tiles>=columns&&(rows-1)*pitch_tiles+columns-1<2048;
}
struct OpaqueColorOverwriteProgram {
    std::string source_sha256;
    std::string compiler_version;
    std::string variant_version;
    std::string modified_common_sha256;
    std::string compiled_spirv_sha256;
    uint32_t specialization_mask=0;
    std::vector<uint32_t> spirv;
};
// Startup worker only. Reuses canonical.Get's exact container validation;
// unknown compiler/source/helper shapes return nullptr. Malformed containers
// and DXC errors throw so callers report an optional experiment refusal.
// No disk cache, process launch, Vulkan work or gameplay compilation.
std::shared_ptr<const OpaqueColorOverwriteProgram> TryPrepareOpaqueColorOverwrite(
    NativeShaderCompiler&,std::span<const uint8_t> source);
std::string OpaqueColorOverwriteCompilerVersion();
} // namespace gow3::renderer
