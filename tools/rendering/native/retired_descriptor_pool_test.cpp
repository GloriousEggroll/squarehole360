#include "renderer/native/retired_descriptor_pool.h"
#include <array>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>
using namespace gow3::renderer;
namespace {
uint64_t checks=0;
void Require(bool value,const char* message) {
    ++checks;
    if(!value)throw std::runtime_error(message);
}
struct Observer {uint64_t next=0,created=0,destroyed=0;};
struct Descriptor {
    Observer* observer;
    uint64_t id;
    uint32_t family;
    std::array<uint64_t,3> binding_words{};
    Descriptor(Observer& seen,uint32_t layout):observer(&seen),id(++seen.next),family(layout) {++seen.created;}
    ~Descriptor() {++observer->destroyed;}
};
using Pool=RetiredDescriptorPool<Descriptor>;
auto Factory(Observer& seen,uint32_t family) {
    return [&seen,family] {return std::make_unique<Descriptor>(seen,family);};
}
void Conservation(const Pool& pool) {
    const auto s=pool.statistics();
    Require(s.requests==s.hits+s.misses,"Request conservation failed");
    Require(s.live_sets==s.idle_sets+s.leased_sets,"Backing/lease conservation failed");
    Require(s.live_sets<=s.maximum_sets&&s.peak_live_sets<=s.maximum_sets,
        "Active plus idle backing bound exceeded");
}
void LastOwnerAndPressure() {
    Observer seen;
    {
        Pool pool(2);
        auto first=pool.Acquire(0,Factory(seen,0));
        auto second=pool.Acquire(0,Factory(seen,0));
        const auto first_id=first->id;
        std::weak_ptr<Descriptor> original=first;
        // These are explicit CPU owners, not a claim of real GPU fence proof.
        auto first_packet=first,later_packet=first;
        first.reset();first_packet.reset();
        Require(!original.expired()&&pool.statistics().idle_sets==0,
            "First owner released a later packet's descriptor");
        auto uncached=pool.Acquire(0,Factory(seen,0));
        Require(uncached->id!=first_id&&pool.statistics().capacity_fallbacks==1,
            "Pressure reused a still-owned backing");
        uncached.reset();
        Require(pool.statistics().uncached_destructions==1&&pool.statistics().live_sets==2,
            "Uncached pressure allocation entered the pool");
        later_packet.reset();
        Require(original.expired()&&pool.statistics().idle_sets==1,
            "Last owner did not retire the descriptor generation");
        auto fresh=pool.Acquire(0,Factory(seen,0));
        Require(fresh->id==first_id&&original.expired()&&pool.statistics().hits==1,
            "Physical reuse revived an old lease or missed exact backing reuse");
        fresh->binding_words={0x1111,0x2222,0x3333};
        fresh.reset();
        fresh=pool.Acquire(0,Factory(seen,0));
        // Pooling intentionally does not memoize bindings. The caller must
        // overwrite every used slot, including different buffer windows.
        fresh->binding_words={0xaaaa,0xbbbb,0};
        Require(fresh->binding_words==std::array<uint64_t,3>{0xaaaa,0xbbbb,0},
            "New binding values were not independent of the retired lease");
        fresh.reset();second.reset();Conservation(pool);
    }
    Require(seen.created==seen.destroyed,"Pressure case leaked descriptor backings");
}
void ExactFamilies() {
    Observer seen;
    {
        Pool pool(3);
        auto load0=pool.Acquire(0,Factory(seen,0));
        auto store=pool.Acquire(1,Factory(seen,1));
        auto load1=pool.Acquire(0,Factory(seen,0));
        const auto store_id=store->id;
        load0.reset();store.reset();load1.reset();
        for(uint64_t i=0;i<1024;++i) {
            auto next_store=pool.Acquire(1,Factory(seen,1));
            auto next_load=pool.Acquire(0,Factory(seen,0));
            Require(next_store->family==1&&next_store->id==store_id&&next_load->family==0&&
                next_load->id!=store_id,"Different load/store descriptor layouts were mixed");
            next_store->binding_words={i,~i,i^0x55aa55aa};
            next_load->binding_words={i+3,i+7,0};
            Require(next_store->binding_words[2]==(i^0x55aa55aa)&&next_load->binding_words[2]==0,
                "Descriptor content oracle failed");
            Conservation(pool);
        }
        Require(seen.created==3&&pool.statistics().hits==2048,
            "Retired reuse kept allocating backings");
    }
    Require(seen.created==seen.destroyed,"Exact-family case leaked descriptors");
}
void FailureAndTeardown() {
    Observer seen;
    {
        Pool pool(1);
        bool threw=false;
        try {pool.Acquire(0,[]()->std::unique_ptr<Descriptor>{throw std::runtime_error("factory");});}
        catch(const std::runtime_error&){threw=true;}
        Require(threw&&pool.statistics().live_sets==0&&pool.statistics().factory_failures==1,
            "Throwing allocation retained a pool reservation");
        threw=false;
        try {pool.Acquire(0,[]()->std::unique_ptr<Descriptor>{return {};});}
        catch(const std::runtime_error&){threw=true;}
        Require(threw&&pool.statistics().live_sets==0&&pool.statistics().factory_failures==2,
            "Null allocation retained a pool reservation");
        uint64_t called=0;threw=false;
        try {pool.Acquire(2,[&]{++called;return std::make_unique<Descriptor>(seen,0);});}
        catch(const std::invalid_argument&){threw=true;}
        Require(threw&&called==0&&pool.statistics().requests==2,
            "Invalid family reached the factory or counters");
        auto valid=pool.Acquire(0,Factory(seen,0));valid.reset();Conservation(pool);
    }
    std::shared_ptr<Descriptor> after_parent;
    {
        Pool parent(1);
        after_parent=parent.Acquire(1,Factory(seen,1));
    }
    Require(after_parent&&after_parent->family==1,"Lease retained a destroyed parent pointer");
    const auto old_id=after_parent->id;
    std::weak_ptr<Descriptor> old=after_parent;
    {
        Pool replacement(1);
        auto other=replacement.Acquire(1,Factory(seen,1));
        Require(other->id!=old_id&&replacement.statistics().hits==0,
            "Replacement pool reacquired an old parent lease");
        after_parent.reset();
        Require(old.expired()&&replacement.statistics().returns==0,
            "Old lease returned to a replacement pool");
    }
    Require(seen.created==seen.destroyed,"Parent teardown leaked descriptor backings");
    bool threw=false;
    try {Pool invalid(Pool::kMaximumSets+1);}
    catch(const std::invalid_argument&){threw=true;}
    Require(threw,"Over-limit pool was admitted");
}
void HardBound() {
    Observer seen;
    {
        Pool pool;
        std::vector<std::shared_ptr<Descriptor>> first_packet,later_packet;
        for(uint32_t i=0;i<Pool::kMaximumSets;++i) {
            auto lease=pool.Acquire(i%2,Factory(seen,i%2));
            (i<64?first_packet:later_packet).push_back(std::move(lease));
        }
        Require(pool.statistics().live_sets==128&&pool.statistics().leased_sets==128,
            "Actual hard bound was not reached");
        auto pressure=pool.Acquire(1,Factory(seen,1));pressure.reset();Conservation(pool);
        first_packet.clear();
        Require(pool.statistics().idle_sets==64&&pool.statistics().leased_sets==64,
            "Partial packet retirement released a later owner");
        for(uint32_t i=0;i<64;++i) {
            auto reused=pool.Acquire(i%2,Factory(seen,i%2));
            Require(reused->family==i%2,"Pressure reuse changed descriptor family");
        }
        Require(pool.statistics().allocations==129&&pool.statistics().hits==64&&
            pool.statistics().capacity_fallbacks==1,"Hard-bound fallback/reuse counters failed");
        later_packet.clear();Conservation(pool);
    }
    Require(seen.created==seen.destroyed,"Hard-bound case leaked descriptor backings");
}
}
int main() try {
    LastOwnerAndPressure();ExactFamilies();FailureAndTeardown();HardBound();
    std::cout<<"PASS: "<<checks<<" descriptor policy/owner checks; CPU owners only, actual GPU fences separate\n";
} catch(const std::exception& error) {
    std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;
}
