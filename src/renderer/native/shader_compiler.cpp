#include "renderer/native/shader_compiler.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <fstream>
#include <future>
#include <functional>
#include <limits>
#include <mutex>
#include <random>
#include <stdexcept>
#include <unordered_map>

#include <dxcapi.h>
#include <picosha2.h>
#include "native_shader_embedded.h"
#include "shader_recompiler.h"
#include "renderer/native/shader_abi.h"

namespace gow3::renderer {
namespace {
constexpr size_t kMaximumContainerBytes = 16 * 1024 * 1024;
constexpr size_t kMaximumSpirvBytes = 16 * 1024 * 1024;
constexpr size_t kCacheHeaderBytes = 152;
constexpr std::array<uint8_t, 8> kCacheMagic{'G', '3', 'S', 'P', 'V', '0', '0', '3'};

void Require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

uint32_t Be32(std::span<const uint8_t> bytes, size_t offset) {
  Require(offset <= bytes.size() && bytes.size() - offset >= 4, "Truncated shader container");
  return uint32_t(bytes[offset]) << 24 | uint32_t(bytes[offset + 1]) << 16 |
         uint32_t(bytes[offset + 2]) << 8 | bytes[offset + 3];
}

void Range(size_t offset, size_t count, size_t limit) {
  Require(!(offset & 3) && offset <= limit && count <= limit - offset,
          "Shader container has an invalid or unaligned range");
}

NativeShaderStage ValidateContainer(std::span<const uint8_t> bytes) {
  Require(bytes.size() >= 36 && bytes.size() <= kMaximumContainerBytes,
          "Invalid shader container size");
  const uint32_t flags = Be32(bytes, 0), virtual_size = Be32(bytes, 4), physical_size = Be32(bytes, 8);
  Require((flags & 0xffffff00) == 0x102a1100 && !Be32(bytes, 28) && !Be32(bytes, 32),
          "Invalid Xenos shader container header");
  Require(virtual_size >= 36 && size_t(virtual_size) + physical_size == bytes.size(),
          "Shader container section sizes do not match input");
  Range(virtual_size, physical_size, bytes.size());
  // Named-constant shaders use a different upstream descriptor ABI. Do not let
  // one enter the Gears stripped-constant ABI until that path is implemented.
  Require(Be32(bytes, 16) == 0, "Named-constant shader ABI is not supported by the native Gears renderer");
  const bool pixel = !(flags & 1);
  const size_t shader = Be32(bytes, 24);
  Range(shader, pixel ? 32 : 36, virtual_size);
  const uint32_t code_offset = Be32(bytes, shader), code_size = Be32(bytes, shader + 4);
  Require(code_size && code_size <= 4096 * 12 && code_size % 12 == 0, "Invalid shader instruction length");
  Range(code_offset, code_size, physical_size);
  const uint32_t interpolators = (Be32(bytes, shader + 20) >> 5) & 31;
  size_t declarations = shader + 32;
  uint32_t vertices = 0;
  if (!pixel) {
    vertices = Be32(bytes, shader + 28);
    Require(vertices <= 96, "Too many shader vertex declarations");
    declarations = shader + 36 + size_t(Be32(bytes, shader + 24)) * 4;
  }
  Range(declarations, size_t(vertices + interpolators) * 4, virtual_size);
  for (uint32_t i = 0; i < vertices + interpolators; ++i) {
    const uint32_t declaration = Be32(bytes, declarations + i * 4);
    Require(((declaration >> (i < vertices ? 12 : 4)) & 15) < 14,
            "Invalid shader declaration semantic");
  }
  if (const size_t definitions = Be32(bytes, 20)) {
    Range(definitions, 24, virtual_size);
    size_t cursor = definitions + 20;
    while (const uint32_t definition = Be32(bytes.first(virtual_size), cursor)) {
      Range(cursor, 8, virtual_size);
      const uint32_t first = definition >> 16, count = ((definition & 65535) + 3) / 4;
      Require(first >= (pixel ? 256u : 0u) && first - (pixel ? 256u : 0u) + count <= 256,
              "Invalid shader float constant definition");
      Range(Be32(bytes, cursor + 4), size_t(count) * 16, physical_size);
      cursor += 8;
    }
    cursor += 4;
    while (const uint32_t definition = Be32(bytes.first(virtual_size), cursor)) {
      const uint32_t first = definition >> 16, count = definition & 65535;
      Require(first >= 8992 && (first - 8992) % 4 == 0 && (first - 8992) / 4 + count <= 32,
              "Invalid shader loop constant definition");
      Range(cursor, 8 + size_t(count) * 4, virtual_size);
      cursor += 8 + size_t(count) * 4;
    }
  }
  // The upstream parser follows execution clause offsets directly. Validate
  // every clause before handing it a pointer, including both packed CF words.
  const size_t code = virtual_size + code_offset;
  size_t control_bytes = code_size;
  for (size_t cursor = 0; cursor < control_bytes; cursor += 12) {
    Require(cursor + 12 <= code_size, "Truncated shader control-flow instructions");
    const uint32_t a = Be32(bytes, code + cursor), b = Be32(bytes, code + cursor + 4);
    const uint32_t c = Be32(bytes, code + cursor + 8);
    for (const uint64_t cf : {uint64_t(a) | (uint64_t(b & 65535) << 32),
                             uint64_t(b >> 16) | (uint64_t(c) << 16)}) {
      const uint32_t opcode = (cf >> 44) & 15;
      if ((opcode >= 1 && opcode <= 6) || opcode == 13 || opcode == 14) {
        const uint32_t address = cf & 4095, count = (cf >> 12) & 7;
        Range(size_t(address) * 12, size_t(count) * 12, code_size);
        if (address) control_bytes = std::min(control_bytes, size_t(address) * 12);
      }
    }
  }
  return pixel ? NativeShaderStage::Pixel : NativeShaderStage::Vertex;
}

std::string Hash(std::span<const uint8_t> bytes) {
  return picosha2::hash256_hex_string(bytes.begin(), bytes.end());
}

void Put32(std::vector<uint8_t>& bytes, uint32_t value) {
  for (unsigned shift = 0; shift != 32; shift += 8) bytes.push_back(uint8_t(value >> shift));
}

uint32_t Le32(std::span<const uint8_t> bytes, size_t offset) {
  return bytes[offset] | uint32_t(bytes[offset + 1]) << 8 |
         uint32_t(bytes[offset + 2]) << 16 | uint32_t(bytes[offset + 3]) << 24;
}

bool ValidSpirv(std::span<const uint32_t> words, NativeShaderStage stage) {
  if (words.size() < 5 || words.size_bytes() > kMaximumSpirvBytes || words[0] != 0x07230203 ||
      words[1] > 0x00010600 || words[1] < 0x00010000 || !words[3] || words[4]) return false;
  bool entry = false;
  for (size_t at = 5; at < words.size();) {
    const uint32_t count = words[at] >> 16, opcode = words[at] & 65535;
    if (!count || count > words.size() - at) return false;
    if (opcode == 15) {
      if (count < 4 || words[at + 1] != (stage == NativeShaderStage::Pixel ? 4u : 0u)) return false;
      entry = true;
    }
    at += count;
  }
  return entry;
}

template <typename T> struct ComRelease { void operator()(T* pointer) const { if (pointer) pointer->Release(); } };
template <typename T> using Com = std::unique_ptr<T, ComRelease<T>>;

std::vector<uint32_t> CompileSpirv(const std::string& hlsl, NativeShaderStage stage, NativeShaderMode mode) {
  IDxcCompiler3* raw_compiler = nullptr;
  const HRESULT created = DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&raw_compiler));
  Com<IDxcCompiler3> compiler(raw_compiler);
  Require(SUCCEEDED(created) && compiler, "Failed to initialize the DXC shader compiler");
  DxcBuffer source{hlsl.data(), hlsl.size(), DXC_CP_UTF8};
  std::vector<const wchar_t*> arguments{L"-T", stage == NativeShaderStage::Pixel ? L"ps_6_0" : L"vs_6_0",
      L"-E", L"main", L"-HV", L"2021", L"-spirv", L"-fvk-use-dx-layout"};
  if (stage == NativeShaderStage::Vertex) arguments.push_back(L"-fvk-invert-y");
  if (mode == NativeShaderMode::FragmentInterlock) {
    arguments.insert(arguments.end(), {L"-D", L"GOW3_NATIVE_FSI=1", L"-fspv-target-env=vulkan1.2",
                                      L"-fspv-extension=SPV_EXT_fragment_shader_interlock",
                                      L"-fspv-extension=SPV_EXT_descriptor_indexing",
                                      L"-fspv-extension=SPV_KHR_physical_storage_buffer"});
  }
  IDxcResult* raw_result = nullptr;
  const HRESULT compiled = compiler->Compile(&source, arguments.data(), uint32_t(arguments.size()), nullptr,
                                             IID_PPV_ARGS(&raw_result));
  Com<IDxcResult> result(raw_result);
  Require(SUCCEEDED(compiled) && result, "DXC invocation failed");
  HRESULT status = E_FAIL;
  Require(SUCCEEDED(result->GetStatus(&status)), "DXC did not report compilation status");
  if (FAILED(status)) {
    IDxcBlobUtf8* raw_errors = nullptr;
    result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&raw_errors), nullptr);
    Com<IDxcBlobUtf8> errors(raw_errors);
    throw std::runtime_error(std::string("Native SPIR-V compilation failed: ") +
        (errors ? std::string(errors->GetStringPointer(), errors->GetStringLength()) : "No diagnostics"));
  }
  IDxcBlob* raw_object = nullptr;
  const HRESULT obtained = result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&raw_object), nullptr);
  Com<IDxcBlob> object(raw_object);
  Require(SUCCEEDED(obtained) && object && object->GetBufferSize() <= kMaximumSpirvBytes && object->GetBufferSize() % 4 == 0,
          "DXC returned invalid SPIR-V");
  std::vector<uint32_t> words(object->GetBufferSize() / 4);
  std::memcpy(words.data(), object->GetBufferPointer(), object->GetBufferSize());
  Require(ValidSpirv(words, stage), "DXC returned malformed SPIR-V");
  return words;
}

