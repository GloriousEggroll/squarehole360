#include "renderer/native/sampled_alias_index.h"
#include <cstdlib>
#include <new>
#include <fstream>
#include <iostream>
#include <map>
#include <vector>

// The hit-path assertion measures actual ordinary C++ allocations. Setup,
// serializer misses and diagnostic formatting happen outside the measured span.
namespace allocation_probe {
thread_local bool active=false;thread_local size_t count=0;
// Prevent whole-program allocation-pair folding in this allocation counter;
// this also avoids GCC treating replacement new/free as a mismatched pair.
void (*volatile release)(void*)=std::free;
}
void* operator new(std::size_t size) {
    if(allocation_probe::active)++allocation_probe::count;
    if(auto* value=std::malloc(size?size:1))return value;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size){return ::operator new(size);}
void operator delete(void* value)noexcept{allocation_probe::release(value);}
void operator delete[](void* value)noexcept{allocation_probe::release(value);}
void operator delete(void* value,std::size_t)noexcept{allocation_probe::release(value);}
void operator delete[](void* value,std::size_t)noexcept{allocation_probe::release(value);}

using namespace gow3::renderer;
using Json=nlohmann::json;
namespace {
void Require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
Json Identity() {
    return {{"base","00100000"},{"dimension",1},{"endian",0},{"format",6},{"height",720},
        {"layers",1},{"packed",false},{"pitch",1280},{"tiled",true},{"width",1280}};
}
struct ConstantHash {size_t operator()(const CanonicalSampledAlias&)const{return 0;}};
template<class Hash=CanonicalSampledAliasHash>struct Oracle {
    std::map<std::string,uint64_t> primary,legacy;
    SampledAliasIndex<decltype(primary),Hash> index{primary};
    uint64_t next=1,legacy_next=1;size_t checks=0;
    void Check(const Json& identity,uint32_t sign) {
        const auto old_key=Json::array({identity,sign}).dump();
        const auto [expected,inserted]=legacy.try_emplace(old_key,legacy_next);
        if(inserted)++legacy_next;
        const auto actual=index.FindOrCreate(identity,sign);
        if(!actual->second)actual->second=next++;
        Require(actual->first==old_key&&actual->second==expected->second,
            "Typed alias lookup selected a different legacy key or image identity");
        Require(index.size()<=512,"Sampled alias index escaped fixed entry bound");++checks;
    }
    void CompareMaps()const {
        Require(primary==legacy,"Typed alias index changed ordered primary keys or image identities");
    }
    void RetireAlternate() {
        // This is also the Scene integration contract: clear lookup iterators
        // before the owning map frees any nodes, then reuse the same identities.
        index.Clear();size_t n=0;
        for(auto it=primary.begin();it!=primary.end();) {
            if(n++%2==0)it=primary.erase(it);else ++it;
        }
        n=0;for(auto it=legacy.begin();it!=legacy.end();) {
            if(n++%2==0)it=legacy.erase(it);else ++it;
        }
        CompareMaps();
    }
};
std::vector<Json> Variations() {
    std::vector<Json> values{Identity()};
    for(const char* field:{"dimension","endian","format","height","layers","pitch","width"}) {
        for(uint64_t number:{0ull,1ull,9ull,10ull,99ull,100ull,4294967295ull,4294967296ull}) {
            auto value=Identity();value[field]=number;values.push_back(value);
        }
        for(const auto& number:std::vector<Json>{-1,1.0,1280.0,nullptr,"1",false,Json::array({1})}) {
            auto value=Identity();value[field]=number;values.push_back(value);
        }
    }
    for(const char* field:{"packed","tiled"})for(const auto& flag:std::vector<Json>{true,false,0,1,nullptr,"true"}) {
        auto value=Identity();value[field]=flag;values.push_back(value);
    }
    for(const auto& base:std::vector<Json>{"00000000","ffffffff","a0000000","ABCDEF00","0010000","000100000",
        "0x100000","00100000 ","z0100000",nullptr,1048576}) {
        auto value=Identity();value["base"]=base;values.push_back(value);
    }
    auto extra=Identity();extra["extra"]=1;values.push_back(extra);
    auto missing=Identity();missing.erase("width");values.push_back(missing);
    auto renamed=Identity();renamed["unused"]=renamed["width"];renamed.erase("width");values.push_back(renamed);
    values.push_back(nullptr);values.push_back(Json::array({1,2,3}));values.push_back(42);
    // JSON emits equal positive signed/unsigned integers identically; they
    // must share one key. Floating values keep the distinct legacy spelling.
    auto signed_value=Identity();signed_value["width"]=int64_t(1280);values.push_back(signed_value);
    auto unsigned_value=Identity();unsigned_value["width"]=uint64_t(1280);values.push_back(unsigned_value);
    return values;
}
template<class Hash>size_t Synthetic() {
    Oracle<Hash> oracle;const auto values=Variations();
    for(const auto& identity:values)for(uint32_t sign:{0u,1u,9u,10u,127u,128u,255u,UINT32_MAX}) {
        oracle.Check(identity,sign);oracle.Check(identity,sign);
    }
    oracle.CompareMaps();Require(oracle.index.statistics().hits>0&&oracle.index.statistics().fallbacks>0,
        "Fixture failed to exercise both typed hits and legacy fallbacks");
    oracle.RetireAlternate();
    for(auto it=values.rbegin();it!=values.rend();++it)oracle.Check(*it,0);
    oracle.CompareMaps();
    // Cross the fixed bound with canonical fields, then revisit evicted lookup
    // hints. The authoritative map must retain the same original image IDs.
    for(unsigned width=1;width<=1025;++width){auto identity=Identity();identity["width"]=width;oracle.Check(identity,255);}
    Require(oracle.index.statistics().capacity_clears>0,"Entry pressure did not clear bounded lookup hints");
    for(unsigned width=1025;width;--width){auto identity=Identity();identity["width"]=width;oracle.Check(identity,255);}
    oracle.CompareMaps();oracle.index.Clear();oracle.primary.clear();oracle.legacy.clear();
    oracle.Check(Identity(),0);oracle.CompareMaps();
    return oracle.checks;
}
void TypedSerializer() {
    std::map<std::string,uint64_t> primary;
    SampledAliasIndex<decltype(primary)> index(primary);
    const auto identity=Identity();const auto key=*TryCanonicalSampledAlias(identity,127);
    size_t serializations=0;
    const auto serialize=[&] {++serializations;return identity;};
    const auto first=index.FindOrCreate(key,serialize);
    Require(first->first==Json::array({identity,127}).dump()&&serializations==1,
        "Typed alias miss changed the authoritative legacy key");
    first->second=42;
    Require(index.FindOrCreate(key,serialize)==first&&serializations==1&&
        index.FindOrCreate(identity,127)==first,
        "Typed alias hit serialized identity or failed to share the JSON cache");
    allocation_probe::count=0;allocation_probe::active=true;
    bool same_owner=true;
    for(size_t i=0;i<10000;++i)same_owner&=index.FindOrCreate(key,serialize)==first;
    allocation_probe::active=false;
    Require(same_owner&&serializations==1&&allocation_probe::count==0,
        "Typed canonical alias hits allocated or materialized diagnostic JSON");
    std::cout<<"Typed hit allocations: "<<allocation_probe::count<<" across 10000 lookups\n";
    index.Clear();
    Require(index.FindOrCreate(key,serialize)==first&&serializations==2&&first->second==42,
        "Typed alias hint retirement changed authoritative image ownership");
}
size_t Capture(const char* path) {
    std::ifstream file(path);Require(bool(file),"Cannot read alias capture plan");
    Json plan;file>>plan;Oracle<> oracle;
    for(const auto& operation:plan.at("operations")) {
        if(operation.at("type")=="draw")for(const auto& texture:operation.at("textures")) {
            if(!texture.contains("identity"))continue;
            if(texture.at("source")!="gpu_resolve"&&texture.at("source")!="gpu_alias_or_previous_frame")continue;
            const auto sign=(texture.at("fetch").at(0).get<uint32_t>()>>2)&255u;
            oracle.Check(texture.at("identity"),sign);
        }
        if(operation.at("type")=="present")oracle.RetireAlternate();
    }
    oracle.CompareMaps();const auto& stats=oracle.index.statistics();
    std::cout<<"Capture PASS: "<<oracle.checks<<" serialized-key comparisons, typed hits="<<stats.hits
        <<", misses="<<stats.canonical_misses<<", fallbacks="<<stats.fallbacks<<'\n';
    return oracle.checks;
}
}
int main(int argc,char** argv)try {
    TypedSerializer();
    const auto checks=Synthetic<CanonicalSampledAliasHash>()+Synthetic<ConstantHash>();
    std::cout<<"PASS: "<<checks<<" exact legacy-key/image-ID checks including all fields/sign bits, noncanonical fallback, "
        "forced collisions, ordered primary maps, fixed capacity and retirement/address reuse\n";
    if(argc==2)Capture(argv[1]);else Require(argc==1,"usage: sampled-alias-index-test [scene-plan.json]");
    return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
