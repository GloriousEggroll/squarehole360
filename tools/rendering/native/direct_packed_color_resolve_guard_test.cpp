#include "renderer/native/direct_packed_color_resolve.h"
#include <iostream>
#include <stdexcept>
using namespace gow3::renderer;
int main() {
    uint64_t checks=0;const auto check=[&](bool x){++checks;if(!x)throw std::runtime_error("Direct color guard failed");};
    NativeDirectPackedColorResolveInput good;
    good.native_fsi=true;good.source_width=1280;good.source_height=720;good.source_pitch=1280;
    good.source_base=720;good.source_format=2;good.source_surface_info=1280;
    good.destination_width=1280;good.destination_height=720;good.destination_format=54;
    good.destination_dimension=1;good.destination_layers=1;good.destination_endian=2;
    good.destination_level_count=1;good.destination_block_width=good.destination_block_height=1;
    good.destination_block_bytes=4;good.destination_pitch=1280;good.destination_tiled=true;
    good.source_rectangle=good.destination_rectangle={0,0,1280,720};check(NativeDirectPackedColorResolveAllowed(good));
    auto partial=good;partial.source_rectangle=partial.destination_rectangle={512,320,608,384};
    check(NativeDirectPackedColorResolveAllowed(partial));
    for(auto field:{&NativeDirectPackedColorResolveInput::source_width,&NativeDirectPackedColorResolveInput::source_height,
        &NativeDirectPackedColorResolveInput::source_pitch,&NativeDirectPackedColorResolveInput::source_base,
        &NativeDirectPackedColorResolveInput::source_format,&NativeDirectPackedColorResolveInput::destination_width,
        &NativeDirectPackedColorResolveInput::destination_height,&NativeDirectPackedColorResolveInput::destination_format,
        &NativeDirectPackedColorResolveInput::destination_dimension,&NativeDirectPackedColorResolveInput::destination_layers,
        &NativeDirectPackedColorResolveInput::destination_endian,&NativeDirectPackedColorResolveInput::destination_level_count,
        &NativeDirectPackedColorResolveInput::destination_min_mip,&NativeDirectPackedColorResolveInput::destination_max_mip,
        &NativeDirectPackedColorResolveInput::destination_block_width,&NativeDirectPackedColorResolveInput::destination_block_height,
        &NativeDirectPackedColorResolveInput::destination_block_bytes,&NativeDirectPackedColorResolveInput::destination_pitch,
        &NativeDirectPackedColorResolveInput::flags,&NativeDirectPackedColorResolveInput::level,
        &NativeDirectPackedColorResolveInput::source_slot}) {
        auto a=good;++(a.*field);check(!NativeDirectPackedColorResolveAllowed(a));
    }
    for(uint32_t sample:{0x10000u,0x20000u,0x30000u}) {
        auto a=good;a.source_surface_info|=sample;check(!NativeDirectPackedColorResolveAllowed(a));
    }
    for(auto field:{&NativeDirectPackedColorResolveInput::native_fsi,&NativeDirectPackedColorResolveInput::source_depth,
        &NativeDirectPackedColorResolveInput::destination_tiled,&NativeDirectPackedColorResolveInput::destination_swap}) {
        auto a=good;a.*field=!(a.*field);check(!NativeDirectPackedColorResolveAllowed(a));
    }
    auto exponent=good;exponent.exponent_bias=1;check(!NativeDirectPackedColorResolveAllowed(exponent));
    auto shifted=partial;++shifted.source_rectangle[0];check(!NativeDirectPackedColorResolveAllowed(shifted));
    auto unfamiliar=good;unfamiliar.source_rectangle=unfamiliar.destination_rectangle={0,0,96,64};
    check(!NativeDirectPackedColorResolveAllowed(unfamiliar));
    auto normalized=good;normalized.destination_format=7;check(!NativeDirectPackedColorResolveAllowed(normalized));
    using S=NativeDirectPackedColorResolveSource;
    check(NativeDirectPackedColorResolveChooseSource(true,false,false,0,7,false)==S::Packed);
    check(NativeDirectPackedColorResolveChooseSource(true,true,false,6,7,false)==S::Packed);
    check(NativeDirectPackedColorResolveChooseSource(true,true,false,7,7,false)==S::CurrentHost);
    check(NativeDirectPackedColorResolveChooseSource(true,true,false,8,7,false)==S::Refuse);
    check(NativeDirectPackedColorResolveChooseSource(false,false,false,0,7,false)==S::Refuse);
    check(NativeDirectPackedColorResolveChooseSource(true,false,false,0,0,false)==S::Refuse);
    check(NativeDirectPackedColorResolveChooseSource(true,true,true,6,7,false)==S::Refuse);
    check(NativeDirectPackedColorResolveChooseSource(true,true,false,6,7,true)==S::Refuse);
    std::cout<<"PASS "<<checks<<" direct packed color guard checks; no GPU, Scene, identity-copy or FPS proof\n";
}