bool LoadCache(const std::filesystem::path& path, CompiledNativeShader& shader) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input) return false;
  const auto length = input.tellg();
  if (length < std::streamoff(kCacheHeaderBytes) || length > std::streamoff(kCacheHeaderBytes + kMaximumSpirvBytes)) return false;
  std::vector<uint8_t> bytes(static_cast<size_t>(length));
  input.seekg(0);
  if (!input.read(reinterpret_cast<char*>(bytes.data()), length)) return false;
  if (!std::equal(kCacheMagic.begin(), kCacheMagic.end(), bytes.begin()) ||
      Le32(bytes, 8) != uint32_t(shader.stage) || size_t(Le32(bytes, 16)) * 4 != bytes.size() - kCacheHeaderBytes ||
      Le32(bytes, 20) != uint32_t(shader.mode) ||
      !std::equal(shader.source_sha256.begin(), shader.source_sha256.end(), bytes.begin() + 24)) return false;
  std::vector<uint8_t> checked(bytes.begin() + 8, bytes.begin() + 88);
  checked.insert(checked.end(), bytes.begin() + kCacheHeaderBytes, bytes.end());
  const auto digest = Hash(checked);
  if (!std::equal(digest.begin(), digest.end(), bytes.begin() + 88)) return false;
  shader.spirv.resize(Le32(bytes, 16));
  for (size_t i = 0; i != shader.spirv.size(); ++i) shader.spirv[i] = Le32(bytes, kCacheHeaderBytes + i * 4);
  if (!ValidSpirv(shader.spirv, shader.stage)) { shader.spirv.clear(); return false; }
  shader.specialization_mask = Le32(bytes, 12);
  shader.disk_cache_hit = true;
  return true;
}

