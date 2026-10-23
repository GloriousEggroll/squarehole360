#include "renderer/native/defined_byte_ranges.h"
#include "renderer/native/page_version_table.h"
#include <array>
#include <iostream>
#include <random>
#include <stdexcept>
using namespace gow3::renderer;
using Map=std::map<uint32_t,uint32_t>;
uint64_t checks=0;
void Check(bool ok){++checks;if(!ok)throw std::runtime_error("Byte coverage oracle mismatch");}
// Independent original implementation, preserved from ResolveMemory::Define.
void Original(Map& defined,uint32_t first,uint32_t last) {
    auto it=defined.upper_bound(first);
    if(it!=defined.begin()) {
        auto prev=std::prev(it);
        if(prev->second>=first){first=prev->first;last=std::max(last,prev->second);it=defined.erase(prev);}
    }
    while(it!=defined.end()&&it->first<=last){last=std::max(last,it->second);it=defined.erase(it);}
    defined[first]=last;
}
void Invalidate(Map& defined,uint32_t first,uint32_t last) {
    auto it=defined.lower_bound(first);
    if(it!=defined.begin()&&std::prev(it)->second>first)--it;
    while(it!=defined.end()&&it->first<last) {
        const auto [start,end]=*it;it=defined.erase(it);
        if(start<first)defined.emplace(start,first);
        if(end>last){defined.emplace(last,end);break;}
    }
}
int main() {
    uint64_t avoided=0;
    for(bool blocks:{false,true})for(bool track:{false,true}) {
        Map reference,actual;std::array<bool,4096> bitmap{};
        NativePageVersionTable old_versions,versions;old_versions.Reset(4,blocks,track);versions.Reset(4,blocks,track);
        std::mt19937 random(0xfeed);
        for(uint32_t n=1;n<=40000;++n) {
            uint32_t first=random()%4096,last=std::min(4096u,first+1+uint32_t(random()%200));
            if(n%17==0){first=0;last=4096;}
            if(n%5==0) {
                Invalidate(reference,first,last);Invalidate(actual,first,last);
                std::fill(bitmap.begin()+first,bitmap.begin()+last,false);
            } else {
                // Same page assignment even on covered repeats, including a
                // decreasing serial to catch accidental max-version updates.
                const uint64_t serial=n%257;
                old_versions.Assign(first/1024,(last-1)/1024+1,serial);
                versions.Assign(first/1024,(last-1)/1024+1,serial);
                Original(reference,first,last);
                const auto before=actual;
                const bool reused=DefineNativeByteRange(actual,first,last,true);avoided+=reused;
                if(reused)Check(before==actual);
                std::fill(bitmap.begin()+first,bitmap.begin()+last,true);
            }
            Check(reference==actual);
            for(uint32_t i=0;i<4;++i)Check(old_versions.Max(i,i+1)==versions.Max(i,i+1));
            Check(old_versions.revision()==versions.revision());
            if(n%31==0)for(uint32_t i=0;i<bitmap.size();++i) {
                const auto it=actual.upper_bound(i);
                Check(bitmap[i]==(it!=actual.begin()&&std::prev(it)->second>i));
            }
        }
    }
    for(bool reuse:{false,true}) {
        Map m;DefineNativeByteRange(m,0xfffffff0u,0xffffffffu,reuse);
        Check(DefineNativeByteRange(m,0xfffffff1u,0xfffffffeu,reuse)==reuse);
        DefineNativeByteRange(m,0,1,reuse);DefineNativeByteRange(m,1,2,reuse);
        Check(m.size()==2&&m.at(0)==2&&m.at(0xfffffff0u)==0xffffffffu);
    }
    Check(avoided>10000);
    std::cout<<"PASS: "<<checks<<" coverage/version checks; "<<avoided<<" covered interval reallocations avoided\n";
}
