#include "renderer/native/page_version_table.h"
#include <array>
#include <iostream>
#include <random>
using namespace gow3::renderer;
namespace {
uint64_t checks=0,hits=0,rescans=0;
void Check(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
void Verify(bool blocks) {
    constexpr uint32_t count=513;
    NativePageVersionTable table;table.Reset(count,blocks,true);
    std::array<uint64_t,count> oracle{};
    struct Memo {std::array<std::pair<uint32_t,uint32_t>,2> ranges;uint64_t revision=0,value=0;};
    std::array<Memo,64> memos{};std::mt19937_64 random(615);
    for(auto& memo:memos)for(auto& [first,end]:memo.ranges) {
        first=random()%count;end=std::min(count,first+uint32_t(random()%80));
    }
    memos[0].ranges={{{0,1},{512,513}}};memos[1].ranges={{{63,65},{255,257}}};
    for(unsigned iteration=0;iteration<6000;++iteration) {
        uint32_t first=random()%count,end=std::min(count,first+1+uint32_t(random()%100));
        if(iteration%11==0){first=0;end=count;}
        if(iteration%7==0)end=first;
        // Arbitrary/reducing/zero values must not become write stamps.
        const uint64_t version=iteration%3?random():0;
        table.Assign(first,end,version);std::fill(oracle.begin()+first,oracle.begin()+end,version);
        for(auto& memo:memos)for(unsigned repeat=0;repeat<2;++repeat) {
            uint64_t expected=0;
            for(const auto& [a,b]:memo.ranges)for(auto i=a;i<b;++i)expected=std::max(expected,oracle[i]);
            if(table.UnchangedSince(memo.revision,memo.ranges))++hits;
            else {
                ++rescans;memo.value=0;
                for(const auto& [a,b]:memo.ranges)memo.value=std::max(memo.value,table.Max(a,b));
            }
            memo.revision=table.revision();Check(memo.value==expected,"Guard reused a stale exact page maximum");
        }
    }
    const auto before=table.revision();
    for(auto [a,b]:{std::pair{0u,514u},std::pair{3u,2u},std::pair{514u,514u}}) {
        bool threw=false;try{table.Assign(a,b,99);}catch(const std::out_of_range&){threw=true;}
        Check(threw&&before==table.revision(),"Invalid write mutated stamps");
    }
    NativePageVersionTable flat;flat.Reset(count,false);
    Check(!flat.tracks_changes()&&!flat.UnchangedSince(1,memos[0].ranges),"Disabled tracker authenticated a memo");
}
void BoundariesAndExhaustion() {
    using Range=NativePageChangeTracker<>::Range;
    NativePageChangeTracker<uint8_t> tiny(130);
    const std::array<Range,1> first{{{0,1}}},second{{{64,65}}},tail{{{129,130}}};
    auto token=tiny.revision();tiny.Write(63,64);
    Check(!tiny.UnchangedSince(token,first)&&tiny.UnchangedSince(token,second),"Coarse boundary crossed wrong group");
    token=tiny.revision();tiny.Write(64,65);
    Check(tiny.UnchangedSince(token,first)&&!tiny.UnchangedSince(token,second),"Second group missed write");
    token=tiny.revision();tiny.Write(129,130);
    Check(tiny.UnchangedSince(token,second)&&!tiny.UnchangedSince(token,tail),"Partial final group missed write");
    token=tiny.revision();tiny.Write(130,130);Check(tiny.revision()==token,"Empty write changed epoch");
    for(unsigned i=0;i<1024;++i) {
        token=tiny.revision();tiny.Write(64,65);
        if(tiny.revision())Check(tiny.UnchangedSince(token,first),"Unrelated write invalidated disjoint group");
        else Check(!tiny.UnchangedSince(token,first)&&!tiny.UnchangedSince(0,first),"Exhausted counter reused an old token");
    }
    Check(tiny.revision()==0,"Exhausted tracker became enabled again");
    const std::array<Range,1> bad{{{129,131}}};bool threw=false;
    try{tiny.UnchangedSince(0,bad);}catch(const std::out_of_range&){threw=true;}
    Check(threw,"Exhausted tracker bypassed validation");
}
}
int main()try {
    Verify(false);Verify(true);BoundariesAndExhaustion();
    Check(hits>0&&rescans>0,"Both reuse and exact scans must execute");
    std::cout<<"PASS "<<checks<<" exact change/version checks, "<<hits<<" reuses, "<<rescans<<" rescans\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