void StoreCache(const std::filesystem::path& path, const CompiledNativeShader& shader) {
  // A read-only or full cache must not prevent rendering. Cache writes are best
  // effort; successful compilation is held in memory regardless of disk state.
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  if (error) return;
  std::vector<uint8_t> checked;
  Put32(checked, uint32_t(shader.stage));
  Put32(checked, shader.specialization_mask);
  Put32(checked, uint32_t(shader.spirv.size()));
  Put32(checked, uint32_t(shader.mode));
  checked.insert(checked.end(), shader.source_sha256.begin(), shader.source_sha256.end());
  for (uint32_t word : shader.spirv) Put32(checked, word);
  const auto digest = Hash(checked);
  std::vector<uint8_t> bytes(kCacheMagic.begin(), kCacheMagic.end());
  bytes.insert(bytes.end(), checked.begin(), checked.begin() + 80);
  bytes.insert(bytes.end(), digest.begin(), digest.end());
  bytes.insert(bytes.end(), checked.begin() + 80, checked.end());
  static std::atomic<uint64_t> sequence{0};
  static const uint64_t nonce = [] { std::random_device rng; return uint64_t(rng()) << 32 | rng(); }();
  auto temporary = path;
  temporary += ".tmp." + std::to_string(nonce) + "." + std::to_string(sequence++);
  std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
  if (!output) return;
  output.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
  output.close();
  if (output) {
    std::filesystem::rename(temporary, path, error);
    if (error && std::filesystem::exists(path)) {
      // Windows rename does not replace an existing destination. Retain any
      // valid cache another process published; otherwise replace the corrupt
      // entry. Readers always validate the digest before using a cache file.
      CompiledNativeShader existing;
      existing.stage = shader.stage;
      existing.mode = shader.mode;
      existing.source_sha256 = shader.source_sha256;
      if (!LoadCache(path, existing)) {
        std::filesystem::remove(path, error);
        if (!error) std::filesystem::rename(temporary, path, error);
      }
    }
  }
  std::filesystem::remove(temporary, error);
}
}  // namespace

