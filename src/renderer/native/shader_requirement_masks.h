#pragma once
#include <array>
#include <bit>
#include <cstdint>
#include <set>
#include <stdexcept>
#include <utility>

namespace gow3::renderer {
// The fetch decoder can emit vertex slot 96 (31*3+3). Preserve that malformed
// value so the executor, not this allocation optimization, rejects it later.
class ShaderRequirementMasks {
public:
    ShaderRequirementMasks()=default;
    explicit ShaderRequirementMasks(const std::pair<std::set<unsigned>,std::set<unsigned>>& slots) {
        for(unsigned slot:slots.first) {
            if(slot>96)throw std::logic_error("Shader vertex requirement exceeds decoder range");
            vertices_[slot/64]|=uint64_t(1)<<(slot%64);
        }
        for(unsigned slot:slots.second) {
            if(slot>31)throw std::logic_error("Shader texture requirement exceeds decoder range");
            textures_|=uint32_t(1)<<slot;
        }
    }
    void Merge(const ShaderRequirementMasks& other) {
        vertices_[0]|=other.vertices_[0];vertices_[1]|=other.vertices_[1];textures_|=other.textures_;
    }
    unsigned vertex_count() const {return std::popcount(vertices_[0])+std::popcount(vertices_[1]);}
    bool HasVertex(unsigned slot) const {return slot<=96&&(vertices_[slot/64]&(uint64_t(1)<<(slot%64)));}
    bool HasTexture(unsigned slot) const {return slot<32&&(textures_&(uint32_t(1)<<slot));}
    template<class F>void ForEachVertex(F&& visit) const {
        for(unsigned word=0;word<vertices_.size();++word)
            for(auto remaining=vertices_[word];remaining;remaining&=remaining-1)
                visit(word*64+unsigned(std::countr_zero(remaining)));
    }
    template<class F>void ForEachTexture(F&& visit) const {
        for(auto remaining=textures_;remaining;remaining&=remaining-1)
            visit(unsigned(std::countr_zero(remaining)));
    }
private:
    std::array<uint64_t,2> vertices_{};
    uint32_t textures_=0;
};
}
