#include "renderer/native/content_hash.h"
#include "renderer/native/shader_compiler.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <thread>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace gow3::renderer;

namespace {
struct Options {
  fs::path cache;
  std::optional<fs::path> output;
  unsigned jobs = std::max(1u, std::thread::hardware_concurrency());
  std::vector<fs::path> manifests;
};
struct Source {
  std::string hash;
  NativeShaderStage stage;
  std::vector<uint8_t> bytes;
  std::set<fs::path> paths;
};

Options Parse(int argc, char** argv) {
  Options result;
  bool positional = false;
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if (!positional && argument == "--") { positional = true; continue; }
    if (!positional && (argument == "--cache" || argument == "--spirv-output" || argument == "--jobs")) {
      if (++i == argc || !*argv[i]) throw std::runtime_error("Missing value for " + argument);
      if (argument == "--cache") result.cache = argv[i];
      else if (argument == "--spirv-output") result.output = argv[i];
      else {
        const std::string count = argv[i];
        const auto parsed = std::from_chars(count.data(), count.data() + count.size(), result.jobs);
        if (parsed.ec != std::errc{} || parsed.ptr != count.data() + count.size() || !result.jobs)
          throw std::runtime_error("--jobs must be a positive integer");
      }
    } else if (!positional && argument.starts_with("--")) {
      throw std::runtime_error("Unknown option: " + argument);
    } else result.manifests.emplace_back(argument);
  }
  if (result.cache.empty() || result.manifests.empty())
    throw std::runtime_error("Usage: gow3-native-shader-prewarm --cache DIR [--jobs N] [--spirv-output DIR] manifest.json ...");
  return result;
}

std::vector<uint8_t> Read(const fs::path& path, size_t maximum) {
  if (!fs::is_regular_file(path)) throw std::runtime_error("Not a regular input file: " + path.string());
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  const auto size = file.tellg();
  if (!file || size < 0 || uint64_t(size) > maximum)
    throw std::runtime_error("Invalid or oversized input: " + path.string());
  std::vector<uint8_t> bytes(static_cast<size_t>(size));
  file.seekg(0);
  if (!file.read(reinterpret_cast<char*>(bytes.data()), bytes.size()))
    throw std::runtime_error("Cannot read input: " + path.string());
  return bytes;
}

bool Contains(const fs::path& directory, const fs::path& file) {
  auto d = directory.begin(), f = file.begin();
  for (; d != directory.end(); ++d, ++f) if (f == file.end() || *d != *f) return false;
  return true;
}

fs::path CheckDirectory(const fs::path& path, const std::set<fs::path>& inputs) {
  const auto normalized = fs::weakly_canonical(fs::absolute(path));
  auto ancestor = normalized;
  while (!ancestor.empty() && !fs::exists(ancestor)) ancestor = ancestor.parent_path();
  if (ancestor.empty() || !fs::is_directory(ancestor))
    throw std::runtime_error("Output path has a non-directory ancestor: " + path.string());
  for (const auto& input : inputs) if (Contains(normalized, input))
    throw std::runtime_error("Output directory contains source assets: " + normalized.string());
  return normalized;
}

void CheckOutput(const fs::path& path, const std::set<fs::path>& inputs) {
  if (fs::is_symlink(fs::symlink_status(path)))
    throw std::runtime_error("Output file is a symbolic link: " + path.string());
  if (fs::exists(path)) {
    if (!fs::is_regular_file(path)) throw std::runtime_error("Output is not a regular file: " + path.string());
    for (const auto& input : inputs) if (fs::equivalent(input, path))
      throw std::runtime_error("Output would overwrite a source asset: " + path.string());
  }
}

void Write(const fs::path& path, std::span<const uint8_t> bytes) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  file.close();
  if (!file) throw std::runtime_error("Cannot write output: " + path.string());
}
} // namespace

