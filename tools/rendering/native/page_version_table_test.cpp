#include "renderer/native/page_version_table.h"
#include "renderer/native/gpu_alias_state.h"
#include <array>
#include <chrono>
#include <iostream>
#include <limits>
#include <random>
using namespace gow3::renderer;
namespace {
uint64_t checks=0;
void Check(bool value,const char* why) {++checks;if(!value)throw std::runtime_error(why);}
uint64_t Oracle(const std::vector<uint64_t>& pages,uint32_t first,uint32_t end) {
    uint64_t result=0;for(auto i=first;i<end;++i)if(pages[i]>result)result=pages[i];return result;
}
void Verify(uint32_t size) {
    NativePageVersionTable off,on;off.Reset(size,false);on.Reset(size,true);
    std::vector<uint64_t> reference(size);std::mt19937_64 rng(size+17);
    auto query=[&](uint32_t first,uint32_t end) {
        const auto expected=Oracle(reference,first,end);
        Check(off.Max(first,end)==expected&&on.Max(first,end)==expected,"Exact range maximum differs");
    };
    auto assign=[&](uint32_t first,uint32_t end,uint64_t value) {
        off.Assign(first,end,value);on.Assign(first,end,value);
        for(auto i=first;i<end;++i)reference[i]=value;
    };
    // Full overwrite, lazy block expansion, unchanged holes, reducing the
    // unique maximum, and wrapping through zero. No monotonic-epoch shortcut.
    assign(0,size,UINT64_MAX);assign(0,size,0);
    for(uint32_t i=0;i<size;++i) {
        if(i%64<2||i%64>61||i==size-1) {
            assign(i,i+1,i+23);query(i,i+1);
            if(i)query(i-1,i);if(i+1<size)query(i+1,i+2);
        }
    }
    for(uint32_t iteration=0;iteration<3000;++iteration) {
        uint32_t first=rng()%(size+1),end=rng()%(size+1);if(first>end)std::swap(first,end);
        if(iteration%3==0)end=std::min(size,first+1);
        if(iteration%17==0) {first=0;end=size;}
        const uint64_t value=iteration%19==0?UINT64_MAX:iteration%7==0?0:rng();
        assign(first,end,value);query(first,end);query(0,size);
        if(first)query(0,first);if(end<size)query(end,size);
        for(unsigned i=0;i<5;++i) {
            uint32_t a=rng()%(size+1),b=rng()%(size+1);if(a>b)std::swap(a,b);query(a,b);
        }
    }
    // Invalid operations must not mutate values; empty end-of-allocation is valid.
    query(size,size);
    for(bool write:{false,true})for(auto range:{std::pair{size+1,size+1},std::pair{1u,0u},std::pair{0u,size+1}}) {
        bool threw=false;try {if(write)on.Assign(range.first,range.second,999);else on.Max(range.first,range.second);}
        catch(const std::out_of_range&){threw=true;}Check(threw,"Invalid bounds accepted");query(0,size);
    }
    for(uint32_t i=0;i<size;++i)query(i,i+1);
    Check(on.statistics().uniform_writes>0&&on.statistics().summary_reads>0,"Block path not exercised");
    on.Reset(size,true);Check(on.Max(0,size)==0,"Reset retained old versions");
}
void AliasState() {
    NativePageVersionTable table;table.Reset(130,true);
    std::vector<uint64_t> pages(130);std::map<uint32_t,uint32_t> defined{{3,64*4096},{65*4096,130*4096}};
    NativeGpuAliasFootprint a{{{7,65*4096+9},{129*4096,130*4096}},{}},b=a;
    NativeGpuAliasStateStatistics sa,sb;
    for(uint64_t serial=1;serial<300;++serial) {
        auto first=uint32_t(serial%130),last=std::min(first+21,130u);
        table.Assign(first,last,serial);for(auto i=first;i<last;++i)pages[i]=serial;
        for(unsigned reuse=0;reuse<3;++reuse) {
            auto x=ReadNativeGpuAliasState(a,serial,defined,pages,sa);
            auto y=ReadNativeGpuAliasStateRanges(b,serial,defined,table.size(),
                [&](uint32_t lo,uint32_t hi){return table.Max(lo,hi);},sb);
            Check(x==y,"Alias definedness/version or serial memo differs");
        }
    }
    Check(sa.hits==sb.hits&&sa.scans==sb.scans&&sa.pages_scanned==sb.pages_scanned,"Alias diagnostic counts differ");
}
struct Measurement {double ms;uint64_t sum;};
Measurement Bench(bool indexed) {
    NativePageVersionTable table;table.Reset(2560,indexed);uint64_t sum=0,serial=0;
    auto start=std::chrono::steady_clock::now();
    for(unsigned frame=0;frame<180;++frame)for(unsigned draw=0;draw<1200;++draw) {
        // Two broad disjoint primary planes, plus bounded secondary and tiny
        // alias accesses. Synthetic metadata workload, not a game FPS model.
        table.Assign(0,900,++serial);table.Assign(900,1800,++serial);
        sum+=table.Max(0,900);sum+=table.Max(900,1800);
        if(draw%7==0) {table.Assign(1799,2249,++serial);sum+=table.Max(2100,2155);}
        if(draw%11==0) {table.Assign(2559,2560,++serial);sum+=table.Max(2048,2560);}
    }
    return {std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count(),sum};
}
}
int main()try {
    for(uint32_t size:{1u,63u,64u,65u,127u,2560u,131072u})Verify(size);
    AliasState();
    std::vector<double> off,on;
    for(unsigned i=0;i<7;++i) {
        Measurement x,y;if(i%2) {y=Bench(true);x=Bench(false);}else {x=Bench(false);y=Bench(true);}
        Check(x.sum==y.sum,"Metadata benchmark checksum differs");off.push_back(x.ms);on.push_back(y.ms);
    }
    std::sort(off.begin(),off.end());std::sort(on.begin(),on.end());
    std::cout<<"PASS "<<checks<<" independent exact version/alias checks\n"
             <<"Synthetic metadata benchmark median: flat="<<off[3]<<" ms blocks="<<on[3]<<" ms (not game frame time)\n";
} catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
