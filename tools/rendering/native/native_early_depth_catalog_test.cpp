#include "renderer/native/native_early_depth_catalog.h"
#include "renderer/native/native_early_depth_catalog_certificate.h"
#include "renderer/native/content_hash.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <vector>

using namespace gow3::renderer;
namespace {
struct Instruction { size_t at; uint32_t opcode, count; };
std::vector<uint32_t> Read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file || file.tellg() < 20 || file.tellg() > 128 * 1024 || size_t(file.tellg()) % 4)
        throw std::runtime_error("Invalid catalogue fixture SPIR-V input");
    std::vector<uint8_t> bytes(size_t(file.tellg()));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), bytes.size()))
        throw std::runtime_error("Truncated catalogue fixture input");
    std::vector<uint32_t> result(bytes.size() / 4);
    for (size_t n = 0; n != result.size(); ++n)
        for (unsigned b = 0; b != 4; ++b) result[n] |= uint32_t(bytes[4 * n + b]) << (8 * b);
    return result;
}
std::string Digest(std::span<const uint32_t> words) {
    std::vector<uint8_t> bytes;
    for (uint32_t word : words)
        for (unsigned shift = 0; shift != 32; shift += 8) bytes.push_back(uint8_t(word >> shift));
    return ContentHash(bytes);
}
void Write(const std::filesystem::path& path, std::span<const uint32_t> words) {
    if (std::filesystem::exists(path)) throw std::runtime_error("Candidate output already exists");
    std::ofstream file(path, std::ios::binary);
    for (uint32_t word : words)
        for (unsigned shift = 0; shift != 32; shift += 8) file.put(char(word >> shift));
    if (!file) throw std::runtime_error("Cannot write catalogue fixture output");
}
std::vector<Instruction> Instructions(std::span<const uint32_t> words) {
    std::vector<Instruction> result;
    for (size_t at = 5; at < words.size();) {
        uint32_t count = words[at] >> 16;
        if (!count || count > words.size() - at) throw std::runtime_error("Bad fixture instruction");
        result.push_back({at, words[at] & 65535u, count}); at += count;
    }
    return result;
}
Instruction Find(std::span<const uint32_t> words, uint32_t opcode) {
    for (const auto i : Instructions(words)) if (i.opcode == opcode) return i;
    throw std::runtime_error("Required fixture opcode missing");
}
// Independent store-class oracle. It does not call the production structural
// predicate; each external pointer must resolve to the sole set4/binding0 root.
size_t ExternalStore(std::span<const uint32_t> words, size_t& count) {
    std::map<uint32_t, uint32_t> storage, values, roots, set, binding;
    const auto instructions = Instructions(words);
    for (const auto i : instructions) {
        const auto at = i.at;
        if (i.opcode == 32) storage[words[at + 1]] = words[at + 2];
        if (i.opcode == 71 && i.count == 4) {
            if (words[at + 2] == 34) set[words[at + 1]] = words[at + 3];
            if (words[at + 2] == 33) binding[words[at + 1]] = words[at + 3];
        }
    }
    size_t first = 0; count = 0;
    for (const auto i : instructions) {
        const auto at = i.at;
        if ((i.opcode == 59 || i.opcode == 61 || i.opcode == 65 || i.opcode == 124 ||
            i.opcode == 169 || i.opcode == 245) && i.count >= 3 && storage.contains(words[at + 1]))
            values[words[at + 2]] = storage.at(words[at + 1]);
        if (i.opcode == 59) roots[words[at + 2]] = words[at + 2];
        if (i.opcode == 65) roots[words[at + 2]] = roots.contains(words[at + 3]) ? roots.at(words[at + 3]) : 0;
        if (i.opcode != 62) continue;
        const uint32_t pointer = words[at + 1];
        if (!values.contains(pointer)) throw std::runtime_error("Unknown independent store class");
        if (values.at(pointer) == 3 || values.at(pointer) == 7) continue;
        if (values.at(pointer) != 12 || !roots.contains(pointer) ||
            !set.contains(roots.at(pointer)) || set.at(roots.at(pointer)) != 4 ||
            !binding.contains(roots.at(pointer)) || binding.at(roots.at(pointer)) != 0)
            throw std::runtime_error("Independent external-write oracle failed");
        if (!first) first = at;
        ++count;
    }
    return first;
}
}
int main(int argc, char** argv) try {
    if (argc != 4 && argc != 5)
        throw std::runtime_error("Usage: catalogue-shader-test canonical-a3.spv canonical-55.spv canonical-75-vs.spv [new-output-directory]");
    const std::array<std::vector<uint32_t>, 2> pixels{Read(argv[1]), Read(argv[2])};
    const auto vertex = Read(argv[3]);
    const auto pixels_before = pixels;
    const auto vertex_before = vertex;
    const auto pins = NativeEarlyDepthCatalogPins();
    size_t checks = 0, structural_checks = 0;
    const auto check = [&](bool ok, const char* message) {
        ++checks; if (!ok) throw std::runtime_error(message);
    };
    check(pins.size() == 2 && !kNativeEarlyDepthCatalogDefaultEnabled, "Closed default-OFF catalogue missing");
    check(NativeEarlyDepthCatalogVersion().size() == 64 && NativeEarlyDepthCatalogVersion() != kNativeEarlyDepthCanonicalVersion &&
        std::ranges::all_of(NativeEarlyDepthCatalogVersion(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }), "Bad distinct catalogue producer");
    check(Digest(vertex) == pins[0].vertex_spirv_sha256 && vertex.size() * 4 == pins[0].vertex_bytes, "Wrong actual canonical VS");
    check(NativeEarlyDepthCatalogVertexCertified(vertex, pins[0].vertex_source, kNativeEarlyDepthCanonicalVersion), "Original VS certificate failed");
    check(detail::NativeEarlyDepthCatalogStructureForFixture(vertex, false, 0), "Original VS structure failed");
    size_t stores = 0;
    check(ExternalStore(vertex, stores) == 0 && stores == 0, "VS external writes exist");
    check(!NativeEarlyDepthCatalogPair(kNativeEarlyDepthVertexSource, kNativeEarlyDepthPixelSource), "V2 unexpectedly admits V1 pair");
    check(!NativeEarlyDepthCatalogPair(pins[0].pixel_source, pins[0].vertex_source), "Reversed pair admitted");
    check(!NativeEarlyDepthCatalogVertexCertified(vertex, pins[0].vertex_source, "unknown"), "Wrong canonical VS namespace admitted");
    check(!NativeEarlyDepthCatalogVertexCertified(vertex, pins[0].pixel_source, kNativeEarlyDepthCanonicalVersion), "Wrong VS source admitted");
    check(!NativeEarlyDepthCatalogVertexCertified(vertex, kNativeEarlyDepthVertexSource, kNativeEarlyDepthCanonicalVersion), "V1 VS source admitted by V2");
    std::array<std::shared_ptr<const NativeEarlyDepthProgram>, 2> programs;
    for (size_t n = 0; n != pixels.size(); ++n) {
        const auto& pixel = pixels[n]; const auto& pin = pins[n];
        check(NativeEarlyDepthCatalogPair(pin.vertex_source, pin.pixel_source), "Pinned pair missing");
        check(Digest(pixel) == pin.pixel_spirv_sha256 && pixel.size() * 4 == pin.pixel_bytes, "Wrong actual canonical PS");
        programs[n] = TryPrepareNativeEarlyDepthCatalogShader(pixel, pin.pixel_source, kNativeEarlyDepthCanonicalVersion);
        const auto& candidate = programs[n]; check(bool(candidate), "Real catalogue module refused");
        check(detail::NativeEarlyDepthCatalogStructureForFixture(pixel, true, n), "Real catalogue structure refused");
        check(candidate->source_sha256 == pin.pixel_source && candidate->canonical_spirv_sha256 == pin.pixel_spirv_sha256, "Program identity mismatch");
        const auto insertion = candidate->added_execution_mode_word;
        check(candidate->spirv.size() == pixel.size() + 3 && insertion >= 5 && insertion + 3 < candidate->spirv.size(), "Candidate size mismatch");
        check(candidate->entry_point == 2 && candidate->spirv[insertion] == ((3u << 16) | 16u) &&
            candidate->spirv[insertion + 1] == 2 && candidate->spirv[insertion + 2] == 9, "Wrong added mode");
        auto restored = candidate->spirv;
        restored.erase(restored.begin() + insertion, restored.begin() + insertion + 3);
        check(restored == pixel && Digest(restored) == pin.pixel_spirv_sha256, "Exact original-word restoration failed");
        check(candidate->program_sha256 == Digest(candidate->spirv) && candidate->program_sha256 != pin.pixel_spirv_sha256, "Candidate digest mismatch");
        check(candidate->canonical_edram_stores == 3 && candidate->canonical_implicit_lod_samples == pin.pixel_implicit_lod_samples &&
            candidate->canonical_derivative_instructions == 2, "Candidate effect inventory mismatch");
        const auto external = ExternalStore(pixel, stores); check(stores == 3 && external, "Independent external-store oracle failed");
        const auto instructions = Instructions(pixel);
        check(std::ranges::count(instructions, 87u, &Instruction::opcode) == pin.pixel_implicit_lod_samples &&
            std::ranges::count(instructions, 213u, &Instruction::opcode) == 1 &&
            std::ranges::count(instructions, 214u, &Instruction::opcode) == 1, "Independent texture/derivative inventory failed");
        check(std::ranges::count(instructions, 154u, &Instruction::opcode) == (n == 1 ? 1 : 0), "Closed OpAny inventory mismatch");
        check(!TryPrepareNativeEarlyDepthCatalogShader(pixel, pins[1 - n].pixel_source, kNativeEarlyDepthCanonicalVersion), "Cross-pin pixels admitted");
        check(!TryPrepareNativeEarlyDepthCatalogShader(pixel, pin.pixel_source, "unknown"), "Wrong pixel namespace admitted");
        check(!TryPrepareNativeEarlyDepthCatalogShader(candidate->spirv, pin.pixel_source, kNativeEarlyDepthCanonicalVersion), "Repeated early edit admitted");
        check(!TryPrepareNativeEarlyDepthCatalogShader(vertex, pin.pixel_source, kNativeEarlyDepthCanonicalVersion), "Wrong stage admitted");
        check(!TryPrepareNativeEarlyDepthCatalogShader({}, pin.pixel_source, kNativeEarlyDepthCanonicalVersion), "Empty pixel admitted");
        auto truncated = pixel; truncated.pop_back();
        check(!TryPrepareNativeEarlyDepthCatalogShader(truncated, pin.pixel_source, kNativeEarlyDepthCanonicalVersion), "Truncated pixel admitted");
        const auto refuse_shape = [&](const std::vector<uint32_t>& changed) {
            ++structural_checks;
            check(!detail::NativeEarlyDepthCatalogStructureForFixture(changed, true, n), "Mutated external-effect structure admitted");
            check(!TryPrepareNativeEarlyDepthCatalogShader(changed, pin.pixel_source, kNativeEarlyDepthCanonicalVersion), "Mutated byte certificate admitted");
        };
        auto changed = pixel; changed[0] ^= 1; refuse_shape(changed);
        changed = pixel; changed[4] = 1; refuse_shape(changed);
        changed = pixel; changed[Find(pixel, 16).at + 2] = 9; refuse_shape(changed); // Existing early mode.
        changed = pixel; changed[Find(pixel, 16).at + 1] ^= 128; refuse_shape(changed); // Foreign entry.
        changed = pixel; changed[external + 1] = pixel[3] + 1; refuse_shape(changed); // Unknown pointer.
        changed = pixel; changed[external] = (changed[external] & 0xffff0000u) | 99; refuse_shape(changed); // ImageWrite.
        changed = pixel; changed[external] = (changed[external] & 0xffff0000u) | 63; refuse_shape(changed); // CopyMemory.
        changed = pixel; changed[external] = (changed[external] & 0xffff0000u) | 234; refuse_shape(changed); // AtomicIAdd.
        changed = pixel; changed[external] = (changed[external] & 0xffff0000u) | 5056; refuse_shape(changed); // ReadClockKHR.
        changed = pixel; changed[Find(pixel, 17).at + 1] = 61; refuse_shape(changed); // GroupNonUniform.
        bool mutated_builtin = false, mutated_descriptor = false, mutated_coherent = false;
        for (const auto i : instructions) if (i.opcode == 71 && i.count == 4 && pixel[i.at + 2] == 11 && pixel[i.at + 3] == 15) {
            changed = pixel; changed[i.at + 3] = 22; refuse_shape(changed); // FragDepth.
            changed = pixel; changed[i.at + 3] = 23; refuse_shape(changed); // HelperInvocation.
            mutated_builtin = true; break;
        }
        check(mutated_builtin, "FragDepth mutation not reached");
        for (const auto i : instructions) if (i.opcode == 71 && i.count == 4 && pixel[i.at + 2] == 34 && pixel[i.at + 3] == 4) {
            changed = pixel; changed[i.at + 3] = 5; refuse_shape(changed); mutated_descriptor = true; break; // Uncertified storage descriptor.
        }
        check(mutated_descriptor, "Storage descriptor mutation not reached");
        for (const auto i : instructions) if (i.opcode == 71 && i.count == 3 && pixel[i.at + 2] == 23) {
            changed = pixel; changed[i.at + 2] = 0; refuse_shape(changed); mutated_coherent = true; break;
        }
        check(mutated_coherent, "Coherent mutation not reached");
        const auto barrier = Find(pixel, 225); bool mutated_semantics = false;
        for (const auto i : instructions) if (i.opcode == 43 && i.count == 4 && pixel[i.at + 2] == pixel[barrier.at + 2]) {
            changed = pixel; changed[i.at + 3] = 0; refuse_shape(changed); mutated_semantics = true; break;
        }
        check(mutated_semantics, "Barrier semantics mutation not reached");
        changed = pixel; changed.erase(changed.begin() + barrier.at, changed.begin() + barrier.at + barrier.count); refuse_shape(changed);
        changed = pixel;
        const std::array<uint32_t, 3> moved_store{pixel[external], pixel[external + 1], pixel[external + 2]};
        changed.erase(changed.begin() + external, changed.begin() + external + 3);
        const auto begin = Find(changed, 5364);
        changed.insert(changed.begin() + begin.at, moved_store.begin(), moved_store.end());
        refuse_shape(changed); // EDRAM write before the interlock epilogue.
        changed = pixel; const auto end = Find(pixel, 5365); changed.erase(changed.begin() + end.at, changed.begin() + end.at + end.count); refuse_shape(changed);
        changed = pixel; changed[5] &= 65535u; refuse_shape(changed); // Zero instruction length.
        check(!detail::NativeEarlyDepthCatalogStructureForFixture(pixel, true, 2), "Out-of-catalogue shape index admitted");
        const auto again = TryPrepareNativeEarlyDepthCatalogShader(pixel, pin.pixel_source, kNativeEarlyDepthCanonicalVersion);
        check(again && again->spirv == candidate->spirv && again->program_sha256 == candidate->program_sha256, "Nondeterministic edit");
    }
    check(programs[0]->program_sha256 != programs[1]->program_sha256, "Distinct material identities collapsed");
    auto changed_vertex = vertex; changed_vertex.back() ^= 1;
    check(!NativeEarlyDepthCatalogVertexCertified(changed_vertex, pins[0].vertex_source, kNativeEarlyDepthCanonicalVersion), "Changed VS admitted");
    changed_vertex = vertex; changed_vertex[Find(vertex, 17).at + 1] = 61;
    check(!detail::NativeEarlyDepthCatalogStructureForFixture(changed_vertex, false, 0), "VS subgroup effect admitted");
    check(pixels == pixels_before && vertex == vertex_before, "Original input ownership changed");
    if (argc == 5) {
        const std::filesystem::path directory(argv[4]);
        if (std::filesystem::exists(directory) || !std::filesystem::create_directory(directory))
            throw std::runtime_error("Output directory must be new");
        for (size_t n = 0; n != programs.size(); ++n)
            Write(directory / (std::string(pins[n].pixel_source) + ".early-catalog-v2.spv"), programs[n]->spirv);
    }
    std::cout << "PASS: " << checks << " catalogue byte/pair/restoration/owner checks; "
        << structural_checks << " direct parser/effect refusals; producer=" << NativeEarlyDepthCatalogVersion()
        << " a3=" << programs[0]->program_sha256 << " 55=" << programs[1]->program_sha256
        << "; no GPU, host-state or numerical proof\n";
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
