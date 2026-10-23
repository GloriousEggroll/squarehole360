// Times native shader compilation stages on real shader containers: Xenos
// translation, then DXC with the production arguments and with lower
// optimization levels. Uses DXC directly, so the production compiler (and the
// shader cache version it defines) is untouched.
// Usage: shader-compile-bench <dir with HASH.shader.bin> <dir with HASH.fsi.spv> [max shaders]
#include "native_shader_embedded.h"
#include "shader_recompiler.h"
#include <dxcapi.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {
template <typename T> struct ComRelease { void operator()(T* p) const { if (p) p->Release(); } };
template <typename T> using Com = std::unique_ptr<T, ComRelease<T>>;
using Clock = std::chrono::steady_clock;
double Ms(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); }

// Same arguments as shader_compiler.cpp for a fragment-interlock pixel shader.
bool Compile(IDxcCompiler3* compiler, const std::string& hlsl, const wchar_t* optimization, size_t& words) {
  DxcBuffer source{hlsl.data(), hlsl.size(), DXC_CP_UTF8};
  std::vector<const wchar_t*> arguments{L"-T", L"ps_6_0", L"-E", L"main", L"-HV", L"2021", L"-spirv",
      L"-fvk-use-dx-layout", L"-D", L"GOW3_NATIVE_FSI=1", L"-fspv-target-env=vulkan1.2",
      L"-fspv-extension=SPV_EXT_fragment_shader_interlock", L"-fspv-extension=SPV_EXT_descriptor_indexing",
      L"-fspv-extension=SPV_KHR_physical_storage_buffer"};
  if (optimization) arguments.push_back(optimization);
  IDxcResult* raw = nullptr;
  if (FAILED(compiler->Compile(&source, arguments.data(), uint32_t(arguments.size()), nullptr, IID_PPV_ARGS(&raw))) || !raw) return false;
  Com<IDxcResult> result(raw);
  HRESULT status = E_FAIL;
  if (FAILED(result->GetStatus(&status)) || FAILED(status)) {
    IDxcBlobUtf8* errors = nullptr;
    result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
    Com<IDxcBlobUtf8> owned_errors(errors);
    (void)owned_errors;
    return false;
  }
  IDxcBlob* object = nullptr;
  result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr);
  Com<IDxcBlob> owned(object);
  words = owned ? owned->GetBufferSize() / 4 : 0;
  return words != 0;
}
}

int main(int argc, char** argv) {
  if (argc < 3) { std::fprintf(stderr, "usage: shader-compile-bench SOURCES COMPILED [max]\n"); return 1; }
  const std::filesystem::path sources = argv[1], compiled = argv[2];
  const size_t limit = argc >= 4 ? std::stoul(argv[3]) : 40;
  IDxcCompiler3* raw = nullptr;
  if (FAILED(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&raw))) || !raw) { std::fprintf(stderr, "no DXC\n"); return 1; }
  Com<IDxcCompiler3> compiler(raw);
  if (std::string(argv[1]) == "--depth-only") {
    // The production depth-only FSI shader: the shared FSI epilogue plus a tiny main.
    const std::string hlsl = std::string(kNativeShaderCommon) + R"HLSL(
void main(float4 p : SV_Position, bool face : SV_IsFrontFace) {
  float2 slope = float2(ddx_coarse(p.z), ddy_coarse(p.z));
  const float4 zero = float4(0, 0, 0, 0);
  gow3NativeRasterOutput(p.xy, p.z, face, zero, zero, zero, zero, 0u, false, slope);
}
)HLSL";
    const wchar_t* levels[3] = {nullptr, L"-O1", L"-O0"};
    const char* names[3] = {"DXC default (O3)", "DXC -O1        ", "DXC -O0        "};
    for (int level = 0; level < 3; ++level) {
      double best = 1e9; size_t words = 0;
      for (int i = 0; i < 5; ++i) {
        const auto a = Clock::now();
        if (!Compile(compiler.get(), hlsl, levels[level], words)) { std::fprintf(stderr, "compile failed\n"); return 1; }
        best = std::min(best, Ms(a, Clock::now()));
      }
      std::printf("depth-only FSI %s best of 5: %7.2f ms  SPIR-V words %zu\n", names[level], best, words);
    }
    return 0;
  }
  std::vector<std::filesystem::path> pixel;
  for (const auto& entry : std::filesystem::directory_iterator(sources)) {
    const auto name = entry.path().filename().string();
    if (name.size() < 12 || name.substr(name.size() - 11) != ".shader.bin") continue;
    if (std::filesystem::exists(compiled / (name.substr(0, name.size() - 11) + ".fsi.spv"))) pixel.push_back(entry.path());
  }
  std::sort(pixel.begin(), pixel.end(), [](auto& a, auto& b) { return std::filesystem::file_size(a) > std::filesystem::file_size(b); });
  if (pixel.size() > limit) pixel.resize(limit);
  double translate = 0, sum[3] = {0, 0, 0}, worst[3] = {0, 0, 0};
  size_t done = 0, skipped = 0, words[3] = {0, 0, 0};
  const wchar_t* levels[3] = {nullptr, L"-O1", L"-O0"};
  for (const auto& path : pixel) {
    std::ifstream file(path, std::ios::binary);
    std::vector<char> bytes{std::istreambuf_iterator<char>(file), {}};
    std::vector<uint32_t> aligned((bytes.size() + 3) / 4 + 16384);
    std::memcpy(aligned.data(), bytes.data(), bytes.size());
    const auto t0 = Clock::now();
    auto owned = std::make_unique<ShaderRecompiler>();
    auto& recompiler = *owned;
    recompiler.recompile(reinterpret_cast<const uint8_t*>(aligned.data()), kNativeShaderCommon);
    const auto t1 = Clock::now();
    double ms[3]; size_t count[3] = {0, 0, 0}; bool ok = true;
    for (int level = 0; level < 3 && ok; ++level) {
      const auto a = Clock::now();
      ok = Compile(compiler.get(), recompiler.out, levels[level], count[level]);
      ms[level] = Ms(a, Clock::now());
    }
    if (!ok) { ++skipped; continue; }  // Also fails in production; not timed.
    translate += Ms(t0, t1);
    for (int level = 0; level < 3; ++level) {
      sum[level] += ms[level]; worst[level] = std::max(worst[level], ms[level]); words[level] += count[level];
    }
    ++done;
  }
  std::printf("pixel FSI shaders=%zu (largest first), skipped (DXC errors)=%zu\n", done, skipped);
  if (!done) return 1;
  std::printf("  translation      avg %7.2f ms\n", translate / done);
  const char* names[3] = {"DXC default (O3)", "DXC -O1        ", "DXC -O0        "};
  for (int level = 0; level < 3; ++level)
    std::printf("  %s avg %7.2f ms  worst %7.2f ms  avg SPIR-V words %zu\n", names[level], sum[level] / done, worst[level], words[level] / done);
  return 0;
}
