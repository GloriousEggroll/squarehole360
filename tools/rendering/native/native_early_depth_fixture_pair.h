#pragma once

// Test-only editor for a separately compiled, source-authenticated controlled
// material fixture. Never include this header in Scene/runtime or use its
// caller-supplied certificate to extend production shader admission.
#include "renderer/native/content_hash.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace gow3::renderer::fixture {
namespace early_depth_detail {
struct Instruction { size_t at; uint32_t opcode; std::span<const uint32_t> args; };
struct Pointer { uint32_t storage, pointee; };
struct Shape { size_t insertion; uint32_t entry; };
inline void Require(bool condition, const char* error) {
    if (!condition) throw std::invalid_argument(error);
}
inline bool Equal(std::span<const uint32_t> a, std::initializer_list<uint32_t> b) {
    return std::ranges::equal(a, b);
}
inline std::string Digest(std::span<const uint32_t> words) {
    std::vector<uint8_t> bytes;
    bytes.reserve(words.size() * 4);
    for (const auto word : words)
        for (unsigned shift = 0; shift != 32; shift += 8) bytes.push_back(uint8_t(word >> shift));
    return ContentHash(bytes);
}
inline std::string Literal(std::span<const uint32_t> words) {
    std::string result;
    for (size_t n = 0; n < words.size(); ++n)
        for (unsigned b = 0; b != 4; ++b) {
            const auto c = char((words[n] >> (b * 8)) & 255);
            if (!c) {
                Require(n + 1 == words.size() && (words[n] >> (b * 8)) == 0,
                    "Malformed fixture SPIR-V literal");
                return result;
            }
            result.push_back(c);
        }
    throw std::invalid_argument("Unterminated fixture SPIR-V literal");
}
inline bool Opcode(uint32_t op) {
    switch (op) {
    case 3: case 5: case 6: case 10: case 11: case 12: case 14: case 15:
    case 16: case 17: case 19: case 20: case 21: case 22: case 23: case 25:
    case 26: case 27: case 28: case 29: case 30: case 32: case 33: case 41:
    case 42: case 43: case 44: case 46: case 50: case 52: case 54: case 56:
    case 59: case 61: case 62: case 65: case 71: case 72: case 79: case 80:
    case 81: case 82: case 83: case 86: case 87: case 88: case 103:
    case 109: case 110: case 111: case 112: case 113: case 124: case 127:
    case 128: case 129: case 130: case 131: case 132: case 133: case 134:
    case 136: case 137: case 142: case 148: case 154: case 156: case 168:
    case 169: case 170: case 171: case 172: case 174: case 176: case 177:
    case 178: case 180: case 182: case 184: case 186: case 188: case 190:
    case 194: case 195: case 196: case 197: case 198: case 199: case 200:
    case 213: case 214: case 225: case 245: case 246: case 247: case 248:
    case 249: case 250: case 251: case 252: case 253: case 5364: case 5365:
        return true;
    default: return false;
    }
}
inline bool Glsl(uint32_t op) {
    switch (op) {
    case 2: case 3: case 4: case 8: case 10: case 13: case 14: case 29:
    case 30: case 31: case 32: case 38: case 43: case 58: case 62:
    case 79: case 80: return true;
    default: return false;
    }
}
inline Shape Certify(std::span<const uint32_t> words, std::string_view expected_hash) {
    Require(expected_hash.size() == 64 && std::ranges::all_of(expected_hash,
        [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }),
        "Fixture baseline digest must come from its authenticated compiled artifact");
    Require(words.size() >= 5 && words.size() <= 16u * 1024u * 1024u / 4u &&
        words[0] == 0x07230203 && words[1] >= 0x10000 && words[1] <= 0x10500 &&
        words[3] && words[4] == 0 && Digest(words) == expected_hash,
        "Fixture baseline digest/header mismatch");
    std::vector<Instruction> instructions;
    for (size_t at = 5; at < words.size();) {
        const auto count = words[at] >> 16, op = words[at] & 65535;
        Require(count && count <= words.size() - at && Opcode(op),
            "Unaudited controlled fixture instruction");
        instructions.push_back({at, op, words.subspan(at + 1, count - 1)});
        at += count;
    }
    std::map<uint32_t, std::vector<std::vector<uint32_t>>> decorations;
    std::map<uint32_t, Pointer> types;
    std::map<uint32_t, size_t> structures;
    std::set<std::array<uint32_t, 2>> readonly_members;
    std::map<uint32_t, std::array<uint32_t, 2>> constants;
    std::set<uint32_t> uint_types, glsl;
    uint32_t entry = 0; size_t insertion = 0, entries = 0, models = 0, functions = 0;
    size_t origin_modes = 0, interlock_modes = 0;
    for (const auto& i : instructions) {
        const auto a = i.args;
        switch (i.opcode) {
        case 15:
            Require(a.size() >= 3 && a[0] == 4 && a[1], "Fixture requires one fragment entry");
            entry = a[1]; ++entries; break;
        case 14:
            Require(Equal(a, {5348, 1}), "Fixture requires native GLSL450 physical-read ABI");
            ++models; break;
        case 16:
            Require(a.size() == 2 && (a[1] == 7 || a[1] == 5366),
                "Fixture has early/export/sample/stencil execution mode");
            if (!insertion) insertion = i.at;
            if (a[1] == 7) ++origin_modes; else ++interlock_modes;
            break;
        case 17:
            Require(a.size() == 1 && (a[0] == 1 || a[0] == 11 || a[0] == 50 ||
                a[0] == 51 || a[0] == 5302 || a[0] == 5347 || a[0] == 5378),
                "Fixture has unaudited shader capability"); break;
        case 10: {
            const auto extension = Literal(a);
            Require(extension == "SPV_EXT_descriptor_indexing" ||
                extension == "SPV_EXT_fragment_shader_interlock" ||
                extension == "SPV_KHR_physical_storage_buffer", "Fixture has unaudited extension");
            break;
        }
        case 11:
            Require(a.size() >= 2 && Literal(a.subspan(1)) == "GLSL.std.450",
                "Fixture has unaudited extended instruction import");
            glsl.insert(a[0]); break;
        case 71:
            Require(a.size() >= 2, "Truncated fixture decoration");
            decorations[a[0]].emplace_back(a.begin() + 1, a.end());
            if (a[1] == 11) Require(a.size() == 3 && (a[2] == 15 || a[2] == 17),
                "Fixture has helper/subgroup/sample/depth/stencil builtin");
            break;
        case 72:
            Require(a.size() >= 3, "Truncated fixture member decoration");
            if (a[2] == 11) Require(a.size() == 4 && (a[3] == 15 || a[3] == 17),
                "Fixture has observable/exported builtin member");
            if (a[2] == 24) readonly_members.insert({a[0], a[1]});
            break;
        case 32:
            Require(a.size() == 3, "Truncated fixture pointer type");
            types.emplace(a[0], Pointer{a[1], a[2]}); break;
        case 30:
            Require(!a.empty(), "Truncated fixture structure");
            structures.emplace(a[0], a.size() - 1); break;
        case 21:
            if (a.size() == 3 && a[1] == 32 && a[2] == 0) uint_types.insert(a[0]);
            break;
        case 43:
            if (a.size() == 3) constants.emplace(a[1], std::array<uint32_t, 2>{a[0], a[2]});
            break;
        case 54: ++functions; break;
        default: break;
        }
    }
    Require(entries == 1 && models == 1 && functions == 1 && origin_modes == 1 &&
        interlock_modes == 1 && insertion, "Wrong controlled fixture entry shape");
    auto decorated = [&](uint32_t id, std::initializer_list<uint32_t> value) {
        const auto p = decorations.find(id);
        return p != decorations.end() && std::ranges::any_of(p->second,
            [&](const auto& d) { return Equal(d, value); });
    };
    uint32_t edram = 0;
    for (const auto& [id, values] : decorations)
        if (decorated(id, {34, 4}) && decorated(id, {33, 0})) {
            Require(!edram, "Ambiguous controlled fixture EDRAM descriptor"); edram = id;
        }
    Require(edram && decorated(edram, {23}), "Fixture requires coherent set4binding0 EDRAM");
    std::unordered_map<uint32_t, uint32_t> values, roots, variables;
    size_t begin = 0, end = 0, barriers = 0;
    for (size_t n = 0; n < instructions.size(); ++n) {
        const auto& i = instructions[n]; const auto a = i.args;
        if (i.opcode == 16) Require(a[0] == entry, "Foreign fixture execution mode");
        if (i.opcode == 54) Require(a.size() == 4 && a[1] == entry, "Foreign fixture function");
        if (i.opcode == 12) Require(a.size() >= 4 && glsl.contains(a[2]) && Glsl(a[3]),
            "Fixture has unaudited extended instruction effect");
        if ((i.opcode == 59 || i.opcode == 61 || i.opcode == 65 || i.opcode == 83 ||
             i.opcode == 124 || i.opcode == 169 || i.opcode == 245) &&
             a.size() >= 2 && types.contains(a[0])) values.emplace(a[1], a[0]);
        if (i.opcode == 59) {
            Require(a.size() >= 3 && types.contains(a[0]) && types.at(a[0]).storage == a[2],
                "Unknown fixture variable ancestry");
            variables.emplace(a[1], a[2]); roots.emplace(a[1], a[1]);
            if (a[2] == 12 && a[1] != edram) {
                const auto block = types.at(a[0]).pointee;
                bool readonly = decorated(a[1], {24});
                if (!readonly && structures.contains(block) && structures.at(block)) {
                    readonly = true;
                    for (size_t member = 0; member < structures.at(block); ++member)
                        readonly &= readonly_members.contains({block, uint32_t(member)});
                }
                Require(readonly, "Fixture has additional writable storage buffer");
            }
        } else if (i.opcode == 65 || i.opcode == 83) {
            Require(a.size() >= 3, "Truncated fixture pointer ancestry");
            roots.emplace(a[1], roots.contains(a[2]) ? roots.at(a[2]) : 0);
        }
        if (i.opcode != 225) continue;
        Require(a.size() == 2 && constants.contains(a[0]) && constants.contains(a[1]),
            "Wrong fixture FSI barrier operands");
        const auto scope = constants.at(a[0]), semantics = constants.at(a[1]);
        Require(uint_types.contains(scope[0]) && scope[1] == 1 &&
            uint_types.contains(semantics[0]) && semantics[1] == 0x848,
            "Wrong fixture FSI barrier semantics");
        if (!barriers) {
            Require(n && instructions[n - 1].opcode == 5364 && instructions[n - 1].args.empty(),
                "Missing original fixture interlock begin");
            begin = instructions[n - 1].at;
        } else {
            Require(n + 1 < instructions.size() && instructions[n + 1].opcode == 5365 &&
                instructions[n + 1].args.empty(), "Missing original fixture interlock end");
            end = instructions[n + 1].at;
        }
        ++barriers;
    }
    Require(variables.contains(edram) && variables.at(edram) == 12 &&
        barriers == 2 && begin && begin < end, "Wrong original fixture EDRAM epilogue");
    size_t stores = 0;
    for (const auto& i : instructions) {
        const auto a = i.args;
        if (begin < i.at && i.at < end)
            Require(i.opcode != 5364 && i.opcode != 5365, "Nested fixture interlock");
        if (i.opcode == 61 && a.size() >= 3 && roots.contains(a[2]) && roots.at(a[2]) == edram)
            Require(begin < i.at && i.at < end, "Fixture EDRAM read outside epilogue");
        if (i.opcode != 62) continue;
        Require(a.size() >= 2 && values.contains(a[0]), "Unknown fixture store pointer ancestry");
        const auto storage = types.at(values.at(a[0])).storage;
        if (storage == 3 || storage == 6 || storage == 7) continue;
        Require(storage == 12 && roots.contains(a[0]) && roots.at(a[0]) == edram &&
            begin < i.at && i.at < end, "Fixture external write outside canonical EDRAM epilogue");
        ++stores;
    }
    Require(stores, "Fixture requires observable canonical packed EDRAM stores");
    return {insertion, entry};
}
} // namespace early_depth_detail

