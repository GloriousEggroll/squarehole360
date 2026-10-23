#pragma once

#include "renderer/native/content_hash.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

namespace gow3::renderer {
inline constexpr size_t kNativeNullDepthFloatPrefixBytes = 176;
inline constexpr std::string_view kNativeNullDepthConstantCanonicalVersion =
    "1c697af73d13496ae2424cfd4e65de2856f16bd612bd715b92160403c8bd762b";
inline constexpr std::array<std::string_view, 2> kNativeNullDepthConstantVertexSources{
    "a14c7ab2b53a15fd0f1a11ee1a001cd6fb2bc839aa299f3bb10c039cd2ec8665",
    "3f771dd73ee00c6649fb5356ed20dd677ad74156f8505b61e67315be642e15c5"};
inline constexpr std::array<std::string_view, 2> kNativeNullDepthConstantVertexSpirvHashes{
    "5ca1d26332f621747b741c431e784f23199065b7abcfbc663121901be2746c2e",
    "55e3044c9832a75c74107e40a9dcb35b6e93950fb6648074fbd3e5980715a781"};
inline constexpr std::string_view kNativeNullDepthConstantPixelSpirvHash =
    "0a3fb61d9ff86baad8e1c341cf4309340be244b37598e833b0a39a03babb94da";

namespace detail {
// This is an exact-byte upload-footprint certificate for three reviewed
// canonical modules, not a SPIR-V validator or permission to alter a shader.
// Valid compiler output is required. Structural checks additionally close every
// physical load's pointer ancestry, so mutation fixtures exercise dynamic-index
// rejection independently of the public SHA pins. Full shared2624-byte contents,
// push addresses, vertex data, shader code and the10816-byte buffer ABI remain.
struct NativeNullDepthConstantNode {
    enum Root : uint8_t { Unknown, Literal, Push, PushMember, Vertex, Shared, Geometry };
    Root root = Unknown;
    uint64_t offset = 0;
};
struct NativeNullDepthConstantType {
    enum Kind : uint8_t { Unknown, Integer, Float, Vector, Pointer, Structure };
    Kind kind = Unknown;
    uint32_t width = 0, element = 0, count = 0, storage = 0;
    std::array<uint32_t, 3> members{};
};
inline bool NativeNullDepthConstantOpcode(uint32_t op) {
    switch (op) {
    case 3: case 5: case 6: case 10: case 11: case 12: case 14: case 15: case 16: case 17:
    case 19: case 20: case 21: case 22: case 23: case 29: case 30: case 32: case 33:
    case 41: case 42: case 43: case 44: case 46: case 54: case 56: case 59: case 61:
    case 62: case 65: case 71: case 72: case 79: case 80: case 81: case 82:
    case 109: case 110: case 112: case 113: case 124: case 127: case 128: case 129:
    case 130: case 132: case 133: case 134: case 137: case 142: case 156: case 168:
    case 169: case 170: case 171: case 172: case 174: case 176: case 177: case 178:
    case 186: case 194: case 196: case 197: case 198: case 199: case 200:
    case 213: case 214: case 225: case 245: case 246: case 247: case 248: case 249:
    case 250: case 251: case 252: case 253: case 5364: case 5365: return true;
    default: return false;
    }
}

inline bool NativeNullDepthConstantStructureForFixture(std::span<const uint32_t> words, bool pixel) {
    using Node = NativeNullDepthConstantNode;
    using Type = NativeNullDepthConstantType;
    if (words.size() != (pixel ? 2423u : 2524u) || words[0] != 0x07230203 ||
        words[3] == 0 || words[3] > 4096 || words[4] != 0) return false;
    const auto bound = words[3];
    std::vector<Type> types(bound);
    std::vector<Node> nodes(bound);
    std::vector<uint32_t> value_types(bound);
    std::array<uint32_t, 9> vertex_offsets{};
    std::array<bool, 9> vertex_seen{};
    constexpr std::array<uint32_t, 9> permitted{0, 16, 32, 48, 96, 112, 128, 144, 160};
    uint32_t entries = 0, push_variables = 0, physical_loads = 0, vertex_loads = 0,
        shared_loads = 0, geometry_loads = 0;
    auto valid = [&](uint32_t id) { return id && id < bound; };
    auto width = [&](uint32_t id) -> uint32_t {
        if (!valid(id)) return 0;
        const auto& type = types[id];
        if (type.kind == Type::Integer || type.kind == Type::Float) return type.width / 8;
        if (type.kind == Type::Vector && valid(type.element)) {
            const auto& element = types[type.element];
            if ((element.kind == Type::Integer || element.kind == Type::Float) && element.width == 32 &&
                type.count > 0 && type.count <= 4) return 4 * type.count;
        }
        return 0;
    };
    auto unsigned64 = [&](uint32_t id) { return valid(id) && types[id].kind == Type::Integer && types[id].width == 64 && types[id].count == 0; };
    for (size_t at = 5; at < words.size();) {
        const uint32_t count = words[at] >> 16, op = words[at] & 65535;
        if (!count || count > words.size() - at || !NativeNullDepthConstantOpcode(op)) return false;
        const auto a = words.subspan(at + 1, count - 1);
        if (a.size() >= 2 && valid(a[0]) && types[a[0]].kind != Type::Unknown && valid(a[1])) value_types[a[1]] = a[0];
        if (op == 15) {
            if (a.size() < 3 || a[0] != (pixel ? 4u : 0u)) return false;
            ++entries;
        } else if (op == 21 || op == 22) {
            if (a.size() != (op == 21 ? 3u : 2u) || !valid(a[0]) ||
                (a[1] != 32 && a[1] != 64)) return false;
            types[a[0]] = {op == 21 ? Type::Integer : Type::Float, a[1], 0, op == 21 ? a[2] : 0};
        } else if (op == 23) {
            if (a.size() != 3 || !valid(a[0]) || !valid(a[1])) return false;
            types[a[0]] = {Type::Vector, 0, a[1], a[2]};
        } else if (op == 30) {
            if (a.empty() || !valid(a[0])) return false;
            types[a[0]].kind = Type::Structure;
            types[a[0]].count = uint32_t(a.size() - 1);
            if (a.size() == 4) for (size_t i = 0; i < 3; ++i) types[a[0]].members[i] = a[i + 1];
        } else if (op == 32) {
            if (a.size() != 3 || !valid(a[0]) || !valid(a[2])) return false;
            types[a[0]] = {Type::Pointer, 0, a[2], 0, a[1]};
        } else if (op == 43 && a.size() >= 3 && valid(a[0]) && valid(a[1]) && types[a[0]].kind == Type::Integer) {
            const auto bits = types[a[0]].width;
            if (a.size() != (bits == 64 ? 4u : 3u)) return false;
            nodes[a[1]] = {Node::Literal, uint64_t(a[2]) | (bits == 64 ? uint64_t(a[3]) << 32 : 0)};
        } else if (op == 59) {
            if (a.size() < 3 || !valid(a[0]) || !valid(a[1]) || types[a[0]].kind != Type::Pointer || types[a[0]].storage != a[2]) return false;
            if (a[2] == 9) {
                const auto& structure = types[types[a[0]].element];
                if (structure.kind != Type::Structure || structure.count != 3 ||
                    !std::ranges::all_of(structure.members, unsigned64)) return false;
                nodes[a[1]] = {Node::Push, 0}; ++push_variables;
            }
        } else if (op == 65) {
            if (a.size() < 4 || !valid(a[0]) || !valid(a[1]) || !valid(a[2])) return false;
            if (nodes[a[2]].root == Node::Push) {
                if (a.size() != 4 || !valid(a[3]) || nodes[a[3]].root != Node::Literal || nodes[a[3]].offset > 2 ||
                    nodes[a[3]].offset == 1 || (pixel && nodes[a[3]].offset != 2) ||
                    types[a[0]].kind != Type::Pointer || types[a[0]].storage != 9 || !unsigned64(types[a[0]].element)) return false;
                nodes[a[1]] = {Node::PushMember, nodes[a[3]].offset};
            }
            // Physical access chains are not part of these reviewed modules.
            // Never lose address provenance through an unrecognized pointer op.
            if (types[a[0]].kind == Type::Pointer && types[a[0]].storage == 5349) return false;
        } else if (op == 128) {
            if (a.size() != 4 || !valid(a[1]) || !valid(a[2]) || !valid(a[3])) return false;
            const auto l = nodes[a[2]], r = nodes[a[3]];
            const auto address = [](Node n) { return n.root == Node::Vertex || n.root == Node::Shared || n.root == Node::Geometry; };
            const auto base = address(l) ? l : r, offset = address(l) ? r : l;
            if (address(base)) {
                if (!unsigned64(a[0]) || !unsigned64(value_types[a[2]]) || !unsigned64(value_types[a[3]])) return false;
                if (base.root == Node::Geometry) {
                    if (address(offset)) return false;
                    nodes[a[1]] = base;
                }
                else {
                    if (offset.root != Node::Literal || base.offset > std::numeric_limits<uint64_t>::max() - offset.offset) return false;
                    nodes[a[1]] = {base.root, base.offset + offset.offset};
                }
            }
        } else if (op == 124) {
            if (a.size() != 3 || !valid(a[0]) || !valid(a[1]) || !valid(a[2])) return false;
            if (types[a[0]].kind == Type::Pointer && types[a[0]].storage == 5349) {
                const auto root = nodes[a[2]].root;
                if (root != Node::Vertex && root != Node::Shared && root != Node::Geometry) return false;
                nodes[a[1]] = nodes[a[2]];
            }
        } else if (op == 61) {
            if (a.size() < 3 || !valid(a[0]) || !valid(a[1]) || !valid(a[2]) || !valid(value_types[a[2]])) return false;
            const auto& pointer = types[value_types[a[2]]];
            if (pointer.kind != Type::Pointer || pointer.element != a[0]) return false;
            const auto n = nodes[a[2]];
            if (pointer.storage == 9) {
                if (n.root != Node::PushMember || !unsigned64(a[0])) return false;
                nodes[a[1]] = {n.offset == 0 ? Node::Vertex : Node::Shared, 0};
            } else if (pointer.storage == 5349) {
                const auto bytes = width(a[0]);
                if (!bytes) return false;
                ++physical_loads;
                if (n.root == Node::Vertex) {
                    if (pixel || types[a[0]].kind != Type::Vector || types[a[0]].count != 4 ||
                        types[types[a[0]].element].kind != Type::Float || bytes != 16 || n.offset > kNativeNullDepthFloatPrefixBytes - bytes) return false;
                    auto it = std::ranges::find(permitted, n.offset);
                    if (it == permitted.end()) return false;
                    const auto index = size_t(it - permitted.begin()); vertex_seen[index] = true; ++vertex_offsets[index]; ++vertex_loads;
                } else if (n.root == Node::Shared) {
                    if (n.offset > 2624 - bytes) return false;
                    ++shared_loads;
                    if (!pixel && n.offset == 2224 && unsigned64(a[0])) nodes[a[1]] = {Node::Geometry, 0};
                    else if (unsigned64(a[0])) return false;
                } else if (n.root == Node::Geometry) {
                    if (pixel || types[a[0]].kind != Type::Integer || types[a[0]].width != 32 || bytes != 4) return false;
                    ++geometry_loads;
                } else return false;
            }
        } else if (op == 62) {
            if (a.size() < 2 || !valid(a[0]) || !valid(value_types[a[0]])) return false;
            if (types[value_types[a[0]]].kind != Type::Pointer || types[value_types[a[0]]].storage == 5349) return false;
        }
        at += count;
    }
    if (entries != 1 || push_variables != 1) return false;
    if (pixel) return vertex_loads == 0 && geometry_loads == 0 && shared_loads == 4 && physical_loads == 4;
    return physical_loads == 24 && vertex_loads == 11 && shared_loads == 10 && geometry_loads == 3 &&
        std::ranges::all_of(vertex_seen, [](bool value) { return value; }) && vertex_offsets[3] == 2 && vertex_offsets[4] == 2;
}
inline std::string NativeNullDepthConstantDigest(std::span<const uint32_t> words) {
    std::vector<uint8_t> bytes; bytes.reserve(words.size_bytes());
    for (auto word : words) for (unsigned shift = 0; shift < 32; shift += 8) bytes.push_back(uint8_t(word >> shift));
    return ContentHash(bytes);
}
} // namespace detail

// Cache these certificates only when the actual canonical shader module is
// admitted/prewarmed. No hashing, parsing or new ownership occurs per Draw.
inline bool NativeNullDepthConstantVertexCertified(std::span<const uint32_t> words,
    std::string_view source, std::string_view canonical_version) noexcept {
    try {
        const auto it = std::ranges::find(kNativeNullDepthConstantVertexSources, source);
        return canonical_version == kNativeNullDepthConstantCanonicalVersion && words.size() == 2524 &&
            it != kNativeNullDepthConstantVertexSources.end() && detail::NativeNullDepthConstantDigest(words) ==
                kNativeNullDepthConstantVertexSpirvHashes[size_t(it - kNativeNullDepthConstantVertexSources.begin())] &&
            detail::NativeNullDepthConstantStructureForFixture(words, false);
    } catch (...) { return false; }
}
inline bool NativeNullDepthConstantPixelCertified(std::span<const uint32_t> words,
    std::string_view canonical_version) noexcept {
    try {
        return canonical_version == kNativeNullDepthConstantCanonicalVersion && words.size() == 2423 &&
            detail::NativeNullDepthConstantDigest(words) == kNativeNullDepthConstantPixelSpirvHash &&
            detail::NativeNullDepthConstantStructureForFixture(words, true);
    } catch (...) { return false; }
}
} // namespace gow3::renderer
