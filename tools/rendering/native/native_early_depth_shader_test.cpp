#include "renderer/native/native_early_depth_shader.h"
#include "renderer/native/content_hash.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace gow3::renderer;
namespace {
std::vector<uint32_t> Read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file || file.tellg() < 20 || file.tellg() > 16 * 1024 * 1024 ||
        size_t(file.tellg()) % 4) throw std::runtime_error("Invalid fixture SPIR-V input");
    std::vector<uint8_t> bytes(size_t(file.tellg()));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), bytes.size()))
        throw std::runtime_error("Truncated fixture SPIR-V input");
    std::vector<uint32_t> result(bytes.size() / 4);
    for (size_t n = 0; n < result.size(); ++n)
        for (unsigned b = 0; b != 4; ++b)
            result[n] |= uint32_t(bytes[n * 4 + b]) << (b * 8);
    return result;
}
std::string Digest(std::span<const uint32_t> words) {
    std::vector<uint8_t> bytes;
    for (const auto word : words)
        for (unsigned shift = 0; shift != 32; shift += 8)
            bytes.push_back(uint8_t(word >> shift));
    return ContentHash(bytes);
}
void Write(const std::filesystem::path& path, std::span<const uint32_t> words) {
    if (std::filesystem::exists(path)) throw std::runtime_error("Fixture output already exists");
    std::ofstream file(path, std::ios::binary);
    for (const auto word : words)
        for (unsigned shift = 0; shift != 32; shift += 8) file.put(char(word >> shift));
    if (!file) throw std::runtime_error("Cannot write fixture candidate");
}
}
int main(int argc, char** argv) try {
    if (argc != 3 && argc != 4)
        throw std::runtime_error("Usage: early-depth-shader-test canonical-pixel.spv canonical-vertex.spv [new-candidate.spv]");
    const auto pixel = Read(argv[1]), vertex = Read(argv[2]);
    const auto pixel_before = pixel, vertex_before = vertex;
    size_t checks = 0;
    const auto check = [&](bool ok) {
        ++checks;
        if (!ok) throw std::runtime_error("Native early-depth byte-certificate fixture failed");
    };
    check(Digest(pixel) == kNativeEarlyDepthPixelSpirvHash);
    check(Digest(vertex) == kNativeEarlyDepthVertexSpirvHash);
    const auto candidate = TryPrepareNativeEarlyDepthShader(pixel,
        kNativeEarlyDepthPixelSource, kNativeEarlyDepthCanonicalVersion);
    check(bool(candidate));
    check(NativeEarlyDepthVertexCertified(vertex, kNativeEarlyDepthVertexSource,
        kNativeEarlyDepthCanonicalVersion));
    check(candidate->source_sha256 == kNativeEarlyDepthPixelSource &&
        candidate->canonical_spirv_sha256 == kNativeEarlyDepthPixelSpirvHash);
    check(candidate->spirv.size() == pixel.size() + 3 && candidate->entry_point == 2);
    const auto insertion = candidate->added_execution_mode_word;
    check(insertion >= 5 && insertion + 3 < candidate->spirv.size());
    check(candidate->spirv[insertion] == ((3u << 16) | 16u) &&
        candidate->spirv[insertion + 1] == 2 && candidate->spirv[insertion + 2] == 9);
    auto restored = candidate->spirv;
    restored.erase(restored.begin() + insertion, restored.begin() + insertion + 3);
    check(restored == pixel && Digest(restored) == kNativeEarlyDepthPixelSpirvHash);
    check(candidate->canonical_edram_stores == 3 &&
        candidate->canonical_implicit_lod_samples == 8 &&
        candidate->canonical_derivative_instructions == 2);
    check(candidate->program_sha256 == Digest(candidate->spirv) &&
        candidate->program_sha256 != candidate->canonical_spirv_sha256);
    check(NativeEarlyDepthShaderVersion().size() == 64 &&
        NativeEarlyDepthShaderVersion() != kNativeEarlyDepthCanonicalVersion);
    check(!TryPrepareNativeEarlyDepthShader(pixel, kNativeEarlyDepthVertexSource,
        kNativeEarlyDepthCanonicalVersion));
    check(!TryPrepareNativeEarlyDepthShader(pixel, kNativeEarlyDepthPixelSource, "unknown-version"));
    check(!TryPrepareNativeEarlyDepthShader(vertex, kNativeEarlyDepthPixelSource,
        kNativeEarlyDepthCanonicalVersion));
    check(!TryPrepareNativeEarlyDepthShader(candidate->spirv, kNativeEarlyDepthPixelSource,
        kNativeEarlyDepthCanonicalVersion));
    check(!NativeEarlyDepthVertexCertified(vertex, kNativeEarlyDepthPixelSource,
        kNativeEarlyDepthCanonicalVersion));
    check(!NativeEarlyDepthVertexCertified(vertex, kNativeEarlyDepthVertexSource, "unknown-version"));
    check(!NativeEarlyDepthVertexCertified(pixel, kNativeEarlyDepthVertexSource,
        kNativeEarlyDepthCanonicalVersion));
    check(!TryPrepareNativeEarlyDepthShader({}, kNativeEarlyDepthPixelSource,
        kNativeEarlyDepthCanonicalVersion));
    auto truncated = pixel; truncated.pop_back();
    check(!TryPrepareNativeEarlyDepthShader(truncated, kNativeEarlyDepthPixelSource,
        kNativeEarlyDepthCanonicalVersion));
    // The admission certificate binds every original byte, including unknown
    // pointer ancestry or new exports/effects introduced by future compilers.
    // Alter one representative instruction of every sensitive category.
    const std::vector<uint32_t> sensitive{15,16,17,32,59,61,62,65,71,87,124,213,214,225,252,5364,5365};
    for (const auto wanted : sensitive) {
        auto changed = pixel; bool found = false;
        for (size_t at = 5; at < changed.size();) {
            const auto count = changed[at] >> 16;
            if ((changed[at] & 65535) == wanted) {
                changed[at + (count > 1 ? count - 1 : 0)] ^= 1; found = true; break;
            }
            at += count;
        }
        check(found);
        check(!TryPrepareNativeEarlyDepthShader(changed, kNativeEarlyDepthPixelSource,
            kNativeEarlyDepthCanonicalVersion));
    }
    auto changed_vertex = vertex; changed_vertex.back() ^= 1;
    check(!NativeEarlyDepthVertexCertified(changed_vertex, kNativeEarlyDepthVertexSource,
        kNativeEarlyDepthCanonicalVersion));
    const auto again = TryPrepareNativeEarlyDepthShader(pixel,
        kNativeEarlyDepthPixelSource, kNativeEarlyDepthCanonicalVersion);
    check(again && again->spirv == candidate->spirv &&
        again->program_sha256 == candidate->program_sha256);
    check(pixel == pixel_before && vertex == vertex_before);
    if (argc == 4) Write(argv[3], candidate->spirv);
    std::cout << "PASS: " << checks << " exact canonical VS/PS certificate and restoration checks; "
        << "original_pixel=" << kNativeEarlyDepthPixelSpirvHash << " candidate="
        << candidate->program_sha256 << " producer=" << NativeEarlyDepthShaderVersion()
        << "; no GPU/numerical/host-state proof\n";
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
