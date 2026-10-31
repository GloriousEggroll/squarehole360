#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace gow3::renderer {

enum class NativeShaderStage : uint32_t { Vertex, Pixel };
enum class NativeShaderMode : uint32_t { Raster, FragmentInterlock };

struct CompiledNativeShader {
  NativeShaderStage stage;
  NativeShaderMode mode = NativeShaderMode::Raster;
  std::string source_sha256;
  std::vector<uint32_t> spirv;
  uint32_t specialization_mask = 0;
  bool disk_cache_hit = false;
};

// Compiles the user's extracted shader containers without launching processes.
// Instances are safe to call from multiple worker threads. Failures are reported
// as exceptions so a caller can retain the reference renderer as a fallback.
class NativeShaderCompiler {
 public:
  explicit NativeShaderCompiler(std::filesystem::path cache_directory = {});
  ~NativeShaderCompiler();
  NativeShaderCompiler(const NativeShaderCompiler&) = delete;
  NativeShaderCompiler& operator=(const NativeShaderCompiler&) = delete;

  std::shared_ptr<const CompiledNativeShader> Get(
      std::span<const uint8_t> container, NativeShaderMode mode = NativeShaderMode::Raster);
  std::shared_ptr<const CompiledNativeShader> GetDepthOnlyFragmentInterlock();
  static std::string CacheVersion();
  // A cached shader: <version folder>/<first two hex digits>/<key>.g3spv, key
  // being the source hash (plus ".fsi" for fragment interlock). Wine scans a
  // whole folder to look up a missing file, which a flat folder of 100k+
  // shaders made ~50 ms a lookup; subfolders keep each one small.
  static std::filesystem::path CachePath(const std::filesystem::path& version_directory, std::string_view key);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace gow3::renderer
