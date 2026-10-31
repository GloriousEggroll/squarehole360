// Times the native shader compiler (XenosRecomp + DXC to SPIR-V) on extracted
// shader containers, without a disk cache, to compare platforms and runtimes:
//   gow3-native-compiler-timing <folder of *.vertex.bin / *.pixel.bin> [threads]
#include "renderer/native/shader_compiler.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <shader folder> [threads]\n", argv[0]);
    return 2;
  }
  std::vector<std::vector<uint8_t>> containers;
  for (const auto& entry : std::filesystem::directory_iterator(argv[1])) {
    if (entry.path().extension() != ".bin") continue;
    std::ifstream file(entry.path(), std::ios::binary);
    containers.emplace_back(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
  }
  const unsigned threads = argc > 2 ? unsigned(std::max(1, std::atoi(argv[2]))) : 1;
  gow3::renderer::NativeShaderCompiler compiler;
  std::vector<double> times(containers.size(), -1);
  std::atomic<size_t> next{0};
  std::atomic<size_t> failures{0};
  const auto begin = std::chrono::steady_clock::now();
  std::vector<std::thread> workers;
  for (unsigned t = 0; t < threads; ++t)
    workers.emplace_back([&] {
      for (size_t i; (i = next++) < containers.size();) {
        const auto start = std::chrono::steady_clock::now();
        try {
          compiler.Get(containers[i]);
          times[i] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        } catch (...) {
          ++failures;
        }
      }
    });
  for (auto& worker : workers) worker.join();
  const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
  std::erase(times, -1.0);
  std::sort(times.begin(), times.end());
  if (times.empty()) {
    std::printf("no shader compiled (%zu failures)\n", failures.load());
    return 1;
  }
  double total = 0;
  for (double t : times) total += t;
  std::printf("shaders=%zu failures=%zu threads=%u wall=%.2fs mean=%.1fms median=%.1fms p90=%.1fms max=%.1fms\n",
              times.size(), failures.load(), threads, wall, total / times.size(), times[times.size() / 2],
              times[times.size() * 9 / 10], times.back());
}
