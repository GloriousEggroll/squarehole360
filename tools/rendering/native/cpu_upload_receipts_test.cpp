// Exact CPU-upload receipt coverage, authority, lifetime and refusal regression tests.
#include "renderer/native/cpu_upload_receipts.h"
#include "renderer/native/page_version_table.h"
#include <cstdlib>
#include <iostream>
#include <limits>
#include <random>
#include <string>
#include <vector>

// Deliberate ordinary-allocation injection covers the receipt's only allocation.
static bool fail_allocation=false;
static uint64_t allocations=0;
void* operator new(std::size_t n) {
    if(fail_allocation)throw std::bad_alloc();
    if(auto* p=std::malloc(n?n:1)){++allocations;return p;}
    throw std::bad_alloc();
}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept{std::free(p);}
void operator delete[](void* p) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);}
void operator delete[](void* p,std::size_t) noexcept{std::free(p);}
using gow3::renderer::NativeCpuUploadReceipts;
using gow3::renderer::NativePageVersionTable;
using Receipts=NativeCpuUploadReceipts;
constexpr uint32_t P=Receipts::kPageBytes;
uint64_t checks=0,cases=0,oracle_bytes=0;
void Require(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
struct Desc {uint32_t first,last,destination;};
struct Model {
    Receipts receipts;
    NativePageVersionTable versions;
    std::vector<uint8_t> native;
    uint64_t serial=0;
    explicit Model(bool blocks,bool requested=true,bool checked=true):native(256*P,0x35) {
        versions.Reset(256,blocks,true);receipts.Configure(requested,checked);
    }
    uint64_t Version(uint32_t page){return versions.Max(page,page+1);}
    void Mutate(uint32_t address,uint32_t count,uint8_t value,bool remember=false,bool synchronous=true) {
        Require(count&&uint64_t(address)+count<=native.size(),"Invalid fixture write");
        if(++serial==0)receipts.ClearOnSerialWrap();
        for(uint32_t i=0;i<count;++i)native[address+i]=uint8_t(value+i*17+(i>>5));
        versions.Assign(address/P,(address+count-1)/P+1,serial);
        if(remember)receipts.Remember(address,std::span<const uint8_t>(native.data()+address,count),synchronous,
            [&](uint32_t page){return Version(page);});
    }
    bool Forward(std::span<const Desc> descriptors,bool expected_hit) {
        ++cases;std::vector<uint8_t> actual(128*P,0x73),expected=actual;
        std::vector<Receipts::Copy> copies;
        for(const auto& d:descriptors) {
            Require(d.first<d.last&&d.last<=native.size()&&uint64_t(d.destination)+d.last-d.first<=actual.size(),"Invalid fixture fragment");
            copies.push_back({d.first,d.last,actual.data()+d.destination});
            if(expected_hit)std::memcpy(expected.data()+d.destination,native.data()+d.first,d.last-d.first);
            oracle_bytes+=d.last-d.first;
        }
        const auto before_allocations=allocations;
        const bool hit=receipts.TryForward(copies,[&](uint32_t page){return Version(page);});
        Require(allocations==before_allocations,"Forward allocated storage");
        Require(hit==expected_hit,"Forward admission differs");
        Require(actual==expected,"Forward changed byte coverage/order or miss was partial");
        return hit;
    }
    bool Forward(std::initializer_list<Desc> descriptors,bool expected_hit){return Forward(std::span(descriptors),expected_hit);}
};
void CheckCoverage(bool blocks) {
    Model m(blocks);
    // Real 13 dirty pages cannot stand in for the remaining three pages.
    for(uint32_t page=0;page<13;++page)m.Mutate(page*P,P,uint8_t(page),true);
    m.Forward({{0,16*P,0}},false);
    for(uint32_t page=13;page<16;++page)m.Mutate(page*P,P,uint8_t(page),true);
    m.Forward({{0,16*P,0}},true);
    for(uint32_t phase=0;phase<8;++phase) {
        for(uint32_t page=0;page<13;++page)m.Mutate(page*P,P,uint8_t(page+phase*19),true);
        m.Forward({{0,16*P,0}},true);
    }
    // Sparse physical fragments, cross-page pieces, untouched CPU holes, and
    // overlapping destinations intentionally maintain original copy order.
    m.Forward({{3,P+15,7},{6*P+13,6*P+91,100},{2*P,4*P+37,3*P},{P,P+300,5}},true);
    m.Forward({{0,16*P,0},{2*P,3*P,9}},true);
    // An ordinary write on an unrelated page must not globally invalidate us.
    m.Mutate(63*P+5,7,90);m.Forward({{0,16*P,0}},true);
    // A last-fragment failure must not write even the first fragment.
    m.Mutate(15*P+99,1,71);m.Forward({{0,15*P,0},{15*P,16*P,15*P}},false);
    m.Mutate(15*P,P,71,true);m.Forward({{0,16*P,0}},true);
    // A subpage write outside the requested bytes still changes the page.
    m.Mutate(9*P+300,1,1);m.Forward({{9*P,9*P+100,0}},false);
    m.Mutate(9*P,P,1,true);
    // Unknown writers model ordinary Initialize, GPU Store, import, copy,
    // resolve and alias mutations. Each uses the real exact page-version table.
    for(uint32_t kind=0;kind<7;++kind) {
        m.Mutate(7*P+kind*4,4,uint8_t(kind));m.Forward({{0,16*P,0}},false);
        m.Mutate(7*P,P,uint8_t(kind),true);m.Forward({{0,16*P,0}},true);
    }
    // Actual queued upload has no receipt promotion, even after completion.
    m.Mutate(5*P,P,20,true,false);m.Forward({{0,16*P,0}},false);
    m.Forward({{5*P,6*P,0}},false);
    // A requested queued upload that actually completed synchronously does.
    m.Mutate(5*P,P,21,true,true);m.Forward({{0,16*P,0}},true);
    // Direct-mapped exact page keys guard collisions.
    m.Mutate((5+64)*P,P,22,true);m.Forward({{5*P,6*P,0}},false);
    m.Forward({{69*P,70*P,0}},true);
    m.Mutate(5*P,P,23,true);m.Forward({{0,16*P,0}},true);
    // Version zero after wrap is valid only for newly uploaded receipts.
    m.serial=std::numeric_limits<uint64_t>::max();m.Mutate(0,P,55,true);
    Require(m.Version(0)==0,"Wrap fixture did not assign zero");
    m.Forward({{0,P,0}},true);m.Forward({{P,2*P,0}},false);
    Require(m.receipts.statistics().serial_wrap_clears==1,"Wrap did not clear receipts");
    // The helper authenticates bytes actually uploaded, never newer guest data.
    std::vector<uint8_t> guest(P,0xa9);m.Mutate(4*P,P,50);
    guest.assign(m.native.begin()+4*P,m.native.begin()+5*P);
    m.receipts.Remember(4*P,guest,true,[&](uint32_t p){return m.Version(p);});
    guest.assign(P,0x11);guest.clear();guest.shrink_to_fit();
    m.Forward({{4*P,5*P,0}},true);
    // Simulates the required call ordering: original callbacks finish first;
    // a callback write invalidates before any attempted copy to the payload.
    m.Mutate(4*P+7,1,0xbb);m.Forward({{4*P,5*P,0}},false);
}
void CheckBoundsAndExceptions() {
    Receipts r;r.Configure(true,true);
    std::array<uint8_t,P> source{},out{};source.fill(0x64);out.fill(0x93);
    uint64_t getter_calls=0;
    const auto version=[&](uint32_t)->uint64_t{++getter_calls;return 4;};
    for(const auto address:std::array<uint32_t,3>{1,512u*1024*1024,0xfffff000u})r.Remember(address,source,true,version);
    r.Remember(0,std::span<const uint8_t>(source).first(P-1),true,version);
    r.Remember(0,std::span<const uint8_t>{},true,version);
    // A span with null data and positive extent is not constructed: that would
    // itself violate std::span's valid-range precondition before the helper.
    Require(getter_calls==0,"Invalid upload reached version query");
    r.Remember(0,source,false,version);Require(getter_calls==0,"Queued upload reached version query");
    r.Remember(0,source,true,version);
    r.Remember(512u*1024*1024-P,source,true,version);
    const std::array<Receipts::Copy,1> high{{{512u*1024*1024-P,512u*1024*1024,out.data()}}};
    Require(r.TryForward(high,version)&&out==source,"Highest legal page failed");
    out.fill(0x93);const auto unchanged=out;
    for(const auto bad:std::array<Receipts::Copy,5>{{{0,0,out.data()},{3,2,out.data()},
            {0,512u*1024*1024+1,out.data()},{0,P,nullptr},{0xffffffffu,0xffffffffu,out.data()}}}) {
        Require(!r.TryForward(std::span(&bad,1),version),"Invalid fragment admitted");
        Require(out==unchanged,"Invalid fragment changed output");
    }
    const std::array<Receipts::Copy,1> low{{{0,P,out.data()}}};
    const auto throwing=[](uint32_t)->uint64_t{throw 7;};
    r.Remember(P,source,true,throwing);
    Require(!r.TryForward(low,throwing)&&out==unchanged,"Getter exception did not refuse atomically");
    Require(r.TryForward(low,version)&&out==source,"Exception damaged another receipt");
    // Zero fragments must not become a hit or call a callback.
    const auto calls=getter_calls;
    Require(!r.TryForward({},version)&&getter_calls==calls,"Empty request admitted");
    bool reconfigured=false;try{r.Configure(false,true);}catch(const std::logic_error&){reconfigured=true;}
    Require(reconfigured,"Repeated configuration accepted");
}
void CheckDisabledAndAllocation() {
    std::array<uint8_t,P> bytes{},out{};out.fill(17);const auto unchanged=out;
    const std::array<Receipts::Copy,1> copies{{{0,P,out.data()}}};
    uint64_t callbacks=0;const auto version=[&](uint32_t)->uint64_t{++callbacks;return 1;};
    for(const auto [requested,checked]:std::array<std::pair<bool,bool>,3>{{{false,false},{false,true},{true,false}}}) {
        Receipts r;const auto before=allocations;r.Configure(requested,checked);
        r.Remember(0,bytes,true,version);Require(!r.TryForward(copies,version),"Disabled helper admitted");
        r.ClearOnSerialWrap();
        Require(allocations==before&&callbacks==0&&out==unchanged,"Disabled helper allocated/called/copied");
        const auto& s=r.statistics();Require(!s.enabled&&!s.storage_bytes&&!s.retained_pages,"Disabled helper retained storage");
        Require(s.unchecked_queue_refusals==(requested&&!checked),"Unchecked queue counter differs");
    }
    Receipts failed;fail_allocation=true;failed.Configure(true,true);fail_allocation=false;
    Require(!failed.statistics().enabled&&failed.statistics().allocation_refusals==1&&!failed.statistics().storage_bytes,
        "Allocation failure did not disable cleanly");
    Require(!failed.TryForward(copies,version)&&out==unchanged,"Allocation failure changed payload");
    Receipts r;r.Configure(true,true);r.Remember(0,bytes,true,version);
    fail_allocation=true;
    r.Remember(0,bytes,true,version);const bool hit=r.TryForward(copies,version);r.ClearOnSerialWrap();
    fail_allocation=false;
    Require(hit&&out==bytes,"Steady-state receipt operations allocated or failed");
    Require(r.statistics().storage_bytes<=264u*1024,"Storage bound widened");
}
void CheckRandomized(bool blocks) {
    Model m(blocks);std::mt19937 random(0x5c1a240u);
    // Independent oracle: indexed exact receipts, not the candidate's entries.
    struct Slot {bool valid=false;uint32_t page=0;uint64_t version=0;};
    std::array<Slot,64> slots{};
    for(uint32_t iteration=0;iteration<2500;++iteration) {
        const uint32_t page=random()%256,choice=random()%5;
        const uint8_t value=uint8_t(random());
        if(choice<3) {
            m.Mutate(page*P,P,value,true,choice!=2);
            if(choice!=2)slots[page%64]={true,page,m.Version(page)};
        } else {
            const uint32_t offset=random()%P,count=1+random()%(P-offset);
            m.Mutate(page*P+offset,count,value);
        }
        std::vector<Desc> descriptors;bool expected=true;
        const uint32_t fragments=1+random()%7;
        for(uint32_t i=0;i<fragments;++i) {
            const uint32_t sample=(i==0&&iteration%2)?page:random()%256;
            const uint32_t first=sample*P+random()%P;
            const uint32_t length=1+random()%std::min<uint32_t>(2*P,uint32_t(m.native.size())-first);
            descriptors.push_back({first,first+length,uint32_t(random()%(60*P))});
            for(uint32_t p=first/P;p<=(first+length-1)/P;++p) {
                const auto& slot=slots[p%64];expected&=slot.valid&&slot.page==p&&slot.version==m.Version(p);
            }
        }
        m.Forward(descriptors,expected);
    }
}
int main() {
    try {
        CheckDisabledAndAllocation();CheckBoundsAndExceptions();
        for(bool blocks:{false,true}){CheckCoverage(blocks);CheckRandomized(blocks);}
        std::cout<<"PASS cpu_upload_receipts cases="<<cases<<" checks="<<checks<<" oracle_bytes="<<oracle_bytes
            <<" block_versions=off,on checked_queue=required allocation_failure=injected gpu_execution=false\n";
        return 0;
    }catch(const std::exception& e){fail_allocation=false;std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
}
