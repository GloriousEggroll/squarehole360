#include "renderer/native/fsi_material_shader_cache_headroom.h"
#include "renderer/native/cache_bookkeeping.h"
#include <array>
#include <iostream>
#include <map>
#include <stdexcept>
using namespace gow3::renderer;
namespace {
uint64_t checks=0;
void Check(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
struct Entry {uint64_t last_used_epoch,bytes;};
struct Pressure {uint64_t drains,evictions,resident,bytes;};
Pressure Exercise(bool headroom) {
    // Controlled pressure trace, not a replay of the user's cache identities.
    // 48 MiB generic prewarm plus the observed 203/16,636,300-byte optional
    // module admissions. 32 subsequent generic modules force the old byte cap.
    std::map<unsigned,Entry> cache;PersistentCacheStatistics stats;
    const auto budget=NativeFsiMaterialShaderCacheMaximum(headroom);
    unsigned key=0;constexpr uint64_t optional_bytes=16636300,optional_count=203;
    for(unsigned i=0;i<optional_count;++i) {
        const auto bytes=optional_bytes/optional_count+(i<optional_bytes%optional_count);
        cache.emplace(key++,Entry{1,bytes});stats.bytes+=bytes;
    }
    for(unsigned i=0;i<768;++i){cache.emplace(key++,Entry{2,65536});stats.bytes+=65536;}
    uint64_t completed_fences=0;
    for(unsigned i=0;i<32;++i) {
        constexpr uint64_t incoming=81920;
        ReservePersistentCache(cache,incoming,budget.entries,budget.bytes,stats,
            [](const auto& e){return e.bytes;},[](const auto&){return false;},[&]{++completed_fences;});
        cache.emplace(key++,Entry{3,incoming});stats.bytes+=incoming;
        Check(cache.size()<=budget.entries&&stats.bytes<=budget.bytes,"Combined hard bound exceeded");
    }
    Check(completed_fences==stats.pressure_drains,"Pressure drain callback/counter conservation failed");
    return {stats.pressure_drains,stats.evictions,cache.size(),stats.bytes};
}
}
int main()try {
    constexpr uint64_t mib=1024u*1024;
    constexpr auto base=NativeFsiMaterialShaderCacheMaximum(false),extra=NativeFsiMaterialShaderCacheMaximum(true);
    static_assert(base.entries==1024&&base.bytes==64*mib);
    static_assert(extra.entries==1280&&extra.bytes==80*mib);
    for(bool headroom:{false,true})for(bool composition:{false,true})for(bool specialization:{false,true}) {
        const bool allowed=!headroom||(composition&&specialization);
        const auto cap=NativeFsiMaterialShaderCacheMaximum(headroom);
        for(uint64_t entries:std::array<uint64_t,10>{0,1,2,1023,1024,1025,1279,1280,1281,UINT64_MAX})
            for(uint64_t bytes:std::array<uint64_t,10>{0,1,48*mib,64*mib-1,64*mib,64*mib+1,80*mib-1,80*mib,80*mib+1,UINT64_MAX}) {
                const bool expected=allowed&&entries>=2&&entries<=cap.entries&&bytes&&bytes<=cap.bytes;
                Check(NativeFsiMaterialShaderCacheBoundsValid(headroom,composition,specialization,entries,bytes)==expected,
                    "Context/boundary policy disagrees with independent case");
            }
    }
    // The optional 256/16 MiB limits and generic 768/48 MiB prewarm limits are
    // not arguments to this function and remain unchanged at their call sites.
    Check(NativeFsiMaterialShaderCacheBoundsValid(true,true,true,2,1),"Small explicit pressure budget changed");
    const auto old=Exercise(false),enlarged=Exercise(true);
    Check(old.drains>0&&old.evictions>0,"Old byte-pressure witness was vacuous");
    Check(enlarged.drains==0&&enlarged.evictions==0&&enlarged.resident==1003,
        "Bounded headroom did not preserve the controlled working set");
    std::cout<<"PASS checks="<<checks<<" old_drains="<<old.drains<<" old_evictions="<<old.evictions
        <<" enlarged_resident="<<enlarged.resident<<" enlarged_bytes="<<enlarged.bytes<<'\n';
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