int main(int argc, char** argv) {
  try {
    const auto started = std::chrono::steady_clock::now();
    const auto options = Parse(argc, argv);
    std::map<std::string, Source> unique;
    std::set<fs::path> inputs, manifests;
    size_t manifest_entries = 0, retained_bytes = 0;
    // Finish path, identity and manifest validation before creating any output.
    // Retained bytes keep compilation independent of later input-file changes.
    for (const auto& argument : options.manifests) {
      const auto path = fs::canonical(argument);
      inputs.insert(path);
      manifests.insert(path);
      const auto data = Read(path, 64 * 1024 * 1024);
      const auto manifest = json::parse(data);
      if (!manifest.at("shaders").is_array()) throw std::runtime_error("Manifest shaders must be an array");
      for (const auto& item : manifest.at("shaders")) {
        ++manifest_entries;
        const auto hash = item.at("sha256").get<std::string>();
        if (hash.size() != 64 || hash.find_first_not_of("0123456789abcdef") != std::string::npos)
          throw std::runtime_error("Invalid shader SHA-256 in " + path.string());
        const auto stage_name = item.at("stage").get<std::string>();
        if (stage_name != "vertex" && stage_name != "pixel") throw std::runtime_error("Invalid shader stage: " + stage_name);
        const auto stage = stage_name == "vertex" ? NativeShaderStage::Vertex : NativeShaderStage::Pixel;
        const auto source_path = fs::canonical(path.parent_path() / item.at("path").get<std::string>());
        inputs.insert(source_path);
        auto bytes = Read(source_path, 16 * 1024 * 1024);
        if (ContentHash(bytes) != hash) throw std::runtime_error("Shader SHA-256 mismatch: " + source_path.string());
        for (const char* field : {"size", "bytes"}) if (item.contains(field) && item.at(field).get<uint64_t>() != bytes.size())
          throw std::runtime_error("Shader byte count mismatch: " + source_path.string());
        if (bytes.size() < 4) throw std::runtime_error("Truncated shader container: " + source_path.string());
        // Container flags are big endian; full structural validation is performed
        // by the compiler. Catch manifest stage mistakes before touching its cache.
        const auto encoded_stage = (bytes[3] & 1) ? NativeShaderStage::Vertex : NativeShaderStage::Pixel;
        if (encoded_stage != stage) throw std::runtime_error("Manifest shader stage mismatch: " + source_path.string());
        if (const auto found = unique.find(hash); found != unique.end()) {
          if (found->second.stage != stage || found->second.bytes != bytes)
            throw std::runtime_error("Conflicting duplicate shader: " + hash);
          found->second.paths.insert(source_path);
        } else {
          retained_bytes += bytes.size();
          if (retained_bytes > 512 * 1024 * 1024) throw std::runtime_error("Unique shader containers exceed the 512 MiB input limit");
          unique.emplace(hash, Source{hash, stage, std::move(bytes), {source_path}});
        }
      }
    }
    if (unique.empty()) throw std::runtime_error("Manifests contain no shaders");
    const auto version = NativeShaderCompiler::CacheVersion();
    const auto cache = fs::weakly_canonical(fs::absolute(options.cache));
    const auto version_directory = CheckDirectory(cache / version, inputs);
    const auto report_path = version_directory / "prewarm-report.json";
    CheckOutput(report_path, inputs);
    std::optional<fs::path> output;
    if (options.output) output = CheckDirectory(*options.output, inputs);
    if (fs::exists(version_directory)) {
      // The compiler also creates a generated depth-only entry. Validate existing
      // entries without depending on its private generated-source cache key.
      for (const auto& entry : fs::recursive_directory_iterator(version_directory))
        if (!entry.is_directory() || entry.is_symlink()) CheckOutput(entry.path(), inputs);
    }
    for (const auto& [hash, source] : unique) {
      CheckOutput(NativeShaderCompiler::CachePath(version_directory, hash), inputs);
      CheckOutput(NativeShaderCompiler::CachePath(version_directory, hash + ".fsi"), inputs);
      if (output) {
        CheckOutput(*output / (hash + ".spv"), inputs);
        CheckOutput(*output / (hash + ".fsi.spv"), inputs);
      }
    }
    if (output) CheckOutput(*output / "depth-only.fsi.spv", inputs);
    fs::create_directories(version_directory);
    if (output) fs::create_directories(*output);

    NativeShaderCompiler compiler(cache), persisted(cache);
    std::vector<const Source*> sources;
    size_t variants = 1;
    for (const auto& [hash, source] : unique) {
      sources.push_back(&source);
      variants += source.stage == NativeShaderStage::Pixel ? 2 : 1;
    }
    std::vector<json> results(sources.size() + 1);
    std::atomic<size_t> next{0};
    std::mutex progress_mutex;
    size_t completed = 0, failed = 0, hits = 0;
    size_t last_percent = 0;
    const auto worker_count = std::min<size_t>(options.jobs, sources.size());
    std::cerr << "Prewarming " << sources.size() << " unique shaders (" << variants << " variants) with "
              << worker_count << " workers; cache " << version_directory << '\n';
    auto compile_variant = [&](const Source* source, NativeShaderMode mode) {
      json result = {{"mode", mode == NativeShaderMode::Raster ? "raster" : "fragment_interlock"},
                     {"success", false}, {"disk_cache_hit", false}};
      try {
        const auto compiled = source ? compiler.Get(source->bytes, mode) : compiler.GetDepthOnlyFragmentInterlock();
        if (source && (compiled->source_sha256 != source->hash || compiled->stage != source->stage))
          throw std::runtime_error("Compiled source identity or stage mismatch");
        if (compiled->mode != mode || compiled->spirv.empty()) throw std::runtime_error("Invalid compiled shader mode/output");
        result["source_sha256"] = compiled->source_sha256;
        result["disk_cache_hit"] = compiled->disk_cache_hit;
        result["spirv_bytes"] = compiled->spirv.size() * sizeof(uint32_t);
        const auto cache_path = NativeShaderCompiler::CachePath(version_directory, compiled->source_sha256 +
            (mode == NativeShaderMode::Raster ? "" : ".fsi"));
        if (!fs::is_regular_file(cache_path)) throw std::runtime_error("Compiler did not persist cache entry: " + cache_path.string());
        const auto verified = source ? persisted.Get(source->bytes, mode) : persisted.GetDepthOnlyFragmentInterlock();
        if (!verified->disk_cache_hit || verified->spirv != compiled->spirv)
          throw std::runtime_error("Persisted cache did not validate identically");
        result["persisted_cache_verified"] = true;
        if (output) {
          const auto destination = *output / (source ? source->hash +
              (mode == NativeShaderMode::Raster ? ".spv" : ".fsi.spv") : "depth-only.fsi.spv");
          Write(destination, {reinterpret_cast<const uint8_t*>(compiled->spirv.data()), compiled->spirv.size() * sizeof(uint32_t)});
          result["spirv_path"] = destination.string();
        }
        result["success"] = true;
      } catch (const std::exception& error) { result["error"] = error.what(); }
      {
        std::lock_guard lock(progress_mutex);
        ++completed;
        failed += !result.at("success").get<bool>();
        hits += result.at("disk_cache_hit").get<bool>();
        const auto percent = completed * 100 / variants;
        if (completed == 1 || percent != last_percent) {
          std::cerr << "Shaders " << completed << '/' << variants << " (" << percent
                    << "%) cache hits " << hits << ", failures " << failed << '\n';
          last_percent = percent;
        }
        if (!result.at("success").get<bool>()) std::cerr << (source ? source->hash : "depth-only") << ": " << result.at("error").get<std::string>() << '\n';
      }
      return result;
    };
    {
      std::vector<std::jthread> workers;
      workers.reserve(worker_count);
      for (size_t thread = 0; thread < worker_count; ++thread) workers.emplace_back([&] {
        for (;;) {
          const auto index = next.fetch_add(1);
          if (index >= sources.size() + 1) return;
          if (index == sources.size()) {
            results[index] = {{"kind", "generated_depth_only"}, {"stage", "pixel"},
                              {"variants", json::array({compile_variant(nullptr, NativeShaderMode::FragmentInterlock)})}};
          } else {
            const auto* source = sources[index];
            json paths = json::array();
            for (const auto& path : source->paths) paths.push_back(path.string());
            auto modes = json::array({compile_variant(source, NativeShaderMode::Raster)});
            if (source->stage == NativeShaderStage::Pixel) modes.push_back(compile_variant(source, NativeShaderMode::FragmentInterlock));
            results[index] = {{"source_sha256", source->hash}, {"stage", source->stage == NativeShaderStage::Vertex ? "vertex" : "pixel"},
                              {"bytes", source->bytes.size()}, {"paths", paths}, {"variants", std::move(modes)}};
          }
        }
      });
    }
    json manifest_paths = json::array();
    for (const auto& path : manifests) manifest_paths.push_back(path.string());
    json report = {{"success", failed == 0}, {"cache_version", version}, {"cache_directory", version_directory.string()},
                   {"report_path", report_path.string()}, {"manifests", manifest_paths}, {"manifest_entries", manifest_entries},
                   {"unique_sources", sources.size()}, {"duplicates_removed", manifest_entries - sources.size()},
                   {"workers", worker_count}, {"variants_total", variants}, {"variants_completed", completed},
                   {"variants_succeeded", completed - failed}, {"variants_failed", failed}, {"disk_cache_hits", hits},
                   {"elapsed_seconds", std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count()},
                   {"sources", results}, {"scope", "Known extracted shader containers only; does not discover all game shaders or build Vulkan pipelines."}};
    const auto serialized = report.dump(2) + '\n';
    Write(report_path, {reinterpret_cast<const uint8_t*>(serialized.data()), serialized.size()});
    report.erase("sources");
    std::cout << report.dump(2) << '\n';
    return failed ? 1 : 0;
  } catch (const std::exception& error) {
    std::cerr << "Shader prewarm failed: " << error.what() << '\n';
    std::cout << json({{"success", false}, {"error", error.what()}}).dump(2) << '\n';
    return 1;
  }
}
