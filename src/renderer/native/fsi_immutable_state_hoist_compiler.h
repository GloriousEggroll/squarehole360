#pragma once
#include "shader_compiler.h"
#include "fsi_immutable_state_hoist.h"
#include <filesystem>
#include <memory>
#include <span>
#include <string>

namespace gow3::renderer {
// Optional Native-only provider. Canonical compiler inputs/API/cache/version
// remain unchanged. Every source is validated by canonical.Get first; derived
// output has a separate on-disk and Scene module/pipeline identity.
class NativeFsiHoistCompiler {
public:
    explicit NativeFsiHoistCompiler(NativeShaderCompiler& canonical, NativeFsiHoistMode mode,
        std::filesystem::path cache_directory={});
    ~NativeFsiHoistCompiler();
    NativeFsiHoistCompiler(const NativeFsiHoistCompiler&)=delete;
    NativeFsiHoistCompiler& operator=(const NativeFsiHoistCompiler&)=delete;
    std::shared_ptr<const CompiledNativeShader> Get(std::span<const uint8_t> container);
    std::shared_ptr<const CompiledNativeShader> GetDepthOnly();
    static std::string CacheVersion(NativeFsiHoistMode mode);
    NativeFsiHoistMode Mode() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace gow3::renderer
