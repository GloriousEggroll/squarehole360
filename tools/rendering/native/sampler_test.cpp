#include "renderer/native/texture_upload.h"
#include <iostream>
#include <stdexcept>
using namespace gow3::renderer;
using namespace plume;
namespace {
void Require(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
TextureLayout Layout(uint32_t mag,uint32_t min,uint32_t mip,uint32_t aniso) {
    TextureLayout layout{};layout.dimension=1;layout.layers=1;
    layout.width=layout.height=64;layout.min_mip=0;layout.max_mip=6;
    layout.fetch[3]=(mag<<19)|(min<<21)|(mip<<23)|(aniso<<25);
    return layout;
}
}
int main() try {
    const auto filtered=Layout(1,1,0,3);
    for(int override=-1;override<=5;++override) {
        const auto sampler=TextureSampler(filtered,override);
        const unsigned expected=override<0?4:override<2?1:1u<<(override-1);
        Require(sampler.maxAnisotropy==expected,"Filtering setting did not select the requested level");
        Require(sampler.anisotropyEnabled==(expected>1),"Unexpected host anisotropy enable");
        Require(sampler.mipmapMode==(override==0?RenderMipmapMode::NEAREST:RenderMipmapMode::LINEAR),
                "Anisotropic sampling must enable trilinear filtering");
    }
    for(const auto layout:{Layout(0,1,0,0),Layout(1,0,0,0),Layout(1,1,2,0)}) {
        const auto sampler=TextureSampler(layout,5);
        Require(!sampler.anisotropyEnabled,"Override changed ineligible point/base-only filtering");
    }
    auto single=filtered;single.max_mip=single.min_mip;
    Require(TextureSampler(single,5).maxAnisotropy==4,"Override changed a single-level texture");
    const auto capped=TextureSampler(filtered,5,12);
    Require(capped.maxAnisotropy==8,"Host device cap was not rounded to a supported guest level");
    const auto unavailable=TextureSampler(filtered,5,0);
    Require(!unavailable.anisotropyEnabled&&unavailable.maxAnisotropy==1,
            "Disabled device feature enabled anisotropic sampling");
    Require(unavailable.mipmapMode==RenderMipmapMode::NEAREST,"Disabled device feature changed mip filtering");
    auto stacked=Layout(1,1,1,0);stacked.layers=16;stacked.fetch[0]=0;
    Require(TextureSampler(stacked,5).addressW==RenderTextureAddressMode::CLAMP,
            "Stack layer clamp changed with graphics settings");
    for(int invalid:{-2,6}) {
        bool rejected=false;try {TextureSampler(filtered,invalid);}catch(const std::runtime_error&){rejected=true;}
        Require(rejected,"Invalid anisotropy setting was accepted");
    }
    std::cout<<"PASS: anisotropy settings, trilinear filtering, eligibility, device limits, stacked clamp\n";
} catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
