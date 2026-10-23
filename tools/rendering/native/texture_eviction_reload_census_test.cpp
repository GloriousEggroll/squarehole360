#include "renderer/native/texture_eviction_reload_census.h"
#include <cmath>
#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>

using namespace gow3::renderer;
namespace {
uint64_t checks=0;
void Require(bool value) {++checks;if(!value)throw std::runtime_error("texture census CPU fixture assertion");}
struct Key {
    std::array<uint32_t,6> fetch{};
    std::optional<std::string> base,mips;
    uint64_t version=0;
    bool operator<(const Key& other)const {
        return std::tie(fetch,base,mips,version)<std::tie(other.fetch,other.base,other.mips,other.version);
    }
};
struct Entry {DormantCacheEntry residency;};
using Cache=std::map<Key,Entry>;
Key Make(uint64_t version) {Key key;key.base=std::string(64,'a');key.version=version;return key;}
void Create(DormantCache& policy,Cache& cache,const Key& key,uint64_t bytes) {
    auto& entry=cache[key];policy.Access(entry.residency,false);policy.Created(entry.residency,bytes);
}
void Same(const Cache& a,const Cache& b,const DormantCache& pa,const DormantCache& pb) {
    Require(a.size()==b.size());auto x=a.begin(),y=b.begin();
    for(;x!=a.end();++x,++y) {
        Require(!(x->first<y->first)&&!(y->first<x->first));
        Require(x->second.residency.last_used_epoch==y->second.residency.last_used_epoch);
        Require(x->second.residency.allocation_bytes==y->second.residency.allocation_bytes);
    }
    const auto& sa=pa.statistics();const auto& sb=pb.statistics();
    Require(sa.hits==sb.hits&&sa.misses==sb.misses&&sa.creations==sb.creations&&sa.evictions==sb.evictions);
    Require(sa.age_evictions==sb.age_evictions&&sa.budget_evictions==sb.budget_evictions&&
        sa.binding_evictions==sb.binding_evictions&&sa.wrap_evictions==sb.wrap_evictions);
    Require(sa.resident_bytes==sb.resident_bytes&&sa.peak_resident_bytes==sb.peak_resident_bytes&&
        sa.peak_dormant_entries==sb.peak_dormant_entries&&sa.peak_dormant_bytes==sb.peak_dormant_bytes);
}
void End(DormantCache& policy,Cache& cache,NativeTextureEvictionReloadCensus& census,uint32_t frame) {
    policy.EndFrame(cache,[](const auto&){},NativeTextureEvictionReloadCensus::Observer{&census,frame});census.EndFrame();
}
void PolicyCase(DormantCache::Limits limits,DormantCacheEvictionKind expected,bool entries,bool bytes) {
    DormantCache plain(limits),observed(limits);Cache a,b;NativeTextureEvictionReloadCensus census;
    for(auto version:{1u,2u}) {Create(plain,a,Make(version),64);Create(observed,b,Make(version),64);}
    plain.EndFrame(a,[](const auto&){});End(observed,b,census,7);Same(a,b,plain,observed);
    plain.EndFrame(a,[](const auto&){});End(observed,b,census,7);Same(a,b,plain,observed);
    Require(census.total.evictions==observed.statistics().evictions);
    Require(census.total.evictions>0);
    if(expected==DormantCacheEvictionKind::Budget) {
        Require(census.total.entry_budget_evictions==(entries&&!bytes?1u:0u));
        Require(census.total.byte_budget_evictions==(!entries&&bytes?1u:0u));
        Require(census.total.both_budget_evictions==(entries&&bytes?1u:0u));
    } else if(expected==DormantCacheEvictionKind::Age)Require(census.total.age_evictions==2);
    else Require(census.total.disabled_budget_evictions==2);
    Require(std::isfinite(census.total.eviction_ms)&&census.total.eviction_ms>=0);
    // The actual policy selects Make(1) first; tombstone owns all exact bytes.
    auto creation=census.BeginCreation(Make(1),7,{});Require(creation.recent);
    Require(creation.kind==expected&&creation.entry_pressure==entries&&creation.byte_pressure==bytes);
    census.EndCreation(creation,64,{1,2,3,4});Require(census.total.recent_reloads==1);
    Require(census.total.payload_ms==1&&census.total.overlay_ms==2&&census.total.decode_ms==3&&census.total.upload_ms==4);
    Require(!census.BeginCreation(Make(1),7,{}).recent); // consumed, not an eternal first-use ledger
    Require(std::isfinite(census.total.creation_ms)&&census.total.creation_ms>=census.total.reload_ms);
    bool second_epoch=false;for(const auto& frame:census.frames)if(frame.valid&&frame.frame==7&&frame.completed_frame_epoch==1)second_epoch=true;
    Require(second_epoch);
}
void IdentityAndBounds() {
    NativeTextureEvictionReloadCensus census;auto key=Make(1);
    auto eviction=census.BeginEviction(key,64,1,DormantCacheEvictionKind::Age,false,false);
    key.base.reset();census.EndEviction(eviction); // source strings no longer exist
    Require(census.BeginCreation(Make(1),2,{}).recent);
    auto different=Make(1);different.fetch[5]=1;Require(!census.BeginCreation(different,2,{}).recent);
    different=Make(1);different.version=2;Require(!census.BeginCreation(different,2,{}).recent);
    different=Make(1);different.base->at(63)='b';Require(!census.BeginCreation(different,2,{}).recent);
    different=Make(1);different.mips=std::string(64,'a');Require(!census.BeginCreation(different,2,{}).recent);
    different=Make(1);different.base.reset();Require(!census.BeginCreation(different,2,{}).recent);
    different=Make(1);different.base=std::string(65,'a');auto malformed=census.BeginCreation(different,2,{});
    Require(!malformed.identity_valid&&!malformed.recent);census.EndCreation(malformed,64,{});
    Require(census.total.untrackable_creations==1);
    different=Make(1);different.base->at(0)='G';Require(!census.BeginCreation(different,2,{}).identity_valid);
    for(uint64_t i=0;i<NativeTextureEvictionReloadCensus::kMaximumTombstones+1;++i) {
        const auto e=census.BeginEviction(Make(i+100),64,3,DormantCacheEvictionKind::Budget,true,false);census.EndEviction(e);
    }
    Require(census.tombstones_overwritten==2); // original plus oldest overflowed
    Require(!census.BeginCreation(Make(100),4,{}).recent);
    Require(census.BeginCreation(Make(612),4,{}).recent);
    for(uint32_t frame=0;frame<NativeTextureEvictionReloadCensus::kMaximumFrames+3;++frame) {
        const auto creation=census.BeginCreation(Make(frame+1000),frame,{});census.EndCreation(creation,64,{});census.EndFrame();
    }
    std::size_t count=0;for(const auto& frame:census.frames)if(frame.valid)++count;
    Require(count==NativeTextureEvictionReloadCensus::kMaximumFrames);
}
void BindingWrapAndRejectedErase() {
    DormantCache policy({1024,16,120});Cache cache;NativeTextureEvictionReloadCensus census;
    Create(policy,cache,Make(1),64);End(policy,cache,census,0);
    Require(policy.EvictUnusedForBindings(cache,[](const auto&){},[](const auto&){return true;},
        NativeTextureEvictionReloadCensus::Observer{&census,1}));Require(census.total.binding_evictions==1);
    DormantCache wrap({1024,16,120},UINT64_MAX);Cache wrap_cache;
    Create(wrap,wrap_cache,Make(2),64);wrap_cache.begin()->second.residency.last_used_epoch=UINT64_MAX-1;
    End(wrap,wrap_cache,census,1);Require(census.total.wrap_evictions==1);
    DormantCache reject({0,0,120});Cache reject_cache;Create(reject,reject_cache,Make(3),64);reject.EndFrame(reject_cache,[](const auto&){});
    const auto evictions=census.total.evictions;bool threw=false;
    try {reject.EndFrame(reject_cache,[](const auto&){throw std::runtime_error("before erase rejection");},
        NativeTextureEvictionReloadCensus::Observer{&census,2});}catch(const std::runtime_error&){threw=true;}
    Require(threw&&reject_cache.size()==1&&reject.statistics().evictions==0&&reject.statistics().resident_bytes==64);
    Require(census.total.evictions==evictions&&!census.BeginCreation(Make(3),3,{}).recent);
}
}
int main() {
    try {
        PolicyCase({1024,1,120},DormantCacheEvictionKind::Budget,true,false);
        PolicyCase({100,16,120},DormantCacheEvictionKind::Budget,false,true);
        PolicyCase({100,1,120},DormantCacheEvictionKind::Budget,true,true);
        PolicyCase({1024,16,0},DormantCacheEvictionKind::Age,false,false);
        PolicyCase({0,0,120},DormantCacheEvictionKind::DisabledBudget,false,false);
        IdentityAndBounds();BindingWrapAndRejectedErase();
        std::cout<<"PASS "<<checks<<" CPU diagnostic policy/key/bounds checks; no GPU or timing benefit claim\n";
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
