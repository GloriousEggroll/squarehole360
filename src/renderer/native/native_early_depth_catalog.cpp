#include "renderer/native/native_early_depth_catalog.h"
#include "renderer/native/native_early_depth_catalog_certificate.h"
#include "renderer/native/content_hash.h"
#include "native_early_depth_catalog_version.h"

#include <algorithm>
#include <array>
#include <initializer_list>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_map>

namespace gow3::renderer {
namespace {
constexpr size_t kMaximumWords = 128u * 1024u / sizeof(uint32_t);
constexpr std::array<NativeEarlyDepthCatalogPin, 2> kPins{{
    {kNativeEarlyDepthCatalogVertexSource, kNativeEarlyDepthCatalogPixelSources[0],
     "34a3d98d8385d2ab97d1fd366ab0136a29b0a2cf81e2711a4b24469d7ba125df",
     "4c590ab0717edce95b173114ae12848857e0232f964a12f83aa6f0bedce6d42a",
     33808, 84116, 3, 6, 2},
    {kNativeEarlyDepthCatalogVertexSource, kNativeEarlyDepthCatalogPixelSources[1],
     "34a3d98d8385d2ab97d1fd366ab0136a29b0a2cf81e2711a4b24469d7ba125df",
     "96e0370ac9d536b4d43bb112527e891554e8ba3f6f25b14e108646056c37a107",
     33808, 74552, 3, 5, 2}
}};
// Original DXC cleanup ends/kill sites are preserved, not normalized.
constexpr std::array<uint32_t, 2> kOriginalInterlockEnds{2, 3};
constexpr std::array<uint32_t, 2> kOriginalDiscards{2, 3};
struct Instruction {
    size_t at;
    uint32_t opcode;
    std::span<const uint32_t> args;
};
struct PointerType { uint32_t storage, pointee; };
struct Shape {
    uint32_t entry = 0, stores = 0, implicit_lod = 0, derivatives = 0;
    size_t insert = 0;
};
void Require(bool condition, const char* message) {
    if (!condition) throw std::invalid_argument(message);
}
bool Equal(std::span<const uint32_t> a, std::initializer_list<uint32_t> b) {
    return std::ranges::equal(a, b);
}
std::string Digest(std::span<const uint32_t> words) {
    // SPIR-V wire bytes are little endian, independent of host endianness.
    std::vector<uint8_t> bytes;
    bytes.reserve(words.size() * 4);
    for (const auto word : words)
        for (unsigned shift = 0; shift != 32; shift += 8)
            bytes.push_back(uint8_t(word >> shift));
    return ContentHash(bytes);
}
std::string Literal(std::span<const uint32_t> words) {
    std::string text;
    for (size_t n = 0; n < words.size(); ++n)
        for (unsigned shift = 0; shift != 32; shift += 8) {
            const auto c = char((words[n] >> shift) & 255);
            if (!c) {
                Require(n + 1 == words.size() && (words[n] >> shift) == 0,
                        "Malformed native early-depth catalogue literal");
                return text;
            }
            text.push_back(c);
        }
    throw std::invalid_argument("Unterminated native early-depth catalogue literal");
}
bool AuditedOpcode(uint32_t op) {
    // Closed opcode union of the three authenticated canonical modules only.
    // No calls/copies/atomics/image writes/subgroup/helper/clock effects.
    switch (op) {
    case 3: case 5: case 6: case 10: case 11: case 12: case 14: case 15:
    case 16: case 17: case 19: case 20: case 21: case 22: case 23: case 25:
    case 26: case 27: case 28: case 29: case 30: case 32: case 33: case 41:
    case 42: case 43: case 44: case 46: case 50: case 52: case 54: case 56:
    case 59: case 61: case 62: case 65: case 71: case 72: case 79: case 80:
    case 81: case 82: case 86: case 87: case 109: case 110: case 111: case 112:
    case 113: case 124: case 127: case 128: case 129: case 130: case 131: case 132:
    case 133: case 134: case 136: case 137: case 142: case 148: case 154: case 156:
    case 168: case 169: case 170: case 171: case 172: case 174: case 176: case 177:
    case 178: case 180: case 182: case 184: case 186: case 190: case 194: case 195:
    case 196: case 197: case 198: case 199: case 200: case 213: case 214: case 225:
    case 245: case 246: case 247: case 248: case 249: case 250: case 251: case 252:
    case 253: case 5364: case 5365: return true;
    default: return false;
    }
}
bool AuditedGlsl(uint32_t op) {
    switch (op) {
    case 2: case 3: case 4: case 8: case 29: case 30: case 32: case 38:
    case 43: case 58: case 62: case 79: case 80: return true;
    default: return false;
    }
}
std::vector<Instruction> Parse(std::span<const uint32_t> words) {
    Require(words.size() >= 5 && words.size() <= kMaximumWords &&
        words[0] == 0x07230203 &&
        (words[1] == 0x00010000 || words[1] == 0x00010500) && words[3] &&
        words[4] == 0, "Unsupported native early-depth catalogue SPIR-V header");
    std::vector<Instruction> result;
    for (size_t at = 5; at < words.size();) {
        const auto count = words[at] >> 16, op = words[at] & 65535;
        Require(count && count <= words.size() - at && AuditedOpcode(op),
            "Unsupported native early-depth catalogue instruction");
        result.push_back({at, op, words.subspan(at + 1, count - 1)});
        at += count;
    }
    return result;
}
Shape Certify(std::span<const uint32_t> words, bool pixel, size_t pin_index) {
    Require(pin_index < kPins.size(), "Unknown catalogue structural pin");
    const auto& pin = kPins[pin_index];
    const auto instructions = Parse(words);
    Shape shape;
    size_t entries = 0, models = 0, functions = 0, ends = 0, modes = 0;
    size_t first_mode = 0, first_barrier = 0, last_barrier = 0;
    size_t barriers = 0, begins = 0, interlock_ends = 0, discards = 0;
    uint32_t edram = 0;
    std::map<uint32_t, std::vector<std::vector<uint32_t>>> decorations;
    std::unordered_map<uint32_t, PointerType> pointer_types;
    std::unordered_map<uint32_t, uint32_t> pointer_values, roots, variables;
    std::map<uint32_t, std::array<uint32_t, 2>> constants;
    std::set<uint32_t> uint_types, glsl;
    for (const auto& i : instructions) {
        const auto a = i.args;
        if (i.opcode == 15) {
            Require(a.size() >= 3 && a[0] == (pixel ? 4u : 0u) && a[1],
                "Wrong native early-depth catalogue stage");
            shape.entry = a[1]; ++entries;
        } else if (i.opcode == 14) {
            Require(Equal(a, {5348, 1}), "Wrong native early-depth catalogue memory model");
            ++models;
        } else if (i.opcode == 16) {
            Require(pixel && a.size() == 2 && (a[1] == 7 || a[1] == 5366),
                "Unsupported native early-depth catalogue execution mode");
            if (!first_mode) first_mode = i.at;
            ++modes;
        } else if (i.opcode == 17) {
            Require(a.size() == 1 && (a[0] == 1 || a[0] == 11 ||
                a[0] == 5302 || a[0] == 5347 ||
                (pixel && (a[0] == 50 || a[0] == 51 || a[0] == 5378))),
                "Unsupported native early-depth catalogue capability");
        } else if (i.opcode == 10) {
            const auto extension = Literal(a);
            Require(extension == "SPV_EXT_descriptor_indexing" ||
                extension == "SPV_KHR_physical_storage_buffer" ||
                (pixel && extension == "SPV_EXT_fragment_shader_interlock"),
                "Unsupported native early-depth catalogue extension");
        } else if (i.opcode == 11) {
            Require(a.size() >= 2 && Literal(a.subspan(1)) == "GLSL.std.450",
                "Unsupported native early-depth catalogue extended instruction set");
            glsl.insert(a[0]);
        } else if (i.opcode == 71) {
            Require(a.size() >= 2, "Truncated native early-depth catalogue decoration");
            decorations[a[0]].emplace_back(a.begin() + 1, a.end());
            if (a[1] == 11) {
                Require(a.size() == 3 && (pixel ? (a[2] == 15 || a[2] == 17) :
                    (a[2] == 0 || a[2] == 42)), "Observable or exported shader builtin");
            }
        } else if (i.opcode == 32) {
            Require(a.size() == 3, "Truncated native early-depth catalogue pointer type");
            pointer_types.emplace(a[0], PointerType{a[1], a[2]});
        } else if (i.opcode == 21 && a.size() == 3 && a[1] == 32 && a[2] == 0) {
            uint_types.insert(a[0]);
        } else if (i.opcode == 43 && a.size() == 3) {
            constants.emplace(a[1], std::array<uint32_t, 2>{a[0], a[2]});
        } else if (i.opcode == 54) {
            Require(a.size() == 4, "Truncated native early-depth catalogue function");
            ++functions;
        } else if (i.opcode == 56) ++ends;
        else if (i.opcode == 225) {
            if (!barriers) first_barrier = i.at;
            last_barrier = i.at; ++barriers;
        } else if (i.opcode == 5364) ++begins;
        else if (i.opcode == 5365) ++interlock_ends;
        else if (i.opcode == 252) ++discards;
        else if (i.opcode == 87) ++shape.implicit_lod;
        else if (i.opcode == 213 || i.opcode == 214) ++shape.derivatives;
    }
    Require(entries == 1 && models == 1 && functions == 1 && ends == 1 &&
        modes == (pixel ? 2u : 0u), "Unsupported native early-depth catalogue entry shape");
    auto decorated = [&](uint32_t id, std::initializer_list<uint32_t> value) {
        const auto it = decorations.find(id);
        return it != decorations.end() && std::ranges::any_of(it->second,
            [&](const auto& decoration) { return Equal(decoration, value); });
    };
    for (const auto& [id, values] : decorations)
        if (decorated(id, {34, 4}) && decorated(id, {33, 0})) {
            Require(pixel && !edram, "Ambiguous external EDRAM descriptor");
            edram = id;
        }
    for (const auto& i : instructions) {
        const auto a = i.args;
        if (i.opcode == 16) Require(a[0] == shape.entry, "Foreign execution mode");
        if (i.opcode == 54) Require(a[1] == shape.entry, "Foreign shader function");
        if (i.opcode == 12)
            Require(a.size() >= 4 && glsl.contains(a[2]) && AuditedGlsl(a[3]),
                "Unaudited extended instruction effect");
        if ((i.opcode == 59 || i.opcode == 61 || i.opcode == 65 ||
             i.opcode == 124 || i.opcode == 169 || i.opcode == 245) &&
             a.size() >= 2 && pointer_types.contains(a[0]))
            pointer_values.emplace(a[1], a[0]);
        if (i.opcode == 59) {
            Require(a.size() >= 3 && pointer_types.contains(a[0]) &&
                pointer_types.at(a[0]).storage == a[2], "Invalid shader variable");
            variables.emplace(a[1], a[2]); roots.emplace(a[1], a[1]);
            Require(a[2] != 12 || (pixel && a[1] == edram),
                "Additional storage-buffer effects require a new certificate");
        } else if (i.opcode == 65) {
            Require(a.size() >= 3, "Truncated access-chain ancestry");
            roots.emplace(a[1], roots.contains(a[2]) ? roots.at(a[2]) : 0);
        }
    }
    size_t main_begin = 0, main_end = 0;
    if (pixel) {
        Require(edram && variables.contains(edram) && variables.at(edram) == 12 &&
            decorated(edram, {23}) && barriers == 2 && begins == 1 &&
            interlock_ends == kOriginalInterlockEnds[pin_index] && discards == kOriginalDiscards[pin_index],
            "Wrong original coherent FSI epilogue certificate");
        for (size_t n = 0; n < instructions.size(); ++n) {
            const auto& i = instructions[n];
            if (i.opcode != 225) continue;
            const auto a = i.args;
            Require(a.size() == 2 && constants.contains(a[0]) && constants.contains(a[1]),
                "Wrong original FSI barrier operands");
            const auto scope = constants.at(a[0]), semantics = constants.at(a[1]);
            Require(uint_types.contains(scope[0]) && scope[1] == 1 &&
                uint_types.contains(semantics[0]) && semantics[1] == 0x848,
                "Wrong original FSI Device barrier semantics");
            if (i.at == first_barrier) {
                Require(n && instructions[n - 1].opcode == 5364 &&
                    instructions[n - 1].args.empty(), "Missing original FSI begin");
                main_begin = instructions[n - 1].at;
            }
            if (i.at == last_barrier) {
                Require(n + 1 < instructions.size() && instructions[n + 1].opcode == 5365 &&
                    instructions[n + 1].args.empty(), "Missing original FSI end");
                main_end = instructions[n + 1].at;
            }
        }
        Require(main_begin && main_begin < main_end, "Invalid original epilogue interval");
        shape.insert = first_mode;
    } else Require(!edram && !barriers && !begins && !interlock_ends && !discards,
        "Vertex has external synchronization effects");
    for (const auto& i : instructions) {
        const auto a = i.args;
        if (i.opcode == 61 && a.size() >= 3 && roots.contains(a[2]) &&
            roots.at(a[2]) == edram && edram)
            Require(main_begin < i.at && i.at < main_end, "EDRAM read outside packed epilogue");
        if (i.opcode != 62) continue;
        Require(a.size() >= 2 && pointer_values.contains(a[0]), "Unknown shader-store ancestry");
        const auto storage = pointer_types.at(pointer_values.at(a[0])).storage;
        if (storage == 3 || storage == 7) continue; // Output or invocation-local.
        Require(pixel && storage == 12 && roots.contains(a[0]) &&
            roots.at(a[0]) == edram && main_begin < i.at && i.at < main_end,
            "Write outside original packed EDRAM epilogue");
        ++shape.stores;
    }
    Require(shape.stores == (pixel ? pin.pixel_edram_stores : 0u) &&
        shape.implicit_lod == (pixel ? pin.pixel_implicit_lod_samples : 0u) &&
        shape.derivatives == (pixel ? pin.pixel_derivative_instructions : 0u), "Pinned effect-count mismatch");
    return shape;
}
} // namespace

std::span<const NativeEarlyDepthCatalogPin> NativeEarlyDepthCatalogPins() { return kPins; }
bool NativeEarlyDepthCatalogPair(std::string_view vertex, std::string_view pixel) {
    return std::ranges::any_of(kPins, [&](const auto& pin) {
        return vertex == pin.vertex_source && pixel == pin.pixel_source;
    });
}
std::shared_ptr<const NativeEarlyDepthProgram> TryPrepareNativeEarlyDepthCatalogShader(
    std::span<const uint32_t> canonical, std::string_view source,
    std::string_view canonical_cache_version) {
    if (canonical_cache_version != kNativeEarlyDepthCanonicalVersion) return {};
    const auto pin = std::ranges::find(kPins, source, &NativeEarlyDepthCatalogPin::pixel_source);
    if (pin == kPins.end() || canonical.size() != pin->pixel_bytes / 4u ||
        Digest(canonical) != pin->pixel_spirv_sha256) return {};
    Shape shape;
    try { shape = Certify(canonical, true, size_t(pin - kPins.begin())); }
    catch (const std::invalid_argument&) { return {}; }
    auto program = std::make_shared<NativeEarlyDepthProgram>();
    program->source_sha256 = std::string(source);
    program->canonical_spirv_sha256 = std::string(pin->pixel_spirv_sha256);
    program->added_execution_mode_word = shape.insert;
    program->entry_point = shape.entry;
    program->canonical_edram_stores = shape.stores;
    program->canonical_implicit_lod_samples = shape.implicit_lod;
    program->canonical_derivative_instructions = shape.derivatives;
    program->spirv.reserve(canonical.size() + 3);
    program->spirv.insert(program->spirv.end(), canonical.begin(), canonical.begin() + shape.insert);
    program->spirv.insert(program->spirv.end(), {(3u << 16) | 16u, shape.entry, 9u});
    program->spirv.insert(program->spirv.end(), canonical.begin() + shape.insert, canonical.end());
    Require(program->spirv.size() == canonical.size() + 3 &&
        std::ranges::equal(std::span(program->spirv).first(shape.insert), canonical.first(shape.insert)) &&
        std::ranges::equal(std::span(program->spirv).subspan(shape.insert + 3), canonical.subspan(shape.insert)),
        "Early-depth original-word restoration failed");
    program->program_sha256 = Digest(program->spirv);
    return program;
}

bool NativeEarlyDepthCatalogVertexCertified(std::span<const uint32_t> canonical,
    std::string_view source, std::string_view canonical_cache_version) {
    const auto& pin = kPins.front();
    if (source != pin.vertex_source ||
        canonical_cache_version != kNativeEarlyDepthCanonicalVersion ||
        canonical.size() != pin.vertex_bytes / 4u ||
        Digest(canonical) != pin.vertex_spirv_sha256) return false;
    try { Certify(canonical, false, 0); return true; }
    catch (const std::invalid_argument&) { return false; }
}
std::string NativeEarlyDepthCatalogVersion() { return kNativeEarlyDepthCatalogVersion; }
namespace detail {
bool NativeEarlyDepthCatalogStructureForFixture(std::span<const uint32_t> words,
    bool pixel, size_t pin_index) {
    try { Certify(words, pixel, pin_index); return true; }
    catch (const std::invalid_argument&) { return false; }
}
}
} // namespace gow3::renderer
