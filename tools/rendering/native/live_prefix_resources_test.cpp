#include "renderer/native/live_plan.h"
#include <array>
#include <cstdlib>
#include <iostream>
#include <new>
#include <numeric>
#include <type_traits>

using namespace gow3::renderer;
static_assert(std::is_copy_constructible_v<LiveRenderFrame>);
static_assert(!std::is_copy_assignable_v<LiveRenderFrame>);
static_assert(std::is_nothrow_move_constructible_v<LiveRenderFrame>);
static_assert(std::is_nothrow_move_assignable_v<LiveRenderFrame>);
#if defined(_MSC_VER)
#define GOW3_TEST_NOINLINE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
#define GOW3_TEST_NOINLINE __attribute__((noinline))
#else
#define GOW3_TEST_NOINLINE
#endif
namespace {
struct AllocationCounts {size_t allocations=0,deallocations=0,bytes=0;};
thread_local bool count_allocations=false;
thread_local AllocationCounts allocation_counts;
GOW3_TEST_NOINLINE void* Allocate(size_t bytes) {
    if(void* p=std::malloc(bytes?bytes:1)) {
        if(count_allocations){++allocation_counts.allocations;allocation_counts.bytes+=bytes;}
        return p;
    }
    throw std::bad_alloc();
}
GOW3_TEST_NOINLINE void Release(void* p) noexcept {
    if(p&&count_allocations)++allocation_counts.deallocations;
    std::free(p);
}
}
void* operator new(size_t bytes){return Allocate(bytes);}
void* operator new[](size_t bytes){return Allocate(bytes);}
void operator delete(void* p)noexcept{Release(p);}
void operator delete[](void* p)noexcept{Release(p);}
void operator delete(void* p,size_t)noexcept{Release(p);}
void operator delete[](void* p,size_t)noexcept{Release(p);}
namespace {
using Bytes=std::vector<uint8_t>;
using Json=nlohmann::json;
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
template<class Action>void Reject(Action&& action) {
    bool rejected=false;try{action();}catch(const std::exception&){rejected=true;}
    Check(rejected,"Invalid flat prefix operation was accepted");
}
std::string Key(unsigned value) {
    static constexpr char hex[]="0123456789abcdef";
    std::string key(64,'0');
    for(unsigned i=0;i<8;++i){key[63-i]=hex[value&15];value>>=4;}
    return key;
}
std::vector<std::string> Capped(const LiveResourceMap& values,size_t limit) {
    std::vector<std::string> included;
    for(const auto& [key,bytes]:values)if(bytes->size()<=limit) {
        included.push_back(key);limit-=bytes->size();
    }
    return included;
}
size_t LegacyOracle() {
    std::array<std::string,64> keys;
    std::array<LiveResourceBytes,64> owners;
    for(unsigned i=0;i<keys.size();++i){keys[i]=Key(i);owners[i]=std::make_shared<const Bytes>(i+1,uint8_t(i));}
    size_t cases=0;
    for(unsigned count:{0u,1u,2u,12u,13u,14u,28u,29u,63u,64u,127u,128u})
    for(unsigned seed=0;seed<16;++seed) {
        LivePrefixResources actual(count);LiveResourceMap payloads,shaders;
        for(unsigned i=0;i<count;++i) {
            const auto slot=(i*17+seed*3)%keys.size();
            const bool shader=(i+seed)%3==0;
            // Explicit two-namespace aliases and duplicates match the old
            // insertion implementation, independent of the new sort logic.
            for(bool ns:{false,true})if(!ns||shader) {
                (ns?shaders:payloads).emplace(keys[slot],owners[slot]);
                actual.Append(keys[slot],owners[slot],ns);
            }
        }
        size_t retained=0;for(const auto& [key,owner]:payloads)retained+=owner->size();
        for(const auto& [key,owner]:shaders)retained+=owner->size();
        Check(actual.Seal()==retained,"Flat prefix retained bytes differ from legacy namespaces");
        for(bool ns:{false,true}) {
            const auto& expected=ns?shaders:payloads;
            const auto materialized=actual.Materialize(ns);
            Check(materialized==expected&&materialized.bucket_count()==expected.bucket_count()&&
                  std::equal(materialized.begin(),materialized.end(),expected.begin()),
                  "Diagnostic map keys/owners/buckets/traversal changed");
            for(size_t cap:{0u,1u,31u,256u,4096u})
                Check(Capped(materialized,cap)==Capped(expected,cap),"Capped diagnostic export selected different assets");
            size_t entries=0;actual.ForEach(ns,[&](auto key,const auto& owner) {
                const auto found=expected.find(std::string(key));
                Check(found!=expected.end()&&found->second==owner,"Flat enumeration differs from old map");++entries;
            });
            Check(entries==expected.size()&&actual.size(ns)==expected.size(),"Flat prefix has extra/missing entries");
            for(const auto& key:keys) {
                const auto found=expected.find(key);const auto* owner=actual.Find(key,ns);
                Check((found==expected.end())==!owner,"Missing dependency lookup changed");
                if(owner)Check(*owner==found->second,"Immutable resource owner changed");
            }
            Check(!actual.Find(Key(999),ns),"Absent digest unexpectedly resolved");
        }
        ++cases;
    }
    return cases;
}
void Lifetime() {
    std::shared_ptr<LiveRenderFrame> detached;
    std::weak_ptr<const Bytes> payload_lifetime,shader_lifetime;
    std::weak_ptr<LiveRenderFrame> parent_lifetime;
    const std::string digest=Key(1),missing=Key(2);
    {
        auto parent=std::make_shared<LiveRenderFrame>();parent_lifetime=parent;
        auto payload=std::make_shared<const Bytes>(17,0x63);
        auto shader=std::make_shared<const Bytes>(19,0x89);
        payload_lifetime=payload;shader_lifetime=shader;
        parent->payloads.emplace(digest,payload);
        parent->operations.push_back({{"payload",digest},{"missing",missing}});
        // Simulate a diagnostic hash acquiring a shader namespace after Emit,
        // before publication. The JSON anchor is unaffected by the later bank.
        parent->shaders.emplace(digest,shader);
        detached=std::make_shared<LiveRenderFrame>();detached->operations=parent->operations.Slice(0,1);
        detached->prefix_resources.emplace(2);
        const auto& key=detached->operations[0].Control().at("payload").get_ref<const std::string&>();
        for(bool ns:{false,true})detached->prefix_resources->Append(key,parent->ResourceAt(digest,ns),ns);
        detached->retained_bytes=detached->prefix_resources->Seal();
        Check(detached->retained_bytes==36&&detached->payloads.empty()&&detached->shaders.empty(),"Detached prefix kept producer maps");
        for(unsigned i=0;i<4096;++i)parent->operations.push_back({{"unrelated",Key(i+3)}});
        Check(&detached->operations[0]==&parent->operations[0],"Prefix copied its JSON anchor");
    }
    Check(parent_lifetime.expired(),"Flat prefix retained the unrelated full producer frame");
    Check(!payload_lifetime.expired()&&!shader_lifetime.expired(),"Detached flat prefix lost byte ownership");
    LiveRenderFrame copied=*detached;
    LiveRenderFrame moved=std::move(copied);
    detached.reset();
    Check(moved.ResourceAt(digest)->at(0)==0x63&&moved.ResourceAt(digest,true)->at(0)==0x89,
          "Copy/move/destruction invalidated borrowed JSON keys or namespace owners");
    Check(!moved.FindResource(missing),"Unbound diagnostic dependency became a payload");
    Reject([&]{(void)moved.ResourceAt(missing);});
    moved=LiveRenderFrame{};
    Check(payload_lifetime.expired()&&shader_lifetime.expired(),"Released prefix retained unrelated byte owners");
}
void Errors() {
    Reject([]{LivePrefixResources too_large(129);});
    LivePrefixResources pending(1);Reject([&]{pending.Find("missing",false);});
    Reject([&]{pending.Append("missing",{},false);});
    Check(pending.Seal()==0,"Failed append leaked an entry");
    Reject([&]{pending.Seal();});
    Reject([&]{pending.Append("missing",std::make_shared<const Bytes>(),false);});
    LivePrefixResources conflict(2);const std::string key=Key(1);
    conflict.Append(key,std::make_shared<const Bytes>(1,1),false);
    conflict.Append(key,std::make_shared<const Bytes>(1,2),false);
    Reject([&]{conflict.Seal();});
}
template<class Action>AllocationCounts Measure(Action&& action) {
    allocation_counts={};count_allocations=true;
    try{action();}catch(...){count_allocations=false;throw;}
    count_allocations=false;return allocation_counts;
}
void Allocations() {
    constexpr size_t iterations=1000;
    std::array<std::string,14> keys;
    std::array<LiveResourceBytes,14> owners;
    for(unsigned i=0;i<keys.size();++i){keys[i]=Key(i);owners[i]=std::make_shared<const Bytes>(64,uint8_t(i));}
    size_t legacy_sum=0,flat_sum=0;
    const auto legacy=Measure([&] {
        for(size_t round=0;round<iterations;++round) {
            LiveResourceMap payloads,shaders;
            for(unsigned i=0;i<keys.size();++i)(i<12?payloads:shaders).emplace(keys[i],owners[i]);
            legacy_sum+=payloads.size()+shaders.size();
        }
    });
    const auto flat=Measure([&] {
        for(size_t round=0;round<iterations;++round) {
            LivePrefixResources resources(keys.size());
            for(unsigned i=0;i<keys.size();++i)resources.Append(keys[i],owners[i],i>=12);
            Check(resources.Seal()==14*64,"Measured flat resources changed retained bytes");
            flat_sum+=resources.size(false)+resources.size(true);
        }
    });
    Check(flat_sum==legacy_sum&&flat_sum==iterations*14,"Allocation fixture lost resources");
    Check(flat.allocations==iterations*2&&flat.deallocations==flat.allocations,
          "Flat prefixes allocated per entry or leaked their buffers");
    Check(legacy.allocations>flat.allocations*8&&legacy.deallocations==legacy.allocations,
          "Flat prefix did not remove the expected node/key allocations");
    std::cout<<"Allocation oracle ("<<iterations<<" prefixes, 12 payloads + 2 shaders): legacy="<<legacy.allocations
             <<" alloc/free, "<<legacy.bytes<<" requested bytes; flat="<<flat.allocations
             <<" alloc/free, "<<flat.bytes<<" requested bytes\n";
}
}
int main()try {
    const auto cases=LegacyOracle();Lifetime();Errors();Allocations();
    std::cout<<"PASS: "<<cases<<" independent legacy insertion/namespace/lookup/capped-export cases; JSON key anchoring, parent release, copy/move and owner lifetime\n";
    return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
