#include "renderer/native/fsi_shader_variant.h"

#include <algorithm>
#include <array>
#include <initializer_list>
#include <map>
#include <string>
#include <optional>
#include <set>
#include <stdexcept>
#include <unordered_map>

namespace gow3::renderer {
namespace {
constexpr size_t kMaximumSpirvWords = 16u * 1024 * 1024 / sizeof(uint32_t);
struct Instruction {
    size_t offset;
    uint32_t count, opcode, function;
    std::span<const uint32_t> args;
};
void Require(bool condition, const char* message) {
    if (!condition) throw std::invalid_argument(message);
}
bool Equal(std::span<const uint32_t> args, std::initializer_list<uint32_t> expected) {
    return std::ranges::equal(args, expected);
}
// Closed set of instructions emitted by the audited native FSI frontend.
// In particular, unknown extensions, all atomics/copies/image stores and
// GLSL pointer-output instructions cannot acquire an accidental pass here.
bool AuditedOpcode(uint32_t opcode) {
    switch(opcode) {
    case 1: case 3: case 5: case 6: case 10: case 11: case 12: case 14:
    case 15: case 16: case 17: case 19: case 20: case 21: case 22: case 23:
    case 25: case 26: case 27: case 28: case 29: case 30: case 32: case 33:
    case 41: case 42: case 43: case 44: case 46: case 50: case 52: case 54:
    case 56: case 59: case 61: case 62: case 65: case 71: case 72: case 79:
    case 80: case 81: case 86: case 87: case 88: case 103: case 109:
    case 110: case 111: case 112: case 113: case 124: case 127: case 128:
    case 129: case 130: case 131: case 132: case 133: case 134: case 136:
    case 137: case 142: case 148: case 154: case 156: case 168: case 169:
    case 170: case 171: case 172: case 174: case 176: case 178: case 180:
    case 182: case 184: case 186: case 190: case 194: case 195: case 196:
    case 197: case 198: case 199: case 200: case 213: case 214: case 225:
    case 245: case 246: case 247: case 248: case 249: case 250: case 251:
    case 252: case 253: case 5364: case 5365:
        return true;
    default: return false;
    }
}
bool AuditedGlslInstruction(uint32_t opcode) {
    switch(opcode) {
    case 2: case 3: case 4: case 8: case 10: case 13: case 14: case 29: case 30:
    case 31: case 32: case 38: case 43: case 58: case 62: case 79: case 80:
        return true;
    default: return false;
    }
}
std::string Literal(std::span<const uint32_t> words) {
    std::string result;
    for(size_t n=0;n<words.size();++n)for(unsigned b=0;b<4;++b) {
        const char c=char((words[n]>>(b*8))&255);
        if(!c) {
            Require(n+1==words.size()&&(words[n]>>(b*8))==0,
                    "Malformed SPIR-V literal");
            return result;
        }
        result.push_back(c);
    }
    throw std::invalid_argument("Unterminated SPIR-V literal");
}
struct PointerType { uint32_t storage, pointee; };
struct Constant { uint32_t type, value; };
using Decorations = std::map<uint32_t, std::vector<std::vector<uint32_t>>>;
bool Decorated(const Decorations& decorations, uint32_t id,
               std::initializer_list<uint32_t> value) {
    const auto found = decorations.find(id);
    return found != decorations.end() &&
        std::ranges::any_of(found->second, [&](const auto& decoration) {
            return Equal(decoration, value);
        });
}
} // namespace

std::vector<uint32_t> TransformNativeFsiImplicitBarriers(std::span<const uint32_t> words) {
    Require(words.size() >= 5 && words.size() <= kMaximumSpirvWords &&
            words[0] == 0x07230203 && words[1] >= 0x10000 && words[1] <= 0x10600 &&
            words[3] && words[4] == 0, "Invalid native FSI SPIR-V header");
    std::vector<Instruction> instructions;
    instructions.reserve(words.size() / 4);
    uint32_t function = 0;
    for (size_t at = 5; at < words.size();) {
        const uint32_t count = words[at] >> 16, opcode = words[at] & 65535;
        Require(count && count <= words.size() - at, "Truncated native FSI instruction");
        Require(AuditedOpcode(opcode), "Unaudited native FSI opcode");
        const auto args = words.subspan(at + 1, count - 1);
        if (opcode == 54) {
            Require(!function && args.size() == 4 && args[1], "Invalid FSI function");
            function = args[1];
        }
        instructions.push_back({at, count, opcode, function, args});
        if (opcode == 56) {
            Require(function != 0 && args.empty(), "Unexpected FSI function end");
            function = 0;
        }
        at += count;
    }
    Require(!function, "Unterminated FSI function");

    uint32_t entry = 0;
    size_t entries = 0, models = 0, modes = 0, functions = 0;
    bool capability = false;
    std::set<uint32_t> glsl_imports;
    std::vector<size_t> barriers;
    Decorations decorations;
    std::unordered_map<uint32_t, uint32_t> variables;
    std::unordered_map<uint32_t, Constant> constants;
    std::set<uint32_t> integer_types;
    for (size_t n = 0; n < instructions.size(); ++n) {
        const auto& i = instructions[n];
        const auto a = i.args;
        if(i.opcode == 17) {
            Require(a.size()==1, "Invalid FSI capability");
            Require(a[0]==1||a[0]==11||a[0]==50||a[0]==51||a[0]==5302||
                    a[0]==5347||a[0]==5378, "Unaudited FSI capability");
        }
        if(i.opcode == 10) {
            const auto extension=Literal(a);
            Require(extension=="SPV_EXT_fragment_shader_interlock"||
                    extension=="SPV_EXT_descriptor_indexing"||
                    extension=="SPV_KHR_physical_storage_buffer",
                    "Unaudited FSI extension");
        }
        if(i.opcode == 11) {
            Require(a.size()>=2&&Literal(a.subspan(1))=="GLSL.std.450",
                    "Unaudited FSI extended instruction set");
            glsl_imports.insert(a[0]);
        }
        if (i.opcode == 15) {
            Require(a.size() >= 3 && a[0] == 4 && a[1], "Requires fragment entry");
            entry = a[1]; ++entries;
        } else if (i.opcode == 14) {
            Require(Equal(a, {5348, 1}), "Requires native GLSL450 memory model");
            ++models;
        } else if (i.opcode == 17 && Equal(a, {5378})) capability = true;
        else if (i.opcode == 71) {
            Require(a.size() >= 2, "Truncated FSI decoration");
            decorations[a[0]].emplace_back(a.begin() + 1, a.end());
        } else if (i.opcode == 59) {
            Require(a.size() >= 3, "Truncated FSI variable");
            variables[a[1]] = a[2];
        } else if (i.opcode == 21 && a.size() == 3 && a[1] == 32 && a[2] == 0) {
            integer_types.insert(a[0]);
        } else if (i.opcode == 43 && a.size() == 3) constants[a[1]] = {a[0], a[2]};
        if (i.opcode == 225) barriers.push_back(n);
        if (i.opcode == 54) ++functions;
    }
    Require(entries == 1 && models == 1 && functions == 1 && capability,
            "Requires one native FSI entry function and pixel interlock capability");
    for (const auto& i : instructions) {
        if(i.opcode==12)
            Require(i.args.size()>=4&&glsl_imports.contains(i.args[2])&&
                    AuditedGlslInstruction(i.args[3]), "Unaudited FSI extended instruction");
        if (i.opcode == 54) Require(i.args[1] == entry, "Unexpected FSI function");
        if (i.opcode == 16 && i.args.size() >= 2 &&
            i.args[1] >= 5366 && i.args[1] <= 5371) {
            Require(Equal(i.args, {entry, 5366}), "Requires ordered pixel interlock");
            ++modes;
        }
    }
    Require(modes == 1, "Missing exact FSI execution mode");
    std::optional<uint32_t> edram;
    for (const auto& [id, unused] : decorations) {
        if (Decorated(decorations, id, {34, 4}) && Decorated(decorations, id, {33, 0})) {
            Require(!edram, "Ambiguous EDRAM descriptor");
            edram = id;
        }
    }
    Require(edram && variables.contains(*edram) && variables.at(*edram) == 12 &&
            Decorated(decorations, *edram, {23}),
            "Requires Coherent EDRAM StorageBuffer at set4 binding0");
    Require(barriers.size() == 2 && barriers[0] > 0 &&
            barriers[1] + 1 < instructions.size(), "Requires exactly two epilogue barriers");
    const size_t begin = barriers[0] - 1, end = barriers[1] + 1;
    Require(begin < end && instructions[begin].opcode == 5364 &&
            instructions[end].opcode == 5365 && instructions[begin].count == 1 &&
            instructions[end].count == 1 && instructions[begin].function == entry &&
            instructions[end].function == entry, "Requires barrier-adjacent FSI pair");
    for (size_t n = begin + 1; n < end; ++n)
        Require(instructions[n].opcode != 5364 && instructions[n].opcode != 5365,
                "Nested raster interlock requires separate audit");
    for (const auto n : barriers) {
        const auto& i = instructions[n];
        Require(i.count == 3 && constants.contains(i.args[0]) && constants.contains(i.args[1]),
                "Invalid FSI memory barrier operands");
        const auto scope = constants.at(i.args[0]), semantics = constants.at(i.args[1]);
        Require(integer_types.contains(scope.type) && scope.value == 1 &&
                integer_types.contains(semantics.type) && semantics.value == 0x848,
                "Expected Device AcquireRelease UniformMemory ImageMemory barrier");
    }

    // Prove the epilogue barriers have no other external writes to order.
    // Physical-address loads of immutable constants remain unchanged.
    std::unordered_map<uint32_t, PointerType> pointer_types;
    std::unordered_map<uint32_t, size_t> structures;
    std::set<std::array<uint32_t, 3>> nonwritable_members;
    for (const auto& i : instructions) {
        const auto a = i.args;
        if (i.opcode == 32 && a.size() == 3) pointer_types[a[0]] = {a[1], a[2]};
        else if (i.opcode == 30 && !a.empty()) structures[a[0]] = a.size() - 1;
        else if (i.opcode == 72 && a.size() == 3 && a[2] == 24)
            nonwritable_members.insert({a[0], a[1], a[2]});
    }
    std::unordered_map<uint32_t, uint32_t> pointer_values, roots;
    for (const auto& i : instructions) {
        const auto a = i.args;
        const bool pointer_result = i.opcode == 55 || i.opcode == 59 || i.opcode == 61 ||
            i.opcode == 65 || i.opcode == 66 || i.opcode == 67 || i.opcode == 70 ||
            i.opcode == 83 || i.opcode == 120 || i.opcode == 124 ||
            i.opcode == 169 || i.opcode == 245;
        if (pointer_result && a.size() >= 2 && pointer_types.contains(a[0]))
            pointer_values[a[1]] = a[0];
        if (i.opcode == 59) {
            roots[a[1]] = a[1];
            if (a[2] == 12 && a[1] != *edram) {
                Require(pointer_types.contains(a[0]), "Invalid StorageBuffer pointer type");
                const auto block = pointer_types.at(a[0]).pointee;
                bool readonly = Decorated(decorations, a[1], {24});
                if (!readonly && structures.contains(block)) {
                    readonly = true;
                    for (size_t member = 0; member < structures.at(block); ++member)
                        readonly &= nonwritable_members.contains({block, uint32_t(member), 24});
                }
                Require(readonly, "Additional writable StorageBuffer requires separate audit");
            }
        } else if ((i.opcode == 65 || i.opcode == 66 || i.opcode == 67 ||
                    i.opcode == 70 || i.opcode == 83) && a.size() >= 3) {
            roots[a[1]] = roots.contains(a[2]) ? roots.at(a[2]) : 0;
        }
    }
    for (size_t n = 0; n < instructions.size(); ++n) {
        const auto& i = instructions[n];
        const auto a = i.args;
        Require(i.opcode != 63 && i.opcode != 64 && i.opcode != 99 &&
                !(i.opcode >= 227 && i.opcode <= 242),
                "Memory copy, image write or atomic requires separate audit");
        if (i.opcode == 61 && a.size() >= 3 && roots.contains(a[2]) && roots.at(a[2]) == *edram)
            Require(begin < n && n < end, "EDRAM read outside raster epilogue");
        if (i.opcode != 62) continue;
        Require(a.size() >= 2 && pointer_values.contains(a[0]), "Unclassified shader store");
        const auto storage = pointer_types.at(pointer_values.at(a[0])).storage;
        if (storage == 3 || storage == 7) continue; // Shader output or local data.
        Require(storage == 12 && roots.contains(a[0]) && roots.at(a[0]) == *edram &&
                begin < n && n < end, "External write outside coherent EDRAM epilogue");
    }

    std::vector<uint32_t> output;
    output.reserve(words.size() - 6);
    output.insert(output.end(), words.begin(), words.begin() + 5);
    for (size_t n = 0; n < instructions.size(); ++n) {
        if (n == barriers[0] || n == barriers[1]) continue;
        const auto& i = instructions[n];
        output.insert(output.end(), words.begin() + i.offset, words.begin() + i.offset + i.count);
    }
    Require(output.size() + 6 == words.size(), "Unexpected FSI transform extent");
    return output;
}

} // namespace gow3::renderer
