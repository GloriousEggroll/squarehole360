#include "renderer/native/catalog_shader_cache_headroom.h"
#include "renderer/native/cache_bookkeeping.h"
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>

using namespace gow3::renderer;
namespace {
size_t checks=0;
void Check(bool condition,const char* what) {++checks;if(!condition)throw std::runtime_error(what);}
struct Entry {uint64_t last_used_epoch=0,spirv_bytes=0;};
std::map<uint32_t,Entry> CaptureSizedPopulation() {
    // Synthetic weights reproduce the observed total, not shader contents.
    std::map<uint32_t,Entry> cache;
    constexpr uint64_t originals=745,original_bytes=50326612,candidates=53,candidate_bytes=4065724;
    for(uint64_t i=0;i<originals;++i)cache.emplace(i,Entry{0,original_bytes/originals+(i<original_bytes%originals)});
    for(uint64_t i=0;i<candidates;++i)cache.emplace(originals+i,Entry{0,candidate_bytes/candidates+(i<candidate_bytes%candidates)});
    return cache;
}
}
int main()try {
    NativeCatalogShaderHeadroom headroom;
    auto off=headroom.SoftTargets(false,1024,64u*1024*1024),zero=headroom.SoftTargets(true,1024,64u*1024*1024);
    Check(off.entries==768&&off.bytes==48u*1024*1024,"Original soft targets");
    Check(off.entries==zero.entries&&off.bytes==zero.bytes,"No admission gives no credit");
    Check(!headroom.Admit(0)&&headroom.modules()==0&&headroom.bytes()==0,"Zero extent refused without mutation");
    for(uint64_t i=0;i<53;++i)Check(headroom.Admit(4065724/53+(i<4065724%53)),"Actual-sized startup admission");
    Check(headroom.modules()==53&&headroom.bytes()==4065724,"Only actual admitted bytes counted");
    const auto active=headroom.SoftTargets(true,1024,64u*1024*1024);
    Check(active.entries==821&&active.bytes==54397372,"Bounded additive soft headroom");
    Check(headroom.SoftTargets(false,1024,64u*1024*1024).bytes==off.bytes,"OFF unchanged after admission");
    auto old_cache=CaptureSizedPopulation(),new_cache=old_cache;
    PersistentCacheStatistics old_stats{},new_stats{};old_stats.bytes=new_stats.bytes=54392336;
    TrimPersistentCache(old_cache,off.entries,off.bytes,old_stats,[](const auto& e){return e.spirv_bytes;},[](const auto&){return true;});
    TrimPersistentCache(new_cache,active.entries,active.bytes,new_stats,[](const auto& e){return e.spirv_bytes;},[](const auto&){return true;});
    Check(old_stats.evictions>0&&old_cache.size()<798,"Old target forces admission-only churn");
    Check(new_stats.evictions==0&&new_cache.size()==798&&new_stats.bytes==54392336,"Headroom preserves original plus admitted set");
    const auto tiny=headroom.SoftTargets(true,2,20);
    Check(tiny.entries==2&&tiny.bytes==20,"Custom hard targets cap credit");
    const auto full=headroom.SoftTargets(true,UINT64_MAX,UINT64_MAX);
    constexpr uint64_t full_base=UINT64_MAX/4*3+(UINT64_MAX%4*3)/4;
    Check(full.entries==full_base+53&&full.bytes==full_base+4065724,"Full-width floor and addition do not overflow");
    for(uint64_t hard=0;hard<=1024;++hard) {
        const auto inactive=headroom.SoftTargets(false,hard,hard*257),active=headroom.SoftTargets(true,hard,hard*257);
        Check(inactive.entries==hard*3/4&&inactive.bytes==hard*257*3/4,"OFF exact old targets for custom limits");
        Check(active.entries==std::min(hard,hard*3/4+53)&&active.bytes==std::min(hard*257,hard*257*3/4+4065724),"Custom limits saturate bounded admitted credit");
    }
    NativeCatalogShaderHeadroom byte_cap;
    Check(byte_cap.Admit(NativeCatalogShaderHeadroom::kMaximumBytes),"Full catalog byte bound admitted");
    Check(!byte_cap.Admit(1)&&byte_cap.modules()==1&&byte_cap.bytes()==NativeCatalogShaderHeadroom::kMaximumBytes,"Byte overflow refused atomically");
    NativeCatalogShaderHeadroom entry_cap;
    for(uint64_t i=0;i<128;++i)Check(entry_cap.Admit(20),"Bounded module admission");
    Check(!entry_cap.Admit(20)&&entry_cap.modules()==128&&entry_cap.bytes()==2560,"Module cap refused atomically");
    const auto cap=byte_cap.SoftTargets(true,1024,64u*1024*1024);
    Check(cap.bytes==64u*1024*1024,"Byte soft target never exceeds hard limit");
    bool drained=false;PersistentCacheStatistics pressure{};pressure.bytes=new_stats.bytes;
    ReservePersistentCache(new_cache,64,798,54392336,pressure,[](const auto& e){return e.spirv_bytes;},
        [](const auto&){return false;},[&]{drained=true;});
    Check(drained&&pressure.evictions>0&&new_cache.size()<798&&pressure.bytes<=54392336-64,"Original hard pressure remains enforced");
    std::cout<<"PASS: checks="<<checks<<" synthetic_original_modules=745 catalog_modules=53 hard_pressure_unchanged=true\n";
    return 0;
}catch(const std::exception& error){std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}
