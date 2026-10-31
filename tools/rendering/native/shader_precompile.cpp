// Compiles every pixel shader in the user's own game files into the native
// shader cache; the game does the same at start (see shader_precompile.h).
// Nothing extracted here is shipped; it goes only to the user's cache.
//
//   gow3-shader-precompile <cache root> <cooked package folder>... [--threads N]
//
// Prints "progress <phase> <done> <total>" lines for the setup wizard.
#include "renderer/native/shader_precompile.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>

int main(int argc, char** argv) {
  using gow3::renderer::ShaderPrecompilePhase;
  std::vector<std::filesystem::path> inputs;
  std::filesystem::path cache;
  unsigned threads = std::max(1u, std::thread::hardware_concurrency());
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument == "--threads" && i + 1 < argc) threads = unsigned(std::max(1, std::atoi(argv[++i])));
    else if (cache.empty()) cache = argument;
    else inputs.emplace_back(argument);
  }
  if (cache.empty() || inputs.empty()) {
    std::fprintf(stderr, "usage: %s <cache root> <cooked folder>... [--threads N]\n", argv[0]);
    return 2;
  }
  const char* phase = "scan";
  size_t done = 0, total = 0;
  gow3::renderer::ShaderPrecompileCallbacks callbacks;
  callbacks.begin = [&](ShaderPrecompilePhase next, size_t count) {
    phase = next == ShaderPrecompilePhase::kFinding ? "scan" : "compile";
    done = 0;
    total = count;
  };
  // Workers call this concurrently; stdout's lock keeps lines whole.
  std::mutex mutex;
  callbacks.advance = [&](bool) {
    std::lock_guard lock(mutex);
    if (++done % 64 == 0 || done == total) {
      std::printf("progress %s %zu %zu\n", phase, done, total);
      std::fflush(stdout);
    }
  };
  // A manual run always rescans: it is how to repair a cache by hand.
  const auto result = gow3::renderer::PrecompileShaders(cache, inputs, threads, false, callbacks);
  std::printf("done packages=%zu shaders=%zu compiled=%zu already=%zu failed=%zu\n", result.packages,
              result.shaders, result.compiled.size() + result.failed, result.already, result.failed);
  return 0;
}
