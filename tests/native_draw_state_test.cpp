#include "renderer/native/draw_state.h"
#include <bit>
#include <iostream>
#include <stdexcept>
using namespace gow3::renderer;
int main() try {
    std::array<uint32_t,kDrawStateWords> words{};
    auto set=[&](uint32_t offset,uint32_t value) {words[(offset-kDrawStateOffset)/4]=value;};
    set(0x294c,0x43f);set(0x2944,1u<<19);
    set(0x2908,std::bit_cast<uint32_t>(8.f));set(0x2910,std::bit_cast<uint32_t>(-8.f));
    set(0x2918,std::bit_cast<uint32_t>(1.f));set(0x28bc,16|(16<<16));set(0x28c8,16|(16<<16));
    for(auto offset:{0x2938,0x2958,0x295c,0x2960})set(offset,0x10001);
    const std::array<float,4> constants{.25f,.5f,.75f,.125f};
    for(unsigned i=0;i<4;++i)set(0x28e0+i*4,std::bit_cast<uint32_t>(constants[i]));
    for(uint32_t target=0;target<4;++target)for(uint32_t term=0;term<4;++term) {
        constexpr uint32_t offsets[]{0x2938,0x2958,0x295c,0x2960};
        for(uint32_t factor=0;factor<32;++factor) {
            set(offsets[target],(0x10001&~(31u<<(term*8)))|(factor<<(term*8)));
            bool accepted=false;
            try {
                const auto state=DecodeDrawState(words,16,16,4,true);accepted=true;
                if(state.blend_constant!=constants)throw std::logic_error("Blend RGBA constants were not decoded");
            } catch(const std::runtime_error&) {}
            const bool valid=factor<=1||(factor>=4&&factor<=16);
            if(accepted!=valid)throw std::logic_error("Packed blend factor validity mismatch");
            if(factor>=12&&factor<=15) {
                bool rejected=false;
                try {(void)DecodeDrawState(words,16,16,4);}catch(const std::runtime_error&){rejected=true;}
                if(!rejected)throw std::logic_error("Unsupported host blend constants were accepted");
            }
        }
        set(offsets[target],0x10001);
    }
    // The draw path skips exactly the viewports DecodeDrawState rejects.
    const float nan=std::bit_cast<float>(0x7fc00000u);
    const struct {float xs,ys,zs,zo;bool representable;} viewports[]{
        {8,-8,1,0,true},{8,-8,-1,1,true},{8,-8,0,0,true},{8,-8,.5f,.5f,true},
        {0,-8,1,0,false},{-8,-8,1,0,false},{8,0,1,0,false},{8,8,1,0,false},
        {8,-8,1,.5f,false},{8,-8,1,-.1f,false},{8,-8,-1,.5f,false},{nan,-8,1,0,false},{8,-8,nan,0,false}};
    unsigned viewport_cases=0;
    for(const auto& v:viewports) {
        set(0x2908,std::bit_cast<uint32_t>(v.xs));set(0x2910,std::bit_cast<uint32_t>(v.ys));
        set(0x2918,std::bit_cast<uint32_t>(v.zs));set(0x291c,std::bit_cast<uint32_t>(v.zo));
        bool decoded=true;
        try {(void)DecodeDrawState(words,16,16,4,true);}catch(const std::runtime_error&){decoded=false;}
        if(DecodeDrawViewport(words).Representable()!=v.representable||decoded!=v.representable)
            throw std::logic_error("Viewport skip rule disagrees with the draw-state decode");
        ++viewport_cases;
    }
    std::cout<<"PASS: 512 packed blend factor cases, RGBA constant mapping, 64 host-path rejection cases and "
             <<viewport_cases<<" viewport cases\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
