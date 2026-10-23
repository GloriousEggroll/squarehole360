#pragma once
#include <algorithm>
#include <cstdint>

namespace gow3::renderer {
// Startup-only additional generic PSOs. These limits include persistent owners
// plus the at-most-two temporary modules; no persistent cache target is raised.
inline constexpr uint64_t kNativeOverflowPrewarmMaximumPipelines=1536;
inline constexpr uint64_t kNativeOverflowPrewarmMaximumModules=1024;
inline constexpr uint64_t kNativeOverflowPrewarmMaximumBytes=64u*1024*1024;
enum class NativeOverflowPrewarmStatus : uint8_t {
    Prepared,AlreadyResident,InvalidRecipe,UnsupportedVariant,PipelineCap,
    ModuleCountCap,ByteCap,ShaderCreationFailed,PipelineCreationFailed
};
struct NativeOverflowPrewarmResult {
    NativeOverflowPrewarmStatus status=NativeOverflowPrewarmStatus::InvalidRecipe;
    uint64_t temporary_modules=0,temporary_bytes=0;
    bool ready() const noexcept {
        return status==NativeOverflowPrewarmStatus::Prepared||status==NativeOverflowPrewarmStatus::AlreadyResident;
    }
};
// Subtraction-first checks are shared with the CPU boundary fixture. Count only
// modules missing from the existing canonical resident map, never logical draws.
inline NativeOverflowPrewarmStatus NativeOverflowPrewarmAdmission(
        uint64_t pipelines,uint64_t hard_pipeline_limit,uint64_t modules,uint64_t hard_module_limit,
        uint64_t bytes,uint64_t hard_byte_limit,uint64_t extra_modules,uint64_t extra_bytes) noexcept {
    const auto pipeline_limit=std::min(hard_pipeline_limit,kNativeOverflowPrewarmMaximumPipelines);
    const auto module_limit=std::min(hard_module_limit,kNativeOverflowPrewarmMaximumModules);
    const auto byte_limit=std::min(hard_byte_limit,kNativeOverflowPrewarmMaximumBytes);
    if(pipelines>=pipeline_limit)return NativeOverflowPrewarmStatus::PipelineCap;
    if(modules>module_limit||extra_modules>module_limit-modules)return NativeOverflowPrewarmStatus::ModuleCountCap;
    if(bytes>byte_limit||extra_bytes>byte_limit-bytes)return NativeOverflowPrewarmStatus::ByteCap;
    return NativeOverflowPrewarmStatus::Prepared;
}
} // namespace gow3::renderer
