#include "renderer/native/memory_coherence.h"
#include <iostream>
#include <thread>
using gow3::renderer::NativeMemoryCoherence;
using Ranges=NativeMemoryCoherence::Ranges;
void Require(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
int main() try {
    NativeMemoryCoherence memory;
    Require(!memory.Invalidate(0,8192),"Unowned CPU pages must not generate GPU work");
    memory.Watch(4095,2);
    Require(memory.TakeWatches()==Ranges{{0,8192}},"Subpage range did not protect both pages");
    memory.Watch(4096,1);
    Require(memory.TakeWatches().empty(),"Already watched pages were rearmed");
    Require(memory.Invalidate(4097,1),"CPU write did not invalidate owned page");
    Require(memory.TakeInvalidated()==Ranges{{4096,4096}},"CPU write invalidated unrelated page");
    Require(!memory.Invalidate(4096,4096),"One-shot callback remained armed");
    memory.Watch(4096,4096);
    Require(memory.TakeWatches()==Ranges{{4096,4096}},"GPU reuse did not rearm page");
    memory.Invalidate(0,8192);
    Require(memory.TakeInvalidated()==Ranges{{0,8192}},"Adjacent dirty pages did not coalesce");
    Require(memory.TakeInvalidated().empty(),"Drained invalidation was repeated");
    memory.Watch(63*4096,2*4096);
    Require(memory.TakeWatches()==Ranges{{63*4096,2*4096}},"Watch crossed bitmap-word boundary incorrectly");
    memory.Invalidate(64*4096,4096);
    Require(memory.TakeInvalidated()==Ranges{{64*4096,4096}},"Boundary page invalidation is incorrect");
    memory.Watch(NativeMemoryCoherence::kCapacity-4096,4096);
    memory.TakeWatches();
    memory.Invalidate(NativeMemoryCoherence::kCapacity-1,UINT32_MAX);
    Require(memory.TakeInvalidated()==Ranges{{NativeMemoryCoherence::kCapacity-4096,4096}},"Callback range overflow");
    memory.Watch(0,1024*4096);
    memory.TakeWatches();
    std::thread even([&]{for(unsigned p=0;p<1024;p+=2)memory.Invalidate(p*4096,1);});
    std::thread odd([&]{for(unsigned p=1;p<1024;p+=2)memory.Invalidate(p*4096,1);});
    even.join();odd.join();
    Require(memory.TakeInvalidated()==Ranges{{0,1024*4096}},"Concurrent callbacks lost dirty pages");
    for(auto [address,bytes]:Ranges{{0,0},{UINT32_MAX,2},{NativeMemoryCoherence::kCapacity-1,2}}) {
        bool rejected=false;try{memory.Watch(address,bytes);}catch(const std::runtime_error&){rejected=true;}
        Require(rejected,"Invalid watch range accepted");
    }
    // Reproduces the live path: a CPU write invalidates native history, then
    // that page is retained again as an ordered immutable CPU snapshot. A
    // second CPU update must also be observed, without an intervening GPU draw.
    NativeMemoryCoherence retained;
    constexpr uint32_t page=8*NativeMemoryCoherence::kPage;
    uint64_t epoch=0;
    bool sdk_armed=false;
    std::vector<uint8_t> cpu(NativeMemoryCoherence::kPage,0),native;
    const auto enable=[&](uint32_t first,uint32_t bytes) {
        Require(first==page&&bytes==cpu.size(),"Unexpected CPU snapshot watch");
        sdk_armed=true;
    };
    retained.Watch(page,cpu.size());
    for(const auto& [first,bytes]:retained.TakeWatches())enable(first,bytes);
    for(uint8_t value:{0x35,0xa7}) {
        Require(sdk_armed,"Retained CPU page lost its SDK write protection");
        sdk_armed=false;++epoch; // Notification precedes the faulting store.
        Require(retained.Invalidate(page,1),"Repeated CPU write was not tracked");
        std::fill(cpu.begin(),cpu.end(),value);
        Require(retained.TakeInvalidated()==Ranges{{page,uint32_t(cpu.size())}},
            "Repeated CPU update did not revoke the retained page");
        auto snapshot=retained.CaptureCpuSnapshot(page,cpu.size(),enable,[&]{return epoch;},[&] {
            Require(sdk_armed,"CPU snapshot was read before SDK protection was rearmed");
            return cpu;
        });
        Require(snapshot.first==epoch,"CPU snapshot lost its observed epoch");
        native=std::move(snapshot.second);
        Require(native==cpu,"Retained CPU snapshot did not reflect the latest update");
    }
    // Calls into the SDK/read callback are outside the tracker mutex. A
    // notification in either window must fail before stale bytes are accepted.
    for(unsigned race=0;race<3;++race) {
        NativeMemoryCoherence racing;
        uint64_t write_epoch=4;
        bool read_called=false,rejected=false;
        try {
            racing.CaptureCpuSnapshot(page,cpu.size(),[&](uint32_t first,uint32_t bytes) {
                if(race==0) {++write_epoch;Require(racing.Invalidate(first,bytes),"Rearm race lost watch");}
            },[&]{return write_epoch;},[&] {
                read_called=true;
                auto data=cpu;
                ++write_epoch;
                // Another SDK callback may report an epoch even when it is
                // not this tracker's active watch; that change still matters.
                if(race==1)Require(racing.Invalidate(page,1),"Snapshot race lost watch");
                return data;
            });
        }catch(const std::runtime_error& error){
            rejected=std::string(error.what()).starts_with(race==0?
                "CPU memory changed while rearming native coherence snapshot":
                "CPU memory changed while capturing native coherence snapshot");
        }
        Require(rejected,"Concurrent CPU notification accepted a stale snapshot");
        Require(read_called==(race!=0),"Rearm race did not reject before copying bytes");
    }
    std::cout<<"PASS: native memory ownership, repeated CPU snapshots, rearm/copy epoch races, overflow and concurrent invalidation\n";
    return 0;
} catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
