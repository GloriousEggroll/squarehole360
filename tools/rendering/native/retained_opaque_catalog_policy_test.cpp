#include "renderer/native/retained_opaque_catalog.h"
#include "renderer/native/retained_opaque_authority.h"
#include "renderer/native/retained_opaque_catalog_headroom.h"
#include "renderer/native/cache_bookkeeping.h"
#include <map>
#include <limits>
#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
using namespace gow3::renderer;
namespace {
uint64_t checks=0;
void Require(bool value){++checks;if(!value)throw std::runtime_error("Retained catalogue policy oracle mismatch");}
uint32_t Next(uint32_t& seed){return seed=seed*1664525u+1013904223u;}
void ClosedAdmission() {
    Require(kRetainedOpaqueCatalogPairs.size()==3&&kRetainedOpaqueCatalogMaxPrograms==3&&kRetainedOpaqueCatalogMaxPipelines==6);
    for(const auto& accepted:kRetainedOpaqueCatalogPairs) {
        Require(RetainedOpaqueCatalogPair(accepted.vertex,accepted.pixel)==&accepted);
        for(const auto& other:kRetainedOpaqueCatalogPairs)
            Require(bool(RetainedOpaqueCatalogPair(other.vertex,accepted.pixel))==(other.vertex==accepted.vertex));
        for(size_t i=0;i<64;++i) {
            std::string pixel(accepted.pixel),vertex(accepted.vertex);
            pixel[i]=pixel[i]=='0'?'1':'0';vertex[i]=vertex[i]=='0'?'1':'0';
            Require(!RetainedOpaqueCatalogPair(accepted.vertex,pixel));Require(!RetainedOpaqueCatalogPair(vertex,accepted.pixel));
        }
    }
    Require(!RetainedOpaqueCatalogPair({},{}));
    constexpr std::array<uint32_t,6> cw{3,1,1,4,0,0},ccw{3,2,1,4,0,0};
    Require(RetainedOpaqueCatalogRaster(cw)&&RetainedOpaqueCatalogRaster(ccw));
    for(const auto& original:{cw,ccw})for(size_t word=0;word<6;++word)for(uint32_t bit=0;bit<32;++bit) {
        auto changed=original;changed[word]^=uint32_t(1)<<bit;
        const bool independently_allowed=changed==cw||changed==ccw;
        Require(RetainedOpaqueCatalogRaster(changed)==independently_allowed);
    }
}
void CacheHeadroom() {
    NativeRetainedOpaqueCatalogHeadroom credit;
    Require(!credit.AdmitModule(0)&&!credit.AdmitModule(UINT64_MAX));
    constexpr uint64_t hard_bytes=64u*1024*1024,hard_entries=1024,hard_pipelines=2048;
    struct Entry {uint64_t last_used_epoch,bytes;};std::map<unsigned,Entry> modules;
    PersistentCacheStatistics stats;constexpr uint64_t generic_bytes=50326612;
    for(unsigned i=0;i<745;++i){const auto bytes=generic_bytes/745+(i==744?generic_bytes%745:0);modules.emplace(i,Entry{0,bytes});stats.bytes+=bytes;}
    const auto original=modules;Require(stats.bytes==generic_bytes);
    // Actual observed V1 startup byte aggregate plus two bounded stand-in
    // module sizes. These sizes model metadata pressure, not compiled V2 ISA.
    const std::array<uint64_t,3> additional{81656,65536,98304};
    for(unsigned i=0;i<3;++i) {
        const auto soft=credit.SoftTargets(true,hard_entries,hard_bytes,hard_pipelines);
        Require(modules.size()<=soft.modules&&stats.bytes<=soft.bytes&&credit.CanAdmitModule(additional[i]));
        Require(modules.size()<hard_entries&&additional[i]<=hard_bytes-stats.bytes);
        modules.emplace(745+i,Entry{0,additional[i]});stats.bytes+=additional[i];Require(credit.AdmitModule(additional[i]));
    }
    Require(credit.modules()==3&&!credit.AdmitModule(1));
    for(unsigned i=0;i<6;++i)Require(credit.AdmitPipeline());Require(!credit.AdmitPipeline()&&credit.pipelines()==6);
    const auto soft=credit.SoftTargets(true,hard_entries,hard_bytes,hard_pipelines);
    const auto bytes=stats.bytes;TrimPersistentCache(modules,soft.modules,soft.bytes,stats,
        [](const auto& e){return e.bytes;},[](const auto&){return true;});
    Require(modules.size()==748&&stats.bytes==bytes&&stats.evictions==0);
    for(const auto& [key,value]:original)Require(modules.contains(key)&&modules.at(key).bytes==value.bytes);
    bool rejected=false;unsigned drains=0;
    try{ReservePersistentCache(modules,1,modules.size(),stats.bytes,stats,[](const auto& e){return e.bytes;},
        [](const auto&){return true;},[&]{++drains;});}catch(const std::runtime_error&){rejected=true;}
    Require(rejected&&!drains&&modules.size()==748&&stats.bytes==bytes&&!stats.evictions);
    for(uint64_t bound:std::array<uint64_t,8>{0,1,2,3,4,1024,UINT64_MAX-1,UINT64_MAX}) {
        const auto off=credit.SoftTargets(false,bound,bound,bound);
        const auto base=bound-bound/4-(bound%4?1:0);
        Require(off.modules==base&&off.bytes==base&&off.pipelines==base);
        const auto on=credit.SoftTargets(true,bound,bound,bound);
        Require(on.modules<=bound&&on.bytes<=bound&&on.pipelines<=bound&&on.modules>=base&&on.bytes>=base&&on.pipelines>=base);
    }
    NativeRetainedOpaqueCatalogHeadroom full;
    Require(full.AdmitModule(kRetainedOpaqueCatalogMaxModuleBytes));Require(!full.CanAdmitModule(1)&&!full.AdmitModule(1));
    Require(full.bytes()==kRetainedOpaqueCatalogMaxModuleBytes&&full.modules()==1);
}
size_t Word(bool depth,uint32_t x,uint32_t y) {
    uint32_t col=x%80;if(depth)col=(col+40)%80;
    return ((depth?0:720)+(y/16)*16+x/80)*1280+(y%16)*80+col;
}
// Exact integer depth codes independently simulate both paths. This proves
// source switches do not define authority; it does not execute material GLSL.
void SourceSwitchAuthority() {
    constexpr uint32_t w=1280,h=720;
    std::vector<uint32_t> serial(2048*1280),packed,color(w*h),depth(w*h);
    uint32_t random=0x45a319u;for(auto& word:serial)word=Next(random);packed=serial;
    NativeRetainedOpaqueAuthority authority;uint64_t cv=10,dv=20,generation=0,restores=0,stores=0,switches=0;
    size_t last=3;
    auto restore=[&] {
        authority.Admit(++generation,kNativeRetainedOpaqueLayout,cv,dv);++restores;
        for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x){const auto i=size_t(y)*w+x;color[i]=packed[Word(false,x,y)];depth[i]=packed[Word(true,x,y)]>>8;}
        last=3;
    };
    auto store=[&] {
        authority.BeforeMaterialize(cv,dv);auto ticket=authority.ticket();
        for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x){const auto i=size_t(y)*w+x;packed[Word(false,x,y)]=color[i];const auto at=Word(true,x,y);packed[at]=(depth[i]<<8)|(packed[at]&255);}
        Require(packed==serial);authority.Materialized(ticket,true);++stores;last=3;
    };
    // Admit actual three pair identities in interleaved order. Each mutation
    // advances exact page tokens and preserves one retained generation.
    restore();
    for(uint32_t draw=0;draw<96;++draw) {
        if(draw==47){store();Require(!authority.present());restore();}
        const size_t source=(draw*2+draw/3)%3;const auto& pair=kRetainedOpaqueCatalogPairs[source];
        Require(RetainedOpaqueCatalogPair(pair.vertex,pair.pixel));Require(authority.Reusable(kNativeRetainedOpaqueLayout,cv,dv));
        const auto ticket=authority.ticket();
        for(uint32_t sample=0;sample<512;++sample) {
            const uint32_t x=Next(random)%w,y=Next(random)%h;const auto i=size_t(y)*w+x;
            const auto da=Word(true,x,y),ca=Word(false,x,y);const uint32_t value=Next(random)^uint32_t(source*0x573194a1u);
            const auto incoming=sample%3==0?depth[i]:Next(random)&0xffffffu;
            if(incoming>=(serial[da]>>8)){serial[da]=(incoming<<8)|(serial[da]&255);serial[ca]=value;}
            if(incoming>=depth[i]){depth[i]=incoming;color[i]=value;}
        }
        authority.AcceptedDraw(ticket,cv,dv,cv+2,dv+2);cv+=2;dv+=2;
        if(last<3&&last!=source)++switches;last=source;
    }
    Require(switches>30&&restores==2&&stores==1&&authority.dirty());store();Require(stores==restores&&packed==serial);
    // A same-source or cross-source label never excuses a stale page token.
    restore();const auto ticket=authority.ticket();authority.AcceptedDraw(ticket,cv,dv,cv+1,dv+1);++cv;++dv;
    Require(!authority.Reusable(kNativeRetainedOpaqueLayout,cv+1,dv));bool rejected=false;
    try{authority.BeforeMaterialize(cv+1,dv);}catch(const std::logic_error&){rejected=true;}
    Require(rejected&&authority.failed()&&authority.dirty());
}
}
int main(){try{ClosedAdmission();CacheHeadroom();SourceSwitchAuthority();std::cout<<"PASS retained opaque catalogue policy: "<<checks<<" checks, three-source96-draw exact10MiB authority oracle\n";return 0;}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