struct NativeShaderCompiler::Impl {
  std::filesystem::path cache_directory;
  std::mutex mutex;
  std::unordered_map<std::string, std::shared_future<std::shared_ptr<const CompiledNativeShader>>> compiled;
  std::shared_ptr<const CompiledNativeShader> Get(const std::string& source_hash,
      NativeShaderStage stage, NativeShaderMode mode,
      const std::function<void(CompiledNativeShader&)>& generate);
};

NativeShaderCompiler::NativeShaderCompiler(std::filesystem::path cache_directory) : impl_(std::make_unique<Impl>()) {
  if (!cache_directory.empty()) impl_->cache_directory = std::move(cache_directory) / CacheVersion();
}
NativeShaderCompiler::~NativeShaderCompiler() = default;
std::string NativeShaderCompiler::CacheVersion() { return kNativeShaderVersion; }

std::shared_ptr<const CompiledNativeShader> NativeShaderCompiler::Get(
    std::span<const uint8_t> container, NativeShaderMode mode) {
  const auto stage = ValidateContainer(container);
  Require(mode == NativeShaderMode::Raster || mode == NativeShaderMode::FragmentInterlock,
          "Invalid native shader compilation mode");
  Require(mode != NativeShaderMode::FragmentInterlock || stage == NativeShaderStage::Pixel,
          "Fragment interlock requires a pixel shader");
  const auto source_hash = Hash(container);
  return impl_->Get(source_hash, stage, mode, [&](CompiledNativeShader& shader) {
    // Copy into an aligned allocation before passing the upstream parser a
    // typed pointer. The caller's span may start at an arbitrary byte.
    std::vector<uint32_t> aligned((container.size() + 3) / 4);
    std::memcpy(aligned.data(), container.data(), container.size());
    ShaderRecompiler recompiler;
    recompiler.recompile(reinterpret_cast<const uint8_t*>(aligned.data()), kNativeShaderCommon);
    shader.specialization_mask = recompiler.specConstantsMask;
    shader.spirv = CompileSpirv(recompiler.out, stage, mode);
  });
}

std::shared_ptr<const CompiledNativeShader> NativeShaderCompiler::GetDepthOnlyFragmentInterlock() {
  static const std::string source = std::string(kNativeShaderCommon) + R"HLSL(
void main(float4 p : SV_Position, bool face : SV_IsFrontFace) {
  float2 slope = float2(ddx_coarse(p.z), ddy_coarse(p.z));
  const float4 zero = float4(0, 0, 0, 0);
  gow3NativeRasterOutput(p.xy, p.z, face, zero, zero, zero, zero, 0u, false, slope);
}
)HLSL";
  static const auto hash = Hash(std::span(reinterpret_cast<const uint8_t*>(source.data()), source.size()));
  return impl_->Get(hash, NativeShaderStage::Pixel, NativeShaderMode::FragmentInterlock,
      [&](CompiledNativeShader& shader) {
        shader.spirv = CompileSpirv(source, shader.stage, shader.mode);
      });
}

std::shared_ptr<const CompiledNativeShader> NativeShaderCompiler::Impl::Get(
    const std::string& source_hash, NativeShaderStage stage, NativeShaderMode mode,
    const std::function<void(CompiledNativeShader&)>& generate) {
  const auto key = source_hash + (mode == NativeShaderMode::FragmentInterlock ? ".fsi" : "");
  std::shared_future<std::shared_ptr<const CompiledNativeShader>> future;
  std::shared_ptr<std::promise<std::shared_ptr<const CompiledNativeShader>>> producer;
  {
    std::lock_guard lock(mutex);
    if (auto found = compiled.find(key); found != compiled.end()) future = found->second;
    else {
      producer = std::make_shared<std::promise<std::shared_ptr<const CompiledNativeShader>>>();
      future = producer->get_future().share();
      compiled.emplace(key, future);
    }
  }
  if (producer) {
    try {
      auto shader = std::make_shared<CompiledNativeShader>();
      shader->stage = stage;
      shader->mode = mode;
      shader->source_sha256 = source_hash;
      const auto path = cache_directory / (key + ".g3spv");
      if (cache_directory.empty() || !LoadCache(path, *shader)) {
        generate(*shader);
        if (!cache_directory.empty()) {
          try { StoreCache(path, *shader); }
          catch (const std::exception&) { /* Rendering can proceed without a disk cache. */ }
        }
      }
      producer->set_value(std::move(shader));
    } catch (...) {
      producer->set_exception(std::current_exception());
    }
  }
  return future.get();
}

}  // namespace gow3::renderer
