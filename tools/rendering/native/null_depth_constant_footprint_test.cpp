#include "renderer/native/null_depth_constant_footprint.h"
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace gow3::renderer;
namespace {
uint32_t checks = 0;
void Check(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
std::vector<uint32_t> Read(const char* path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("Missing canonical module input");
    const auto length = file.tellg();
    if (length <= 0 || length > 128 * 1024 || length % 4) throw std::runtime_error("Bad module extent");
    std::vector<uint8_t> bytes(size_t(length), 0);
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), length)) throw std::runtime_error("Module read failed");
    std::vector<uint32_t> words(bytes.size() / 4);
    for (size_t i = 0; i < words.size(); ++i)
        for (unsigned shift = 0; shift < 32; shift += 8) words[i] |= uint32_t(bytes[i * 4 + shift / 8]) << shift;
    return words;
}
template<class Match> size_t Find(const std::vector<uint32_t>& words, uint32_t opcode, Match match) {
    for (size_t at = 5; at < words.size();) {
        const uint32_t count = words[at] >> 16;
        if (!count || count > words.size() - at) throw std::runtime_error("Malformed fixture module");
        if ((words[at] & 65535) == opcode && match(std::span(words).subspan(at + 1, count - 1))) return at;
        at += count;
    }
    throw std::runtime_error("Fixture mutation site absent");
}
void VertexMutations(const std::vector<uint32_t>& original, size_t index) {
    const auto version = kNativeNullDepthConstantCanonicalVersion;
    Check(NativeNullDepthConstantVertexCertified(original, kNativeNullDepthConstantVertexSources[index], version), "Actual pinnedVS admission failed");
    Check(detail::NativeNullDepthConstantStructureForFixture(original, false), "ActualVS structural footprint failed");
    Check(!NativeNullDepthConstantVertexCertified(original, kNativeNullDepthConstantVertexSources[1 - index], version), "CrossVS pin accepted");
    Check(!NativeNullDepthConstantVertexCertified(original, "unknown", version), "UnknownVS accepted");
    Check(!NativeNullDepthConstantVertexCertified(original, kNativeNullDepthConstantVertexSources[index], "other-version"), "Wrong canonical namespace accepted");
    Check(!NativeNullDepthConstantPixelCertified(original, version), "VS admitted as pixel");
    auto changed = original; changed[2] ^= 1;
    Check(!NativeNullDepthConstantVertexCertified(changed, kNativeNullDepthConstantVertexSources[index], version), "Byte-mutated module passed public pin");
    // Direct structural negatives must reach pointer/footprint gates rather
    // than simply fail the public digest. Shader arithmetic is never edited in
    // production; these deliberately invalid candidates are fixture-only.
    const auto c160 = Find(original, 43, [](auto a) { return a.size() == 4 && a[2] == 160 && a[3] == 0; });
    changed = original; changed[c160 + 3] = 176;
    Check(!detail::NativeNullDepthConstantStructureForFixture(changed, false), "VC vector beyond176-byte prefix accepted");
    changed = original; changed[c160 + 3] = 156;
    Check(!detail::NativeNullDepthConstantStructureForFixture(changed, false), "Unaudited literalVC offset accepted");
    const auto vc_add = Find(original, 128, [&](auto a) { return a.size() == 4 && a[3] == original[c160 + 2]; });
    const auto dynamic = Find(original, 113, [](auto a) { return a.size() == 3; });
    changed = original; changed[vc_add + 4] = original[dynamic + 2];
    Check(!detail::NativeNullDepthConstantStructureForFixture(changed, false), "Dynamic VC c[index] ancestry accepted");
    const auto push = Find(original, 59, [](auto a) { return a.size() >= 3 && a[2] == 9; });
    const auto push_chain = Find(original, 65, [&](auto a) { return a.size() == 4 && a[2] == original[push + 2]; });
    changed = original; changed[push_chain + 4] = original[dynamic + 2];
    Check(!detail::NativeNullDepthConstantStructureForFixture(changed, false), "Dynamic push-member address accepted");
    const auto physical_cast = Find(original, 124, [](auto a) { return a.size() == 3 && a[0] == 59; });
    changed = original; changed[physical_cast + 3] = original[dynamic + 2];
    Check(!detail::NativeNullDepthConstantStructureForFixture(changed, false), "Unknown physical load ancestry accepted");
    const auto geometry_add = Find(original, 128, [&](auto a) { return a.size() == 4 && a[3] == original[dynamic + 2]; });
    const auto shared_address = Find(original, 61, [&](auto a) { return a.size() >= 3 && a[2] == original[push_chain + 2]; });
    changed = original; changed[geometry_add + 4] = original[shared_address + 2];
    Check(!detail::NativeNullDepthConstantStructureForFixture(changed, false), "Geometry plusShared address laundering accepted");
    const auto store = Find(original, 62, [](auto a) { return a.size() >= 2; });
    changed = original; changed[store + 1] = original[physical_cast + 2];
    Check(!detail::NativeNullDepthConstantStructureForFixture(changed, false), "Physical write accepted");
    const auto entry = Find(original, 15, [](auto a) { return a.size() >= 3; });
    changed = original; changed[entry + 1] = 4;
    Check(!detail::NativeNullDepthConstantStructureForFixture(changed, false), "WrongVertex stage accepted");
    changed = original; changed[vc_add] = (changed[vc_add] & 0xffff0000) | 57;
    Check(!detail::NativeNullDepthConstantStructureForFixture(changed, false), "Unsupported function-call opcode accepted");
    changed = original; changed[vc_add] &= 65535;
    Check(!detail::NativeNullDepthConstantStructureForFixture(changed, false), "Zero-length instruction accepted");
    changed = original; changed[3] = 4097;
    Check(!detail::NativeNullDepthConstantStructureForFixture(changed, false), "Unbounded SPIR-V id allocation accepted");
    changed = original; changed.pop_back();
    Check(!detail::NativeNullDepthConstantStructureForFixture(changed, false), "Truncated module accepted");
    Check(NativeNullDepthConstantVertexCertified(original, kNativeNullDepthConstantVertexSources[index], version), "OriginalVS mutated by validation");
}
} // namespace
int main(int argc, char** argv) {
    try {
        if (argc != 4) throw std::runtime_error("Usage: null-depth-constant-footprint-test a14VS.spv 3fVS.spv canonical-depth-only.fsi.spv");
        const auto first = Read(argv[1]), second = Read(argv[2]), pixel = Read(argv[3]);
        VertexMutations(first, 0); VertexMutations(second, 1);
        Check(NativeNullDepthConstantPixelCertified(pixel, kNativeNullDepthConstantCanonicalVersion), "Canonical nullPS admission failed");
        Check(detail::NativeNullDepthConstantStructureForFixture(pixel, true), "Canonical nullPS structural proof failed");
        Check(!NativeNullDepthConstantPixelCertified(pixel, "other-version"), "Wrong nullPS namespace accepted");
        Check(!NativeNullDepthConstantVertexCertified(pixel, kNativeNullDepthConstantVertexSources[0], kNativeNullDepthConstantCanonicalVersion), "Pixel admitted asVertex");
        auto changed = pixel; changed[2] ^= 1;
        Check(!NativeNullDepthConstantPixelCertified(changed, kNativeNullDepthConstantCanonicalVersion), "Mutated nullPS public pin accepted");
        const auto push = Find(pixel, 59, [](auto a) { return a.size() >= 3 && a[2] == 9; });
        const auto chain = Find(pixel, 65, [&](auto a) { return a.size() == 4 && a[2] == pixel[push + 2]; });
        const auto uint_type = Find(pixel, 21, [](auto a) { return a.size() == 3 && a[1] == 32 && a[2] == 0; });
        const auto zero = Find(pixel, 43, [&](auto a) { return a.size() == 3 && a[0] == pixel[uint_type + 1] && a[2] == 0; });
        changed = pixel; changed[chain + 4] = pixel[zero + 2];
        Check(!detail::NativeNullDepthConstantStructureForFixture(changed, true), "Pixel float-bank root accepted");
        const auto shared_offset = Find(pixel, 43, [](auto a) { return a.size() == 4 && a[2] >= 2496 && a[3] == 0; });
        changed = pixel; changed[shared_offset + 3] = 2624;
        Check(!detail::NativeNullDepthConstantStructureForFixture(changed, true), "Shared read outside full2624 bytes accepted");
        const auto entry = Find(pixel, 15, [](auto a) { return a.size() >= 3; });
        changed = pixel; changed[entry + 1] = 0;
        Check(!detail::NativeNullDepthConstantStructureForFixture(changed, true), "Wrong pixel stage accepted");
        Check(NativeNullDepthConstantPixelCertified(pixel, kNativeNullDepthConstantCanonicalVersion), "Original nullPS mutated by validation");
        std::cout << "{\"passed\":true,\"checks\":" << checks << ",\"canonical_modules\":3,\"float_prefix_bytes\":176,\"shared_bytes_preserved\":2624,\"shader_words_edited\":0,\"scope\":\"Actual canonical input modules and direct structural mutation gates; CPU only, noGPU/lifetime/image proof\"}\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