// This supplied hash must be pinned by the fixture's immutable source/producer
// manifest after root compiles and validates its ORIGINAL module. It is not a
// digest computed from an arbitrary command-line file and then trusted as proof.
inline std::vector<uint32_t> MakeNativeEarlyDepthFixtureCandidate(
    std::span<const uint32_t> baseline, std::string_view expected_baseline_sha256) {
    const auto shape = early_depth_detail::Certify(baseline, expected_baseline_sha256);
    std::vector<uint32_t> candidate;
    candidate.reserve(baseline.size() + 3);
    candidate.insert(candidate.end(), baseline.begin(), baseline.begin() + shape.insertion);
    candidate.insert(candidate.end(), {(3u << 16) | 16u, shape.entry, 9u});
    candidate.insert(candidate.end(), baseline.begin() + shape.insertion, baseline.end());
    return candidate;
}
inline bool AuditNativeEarlyDepthFixturePair(std::span<const uint32_t> baseline,
    std::span<const uint32_t> candidate, std::string_view expected_baseline_sha256) {
    try {
        const auto exact = MakeNativeEarlyDepthFixtureCandidate(baseline, expected_baseline_sha256);
        return std::ranges::equal(candidate, exact);
    } catch (const std::invalid_argument&) { return false; }
}
} // namespace gow3::renderer::fixture
