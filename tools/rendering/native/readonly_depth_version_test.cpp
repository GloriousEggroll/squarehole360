#include "renderer/native/readonly_depth_version.h"
#include "renderer/native/surface_version_pages.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <initializer_list>
#include <limits>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using gow3::renderer::NativeReadonlyDepthLayoutKey;
using gow3::renderer::NativeReadonlyDepthVersion;
constexpr uint32_t kBytes=2048u*5120u;
constexpr uint32_t kPages=kBytes/4096u;
uint64_t checks=0,writer_events=0,promotions=0,reconversions=0;
void Check(bool condition,const std::string& message) {
    ++checks;if(!condition)throw std::runtime_error(message);
}
// Independent pixel oracle: enumerate every 32-bit packed depth/stencil word,
// then form its physical 4KiB page set. This does not use SurfaceFootprint's
// run construction, cache, range merge, or page-version scanning implementation.
std::vector<uint32_t> Words(const NativeReadonlyDepthLayoutKey& key) {
    const auto [width,height,pitch,base,format,depth]=key;
    (void)format;
    if(!width||!height||!pitch||base>=2048||depth!=1)
        throw std::runtime_error("Invalid modeled packed depth layout");
    std::vector<uint32_t> result;result.reserve(size_t(width)*height);
    for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<width;++x) {
        const uint64_t tile=uint64_t(y/16)*((pitch+79)/80)+x/80;
        const auto column=(x%80+40)%80;
        result.push_back(uint32_t(((uint64_t(base)+tile)*1280+(y%16)*80+column)%(kBytes/4)));
    }
    std::sort(result.begin(),result.end());result.erase(std::unique(result.begin(),result.end()),result.end());
    return result;
}
std::vector<uint32_t> PageSet(const std::vector<uint32_t>& words) {
    std::array<bool,kPages> member{};
    for(auto word:words)member[word/1024]=true;
    std::vector<uint32_t> result;
    for(uint32_t page=0;page<kPages;++page)if(member[page])result.push_back(page);
    return result;
}
struct PageOracle {
    uint64_t serial=0;
    std::array<uint64_t,kPages> versions{};
    std::vector<uint32_t> data=std::vector<uint32_t>(kBytes/4);
    PageOracle() {
        for(uint32_t word=0;word<data.size();++word)data[word]=(word*2654435761u)^0x7935db21u;
    }
    uint64_t Version(const std::vector<uint32_t>& words) const {
        uint64_t maximum=0;
        // Deliberately repeat duplicate page visits rather than use PageSet.
        for(auto word:words)maximum=std::max(maximum,versions[word/1024]);
        return maximum;
    }
    std::vector<uint32_t> Snapshot(const std::vector<uint32_t>& words) const {
        std::vector<uint32_t> result;result.reserve(words.size());
        for(auto word:words)result.push_back(data[word]);return result;
    }
    void MetadataWrite(const std::vector<uint32_t>& words) {
        ++serial;++writer_events;
        for(auto page:PageSet(words))versions[page]=serial;
    }
    void ActualWrite(const std::vector<uint32_t>& words,uint32_t value) {
        MetadataWrite(words);for(auto word:words)data[word]^=value;
    }
    // Models byte-range Initialize/Define/clear writes independently of a
    // surface layout. Endpoint pages are both included for nonempty ranges.
    void ByteRangeWrite(uint32_t first,uint32_t count,uint32_t value,bool change=true) {
        if(!count||uint64_t(first)+count>kBytes)throw std::runtime_error("Invalid modeled writer range");
        ++serial;++writer_events;
        for(uint32_t page=first/4096;page<=(first+count-1)/4096;++page)versions[page]=serial;
        if(change)for(uint32_t word=first/4;word<=(first+count-1)/4;++word)data[word]^=value;
    }
};
void CheckRetained(const NativeReadonlyDepthVersion& entry,const NativeReadonlyDepthLayoutKey& key,
                   const PageOracle& memory,const std::vector<uint32_t>& words,
                   const std::vector<uint32_t>& snapshot,const std::string& context) {
    if(entry.Reusable(key,memory.Version(words)))
        Check(memory.Snapshot(words)==snapshot,"Incorrect retained packed words: "+context);
}
void CheckRefusal(NativeReadonlyDepthVersion& entry,const NativeReadonlyDepthLayoutKey& key,
                  NativeReadonlyDepthVersion::Ticket ticket,uint64_t pre,uint64_t post,
                  bool accepted,bool audited,bool discarded,const std::string& reason) {
    const auto before=entry.source();
    Check(!entry.PromoteAcceptedReadonly(key,ticket,pre,post,accepted,audited,discarded),reason);
    Check(entry.source()==before,"Refused promotion changed token: "+reason);
}
void Preconditions() {
    const NativeReadonlyDepthLayoutKey key{81,19,160,2047,1,1};
    NativeReadonlyDepthVersion zero_generation(0,key,10),zero_source(1,key,0),entry(1,key,10);
    Check(!zero_generation.Reusable(key,10)&&!zero_generation.Capture(key,10),"Zero generation admitted");
    Check(!zero_source.Reusable(key,0)&&!zero_source.Capture(key,0),"Zero source admitted");
    Check(!entry.Reusable(key,0)&&!entry.Capture(key,0),"Zero current version admitted");
    Check(!entry.Capture(key,11),"Nonmatching current version produced ticket");
    for(size_t field=0;field<key.size();++field) {
        auto wrong=key;++wrong[field];
        Check(!entry.Reusable(wrong,10)&&!entry.Capture(wrong,10),"Exact layout word was ignored");
        CheckRefusal(entry,wrong,{1,10},10,11,true,true,false,"Mismatched layout promotion");
    }
    CheckRefusal(entry,key,{1,10},10,11,false,true,false,"Unaccepted draw promotion");
    CheckRefusal(entry,key,{1,10},10,11,true,false,false,"Unaudited writer promotion");
    CheckRefusal(entry,key,{1,10},10,11,true,true,true,"Discarded draw promotion");
    CheckRefusal(entry,key,{0,10},10,11,true,true,false,"Zero ticket generation promotion");
    CheckRefusal(entry,key,{2,10},10,11,true,true,false,"Recycled-generation stale ticket promotion");
    CheckRefusal(entry,key,{1,9},10,11,true,true,false,"Stale ticket source promotion");
    CheckRefusal(entry,key,{1,10},9,11,true,true,false,"Stale observed pre promotion");
    CheckRefusal(entry,key,{1,10},11,12,true,true,false,"Intervening writer promotion");
    CheckRefusal(entry,key,{1,10},10,10,true,true,false,"No metadata advance promotion");
    CheckRefusal(entry,key,{1,10},10,9,true,true,false,"Regressed metadata promotion");
    const auto old_generation_ticket=*entry.Capture(key,10);
    NativeReadonlyDepthVersion recycled(2,key,10);
    CheckRefusal(recycled,key,old_generation_ticket,10,11,true,true,false,"Recreated layout accepted previous-generation ticket");
    NativeReadonlyDepthVersion maximum(2,key,std::numeric_limits<uint64_t>::max());
    CheckRefusal(maximum,key,{2,maximum.source()},maximum.source(),0,true,true,false,"Wrapped serial promotion");
    Check(entry.PromoteAcceptedReadonly(key,{1,10},10,11,true,true,false),"Valid exact promotion refused");
    ++promotions;Check(entry.source()==11,"Valid promotion did not publish post token");
    CheckRefusal(entry,key,{1,10},11,12,true,true,false,"Consumed ticket reused");
}
void NoWriteMetadataCases() {
    using gow3::renderer::NativeSurfaceVersionPages;
    const NativeReadonlyDepthLayoutKey key{81,19,160,2047,1,1};
    NativeReadonlyDepthVersion entry(4,key,19);
    const auto ticket=*entry.Capture(key,19);
    Check(entry.AcceptUnchangedReadonly(key,ticket,37,37,true,true,false),"Exact no-write acceptance refused");
    Check(entry.source()==19,"No-write acceptance changed source token");
    const auto refuse=[&](const NativeReadonlyDepthLayoutKey& layout,NativeReadonlyDepthVersion::Ticket t,
                          uint64_t first,uint64_t last,bool accepted,bool audited,bool discard) {
        Check(!entry.AcceptUnchangedReadonly(layout,t,first,last,accepted,audited,discard),
            "Invalid no-write certificate accepted");
        Check(entry.source()==19,"Refused no-write changed source");
    };
    refuse(key,ticket,0,0,true,true,false);
    refuse(key,ticket,37,38,true,true,false); // Even unrelated writer must recheck.
    refuse(key,ticket,UINT64_MAX,0,true,true,false);
    refuse(key,{5,19},37,37,true,true,false);refuse(key,{4,18},37,37,true,true,false);
    refuse(key,ticket,37,37,false,true,false);refuse(key,ticket,37,37,true,false,false);
    refuse(key,ticket,37,37,true,true,true);
    for(size_t i=0;i<key.size();++i){auto other=key;++other[i];refuse(other,ticket,37,37,true,true,false);}
    // Literal half-open boundaries; adjacent tiles can share a version page.
    const auto mask=[](std::initializer_list<std::pair<uint32_t,uint32_t>> ranges) {
        return NativeSurfaceVersionPages(std::span(ranges.begin(),ranges.size()));
    };
    Check(mask({{0,4096}}).count()==1&&mask({{0,4096}}).test(0),"Endpoint touched next page");
    Check(mask({{4095,4097}}).count()==2,"Straddling range lost endpoint page");
    Check((mask({{5116,5120}})&mask({{5120,5124}})).any(),"Neighbor tile page alias missed");
    Check(!(mask({{0,4096}})&mask({{4096,8192}})).any(),"Disjoint pages alias");
    Check(mask({{0,4},{kBytes-4,kBytes}}).count()==2,"Wrapped range lost last page");
    for(const auto bad:{std::pair{4u,4u},std::pair{8u,4u},std::pair{kBytes-4,kBytes+4}}) {
        bool refused=false;try{(void)mask({bad});}catch(const std::invalid_argument&){refused=true;}
        Check(refused,"Invalid page range accepted");
    }
    // Actual helper against independent pixel-address membership, including
    // wrap, partial tiles, padding and all 2560 page bits for every layout.
    for(const auto layout:{key,NativeReadonlyDepthLayoutKey{640,360,640,1440,1,1},
                            NativeReadonlyDepthLayoutKey{1280,720,1280,720,1,1}}) {
        const auto words=Words(layout);const auto expected=PageSet(words);
        std::vector<std::pair<uint32_t,uint32_t>> ranges;
        for(const auto word:words)ranges.emplace_back(word*4,word*4+4);
        const auto actual=NativeSurfaceVersionPages(ranges);
        for(uint32_t page=0;page<kPages;++page)
            Check(actual.test(page)==std::binary_search(expected.begin(),expected.end(),page),
                "Exact page membership differs from independent pixel oracle");
    }
    std::cout<<"READONLY_NO_WRITE_METADATA_CPU coverage=exact-ticket,serial-writer,wrap,all-layout-words,page-endpoints,neighbor-tile-alias,independent-pixel-pages\n";
}
void PhysicalCases() {
    PageOracle memory;
    const NativeReadonlyDepthLayoutKey key{81,19,160,2047,1,1};
    const auto words=Words(key);const auto pages=PageSet(words);
    Check(pages.front()==0&&pages.back()==kPages-1,"Wrapped footprint did not cross EDRAM boundary");
    memory.MetadataWrite(words);auto snapshot=memory.Snapshot(words);
    NativeReadonlyDepthVersion entry(3,key,memory.Version(words));
    auto ticket=entry.Capture(key,memory.Version(words));Check(bool(ticket),"Valid conversion missing ticket");
    // Unrelated pages may advance the global serial without invalidating this
    // exact physical footprint. Page 1000 is outside the wrapped small surface.
    Check(!std::binary_search(pages.begin(),pages.end(),1000u),"Unrelated test page overlaps");
    memory.ByteRangeWrite(1000*4096,4,0x11);Check(entry.Reusable(key,memory.Version(words)),"Unrelated write invalidates view");
    const auto pre=memory.Version(words);memory.MetadataWrite(words);const auto post=memory.Version(words);
    Check(entry.PromoteAcceptedReadonly(key,*ticket,pre,post,true,true,false),"Immediate accepted no-write promotion refused");
    ++promotions;CheckRetained(entry,key,memory,words,snapshot,"accepted no-write promotion");
    // Different packed words in the same physical page conservatively change
    // the token even though the sampled words themselves remain identical.
    uint32_t outside=pages.front()*1024;
    while(std::binary_search(words.begin(),words.end(),outside))++outside;
    Check(outside/1024==pages.front(),"No same-page nonsampled word found");
    auto old=entry.Capture(key,memory.Version(words));memory.ByteRangeWrite(outside*4,4,0x10);
    Check(memory.Snapshot(words)==snapshot,"Same-page witness unexpectedly modified sampled word");
    Check(!entry.Reusable(key,memory.Version(words)),"Same-page write failed conservative invalidation");
    const auto current=memory.Version(words);memory.MetadataWrite(words);
    CheckRefusal(entry,key,*old,current,memory.Version(words),true,true,false,"Overlap before immediate pre ignored");
    // A true wrapped alias changes sampled data and requires fresh conversion.
    NativeReadonlyDepthVersion replacement(4,key,memory.Version(words));
    auto replacement_ticket=replacement.Capture(key,memory.Version(words));
    memory.ByteRangeWrite(words.back()*4,4,0x80000001u);
    Check(memory.Snapshot(words)!=snapshot,"Overlapping actual write witness is vacuous");
    Check(!replacement.Reusable(key,memory.Version(words)),"Wrapped physical alias stayed reusable");
    const auto changed=memory.Version(words);memory.MetadataWrite(words);
    CheckRefusal(replacement,key,*replacement_ticket,changed,memory.Version(words),true,true,false,"Actual writer before no-write draw promoted");
    NativeReadonlyDepthVersion reconverted(5,key,memory.Version(words));++reconversions;
    snapshot=memory.Snapshot(words);CheckRetained(reconverted,key,memory,words,snapshot,"new conversion after alias");
    // A failed admission must preserve the old token even if external metadata
    // has changed. It cannot publish an unexecuted/no-ticket conversion.
    auto failed_ticket=reconverted.Capture(key,memory.Version(words));const auto failed_pre=memory.Version(words);
    memory.MetadataWrite(words);
    CheckRefusal(reconverted,key,*failed_ticket,failed_pre,memory.Version(words),false,true,false,"Failed admission changed token");
    Check(!reconverted.Reusable(key,memory.Version(words)),"Failed admission incorrectly certified new token");
}
void WriterTraces() {
    PageOracle memory;std::mt19937 random(0x673ac321u);uint64_t generation=20;
    constexpr uint32_t kEvents=4096;
    for(unsigned layout_index=0;layout_index<32;++layout_index) {
        NativeReadonlyDepthLayoutKey key{uint32_t(1+random()%159),uint32_t(1+random()%39),uint32_t(80*(1+random()%3)),
                                        layout_index%2?2047u:uint32_t(random()%2048),uint32_t(random()%2),1};
        const auto words=Words(key),pages=PageSet(words);memory.MetadataWrite(words);
        std::optional<NativeReadonlyDepthVersion> entry(std::in_place,++generation,key,memory.Version(words));
        auto snapshot=memory.Snapshot(words);
        for(unsigned event=0;event<kEvents/32;++event) {
            CheckRetained(*entry,key,memory,words,snapshot,"before modeled writer");
            auto ticket=entry->Capture(key,memory.Version(words));
            const auto op=event%8;
            if(op==0&&ticket) { // Accepted audited read-only metadata write.
                const auto pre=memory.Version(words);memory.MetadataWrite(words);
                Check(entry->PromoteAcceptedReadonly(key,*ticket,pre,memory.Version(words),true,true,false),"Trace valid promotion refused");
                ++promotions;
            } else if(op==1||op==2||op==3) {
                // Surface Store/packed clear/Initialize share the authoritative
                // overlapping page-version contract but change actual words.
                const auto word=words[random()%words.size()];
                memory.ByteRangeWrite(word*4,4,1u<<(random()%32));
                Check(!entry->Reusable(key,memory.Version(words)),"Actual overlap failed invalidation");
            } else if(op==4) { // Defined-membership update still versions pages.
                memory.ByteRangeWrite(words.front()*4,4,0,false);
                Check(!entry->Reusable(key,memory.Version(words)),"Metadata Define ignored");
            } else if(op==5) { // Unrelated pages change only global serial.
                uint32_t page=(pages.back()+317)%kPages;
                while(std::binary_search(pages.begin(),pages.end(),page))page=(page+1)%kPages;
                const bool reusable_before=entry->Reusable(key,memory.Version(words));
                memory.ByteRangeWrite(page*4096,4,0x6b);
                Check(entry->Reusable(key,memory.Version(words))==reusable_before,"Unrelated serial changed exact version result");
            } else if(op==6&&ticket) { // Discarded/failed draw cannot promote.
                const auto pre=memory.Version(words);memory.MetadataWrite(words);
                CheckRefusal(*entry,key,*ticket,pre,memory.Version(words),true,true,true,"Trace discard promoted");
            } else if(op==7&&ticket) {
                const auto pre=memory.Version(words);memory.MetadataWrite(words);
                CheckRefusal(*entry,key,*ticket,pre,memory.Version(words),false,true,false,"Trace rejected draw promoted");
            }
            CheckRetained(*entry,key,memory,words,snapshot,"after modeled writer");
            if(!entry->Reusable(key,memory.Version(words))) {
                // Model accepted conversion publication after the original FIFO
                // seal. No RenderTexture/resource owner exists in this fixture.
                snapshot=memory.Snapshot(words);++reconversions;
                // Explicit replacement supplies a fresh immutable generation;
                // the optional owns policy metadata only, never GPU objects.
                entry.emplace(++generation,key,memory.Version(words));
                CheckRetained(*entry,key,memory,words,snapshot,"published fresh generation");
            }
        }
    }
}
}
int main() {
    try {
        Preconditions();NoWriteMetadataCases();PhysicalCases();WriterTraces();
        std::cout<<"{\"passed\":true,\"checks\":"<<checks<<",\"modeled_writer_events\":"<<writer_events
                 <<",\"accepted_no_write_promotions\":"<<promotions<<",\"reconversions\":"<<reconversions
                 <<",\"random_layouts\":32,\"trace_events\":4096,\"resource_owners\":0}\n";
        return 0;
    } catch(const std::exception& e) {std::cerr<<"Readonly depth policy fixture failed: "<<e.what()<<'\n';return 1;}
}
