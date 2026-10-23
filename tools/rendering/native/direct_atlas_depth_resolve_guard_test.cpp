#include "renderer/native/direct_atlas_depth_resolve.h"
#include <algorithm>
#include <bit>
#include <cfenv>
#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
using namespace gow3::renderer;
namespace {
uint64_t checks=0;
void Require(bool value,const char* message) {++checks;if(!value)throw std::runtime_error(message);}
NativeDirectAtlasDepthResolveInput Input() {
    NativeDirectAtlasDepthResolveInput x;
    x.native_fsi=x.source_depth=x.destination_tiled=true;
    x.source_width=x.source_pitch=x.destination_width=x.destination_pitch=1120;
    x.source_height=x.destination_height=688;x.source_base=1440;x.source_surface_info=293602400;
    x.destination_format=22;x.destination_dimension=x.destination_layers=x.destination_level_count=1;
    x.destination_endian=2;x.destination_block_width=x.destination_block_height=1;x.destination_block_bytes=4;
    x.flags=x.source_slot=4;x.source_rectangle=x.destination_rectangle={0,0,544,352};return x;
}
float Decode(uint32_t depth) {return float(depth+(depth>>23))/16777216.f;}
uint32_t Encode(float depth) {return uint32_t(std::nearbyint(std::clamp(depth,0.f,1.f)*16777215.f));}
uint32_t ResolveThenStore(float depth,uint8_t stencil) {
    // Independent statement of the two ORIGINAL shader steps; direct source
    // admission must not replace them by a raw packed copy or one quantizer.
    return (Encode(Decode(Encode(depth)))<<8)|stencil;
}
}
int main()try {
    Require(std::fegetround()==FE_TONEAREST,"Fixture requires ties-to-even floating point mode");
    auto input=Input();Require(NativeDirectAtlasDepthResolveAllowed(input),"Actual common atlas resolve refused");
    for(const auto r:{std::array<int32_t,4>{0,0,96,96},{160,0,320,96},{320,0,480,64},
                     {0,0,544,288},{0,0,1120,688},{1119,687,1120,688}}) {
        auto x=input;x.source_rectangle=x.destination_rectangle=r;
        Require(NativeDirectAtlasDepthResolveAllowed(x),"Full initialized source does not support an actual/edge rectangle");
    }
    auto offset=input;offset.source_rectangle={41,17,179,91};offset.destination_rectangle={160,200,298,274};
    Require(NativeDirectAtlasDepthResolveAllowed(offset),"Absolute-coordinate source/destination offset refused");
    const auto refuse=[&](auto change){auto x=input;change(x);
        Require(!NativeDirectAtlasDepthResolveAllowed(x),"Unsupported resolve admitted");};
    refuse([](auto& x){x.native_fsi=false;});refuse([](auto& x){x.query_active=true;});
    refuse([](auto& x){x.source_depth=false;});refuse([](auto& x){x.source_width=1119;});
    refuse([](auto& x){x.source_height=689;});refuse([](auto& x){x.source_pitch=1152;});
    refuse([](auto& x){x.source_base=0;});refuse([](auto& x){x.source_format=1;});
    refuse([](auto& x){x.source_surface_info^=1;});refuse([](auto& x){x.source_surface_info|=0x10000;});
    refuse([](auto& x){x.destination_width=1024;});refuse([](auto& x){x.destination_height=720;});
    refuse([](auto& x){x.destination_format=23;});refuse([](auto& x){x.destination_dimension=2;});
    refuse([](auto& x){x.destination_layers=2;});refuse([](auto& x){x.destination_endian=0;});
    refuse([](auto& x){x.destination_tiled=false;});refuse([](auto& x){x.destination_level_count=2;});
    refuse([](auto& x){x.destination_min_mip=1;});refuse([](auto& x){x.destination_max_mip=1;});
    refuse([](auto& x){x.destination_block_width=4;});refuse([](auto& x){x.destination_block_height=4;});
    refuse([](auto& x){x.destination_block_bytes=8;});refuse([](auto& x){x.destination_pitch=1152;});
    refuse([](auto& x){x.flags=0;});refuse([](auto& x){x.level=1;});refuse([](auto& x){x.source_slot=0;});
    refuse([](auto& x){x.exponent_bias=1;});refuse([](auto& x){x.destination_swap=true;});
    for(unsigned side=0;side<2;++side)for(const auto r:{std::array<int32_t,4>{-1,0,543,352},
            {0,-1,544,351},{0,0,0,352},{0,0,544,0},{0,0,1121,352},{0,0,544,689},
            {INT32_MIN,0,INT32_MAX,352},{0,INT32_MIN,544,INT32_MAX}}) {
        auto x=input;(side?x.destination_rectangle:x.source_rectangle)=r;
        Require(!NativeDirectAtlasDepthResolveAllowed(x),"Invalid/overflowing rectangle admitted");
    }
    auto mismatch=input;mismatch.destination_rectangle[2]--;
    Require(!NativeDirectAtlasDepthResolveAllowed(mismatch),"Unequal source/destination dimensions admitted");
    using Kind=NativeDirectAtlasDepthResolveSource;
    // Independently enumerate every defined/host/dirty/current/receipt/current
    // combination. A pre-encode receipt precedes current-host refusal; dirty
    // provenance always refuses, and absent/stale clean source uses packed.
    for(unsigned bits=0;bits<64;++bits) {
        const bool defined=bits&1,host=bits&2,dirty=bits&4,current=bits&8,receipt=bits&16,receipt_current=bits&32;
        const auto actual=NativeDirectAtlasDepthResolveChooseSource(defined,host,dirty,current?77:76,77,receipt,receipt_current?77:76);
        Kind expected=Kind::Packed;
        if(!defined||dirty)expected=Kind::Refuse;
        else if(receipt&&receipt_current)expected=Kind::Receipt;
        else if(host&&current)expected=Kind::CurrentHost;
        Require(actual==expected,"Closed provenance truth table mismatch");
    }
    Require(NativeDirectAtlasDepthResolveChooseSource(true,false,false,0,0,false,0)==Kind::Refuse,"Zero packed token admitted");
    Require(NativeDirectAtlasDepthResolveChooseSource(true,true,false,77,77,true,0)==Kind::CurrentHost,"Zero receipt token substituted for host");
    Require(NativeDirectAtlasDepthResolveChooseSource(true,true,false,77,78,true,77)==Kind::Packed,"Old receipt survived packed write");
    Require(NativeDirectAtlasDepthResolveChooseSource(true,true,false,77,78,true,78)==Kind::Receipt,"Newest pre-encode receipt lost");
    // This positive bit witness forbids replacing current/pre-encode host
    // values with decoding already normalized bytes. It is not a GPU proof.
    const float original=.5f,redecoded=Decode(Encode(original));
    Require(std::bit_cast<uint32_t>(original)!=std::bit_cast<uint32_t>(redecoded),"Pre-encode witness vacuous");
    Require(NativeDirectAtlasDepthResolveChooseSource(true,true,false,77,77,false,0)==Kind::CurrentHost,"Arbitrary current host incorrectly used packed input");
    const uint32_t depths[]{0,1,0x3fffff,0x400000,0x400001,0x7fffff,0x800000,0xfffffe,0xffffff};
    for(auto depth:depths) {
        const float canonical=Decode(depth);
        const auto bits=std::bit_cast<uint32_t>(canonical);
        Require(std::bit_cast<uint32_t>(std::bit_cast<float>(bits))==bits,"Same-token canonical host-copy model changed bits");
        for(unsigned stencil=0;stencil<256;++stencil) {
            const float alpha=float(stencil)/255.f;
            const auto restored_stencil=uint8_t(std::nearbyint(std::clamp(alpha,0.f,1.f)*255.f));
            Require(restored_stencil==stencil,"Canonical alpha does not restore original stencil");
            const auto baseline=ResolveThenStore(std::bit_cast<float>(bits),restored_stencil);
            const auto direct=ResolveThenStore(canonical,uint8_t(stencil));
            Require(baseline==direct,"Unchanged resolve/store arithmetic model differs");
        }
    }
    // New-packet ownership model for an existing deferred receipt. Removing
    // its cache owner cannot destroy it before this packet's owner releases.
    auto receipt=std::make_shared<int>(1);std::weak_ptr<int> weak=receipt;
    {auto packet=receipt;receipt.reset();Require(!weak.expired(),"New packet did not retain receipt model");}
    Require(weak.expired(),"Receipt model remained owned after packet release");
    std::cout<<"{\"passed\":true,\"checks\":"<<checks<<",\"gpu_execution\":false,\"scope\":\"production guard and independent CPU provenance/float/stencil/owner model; real GPU parity and fences remain required\"}\n";
    return 0;
} catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
