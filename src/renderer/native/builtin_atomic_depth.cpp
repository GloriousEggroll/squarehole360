#include "builtin_atomic_depth.h"
#include "content_hash.h"
#include "fsi_shader_variant.h"

#include <cstring>
#include <map>
#include <set>

namespace gow3::renderer {
namespace {
struct Instruction {
    size_t at;
    uint32_t count, opcode;
    std::span<const uint32_t> args;
};
}

bool AuditedAtomicDepthVertex(std::string_view source, std::span<const uint8_t> spirv) {
    if (spirv.size() != 2524 * 4) return false;
    for (size_t i = 0; i < std::size(kAtomicDepthVertexSources); ++i)
        if (source == kAtomicDepthVertexSources[i])
            return ContentHash(spirv) == kAtomicDepthVertexSpirv[i];
    return false;
}

std::optional<std::vector<uint32_t>> TryBuiltinAtomicDepth(std::span<const uint32_t> words) {
    // This is a transform of one audited compiler result, not a general shader
    // optimizer. Pin before trusting any operands; unknown modules stay Native.
    const auto bytes = std::span(reinterpret_cast<const uint8_t*>(words.data()), words.size_bytes());
    if (words.size() != 2423 || ContentHash(bytes) != kAtomicDepthBuiltinSpirv)
        return std::nullopt;
    try {
        (void)TransformNativeFsiImplicitBarriers(words);
    } catch (const std::invalid_argument&) {
        return std::nullopt;
    }
    std::vector<Instruction> instructions;
    std::map<uint32_t, uint32_t> pointer_storage, values;
    for (size_t at = 5; at < words.size();) {
        const uint32_t count = words[at] >> 16, opcode = words[at] & 65535;
        if (!count || count > words.size() - at) return std::nullopt;
        const auto a = words.subspan(at + 1, count - 1);
        instructions.push_back({at, count, opcode, a});
        if (opcode == 32 && a.size() == 3) pointer_storage[a[0]] = a[1];
        if ((opcode == 59 || opcode == 65) && a.size() >= 2 && pointer_storage.contains(a[0]))
            values[a[1]] = pointer_storage.at(a[0]);
        at += count;
    }
    std::set<size_t> remove;
    size_t load = 0, store = 0;
    unsigned loads = 0, stores = 0, begins = 0, ends = 0, barriers = 0, modes = 0, caps = 0, extensions = 0;
    bool uint_type = false, zero = false, one = false;
    for (const auto& i : instructions) {
        const auto a = i.args;
        if (i.opcode == 21 && a.size() == 3 && a[0] == 13 && a[1] == 32 && a[2] == 0) uint_type = true;
        if (i.opcode == 43 && a.size() == 3 && a[0] == 13) {
            zero |= a[1] == 19 && a[2] == 0;
            one |= a[1] == 31 && a[2] == 1;
        }
        if (i.opcode == 61 && a.size() >= 3 && values.contains(a[2]) && values.at(a[2]) == 12) {
            if (a.size() != 3 || a[0] != 13 || a[1] != 185 || a[2] != 184) return std::nullopt;
            load = i.at; ++loads;
        } else if (i.opcode == 62) {
            if (a.size() < 2 || !values.contains(a[0])) return std::nullopt;
            const auto storage = values.at(a[0]);
            if (storage == 12) {
                if (a.size() != 2 || a[0] != 184 || a[1] != 426) return std::nullopt;
                store = i.at; ++stores;
            } else if (storage != 7) return std::nullopt;
        } else if (i.opcode == 59 && a.size() >= 3 && a[2] == 3) {
            return std::nullopt;
        } else if (i.opcode == 16) {
            if (a.size() != 2 || (a[1] != 7 && a[1] != 5366)) return std::nullopt;
            if (a[1] == 5366) { ++modes; remove.insert(i.at); }
        } else if (i.opcode == 17 && a.size() == 1 && a[0] == 5378) {
            ++caps; remove.insert(i.at);
        } else if (i.opcode == 10) {
            constexpr char name[] = "SPV_EXT_fragment_shader_interlock";
            if (a.size_bytes() >= sizeof(name) && std::memcmp(a.data(), name, sizeof(name)) == 0) {
                ++extensions; remove.insert(i.at);
            }
        } else if (i.opcode == 5364) { ++begins; remove.insert(i.at); }
        else if (i.opcode == 5365) { ++ends; remove.insert(i.at); }
        else if (i.opcode == 225) { ++barriers; remove.insert(i.at); }
    }
    if (!uint_type || !zero || !one || loads != 1 || stores != 1 || begins != 1 || ends != 1 ||
        barriers != 2 || modes != 1 || caps != 1 || extensions != 1 || words[3] != 431)
        return std::nullopt;
    std::vector<uint32_t> result(words.begin(), words.begin() + 5);
    result[3] = 432; // One unused result ID for the replacement atomic maximum.
    result.reserve(words.size());
    for (const auto& i : instructions) {
        if (remove.contains(i.at)) continue;
        if (i.at == load) {
            // AtomicLoad uint %185, same pointer %184, Device, Relaxed. A plain
            // coherent load would race another fragment's atomic update.
            result.insert(result.end(), {(6u << 16) | 227u, 13, 185, 184, 31, 19});
        } else if (i.at == store) {
            // %426 is the unchanged original packed result. Exact admitted
            // state makes it max(previous>>8, incoming)<<8 | (previous&255).
            // S8 is invariant throughout this draw, so unsigned whole-word max
            // preserves every stencil bit even when %185 is an older depth.
            result.insert(result.end(), {(7u << 16) | 239u, 13, 431, 184, 31, 19, 426});
        } else {
            result.insert(result.end(), words.begin() + i.at, words.begin() + i.at + i.count);
        }
    }
    return result;
}

} // namespace gow3::renderer
