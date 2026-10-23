#include "renderer/native/shader_requirement_masks.h"
#include <iostream>
#include <vector>

using namespace gow3::renderer;
namespace {
using Slots=std::pair<std::set<unsigned>,std::set<unsigned>>;
void Check(const Slots& a,const Slots& b) {
    // Keep the former tree-based union as an independent ordering oracle.
    auto expected=a;expected.first.insert(b.first.begin(),b.first.end());
    expected.second.insert(b.second.begin(),b.second.end());
    ShaderRequirementMasks mask(a);mask.Merge(ShaderRequirementMasks(b));
    std::vector<unsigned> vertices,textures;
    mask.ForEachVertex([&](unsigned slot){vertices.push_back(slot);});
    mask.ForEachTexture([&](unsigned slot){textures.push_back(slot);});
    if(vertices!=std::vector<unsigned>(expected.first.begin(),expected.first.end())||
       textures!=std::vector<unsigned>(expected.second.begin(),expected.second.end())||
       mask.vertex_count()!=vertices.size())
        throw std::runtime_error("Requirement mask differs from independent sorted set union");
}
}
int main() try {
    size_t cases=0;
    Check({},{});++cases;
    for(unsigned vertex=0;vertex<=96;++vertex)for(unsigned texture=0;texture<32;++texture) {
        Check({{vertex},{texture}},{{96-vertex},{31-texture}});++cases;
    }
    uint32_t random=0x34568aef;
    auto next=[&]{random=random*1664525u+1013904223u;return random;};
    for(unsigned iteration=0;iteration<512;++iteration) {
        Slots a,b;
        for(unsigned slot=0;slot<=96;++slot) {
            if(next()&0x8000)a.first.insert(slot);
            if(next()&0x8000)b.first.insert(slot);
        }
        for(unsigned slot=0;slot<32;++slot) {
            if(next()&0x8000)a.second.insert(slot);
            if(next()&0x8000)b.second.insert(slot);
        }
        Check(a,b);++cases;Check(a,a);++cases;
    }
    Slots all;
    for(unsigned slot=0;slot<=96;++slot)all.first.insert(slot);
    for(unsigned slot=0;slot<32;++slot)all.second.insert(slot);
    Check(all,all);++cases;
    std::cout<<"PASS: "<<cases<<" independent set-union comparisons, empty stages, duplicate slots, "
        "ascending order, full masks and malformed vertex slot 96\n";
    return 0;
} catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
