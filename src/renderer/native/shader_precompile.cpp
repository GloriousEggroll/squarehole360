// Cooked packages (.xxx) are UE3 files whose body is compressed in chunks: a
// header {9E2A83C1, 0x20000 block size, compressed, uncompressed}, a table of
// per-block sizes, then XMemCompress (LZX) blocks. Material shaders sit inside
// the decompressed body in units, each preceded by {uncompressed size,
// compressed size}, compressed again with XMemCompress. A unit decodes to one
// or more pixel shader containers, extracted with the same validator the game
// uses for blocks its shader-cache decoder produces. GlobalShaderCache files
// hold the same units without the package compression.
#include "renderer/native/shader_precompile.h"

#include "renderer/native/content_hash.h"
#include "renderer/native/shader_block_discovery.h"
#include "renderer/native/shader_compiler.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <span>
#include <thread>
#include <tuple>
#include <unordered_set>

extern "C" {
#include <lzx.h>
#include <mspack.h>
}

namespace gow3::renderer {
namespace {
using Bytes = std::vector<uint8_t>;

uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

// libmspack I/O over memory. The decoder's window and buffers are pooled per
// thread: the unit scan tries a decoder at many candidate offsets.
struct MemoryFile {
  const uint8_t* data;
  size_t size, position;
  uint8_t* out;
};
int Read(mspack_file* file, void* buffer, int bytes) {
  auto* m = reinterpret_cast<MemoryFile*>(file);
  const size_t n = std::min<size_t>(size_t(bytes), m->size - m->position);
  std::memcpy(buffer, m->data + m->position, n);
  m->position += n;
  return int(n);
}
int Write(mspack_file* file, void* buffer, int bytes) {
  auto* m = reinterpret_cast<MemoryFile*>(file);
  const size_t n = std::min<size_t>(size_t(bytes), m->size - m->position);
  std::memcpy(m->out + m->position, buffer, n);
  m->position += n;
  return int(n);
}
struct Pool {
  std::vector<std::pair<size_t, void*>> idle;
  ~Pool() {
    for (auto& [size, block] : idle) std::free(static_cast<size_t*>(block) - 2);
  }
};
thread_local Pool pool;
void* Allocate(mspack_system*, size_t bytes) {
  for (auto it = pool.idle.begin(); it != pool.idle.end(); ++it)
    if (it->first == bytes) {
      void* block = it->second;
      pool.idle.erase(it);
      return std::memset(block, 0, bytes);
    }
  auto* header = static_cast<size_t*>(std::malloc(bytes + 2 * sizeof(size_t)));
  if (!header) return nullptr;
  header[0] = bytes;
  return std::memset(header + 2, 0, bytes);
}
void Free(void* block) {
  if (!block) return;
  if (pool.idle.size() < 16) {
    pool.idle.emplace_back(static_cast<size_t*>(block)[-2], block);
    return;
  }
  std::free(static_cast<size_t*>(block) - 2);
}
void Copy(void* source, void* destination, size_t bytes) { std::memmove(destination, source, bytes); }
void Message(mspack_file*, const char*, ...) {}

// Raw LZX (frame headers already removed) into exactly out.size() bytes.
bool Lzx(std::span<const uint8_t> input, std::span<uint8_t> out) {
  mspack_system system{};
  system.read = Read;
  system.write = Write;
  system.alloc = Allocate;
  system.free = Free;
  system.copy = Copy;
  system.message = Message;
  MemoryFile in{input.data(), input.size(), 0, nullptr}, output{nullptr, out.size(), 0, out.data()};
  auto* stream = lzxd_init(&system, reinterpret_cast<mspack_file*>(&in), reinterpret_cast<mspack_file*>(&output),
                           17, 0, 4096, off_t(out.size()), 0);
  if (!stream) return false;
  const int result = lzxd_decompress(stream, off_t(out.size()));
  lzxd_free(stream);
  return result == MSPACK_ERR_OK && output.position == out.size();
}

// XMemCompress output: frames with a 2-byte big-endian compressed size (32 KiB
// out), or 0xFF then 2-byte output and compressed sizes. A unit ends after its
// first 0xFF frame. Returns false when the frames don't describe a unit.
struct Frames {
  Bytes joined;
  size_t consumed = 0, output = 0;
};
bool ParseFrames(std::span<const uint8_t> data, bool single_block, Frames& frames) {
  frames.joined.clear();
  frames.consumed = frames.output = 0;
  while (frames.consumed < data.size()) {
    const uint8_t* q = data.data() + frames.consumed;
    const size_t left = data.size() - frames.consumed;
    size_t out, packed, header;
    // Package blocks end with a few padding bytes after their last frame;
    // the decoder's exact output size validates what was joined.
    if (q[0] == 0xFF) {
      if (left < 5) return single_block;
      out = size_t(q[1]) << 8 | q[2];
      packed = size_t(q[3]) << 8 | q[4];
      header = 5;
    } else {
      if (left < 2) return single_block;
      out = 0x8000;
      packed = size_t(q[0]) << 8 | q[1];
      header = 2;
    }
    if (!out || !packed || packed > 0x9800 || header + packed > left) return single_block && !frames.joined.empty();
    frames.joined.insert(frames.joined.end(), q + header, q + header + packed);
    frames.consumed += header + packed;
    frames.output += out;
    if (header == 5 && !single_block) return true;
  }
  return single_block;
}

// Every package compressed chunk, decompressed and concatenated.
Bytes DecompressPackage(const Bytes& file) {
  Bytes body;
  Frames frames;
  for (size_t i = 0; i + 16 <= file.size(); ++i) {
    if (Be32(&file[i]) != 0x9E2A83C1u || Be32(&file[i + 4]) != 0x20000) continue;
    const uint32_t packed = Be32(&file[i + 8]), unpacked = Be32(&file[i + 12]);
    const uint64_t blocks = (uint64_t(unpacked) + 0x1FFFF) / 0x20000;
    if (!unpacked || i + 16 + 8 * blocks + packed > file.size()) continue;
    const uint8_t* table = &file[i + 16];
    size_t at = i + 16 + 8 * blocks;
    for (uint64_t b = 0; b < blocks; ++b) {
      const uint32_t block_packed = Be32(table + 8 * b), block_unpacked = Be32(table + 8 * b + 4);
      if (at + block_packed > file.size()) break;
      const size_t base = body.size();
      body.resize(base + block_unpacked);
      const bool parsed = ParseFrames({&file[at], block_packed}, true, frames);
      const bool decoded = parsed && Lzx(frames.joined, {body.data() + base, block_unpacked});
      if (!decoded)
        body.resize(base);  // A damaged block is skipped; shaders elsewhere still count.
      at += block_packed;
    }
    i = at - 1;
  }
  return body;
}

// Decoded shader units found in data, each preceded by its sizes.
template <class Emit>
void ScanUnits(std::span<const uint8_t> data, Emit&& emit) {
  Frames frames;
  Bytes first(0x8000), unit;
  for (size_t i = 8; i + 8 < data.size(); ++i) {
    const uint32_t size = Be32(&data[i - 8]), packed = Be32(&data[i - 4]);
    if (size < 36 || size > (8u << 20) || packed < 8 || packed > size + size / 8 + 64 || packed > data.size() - i)
      continue;
    // Cheap check of the first frame before decoding the whole unit.
    const bool explicit_frame = data[i] == 0xFF;
    const size_t out = explicit_frame ? (size_t(data[i + 1]) << 8 | data[i + 2]) : 0x8000;
    const size_t first_packed = explicit_frame ? (size_t(data[i + 3]) << 8 | data[i + 4])
                                               : (size_t(data[i]) << 8 | data[i + 1]);
    const size_t header = explicit_frame ? 5 : 2;
    if (out < 36 || out > 0x8000 || first_packed < 8 || header + first_packed > packed) continue;
    if (!Lzx({&data[i + header], first_packed}, {first.data(), out}) || Be32(first.data()) >> 8 != 0x102A11)
      continue;
    if (!ParseFrames({&data[i], packed}, false, frames) || frames.output != size || frames.consumed > packed ||
        packed - frames.consumed > 16)
      continue;
    unit.resize(size);
    if (!Lzx(frames.joined, unit)) continue;
    emit(std::span<const uint8_t>(unit));
    i += frames.consumed - 1;
  }
}

Bytes ReadFile(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  return Bytes(std::istreambuf_iterator<char>(file), {});
}
struct Package {
  std::filesystem::path path;
  size_t input;
  std::string relative;
  uintmax_t size;
};

// Changes when a package is added, removed or replaced, or the compiler changes.
std::string InputsIdentity(std::vector<Package> packages) {
  std::sort(packages.begin(), packages.end(), [](const Package& a, const Package& b) {
    return std::tie(a.input, a.relative) < std::tie(b.input, b.relative);
  });
  std::string text = "gow3-shader-precompile-v1\n" + NativeShaderCompiler::CacheVersion() + "\n";
  for (const auto& package : packages) {
    std::error_code error;
    const auto time = std::filesystem::last_write_time(package.path, error).time_since_epoch().count();
    text += std::to_string(package.input) + "/" + package.relative + " " + std::to_string(package.size) + " " +
            std::to_string(int64_t(time)) + "\n";
  }
  return ContentHash(std::span(reinterpret_cast<const uint8_t*>(text.data()), text.size()));
}
}  // namespace

ShaderPrecompileResult PrecompileShaders(const std::filesystem::path& cache_root,
                                         const std::vector<std::filesystem::path>& inputs, unsigned threads,
                                         bool skip_unchanged, const ShaderPrecompileCallbacks& callbacks) {
  ShaderPrecompileResult result;
  threads = std::max(1u, threads);
  auto stopping = [&] { return callbacks.stopping && callbacks.stopping(); };
  auto begin = [&](ShaderPrecompilePhase phase, size_t total) { if (callbacks.begin) callbacks.begin(phase, total); };
  auto advance = [&](bool succeeded) { if (callbacks.advance) callbacks.advance(succeeded); };

  std::vector<Package> packages;
  for (size_t input = 0; input < inputs.size(); ++input) {
    std::error_code error;
    for (std::filesystem::recursive_directory_iterator entry(inputs[input], error), end; !error && entry != end;
         entry.increment(error)) {
      const auto name = entry->path().filename().string();
      std::error_code type_error;
      if (!entry->is_regular_file(type_error) || !(entry->path().extension() == ".xxx" || name.starts_with("GlobalShaderCache")))
        continue;
      packages.push_back({entry->path(), input, std::filesystem::relative(entry->path(), inputs[input], type_error).generic_string(),
                          entry->file_size(type_error)});
    }
  }
  result.packages = packages.size();
  // Opening the cache moves entries from an older flat layout into their subfolders.
  NativeShaderCompiler(cache_root / "native-shaders");
  const auto compiled_folder = cache_root / "native-shaders" / NativeShaderCompiler::CacheVersion();
  const auto stamp_path = compiled_folder / "precompile.stamp";
  const auto identity = InputsIdentity(packages);
  if (skip_unchanged) {
    std::ifstream stamp(stamp_path);
    std::string previous;
    if (stamp >> previous && previous == identity) {
      result.up_to_date = result.complete = true;
      return result;
    }
  }

  // Largest first, so one big package doesn't finish last on its own.
  std::sort(packages.begin(), packages.end(), [](const Package& a, const Package& b) { return a.size > b.size; });
  begin(ShaderPrecompilePhase::kFinding, packages.size());
  std::mutex mutex;
  std::map<std::string, std::shared_ptr<const Bytes>> shaders;
  std::atomic<size_t> next{0};
  auto scan = [&] {
    for (size_t index; !stopping() && (index = next++) < packages.size();) {
      const auto file = ReadFile(packages[index].path);
      const bool package = packages[index].path.extension() == ".xxx";
      const auto body = package ? DecompressPackage(file) : Bytes{};
      std::vector<std::shared_ptr<const Bytes>> found;
      ScanUnits(package ? std::span<const uint8_t>(body) : std::span<const uint8_t>(file), [&](auto unit) {
        DiscoverDecodedPixelShaders(unit, [&](auto source) { found.push_back(std::move(source)); });
      });
      {
        std::lock_guard lock(mutex);
        for (auto& source : found) shaders.try_emplace(ContentHash(*source), std::move(source));
      }
      advance(true);
    }
  };
  {
    std::vector<std::thread> workers;
    for (unsigned t = 0; t < threads; ++t) workers.emplace_back(scan);
    for (auto& worker : workers) worker.join();
  }
  if (stopping()) return result;

  // One listing, not a lookup per shader: under Wine each lookup of a missing
  // file scans its folder.
  std::unordered_set<std::string> on_disk;
  {
    std::error_code error;
    for (std::filesystem::recursive_directory_iterator entry(compiled_folder, error), end; !error && entry != end;
         entry.increment(error)) {
      auto name = entry->path().filename().string();
      if (name.ends_with(".g3spv")) on_disk.insert(name.substr(0, name.size() - 6));
    }
  }
  std::vector<std::pair<std::string, std::shared_ptr<const Bytes>>> queue;
  queue.reserve(shaders.size());
  for (auto& [hash, source] : shaders) {
    if (on_disk.contains(hash)) ++result.already;
    else queue.emplace_back(hash, std::move(source));
  }
  on_disk = {};
  result.shaders = shaders.size();
  shaders.clear();

  // The compiler keeps every result in memory; a fresh one per batch bounds
  // the footprint (one compiler for everything peaked near 6 GB).
  begin(ShaderPrecompilePhase::kCompiling, queue.size());
  constexpr size_t kBatch = 2048;
  std::atomic<size_t> failed{0};
  std::vector<char> succeeded(queue.size());
  for (size_t first = 0; first < queue.size() && !stopping(); first += kBatch) {
    const size_t last = std::min(queue.size(), first + kBatch);
    NativeShaderCompiler compiler(cache_root / "native-shaders");
    std::atomic<size_t> claimed{first};
    auto compile = [&] {
      for (size_t index; !stopping() && (index = claimed++) < last;) {
        bool ok = true;
        try {
          compiler.Get(*queue[index].second, NativeShaderMode::Raster);
        } catch (const std::exception&) {
          ok = false;  // The game compiles it again when needed and reports errors there.
          ++failed;
        }
        succeeded[index] = ok;
        queue[index].second.reset();
        advance(ok);
      }
    };
    std::vector<std::thread> workers;
    for (unsigned t = 0; t < threads; ++t) workers.emplace_back(compile);
    for (auto& worker : workers) worker.join();
  }
  for (size_t index = 0; index < queue.size(); ++index)
    if (succeeded[index]) result.compiled.push_back(std::move(queue[index].first));
  result.failed = failed;
  if (stopping()) return result;
  result.complete = true;
  std::error_code error;
  std::filesystem::create_directories(compiled_folder, error);
  std::ofstream(stamp_path, std::ios::trunc) << identity << '\n';
  return result;
}

}  // namespace gow3::renderer
