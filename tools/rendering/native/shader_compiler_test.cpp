#include "renderer/native/shader_compiler.h"

#include <fstream>
#include <future>
#include <iostream>
#include <stdexcept>
#include <nlohmann/json.hpp>

using namespace gow3::renderer;
using json = nlohmann::json;

std::vector<uint8_t> Read(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file || file.tellg() < 0 || file.tellg() > 16 * 1024 * 1024) throw std::runtime_error("Invalid input file");
  std::vector<uint8_t> bytes(static_cast<size_t>(file.tellg()));
  file.seekg(0);
  if (!file.read(reinterpret_cast<char*>(bytes.data()), bytes.size())) throw std::runtime_error("Input read failed");
  return bytes;
}

void Require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

bool HasCapability(std::span<const uint32_t> words, uint32_t capability) {
  for (size_t at = 5; at < words.size(); at += words[at] >> 16) {
    if ((words[at] & 65535) == 17 && words[at] >> 16 == 2 && words[at + 1] == capability) return true;
  }
  return false;
}

int main(int argc, char** argv) {
  try {
    if (argc < 3 || argc > 4) throw std::runtime_error("Usage: gow3-native-shader-compiler-test manifest.json cache-directory [compiled-reference-directory]");
    const std::filesystem::path manifest_path = argv[1], cache = argv[2];
    std::ifstream input(manifest_path);
    const auto manifest = json::parse(input);
    NativeShaderCompiler compiler(cache);
    size_t compiled = 0, disk_hits = 0, rejected = 0, references = 0, interlock_variants = 0;
    std::vector<uint8_t> sample, pixel_sample, vertex_sample;
    std::string same_stage_alternate;
    NativeShaderStage sample_stage{};
    for (const auto& source : manifest.at("shaders")) {
      const auto bytes = Read(manifest_path.parent_path() / source.at("path").get<std::string>());
      auto shader = compiler.Get(bytes);
      Require(shader->source_sha256 == source.at("sha256").get<std::string>(), "Source hash mismatch");
      Require((shader->stage == NativeShaderStage::Pixel) == (source.at("stage").get<std::string>() == "pixel"), "Stage mismatch");
      Require(!shader->spirv.empty(), "Empty compiled shader");
      Require(!HasCapability(shader->spirv, 5378), "Default shader unexpectedly requires fragment interlock");
      if (shader->stage == NativeShaderStage::Pixel) {
        const auto fsi = compiler.Get(bytes, NativeShaderMode::FragmentInterlock);
        Require(fsi->mode == NativeShaderMode::FragmentInterlock && HasCapability(fsi->spirv, 5378),
                "Interlock shader is missing its capability");
        Require(fsi != shader && fsi->spirv != shader->spirv, "Shader modes collided in memory cache");
        Require(fsi->source_sha256 == shader->source_sha256, "Shader mode changed source identity");
        const auto output = cache / NativeShaderCompiler::CacheVersion() / (shader->source_sha256 + ".fsi.spv");
        std::ofstream file(output, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(fsi->spirv.data()), fsi->spirv.size() * 4);
        Require(bool(file), "Cannot write FSI validation artifact");
        if (pixel_sample.empty()) pixel_sample = bytes;
        ++interlock_variants;
      } else if (vertex_sample.empty()) vertex_sample = bytes;
      if (argc == 4) {
        const auto expected = Read(std::filesystem::path(argv[3]) / (shader->source_sha256 + ".spv"));
        const auto* actual = reinterpret_cast<const uint8_t*>(shader->spirv.data());
        Require(expected.size() == shader->spirv.size() * 4 && std::equal(expected.begin(), expected.end(), actual),
                "In-process SPIR-V differs from the standalone validated compiler");
        ++references;
      }
      ++compiled;
      disk_hits += shader->disk_cache_hit;
      if (sample.empty()) { sample = bytes; sample_stage = shader->stage; }
      else if (same_stage_alternate.empty() && sample_stage == shader->stage) same_stage_alternate = shader->source_sha256;
    }
    Require(compiled > 0, "Manifest contains no shaders");
    const auto depth_only = compiler.GetDepthOnlyFragmentInterlock();
    Require(HasCapability(depth_only->spirv, 5378) &&
                depth_only == compiler.GetDepthOnlyFragmentInterlock(),
            "Depth-only interlock shader compilation or deduplication failed");
    {
      std::ofstream file(cache / NativeShaderCompiler::CacheVersion() / "depth-only.fsi.spv",
                         std::ios::binary | std::ios::trunc);
      file.write(reinterpret_cast<const char*>(depth_only->spirv.data()), depth_only->spirv.size() * 4);
      Require(bool(file), "Cannot write depth-only validation artifact");
    }
    bool invalid_mode_rejected = false;
    try { compiler.Get(sample, static_cast<NativeShaderMode>(UINT32_MAX)); }
    catch (const std::exception&) { invalid_mode_rejected = true; }
    Require(invalid_mode_rejected, "Invalid native shader mode was accepted");
    if (!vertex_sample.empty()) {
      bool invalid_stage_rejected = false;
      try { compiler.Get(vertex_sample, NativeShaderMode::FragmentInterlock); }
      catch (const std::exception&) { invalid_stage_rejected = true; }
      Require(invalid_stage_rejected, "Vertex shader accepted fragment interlock mode");
    }
    // Reopen a new provider so this exercises serialized disk cache loading.
    NativeShaderCompiler reopened(cache);
    Require(reopened.Get(sample)->disk_cache_hit, "Persisted cache was not reused");
    Require(reopened.GetDepthOnlyFragmentInterlock()->disk_cache_hit,
            "Persisted depth-only interlock shader was not reused");
    if (!pixel_sample.empty()) {
      const auto fsi = reopened.Get(pixel_sample, NativeShaderMode::FragmentInterlock);
      Require(fsi->disk_cache_hit, "Persisted interlock shader was not reused");
      const auto path = cache / NativeShaderCompiler::CacheVersion() / fsi->source_sha256;
      std::filesystem::copy_file(path.string() + ".fsi.g3spv", path.string() + ".g3spv",
                                std::filesystem::copy_options::overwrite_existing);
      NativeShaderCompiler wrong_mode(cache);
      const auto restored = wrong_mode.Get(pixel_sample);
      Require(!restored->disk_cache_hit && !HasCapability(restored->spirv, 5378),
              "Interlock shader was accepted from a raster cache filename");
    }
    NativeShaderCompiler parallel;
    std::vector<std::future<std::shared_ptr<const CompiledNativeShader>>> requests;
    for (unsigned i = 0; i != 8; ++i) requests.push_back(std::async(std::launch::async, [&] { return parallel.Get(sample); }));
    const auto shared = requests.front().get();
    for (size_t i = 1; i < requests.size(); ++i) Require(requests[i].get() == shared, "Duplicate concurrent shader compilation");
    // Every prefix is malformed; validate without invoking the translator.
    for (size_t length : {size_t(0), size_t(35), size_t(36), sample.size() - 1}) {
      try { parallel.Get(std::span(sample).first(length)); }
      catch (const std::exception&) { ++rejected; continue; }
      throw std::runtime_error("Truncated shader accepted");
    }
    auto malformed = sample;
    malformed[24] = 0xff;  // Shader metadata offset outside the container.
    try { parallel.Get(malformed); }
    catch (const std::exception&) { ++rejected; }
    Require(rejected == 5, "Malformed shader accepted");
    // Unsupported subroutines must be recoverable in-process, never an assert
    // that terminates the game. Replace the first control-flow opcode with CALL.
    auto unsupported = sample;
    const auto be32 = [&](size_t offset) {
      return uint32_t(sample[offset]) << 24 | uint32_t(sample[offset + 1]) << 16 |
             uint32_t(sample[offset + 2]) << 8 | sample[offset + 3];
    };
    const auto code = be32(4) + be32(be32(24));
    unsupported[code + 6] = uint8_t((unsupported[code + 6] & 15) | 0x90);
    bool unsupported_rejected = false;
    try { parallel.Get(unsupported); }
    catch (const std::exception& error) {
      unsupported_rejected = std::string(error.what()).find("Shader subroutines") != std::string::npos;
    }
    Require(unsupported_rejected, "Unsupported shader construct did not return a recoverable error");
    // A corrupted cache payload is discarded and rebuilt from the source.
    const auto shader = compiler.Get(sample);
    const auto cache_file = cache / NativeShaderCompiler::CacheVersion() / (shader->source_sha256 + ".g3spv");
    {
      std::fstream file(cache_file, std::ios::binary | std::ios::in | std::ios::out);
      Require(bool(file), "Cannot open test cache");
      file.seekp(152);
      file.put(0);
    }
    NativeShaderCompiler repaired(cache);
    Require(!repaired.Get(sample)->disk_cache_hit && repaired.Get(sample)->spirv == shader->spirv,
            "Corrupt cache was not safely rebuilt");
    if (!same_stage_alternate.empty()) {
      std::filesystem::copy_file(cache / NativeShaderCompiler::CacheVersion() / (same_stage_alternate + ".g3spv"),
          cache_file, std::filesystem::copy_options::overwrite_existing);
      NativeShaderCompiler misplaced(cache);
      Require(!misplaced.Get(sample)->disk_cache_hit && misplaced.Get(sample)->spirv == shader->spirv,
              "Cache file for a different source shader was accepted");
    }
    // An ordinary file cannot contain a cache directory. Compilation must still
    // succeed when persistent storage is unavailable.
    NativeShaderCompiler unavailable_cache(manifest_path);
    Require(unavailable_cache.Get(sample)->spirv == shader->spirv, "Unavailable disk cache blocked compilation");
    std::cout << "Compiled " << compiled << " shaders in process; " << disk_hits << " disk cache hits; "
              << references << " byte-identical reference binaries; " << rejected
              << " malformed inputs rejected; " << interlock_variants
              << " separate interlock variants; unsupported-construct recovery, concurrent deduplication, cache identity, and unavailable/corrupt-cache recovery passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
