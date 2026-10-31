#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace gow3::renderer {

// Compiles every pixel shader in the user's own game files (cooked packages
// and GlobalShaderCache files) into the native shader cache, so the first
// visit to an area doesn't wait for its shaders. Nothing leaves the cache.
//
// Vertex shaders are not covered: the game links them against a vertex layout
// at load time, which rewrites their instructions.
enum class ShaderPrecompilePhase { kFinding, kCompiling };

struct ShaderPrecompileCallbacks {
  std::function<void(ShaderPrecompilePhase, size_t total)> begin;
  // One package scanned or one shader compiled; false when it failed.
  std::function<void(bool succeeded)> advance;
  // Polled between items; a stopped run keeps what it compiled.
  std::function<bool()> stopping;
};

struct ShaderPrecompileResult {
  bool up_to_date = false;  // Same inputs as the last complete run; nothing scanned.
  bool complete = false;
  size_t packages = 0, shaders = 0, already = 0, failed = 0;
  std::vector<std::string> compiled;  // Content hashes newly in the cache.
};

// cache_root holds native-shaders/<version>. With skip_unchanged, a run whose
// package files (names, sizes, times) and compiler match the last complete
// run's returns at once.
ShaderPrecompileResult PrecompileShaders(const std::filesystem::path& cache_root,
                                         const std::vector<std::filesystem::path>& inputs, unsigned threads,
                                         bool skip_unchanged, const ShaderPrecompileCallbacks& callbacks);

}  // namespace gow3::renderer
