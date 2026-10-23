#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

namespace gow3::renderer {
// Conservative declaration reflection for valid Native vertex/fragment SPIR-V.
// Includes unused globals and every entry point. Unknown syntax retains every
// binding; this is not a replacement for the shader compiler's validation.
inline uint32_t NativeDeclaredDescriptorSets(std::span<const uint8_t> bytes) noexcept {
    constexpr uint32_t unknown = UINT32_MAX;
    if (bytes.size() < 20 || bytes.size() % 4) return unknown;
    auto word = [&](size_t index) {
        uint32_t value;
        std::memcpy(&value, bytes.data() + index * 4, 4);
        return value;
    };
    if (word(0) != 0x07230203 || word(1) < 0x00010000 ||
        word(1) > 0x00010600 || !word(3) || word(4)) return unknown;
    struct Decoration { uint32_t id=0, set=0, binding=0; bool has_set=false, has_binding=false; };
    std::array<Decoration, 128> decorations{};
    std::array<uint32_t, 128> variables{};
    size_t decoration_count=0, variable_count=0;
    bool entry=false, memory_model=false;
    auto text = [&](size_t first, size_t words) -> std::string_view {
        const auto* begin = reinterpret_cast<const char*>(bytes.data() + first * 4);
        const auto* end = static_cast<const char*>(std::memchr(begin, 0, words * 4));
        return end ? std::string_view(begin, size_t(end - begin)) : std::string_view{};
    };
    for (size_t at=5; at < bytes.size()/4;) {
        const uint32_t count=word(at)>>16, op=word(at)&65535;
        if (!count || count > bytes.size()/4-at) return unknown;
        if (op==10) { // Extensions with no alternative descriptor declarations.
            if (count<2) return unknown;
            const auto name=text(at+1,count-1);
            if (name!="SPV_KHR_physical_storage_buffer" &&
                name!="SPV_EXT_descriptor_indexing" &&
                name!="SPV_EXT_fragment_shader_interlock" &&
                name!="SPV_EXT_demote_to_helper_invocation" &&
                name!="SPV_KHR_non_semantic_info" &&
                name!="SPV_KHR_storage_buffer_storage_class") return unknown;
        } else if (op==11) { // Only the standard arithmetic instruction set.
            if (count<3 || text(at+2,count-2)!="GLSL.std.450") return unknown;
        } else if (op==14) {
            if (count!=3 || (word(at+1)!=0 && word(at+1)!=5348) ||
                (word(at+2)!=1 && word(at+2)!=3)) return unknown;
            memory_model=true;
        } else if (op==15) {
            if (count<4 || (word(at+1)!=0 && word(at+1)!=4)) return unknown;
            entry=true;
        } else if (op==59) {
            if (count<4 || count>5) return unknown;
            const uint32_t storage=word(at+3), id=word(at+2);
            if (!id || id>=word(3)) return unknown;
            if (storage==0 || storage==2 || storage==12) {
                if (variable_count==variables.size()) return unknown;
                variables[variable_count++]=id;
            } else if (storage!=1 && storage!=3 && storage!=4 && storage!=6 &&
                       storage!=7 && storage!=9) return unknown;
        } else if (op==71) {
            if (count<3) return unknown;
            const uint32_t decoration=word(at+2);
            if (decoration==33 || decoration==34) {
                if (count!=4 || !word(at+1) || word(at+1)>=word(3)) return unknown;
                const uint32_t id=word(at+1), value=word(at+3);
                size_t i=0;
                while (i<decoration_count && decorations[i].id!=id) ++i;
                if (i==decoration_count) {
                    if (decoration_count==decorations.size()) return unknown;
                    decorations[decoration_count++].id=id;
                }
                auto& d=decorations[i];
                if (decoration==34) {
                    if (value>=32 || (d.has_set && d.set!=value)) return unknown;
                    d.set=value; d.has_set=true;
                } else {
                    if (d.has_binding && d.binding!=value) return unknown;
                    d.binding=value; d.has_binding=true;
                }
            }
        } else if (op==73 || op==74 || op==75 || op==332 || op==5632 || op==5633) {
            return unknown; // Grouped, ID-based or string decorations.
        }
        at+=count;
    }
    if (!entry || !memory_model) return unknown;
    uint32_t sets=0;
    for (size_t i=0; i<variable_count; ++i) {
        size_t j=0;
        while (j<decoration_count && decorations[j].id!=variables[i]) ++j;
        if (j==decoration_count || !decorations[j].has_set || !decorations[j].has_binding)
            return unknown;
        sets |= 1u << decorations[j].set;
    }
    return sets;
}
} // namespace gow3::renderer
