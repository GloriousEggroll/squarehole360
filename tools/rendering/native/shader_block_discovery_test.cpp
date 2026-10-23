#include "renderer/native/shader_block_discovery.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace gow3::renderer;
namespace {
using Bytes = std::vector<uint8_t>;
using Sources = std::vector<std::shared_ptr<const Bytes>>;
size_t checks = 0;

void Require(bool condition, const char* message) {
  ++checks;
  if (!condition) throw std::runtime_error(message);
}
void Put(Bytes& bytes, size_t offset, uint32_t word) {
  if (offset > bytes.size() || bytes.size() - offset < 4) throw std::runtime_error("Bad test fixture");
  for (size_t i = 0; i < 4; ++i) bytes[offset + i] = uint8_t(word >> (24 - 8 * i));
}
uint32_t Get(const Bytes& bytes, size_t offset) {
  return uint32_t(bytes.at(offset)) << 24 | uint32_t(bytes.at(offset + 1)) << 16 |
         uint32_t(bytes.at(offset + 2)) << 8 | bytes.at(offset + 3);
}
Bytes Pixel(size_t virtual_size = 68, size_t physical_size = 12) {
  Bytes bytes(virtual_size + physical_size);
  Put(bytes, 0, 0x102a1100);
  Put(bytes, 4, uint32_t(virtual_size));
  Put(bytes, 8, uint32_t(physical_size));
  Put(bytes, 24, 36);
  Put(bytes, 40, 12);
  return bytes;
}
ShaderBlockDiscoveryStatistics Scan(const Bytes& bytes, Sources& sources) {
  return DiscoverDecodedPixelShaders(bytes, [&](std::shared_ptr<const Bytes> source) {
    sources.push_back(std::move(source));
  });
}
void Reject(const Bytes& bytes) {
  Sources sources;
  const auto result = Scan(bytes, sources);
  Require(sources.empty() && !result.emitted && !result.emitted_bytes,
          "Malformed or unsupported input escaped discovery");
  Require(result.inspected <= bytes.size() && result.candidates <= 128,
          "Invalid scanner bounds");
}
void PackCf(Bytes& bytes, size_t at, uint64_t first, uint64_t second) {
  Put(bytes, at, uint32_t(first));
  Put(bytes, at + 4, uint32_t(first >> 32) | uint32_t(second & 65535) << 16);
  Put(bytes, at + 8, uint32_t(second >> 16));
}

void Synthetic() {
  const auto pixel = Pixel();
  for (size_t offset = 0; offset < 32; ++offset) {
    Bytes block(offset, 0xda);
    block.insert(block.end(), pixel.begin(), pixel.end());
    block.insert(block.end(), pixel.begin(), pixel.end()); // No alignment padding.
    block.push_back(0xff);
    Sources sources;
    const auto result = Scan(block, sources);
    Require(result.emitted == 2 && result.candidates == 2 && !result.refused &&
            !result.limit_reached && result.inspected == block.size(), "Unaligned/adjacent source lost");
    Require(sources.size() == 2 && *sources[0] == pixel && *sources[1] == pixel &&
            result.emitted_bytes == 2 * pixel.size(), "Source bytes changed");
    std::fill(block.begin(), block.end(), 0);
    block.clear(); block.shrink_to_fit();
    Require(*sources[0] == pixel && *sources[1] == pixel, "Source did not own its lifetime");
  }
  for (size_t length = 0; length < pixel.size(); ++length) Reject(Bytes(pixel.begin(), pixel.begin() + length));
  for (const auto [at, value] : std::vector<std::pair<size_t, uint32_t>>{
       {0, 0x102a1101}, {0, 0x102a1161}, {0, 0x102a1110}, {0, 0x00112a10},
       {4, 0}, {4, 35}, {4, 67}, {4, 0xffffffff}, {4, 128 * 1024 + 4},
       {8, 0}, {8, 0xffffffff}, {8, 128 * 1024 + 1}, {12, 1}, {16, 36},
       {28, 1}, {32, 1}, {24, 0}, {24, 35}, {24, 37}, {24, 40}, {24, 0xfffffffc},
       {36, 1}, {36, 4}, {36, 0xfffffffc}, {40, 0}, {40, 4}, {40, 13},
       {40, 4097 * 12}, {56, 1 << 5}, {20, 24}, {20, 0xffffffff}}) {
    auto bad = pixel; Put(bad, at, value); Reject(bad);
  }
  // A refused candidate must not prevent recovery at an immediately following
  // real header; nor may a plausible oversized outer header hide it.
  auto bad_then_good = pixel; Put(bad_then_good, 8, 0xffffffff);
  bad_then_good.insert(bad_then_good.end(), pixel.begin(), pixel.end());
  Sources recovered;
  const auto recovered_stats = Scan(bad_then_good, recovered);
  Require(recovered.size() == 1 && *recovered[0] == pixel && recovered_stats.refused == 1,
          "Refused outer candidate concealed a valid following source");

  auto declarations = Pixel(72);
  Put(declarations, 56, 1 << 5); Put(declarations, 68, 13 << 4);
  Sources declared;
  Require(Scan(declarations, declared).emitted == 1, "Valid declaration refused");
  Put(declarations, 68, 14 << 4); Reject(declarations);
  Put(declarations, 68, 15 << 4); Reject(declarations);

  auto definitions = Pixel(128, 28);
  Put(definitions, 20, 80);
  Put(definitions, 100, (256 << 16) | 4); Put(definitions, 104, 12);
  Put(definitions, 112, (8992 << 16) | 1); Put(definitions, 120, 0x12345678);
  Sources defined;
  Require(Scan(definitions, defined).emitted == 1, "Valid float/loop definitions refused");
  for (const auto [at, value] : std::vector<std::pair<size_t, uint32_t>>{
       {20, 81}, {20, 108}, {100, (255 << 16) | 4}, {100, (512 << 16) | 4},
       {104, 13}, {104, 16}, {104, 0xfffffffc}, {112, (8991 << 16) | 1},
       {112, (8993 << 16) | 1}, {112, (9120 << 16) | 1}, {112, (8992 << 16) | 33},
       {124, (8992 << 16) | 1}}) {
    auto bad = definitions; Put(bad, at, value); Reject(bad);
  }
  auto missing_float_terminator = Pixel(104, 28);
  Put(missing_float_terminator, 20, 80);
  Put(missing_float_terminator, 100, (256 << 16) | 4);
  Reject(missing_float_terminator);
  auto missing_loop_terminator = Pixel(104);
  Put(missing_loop_terminator, 20, 80); Reject(missing_loop_terminator);

  for (uint32_t opcode : {1u, 2u, 3u, 4u, 5u, 6u, 13u, 14u}) {
    for (bool second : {false, true}) {
      auto cf = Pixel(68, 24); Put(cf, 40, 24);
      const uint64_t valid = uint64_t(opcode) << 44 | 1 << 12 | 1;
      PackCf(cf, 68, second ? 0 : valid, second ? valid : 0);
      Sources clauses;
      Require(Scan(cf, clauses).emitted == 1, "Valid packed execution clause refused");
      const uint64_t invalid = uint64_t(opcode) << 44 | 7 << 12 | 4095;
      PackCf(cf, 68, second ? 0 : invalid, second ? invalid : 0); Reject(cf);
    }
  }
  for (const auto [virtual_size, physical_size] :
       std::vector<std::pair<size_t, size_t>>{{128 * 1024, 12}, {68, 128 * 1024}, {128 * 1024, 128 * 1024}}) {
    auto maximum = Pixel(virtual_size, physical_size); Sources sources;
    Require(Scan(maximum, sources).emitted == 1 && *sources[0] == maximum,
            "Inclusive section limit rejected");
  }

  Bytes oversized(kMaxShaderDiscoveryBlockBytes + 1);
  std::copy(pixel.begin(), pixel.end(), oversized.begin());
  Sources ignored;
  const auto oversized_stats = Scan(oversized, ignored);
  Require(!oversized_stats.inspected && !oversized_stats.candidates && oversized_stats.refused == 1 &&
          oversized_stats.limit_reached && ignored.empty(), "Oversized block was inspected");
  Bytes bounded(kMaxShaderDiscoveryBlockBytes);
  std::copy(pixel.begin(), pixel.end(), bounded.end() - pixel.size());
  Require(Scan(bounded, ignored).emitted == 1 && *ignored[0] == pixel,
          "Inclusive block end was rejected");

  Bytes many;
  for (size_t i = 0; i < 129; ++i) many.insert(many.end(), pixel.begin(), pixel.end());
  Sources capped;
  const auto capped_stats = Scan(many, capped);
  Require(capped.size() == 128 && capped_stats.candidates == 128 && capped_stats.limit_reached,
          "Candidate/emission cap failed");
  many.resize(128 * pixel.size()); Sources exact;
  Require(!Scan(many, exact).limit_reached && exact.size() == 128, "Exactly full scan reported incomplete");
  for (size_t i = 0; i < 128; ++i) Put(many, i * pixel.size() + 16, 36);
  many.insert(many.end(), pixel.begin(), pixel.end()); Sources after_invalid;
  const auto invalid_cap = Scan(many, after_invalid);
  Require(invalid_cap.refused == 128 && invalid_cap.candidates == 128 &&
          invalid_cap.limit_reached && after_invalid.empty(), "Rejected headers bypassed validation-work cap");

  // Overlapping valid containers are retained, but their cumulative owned
  // output is independently bounded even though the input fits its own cap.
  Bytes overlap(kMaxShaderDiscoveryBlockBytes);
  for (size_t i = 0; i < 10; ++i) {
    std::copy(pixel.begin(), pixel.end(), overlap.begin() + i * 4096);
    Put(overlap, i * 4096 + 8, 128 * 1024);
  }
  Sources overlapped;
  const auto output_cap = Scan(overlap, overlapped);
  Require(output_cap.limit_reached && output_cap.emitted == 7 && output_cap.candidates == 8 &&
          output_cap.emitted_bytes == 7 * (68 + 128 * 1024) &&
          output_cap.emitted_bytes <= kMaxShaderDiscoveryBlockBytes, "Owned output cap failed");
  bool threw = false;
  try { DiscoverDecodedPixelShaders(pixel, [](auto) { throw std::runtime_error("Callback guard witness"); }); }
  catch (const std::runtime_error& error) { threw = std::string(error.what()) == "Callback guard witness"; }
  Require(threw, "Callback failure was concealed");

  // Deterministic malformed inputs exercise word/range boundaries under ASan
  // and UBSan. This is a memory-safety corpus, not a render-eligibility oracle.
  uint32_t state = 0x7f4a7c15;
  for (size_t trial = 0; trial < 2048; ++trial) {
    Bytes random(trial % 513);
    for (auto& byte : random) { state ^= state << 13; state ^= state >> 17; state ^= state << 5; byte = uint8_t(state); }
    if (random.size() >= 4) Put(random, trial % (random.size() - 3), 0x102a1100);
    Sources sources; const auto stats = Scan(random, sources);
    Require(stats.inspected <= random.size() && stats.emitted_bytes <= kMaxShaderDiscoveryBlockBytes &&
            stats.candidates <= kMaxShaderDiscoveryBlockCandidates, "Adversarial corpus exceeded bounds");
  }
}

void ActualSources(int argc, char** argv) {
  std::vector<std::filesystem::path> paths;
  for (int i = 1; i < argc; ++i) {
    const std::filesystem::path path(argv[i]);
    if (std::filesystem::is_directory(path)) {
      for (const auto& entry : std::filesystem::directory_iterator(path))
        if (entry.is_regular_file() && entry.path().extension() == ".bin") paths.push_back(entry.path());
    } else paths.push_back(path);
  }
  std::sort(paths.begin(), paths.end());
  Bytes batch{0xe7}; // Deliberately unaligned, with adjacent pairs below.
  std::vector<Bytes> expected;
  size_t actual = 0, skipped = 0, batches = 0;
  const auto flush = [&] {
    if (expected.empty()) return;
    Sources observed; const auto result = Scan(batch, observed);
    Require(!result.limit_reached && result.emitted == expected.size() && observed.size() == expected.size(),
            "Actual concatenated source count differs");
    for (size_t i = 0; i < expected.size(); ++i)
      Require(*observed[i] == expected[i], "Actual concatenated source identity differs");
    batch.assign(1, 0xe7); expected.clear(); ++batches;
  };
  for (const auto& path : paths) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) throw std::runtime_error("Cannot open actual source: " + path.string());
    const auto length = input.tellg();
    if (length < 36 || length > std::streamoff(kMaxShaderDiscoveryBlockBytes))
      throw std::runtime_error("Invalid actual source extent: " + path.string());
    Bytes source(static_cast<size_t>(length)); input.seekg(0);
    if (!input.read(reinterpret_cast<char*>(source.data()), length))
      throw std::runtime_error("Cannot read actual source: " + path.string());
    if (Get(source, 0) != 0x102a1100 || Get(source, 16)) { ++skipped; continue; }
    Sources standalone;
    const auto result = Scan(source, standalone);
    if (result.limit_reached || standalone.size() != 1 || *standalone[0] != source)
      throw std::runtime_error("Actual stripped pixel source not discovered exactly: " + path.string());
    if (expected.size() >= 64 || batch.size() > kMaxShaderDiscoveryBlockBytes - 3 ||
        source.size() > kMaxShaderDiscoveryBlockBytes - batch.size() - 3) flush();
    if (actual % 2) batch.insert(batch.end(), 3, 0xe7);
    batch.insert(batch.end(), source.begin(), source.end()); expected.push_back(std::move(source)); ++actual;
  }
  flush();
  if (argc > 1) Require(actual != 0, "No stripped pixel sources tested");
  std::cout << "Actual source checks: " << actual << " stripped pixels, " << skipped
            << " unsupported/named inputs skipped, " << batches << " unaligned/adjacent batches\n";
}
}  // namespace

int main(int argc, char** argv) try {
  Synthetic();
  ActualSources(argc, argv);
  std::cout << "Decoded shader block discovery PASS: " << checks
            << " checks; exact ownership, unaligned/adjacent discovery, structural ranges, "
               "packed CF, definitions, unsupported inputs and work/output bounds\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n'; return 1;
}
