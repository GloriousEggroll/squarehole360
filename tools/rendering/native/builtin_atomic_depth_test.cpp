#include "renderer/native/builtin_atomic_depth.h"
#include "renderer/native/packed_depth_pass_groups.h"
#include "renderer/native/content_hash.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <random>
#include <stdexcept>
using namespace gow3::renderer;
namespace {
uint64_t checks=0;
void Check(bool ok,const char* message) {++checks;if(!ok)throw std::runtime_error(message);}
std::vector<uint32_t> Read(const char* path) {
    std::ifstream file(path,std::ios::binary|std::ios::ate);
    Check(file&&file.tellg()>=20&&file.tellg()%4==0,"Bad input module");
    std::vector<uint32_t> v(size_t(file.tellg())/4);file.seekg(0);
    Check(bool(file.read(reinterpret_cast<char*>(v.data()),v.size()*4)),"Truncated input");return v;
}
uint32_t OrderedStore(uint32_t old,uint32_t incoming) {
    return incoming>=(old>>8)?(incoming<<8)|(old&255):old;
}
// Independent scheduling oracle: every fragment may read an old value, then be
// delayed behind any other fragment before its unsigned atomic maximum occurs.
// Explore all90 interleavings of3 reads+3 writes, not only serial permutations.
void Interleavings(uint32_t initial,std::array<uint32_t,3> depths) {
    uint32_t expected=initial;
    for(auto d:depths)expected=OrderedStore(expected,d);
    std::function<void(uint32_t,std::array<uint8_t,3>,std::array<uint32_t,3>)> visit;
    visit=[&](uint32_t value,std::array<uint8_t,3> phases,std::array<uint32_t,3> loaded) {
        if(phases==std::array<uint8_t,3>{2,2,2}) {
            Check(value==expected,"Concurrent atomic maximum differs from ordered GE");
            Check((value&255)==(initial&255),"Concurrent update corrupted S8");return;
        }
        for(unsigned i=0;i<3;++i)if(phases[i]<2) {
            auto next=phases;auto snapshots=loaded;uint32_t updated=value;
            if(next[i]++==0)snapshots[i]=value;
            else updated=std::max(value,OrderedStore(snapshots[i],depths[i]));
            visit(updated,next,snapshots);
        }
    };
    visit(initial,{},{});
}
void CheckPackedDepthPassGroups(std::span<const uint32_t> original) {
    const auto bytes=[](std::span<const uint32_t> words) {
        return std::span(reinterpret_cast<const uint8_t*>(words.data()),words.size_bytes());
    };
    Check(NativePackedDepthPassPixelCertified(bytes(original)),"Canonical grouped pixel module refused");
    Check(!NativePackedDepthPassPixelCertified({}),"Empty grouped pixel module accepted");
    std::vector<uint32_t> changed(original.begin(),original.end());
    for(size_t i=0;i<changed.size();++i) {
        changed[i]^=1;
        Check(!NativePackedDepthPassPixelCertified(bytes(changed)),"Changed grouped pixel word accepted");
        changed[i]^=1;
    }
    changed.pop_back();Check(!NativePackedDepthPassPixelCertified(bytes(changed)),"Truncated grouped pixel module accepted");
    changed.assign(original.begin(),original.end());changed.push_back(0);
    Check(!NativePackedDepthPassPixelCertified(bytes(changed)),"Extended grouped pixel module accepted");

    NativeFsiConstants raster;raster.pitch=1280;raster.depth_base=0;raster.depth_format=1;raster.depth_control=0x700766;
    NativePackedDepthPassProof proof;proof.width=1280;proof.height=720;proof.surface_info=1280;
    proof.audited_vertex=proof.audited_pixel=proof.cpu_geometry=proof.texture_free=proof.unmodified_variants=true;
    proof.private_color_only_framebuffer=proof.bounded_scissor=true;
    Check(NativePackedDepthPassAllowed(raster,proof),"Exact primary packed writer proof refused");
    auto atomic_proof=proof;atomic_proof.unmodified_variants=false;atomic_proof.atomic_depth_variant=true;
    Check(NativePackedDepthPassAllowed(raster,atomic_proof),"Pinned atomic packed writer proof refused");
    atomic_proof.unmodified_variants=true;
    Check(!NativePackedDepthPassAllowed(raster,atomic_proof),"Mixed pixel access model admitted");
    atomic_proof.unmodified_variants=false;atomic_proof.active_query=true;
    Check(!NativePackedDepthPassAllowed(raster,atomic_proof),"Queried atomic writer admitted");
    atomic_proof.active_query=false;atomic_proof.audited_pixel=false;
    Check(!NativePackedDepthPassAllowed(raster,atomic_proof),"Unaudited atomic writer admitted");
    for(auto member:{&NativePackedDepthPassProof::audited_vertex,&NativePackedDepthPassProof::audited_pixel,
        &NativePackedDepthPassProof::cpu_geometry,&NativePackedDepthPassProof::texture_free,
        &NativePackedDepthPassProof::unmodified_variants,&NativePackedDepthPassProof::private_color_only_framebuffer,
        &NativePackedDepthPassProof::bounded_scissor}) {
        auto p=proof;p.*member=false;Check(!NativePackedDepthPassAllowed(raster,p),"Missing grouped writer authority accepted");
    }
    for(auto member:{&NativePackedDepthPassProof::pixel_shader,&NativePackedDepthPassProof::active_query,
        &NativePackedDepthPassProof::discarded,&NativePackedDepthPassProof::rectangle}) {
        auto p=proof;p.*member=true;Check(!NativePackedDepthPassAllowed(raster,p),"Excluded grouped writer path accepted");
    }
    for(uint32_t value:{0u,1u,1279u,1281u,UINT32_MAX}) {
        auto p=proof;p.width=value;Check(!NativePackedDepthPassAllowed(raster,p),"Nonprimary grouped width accepted");
    }
    for(uint32_t value:{0u,1u,719u,721u,UINT32_MAX}) {
        auto p=proof;p.height=value;Check(!NativePackedDepthPassAllowed(raster,p),"Nonprimary grouped height accepted");
    }
    for(uint32_t value:{0u,1u,1279u,1281u,UINT32_MAX}) {
        auto r=raster;r.pitch=value;Check(!NativePackedDepthPassAllowed(r,proof),"Nonprimary grouped pitch accepted");
    }
    for(uint32_t value:{1u,719u,2047u,2048u,UINT32_MAX}) {
        auto r=raster;r.depth_base=value;Check(!NativePackedDepthPassAllowed(r,proof),"Offset/wrapped grouped depth accepted");
    }
    for(uint32_t value:{0u,2u,UINT32_MAX}) {
        auto r=raster;r.depth_format=value;Check(!NativePackedDepthPassAllowed(r,proof),"Other grouped depth format accepted");
    }
    for(uint32_t value:{0x10000u,0x20000u,0x30000u}) {
        auto p=proof;p.surface_info|=value;Check(!NativePackedDepthPassAllowed(raster,p),"Grouped MSAA accepted");
    }
    for(unsigned bit=0;bit<32;++bit) {
        auto p=proof;p.raw_color_mask=1u<<bit;Check(!NativePackedDepthPassAllowed(raster,p),"Raw grouped color write accepted");
        auto r=raster;r.depth_control^=1u<<bit;Check(!NativePackedDepthPassAllowed(r,proof),"Other grouped depth/stencil control accepted");
    }
    for(unsigned slot=0;slot<4;++slot)for(unsigned bit=0;bit<4;++bit) {
        auto r=raster;r.color[slot].mask=1u<<bit;Check(!NativePackedDepthPassAllowed(r,proof),"Effective grouped color write accepted");
    }

    // Independently enumerate physical tile rows and swapped40-word banks;
    // compare every screen coordinate to that physical ordering. This proves
    // unique in-bounds words, including every shared4KiB page, not just corners.
    std::vector<uint8_t> seen(1280u*720u,0);std::array<uint32_t,900> pages{};
    uint32_t visited=0;
    for(uint32_t tile_y=0;tile_y<45;++tile_y)for(uint32_t tile_x=0;tile_x<16;++tile_x)
        for(uint32_t row=0;row<16;++row)for(uint32_t bank=0;bank<2;++bank)for(uint32_t lane=0;lane<40;++lane) {
            const uint32_t x=80*tile_x+40*bank+lane,y=16*tile_y+row;
            const uint32_t expected=(16*tile_y+tile_x)*1280+80*row+40*(1-bank)+lane;
            const uint32_t actual=NativePackedDepthPassWord(x,y);
            Check(actual==expected&&actual<seen.size(),"Grouped writer physical addressing differs or wraps");
            Check(!seen[actual],"Two grouped writer pixels alias one packed word");seen[actual]=1;
            const uint32_t page=(uint64_t(actual)*4)/4096;
            Check(page<pages.size(),"Grouped writer escaped physical version-page footprint");++pages[page];++visited;
        }
    Check(visited==921600&&std::all_of(seen.begin(),seen.end(),[](auto n){return n==1;}),
        "Grouped writer footprint omitted a primary depth word");
    Check(std::all_of(pages.begin(),pages.end(),[](auto n){return n==1024;}),
        "Grouped writer physical page footprint has a gap or duplicate");
    Check(NativePackedDepthPassWord(0,0)==40&&NativePackedDepthPassWord(40,0)==0&&
        NativePackedDepthPassWord(79,15)==1239&&NativePackedDepthPassWord(80,0)==1320&&
        NativePackedDepthPassWord(1279,719)==921559,"Grouped tile/bank endpoint oracle differs");
    std::cout<<"PACKED_DEPTH_PASS_GROUPS_CPU pixels="<<visited<<" words="<<seen.size()<<" pages="<<pages.size()
        <<" canonical_module_only=true full_state_refusals=true bijection=true\n";
}
}
int main(int argc,char** argv)try {
    if(argc!=5)throw std::runtime_error("Usage: builtin-atomic-depth-test builtin.spv vs-a.spv vs-b.spv output.spv");
    auto original=Read(argv[1]);auto output=TryBuiltinAtomicDepth(original);
    Check(bool(output),"Pinned original refused");Check((*output)[3]==432,"Wrong new result bound");
    for(size_t i:{0u,1u,2u,4u})Check((*output)[i]==original[i],"Non-bound header word changed");
    size_t next=5,removed=0,loads=0,stores=0,discards=0;
    for(size_t at=5;at<original.size();) {
        const auto count=original[at]>>16,op=original[at]&65535;const auto* a=original.data()+at+1;
        const bool extension=op==10&&std::memcmp(a,"SPV_EXT_fragment_shader_interlock",33)==0;
        const bool erase=extension||(op==17&&a[0]==5378)||(op==16&&a[1]==5366)||op==225||op==5364||op==5365;
        if(erase)++removed;
        else if(at==1027) {
            const std::array<uint32_t,6> expected{(6u<<16)|227u,13,185,184,31,19};
            Check(std::equal(expected.begin(),expected.end(),output->begin()+next),"Atomic load pointer/type/scope changed");next+=6;++loads;
        } else if(at==2387) {
            const std::array<uint32_t,7> expected{(7u<<16)|239u,13,431,184,31,19,426};
            Check(std::equal(expected.begin(),expected.end(),output->begin()+next),"Atomic max packed operand/scope changed");next+=7;++stores;
        } else {
            Check(next+count<=output->size()&&std::equal(original.begin()+at,original.begin()+at+count,output->begin()+next),
                "Changed retained arithmetic/address/branch/discard operand");next+=count;
        }
        if(op==252)++discards;
        auto mutated=original;mutated[at]^=1;Check(!TryBuiltinAtomicDepth(mutated),"Changed instruction accepted");at+=count;
    }
    Check(next==output->size()&&removed==7&&loads==1&&stores==1&&discards==1,"Unexpected transform set");
    Check(!TryBuiltinAtomicDepth(*output)&&!TryBuiltinAtomicDepth({}),"Non-original module accepted");
    for(uint32_t op:{62,99,227,259,318,319,5614,5615,6035,65534}) {
        auto v=original;v.push_back((1u<<16)|op);Check(!TryBuiltinAtomicDepth(v),"Unknown external write accepted");
    }
    for(unsigned i=0;i<2;++i) {
        auto vs=Read(argv[2+i]);const auto b=std::span(reinterpret_cast<const uint8_t*>(vs.data()),vs.size()*4);
        Check(AuditedAtomicDepthVertex(kAtomicDepthVertexSources[i],b),"Pinned depth VS refused");
        Check(!AuditedAtomicDepthVertex(kAtomicDepthVertexSources[1-i],b),"Other source/module pair accepted");
        vs.back()^=1;Check(!AuditedAtomicDepthVertex(kAtomicDepthVertexSources[i],b),"Modified depth VS accepted");
    }
    NativeFsiConstants r;r.pitch=1280;r.depth_base=0;r.depth_format=1;r.depth_control=0x700766;
    const auto allow=[&](const NativeFsiConstants& v,uint32_t w=1280,uint32_t h=720,uint32_t mask=0,
                         bool pixel=false,bool query=false,bool vertex=true,uint32_t info=1280) {
        return BuiltinAtomicDepthAllowed(v,w,h,mask,pixel,query,vertex,info);
    };
    Check(allow(r),"Observed geometry state refused");
    for(unsigned bit=0;bit<32;++bit) {auto v=r;v.depth_control^=1u<<bit;Check(!allow(v),"Changed control accepted");Check(!allow(r,1280,720,1u<<bit),"Raw color writer accepted");}
    for(unsigned slot=0;slot<4;++slot)for(unsigned bit=0;bit<4;++bit) {auto v=r;v.color[slot].mask=1u<<bit;Check(!allow(v),"Effective color writer accepted");}
    Check(!allow(r,1280,720,0,true)&&!allow(r,1280,720,0,false,true)&&!allow(r,1280,720,0,false,false,false),"Pixel/query/vertex proof missing");
    for(uint32_t info:{0x10000u,0x20000u,0x30000u})Check(!allow(r,1280,720,0,false,false,true,info),"MSAA admitted");
    for(uint32_t f:{0u,2u,UINT32_MAX}){auto v=r;v.depth_format=f;Check(!allow(v),"Unknown depth format accepted");}
    for(uint32_t x:{0u,8193u,UINT32_MAX})Check(!allow(r,x,720)&&!allow(r,1280,x),"Invalid extent accepted");
    for(uint32_t x:{0u,1199u,16384u,UINT32_MAX}){auto v=r;v.pitch=x;Check(!allow(v),"Noninjective pitch accepted");}
    auto wrapped=r;wrapped.depth_base=2047;Check(allow(wrapped),"Injective wrapped base refused");
    wrapped.depth_base=2048;Check(!allow(wrapped),"Bad base accepted");
    auto huge=r;huge.pitch=8192;Check(!allow(huge,8192,8192),"Aliased wrapped tile displacement accepted");
    for(unsigned stencil=0;stencil<256;++stencil)for(uint32_t depth:{0u,1u,0x7fffffu,0x800000u,0xdfffffu,0xe00000u,0xfffffeu,0xffffffu}) {
        const uint32_t initial=(depth<<8)|stencil;
        Interleavings(initial,{0xffffffu,depth,0u});
        Interleavings(initial,{depth?depth-1:0u,0x123456u,std::min(depth+1,0xffffffu)});
    }
    std::mt19937 rng(0x573146u);
    for(unsigned i=0;i<1000;++i)Interleavings(uint32_t(rng()),{uint32_t(rng())&0xffffffu,uint32_t(rng())&0xffffffu,uint32_t(rng())&0xffffffu});
    CheckPackedDepthPassGroups(original);
    std::ofstream file(argv[4],std::ios::binary);Check(bool(file.write(reinterpret_cast<const char*>(output->data()),output->size()*4)),"Output failed");
    std::cout<<"PASS: "<<checks<<" transform, identity, state and concurrent depth/S8 oracle checks; "<<original.size()<<" -> "<<output->size()<<" words\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
