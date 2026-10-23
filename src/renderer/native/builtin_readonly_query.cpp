#include "builtin_readonly_query.h"
#include "content_hash.h"
#include "fsi_shader_variant.h"

#include <array>
#include <cstring>
#include <map>
#include <set>

namespace gow3::renderer {
namespace {
std::span<const uint8_t> Bytes(std::span<const uint32_t> words) {
    return {reinterpret_cast<const uint8_t*>(words.data()), words.size_bytes()};
}
struct Instruction {
    size_t at;
    uint32_t count, opcode;
    std::span<const uint32_t> args;
};
}

bool AuditedReadonlyQueryVertex(std::string_view source, std::span<const uint8_t> spirv) {
    return source == kReadonlyQueryVertexSource && spirv.size() == 6559 * 4 &&
           ContentHash(spirv) == kReadonlyQueryVertexSpirv;
}

std::optional<std::vector<uint32_t>> TryBuiltinReadonlyQuery(std::span<const uint32_t> words) {
    // Pin first, before parsing or allocating based on any module operands.
    // The existing closed native epilogue audit then independently verifies
    // capabilities, opcodes, coherent set4 and external write destinations.
    if (words.size() != 2423 || ContentHash(Bytes(words)) != kReadonlyQueryBuiltinSpirv)
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
        // This pinned module uses only these two pointer-producing forms.
        if ((opcode == 59 || opcode == 65) && a.size() >= 2 && pointer_storage.contains(a[0]))
            values[a[1]] = pointer_storage.at(a[0]);
        at += count;
    }
    std::set<size_t> remove;
    unsigned stores = 0, begins = 0, ends = 0, barriers = 0, modes = 0, caps = 0, extensions = 0;
    for (const auto& i : instructions) {
        const auto a = i.args;
        if (i.opcode == 62) {
            if (a.size() < 2 || !values.contains(a[0])) return std::nullopt;
            const auto storage = values.at(a[0]);
            if (storage == 12) {
                // The pinned canonical depth builtin has one set4 uint store.
                // Keep this operand proof explicit in addition to whole hash.
                if(a.size()!=2||a[0]!=184||a[1]!=426)return std::nullopt;
                ++stores; remove.insert(i.at);
            }
            else if (storage != 7) return std::nullopt;
        } else if (i.opcode == 59 && a.size() >= 3 && a[2] == 3) {
            return std::nullopt; // No shader output, including FragDepth.
        } else if (i.opcode == 16) {
            if (a.size() != 2 || (a[1] != 7 && a[1] != 5366)) return std::nullopt;
            if (a[1] == 5366) { ++modes; remove.insert(i.at); }
        } else if (i.opcode == 17 && a.size() == 1 && a[0] == 5378) {
            ++caps; remove.insert(i.at);
        } else if (i.opcode == 10) {
            constexpr char name[] = "SPV_EXT_fragment_shader_interlock";
            if (a.size_bytes() >= sizeof(name) &&
                std::memcmp(a.data(), name, sizeof(name)) == 0) {
                ++extensions; remove.insert(i.at);
            }
        } else if (i.opcode == 5364) { ++begins; remove.insert(i.at); }
        else if (i.opcode == 5365) { ++ends; remove.insert(i.at); }
        else if (i.opcode == 225) { ++barriers; remove.insert(i.at); }
    }
    if (stores != 1 || begins != 1 || ends != 1 || barriers != 2 ||
        modes != 1 || caps != 1 || extensions != 1)
        return std::nullopt;
    std::vector<uint32_t> result(words.begin(), words.begin() + 5);
    result.reserve(words.size());
    for (const auto& i : instructions)
        if (!remove.contains(i.at))
            result.insert(result.end(), words.begin() + i.at, words.begin() + i.at + i.count);
    // No arithmetic, pointer, load, branch, discard or instruction operands
    // change. ID holes are valid. Actual late query behavior remains intact.
    return result;
}
} // namespace gow3::renderer
