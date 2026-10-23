// Producer-cost benchmark: replays a captured API stream through GearsLivePlan
// configured like the live renderer (query prefixes, lazy device states,
// direct resource refs, fast fingerprints, selective + deferred snapshots) and
// times Push plus the direct-execution prefix handoff. No GPU, no oracle.
// The capture is replayed `passes` times through one planner (renumbered as
// later frames) so caches reach steady state; pass 0 is reported as cold.
// Usage: live-plan-bench capture.g3d [passes]
#include "renderer/native/live_plan.h"
#include "renderer/native/shader_bindings.h"
#include "renderer/native/immutable_snapshots.h"
#include "renderer/native/deferred_texture_snapshots.h"
#include <chrono>
#include <cstdio>
#include <fstream>
#include <map>
#include <span>
#include <stdexcept>

using namespace gow3::renderer;
namespace {
uint32_t LE(std::span<const uint8_t> b,size_t o) {
    if(o+4>b.size())throw std::runtime_error("Truncated capture");
    return uint32_t(b[o])|uint32_t(b[o+1])<<8|uint32_t(b[o+2])<<16|uint32_t(b[o+3])<<24;
}
std::vector<uint8_t> Read(const char* path) {
    std::ifstream f(path,std::ios::binary);
    if(!f)throw std::runtime_error("Cannot open capture");
    return {std::istreambuf_iterator<char>(f),{}};
}
bool IsDraw(const GraphicsBoundaryEvent& e) {
    return (e.phase==1&&(e.function==0x82AEFF00||e.function==0x82AF0318))||
           (e.phase==2&&(e.function==0x82AEF500||e.function==0x82AEF9C0));
}
// Same parsing and selective block clearing as live_plan_test.cpp.
std::vector<GraphicsBoundaryEvent> Parse(const std::vector<uint8_t>& bytes) {
    if(LE(bytes,8)!=6)throw std::runtime_error("Version-6 capture required");
    std::vector<GraphicsBoundaryEvent> events;
    std::map<uint32_t,std::pair<size_t,size_t>> references;
    size_t at=16,index=0;
    while(at<bytes.size()) {
        const auto size=LE(bytes,at),function=LE(bytes,at+4);
        if(!function)break;
        if(size<60||at+size>bytes.size())throw std::runtime_error("Invalid event size");
        GraphicsBoundaryEvent event;event.sequence=index++;event.function=function;
        event.phase=LE(bytes,at+8);event.frame=LE(bytes,at+12);event.lr=LE(bytes,at+16);
        for(unsigned i=0;i<8;++i)event.args[i]=LE(bytes,at+20+i*4);
        event.result=LE(bytes,at+52);
        size_t cursor=at+60;
        for(unsigned i=0;i<LE(bytes,at+56);++i) {
            GraphicsBoundaryBlock block;block.kind=LE(bytes,cursor);block.address=LE(bytes,cursor+4);
            const auto length=LE(bytes,cursor+8);cursor+=12;size_t start=cursor,count=length;
            if(length&0x80000000u){const auto p=references.at(length&0x7fffffffu);start=p.first;count=p.second;}
            else{if(length)references.emplace(uint32_t(cursor),std::pair{cursor,size_t(length)});cursor+=length;}
            block.bytes.assign(bytes.begin()+start,bytes.begin()+start+count);event.blocks.push_back(std::move(block));
        }
        if(cursor!=at+size)throw std::runtime_error("Event block extent mismatch");
        if(const auto required=SnapshotShaderInputs(event.blocks))
            for(auto& block:event.blocks) {
                if(block.kind>=14&&block.kind<=29&&!required->first.contains(95-(block.kind-14)))block.bytes.clear();
                if(block.kind>=32&&block.kind<=95&&!required->second.contains((block.kind-32)/2))block.bytes.clear();
            }
        events.push_back(std::move(event));
        at+=size;
    }
    return events;
}
}

int main(int argc,char** argv) {
    try {
        if(argc<2)throw std::runtime_error("usage: live-plan-bench capture.g3d [iterations]");
        const int passes=argc>=3?std::max(2,std::atoi(argv[2])):6;
        const auto parsed=Parse(Read(argv[1]));
        size_t draws=0,frames_in_capture=0;
        for(const auto& e:parsed){draws+=IsDraw(e);frames_in_capture+=e.function==0x82AF6B10&&e.phase==0;}
        uint32_t first_frame=UINT32_MAX,last_frame=0;
        for(const auto& e:parsed){first_frame=std::min(first_frame,e.frame);last_frame=std::max(last_frame,e.frame);}
        const uint32_t frame_span=last_frame-first_frame+1;
        std::printf("events=%zu draws=%zu presents=%zu passes=%d\n",parsed.size(),draws,frames_in_capture,passes);
        using Clock=std::chrono::steady_clock;
        ImmutableSnapshots snapshots;
        GearsLivePlan planner({},true,true,true,true);
        double warm_total=0;
        for(int it=0;it<passes;++it) {
            auto events=parsed;  // Untimed copy.
            for(auto& e:events){e.sequence+=uint64_t(it)*parsed.size();e.frame+=uint32_t(it)*frame_span;}
            size_t frames=0,prefixes=0;
            double push_s=0;
            const auto start=Clock::now();
            for(auto& event:events) {
                DeferredTextureSnapshotScope scope;
                std::map<std::pair<uint32_t,uint32_t>,std::shared_ptr<const std::vector<uint8_t>>> sources;
                for(auto& block:event.blocks)if(block.kind>=32&&block.kind<=95&&!block.bytes.empty()) {
                    const auto count=uint32_t(block.bytes.size());
                    sources.emplace(std::pair{block.address,count},std::make_shared<const std::vector<uint8_t>>(std::move(block.bytes)));
                    block.bytes.clear();block.deferred_physical_bytes=count;
                }
                event.materialize_texture=scope.Bind([&](uint32_t address,uint32_t count) {
                    const auto source=sources.at({address,count});
                    return count>=4096?snapshots.Get(address,*source):source;
                });
                for(auto& block:event.blocks)if(block.kind>=14&&block.kind<=95&&block.bytes.size()>=4096) {
                    block.shared_bytes=snapshots.Get(block.address,block.bytes);block.bytes.clear();
                }
                const auto push_start=Clock::now();
                if(planner.Push(std::move(event)))++frames;
                prefixes+=planner.TakePacketPrefix().has_value();
                planner.TakeMemoryCopy();
                prefixes+=planner.TakeQueryPrefix().has_value();
                while(planner.TakeDirectPrefix(32))++prefixes;
                push_s+=std::chrono::duration<double>(Clock::now()-push_start).count();
            }
            const double total=std::chrono::duration<double>(Clock::now()-start).count();
            if(it)warm_total+=total;
            std::printf("pass %d%s: total %.1f ms (planner %.1f ms) = %.2f us/event, %.2f us/draw, %.2f ms/frame; frames=%zu prefixes=%zu\n",
                it,it?"":" (cold)",total*1e3,push_s*1e3,total*1e6/parsed.size(),total*1e6/std::max<size_t>(draws,1),
                total*1e3/std::max<size_t>(frames_in_capture,1),frames,prefixes);
        }
        std::printf("warm average: %.2f us/draw, %.2f ms/frame\n",warm_total*1e6/(passes-1)/std::max<size_t>(draws,1),
            warm_total*1e3/(passes-1)/std::max<size_t>(frames_in_capture,1));
        return 0;
    } catch(const std::exception& e){std::fprintf(stderr,"%s\n",e.what());return 1;}
}
