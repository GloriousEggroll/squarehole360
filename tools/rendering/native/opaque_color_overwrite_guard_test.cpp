#include "renderer/native/opaque_color_overwrite_compiler.h"
#include <iostream>
#include <stdexcept>

using namespace gow3::renderer;
int main() try {
    size_t checks=0;
    const auto check=[&](bool condition){++checks;if(!condition)throw std::runtime_error("Opaque live-state gate mismatch");};
    NativeFsiConstants raster;raster.pitch=1280;raster.depth_base=0;
    raster.depth_format=1;raster.depth_control=0x700766;raster.color[0]={720,3,15,0x10001};
    const std::array<uint32_t,6> key{3,1,1,4,0,0};
    const auto allowed=[&](const NativeFsiConstants& state){return OpaqueColorOverwriteAllowed(state,1280,720,1280,15,
        false,kOpaqueColorOverwriteVertex,kOpaqueColorOverwritePixel,key);};
    check(allowed(raster));
    auto variant=raster;
    for(unsigned mask=0;mask<15;++mask) {variant=raster;variant.color[0].mask=mask;check(!allowed(variant));}
    for(unsigned format:{0u,2u,4u,5u,6u,7u,14u,15u,UINT32_MAX}) {
        variant=raster;variant.color[0].format=format;check(!allowed(variant));
    }
    for(unsigned bit=0;bit<32;++bit) {variant=raster;variant.blend_enabled=1u<<bit;check(!allowed(variant));}
    for(unsigned bit=0;bit<32;++bit) {variant=raster;variant.depth_control^=1u<<bit;check(!allowed(variant));}
    variant=raster;variant.depth_format=0;check(!allowed(variant));
    variant=raster;variant.pitch=1279;check(!allowed(variant));
    variant=raster;variant.pitch=16384;check(!allowed(variant));
    variant=raster;variant.color[0].base=2048;check(!allowed(variant));
    variant=raster;variant.depth_base=2048;check(!allowed(variant));
    for(unsigned surface:{0x10000u,0x20000u,0x30000u})check(!OpaqueColorOverwriteAllowed(raster,1280,720,surface,15,
        false,kOpaqueColorOverwriteVertex,kOpaqueColorOverwritePixel,key));
    for(unsigned raw_mask:{0u,1u,3u,7u,14u,16u,0xffffffffu})check(!OpaqueColorOverwriteAllowed(raster,1280,720,1280,raw_mask,
        false,kOpaqueColorOverwriteVertex,kOpaqueColorOverwritePixel,key));
    check(!OpaqueColorOverwriteAllowed(raster,1280,720,1280,15,true,kOpaqueColorOverwriteVertex,kOpaqueColorOverwritePixel,key));
    check(!OpaqueColorOverwriteAllowed(raster,1280,720,1280,15,false,"unknown",kOpaqueColorOverwritePixel,key));
    check(!OpaqueColorOverwriteAllowed(raster,1280,720,1280,15,false,kOpaqueColorOverwriteVertex,"unknown",key));
    for(unsigned word=0;word<6;++word) {
        auto changed=key;changed[word]=word==1?3:changed[word]+1;
        check(!OpaqueColorOverwriteAllowed(raster,1280,720,1280,15,false,
            kOpaqueColorOverwriteVertex,kOpaqueColorOverwritePixel,changed));
    }
    const std::array<uint32_t,6> other_key{3,2,1,4,0,0};
    check(OpaqueColorOverwriteAllowed(raster,1280,720,1280,15,false,
        kOpaqueColorOverwriteVertex,kOpaqueColorOverwritePixel,other_key));
    for(unsigned width:{0u,1281u,8193u})check(!OpaqueColorOverwriteAllowed(raster,width,720,1280,15,false,
        kOpaqueColorOverwriteVertex,kOpaqueColorOverwritePixel,key));
    for(unsigned height:{0u,8193u})check(!OpaqueColorOverwriteAllowed(raster,1280,height,1280,15,false,
        kOpaqueColorOverwriteVertex,kOpaqueColorOverwritePixel,key));
    variant=raster;variant.pitch=16383;
    check(!OpaqueColorOverwriteAllowed(variant,1280,720,16383,15,false,
        kOpaqueColorOverwriteVertex,kOpaqueColorOverwritePixel,key));
    variant=raster;variant.color[0].base=2047;variant.depth_base=2047;check(allowed(variant));
    std::cout<<"PASS: "<<checks<<" closed opaque live-state selection checks\n";
} catch(const std::exception& exception) {std::cerr<<exception.what()<<'\n';return 1;}
