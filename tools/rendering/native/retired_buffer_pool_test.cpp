#include "renderer/native/retired_buffer_pool.h"
#include <array>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace gow3::renderer;
namespace {
void Require(bool result,const char* message) {if(!result)throw std::runtime_error(message);}
struct Buffer {
    unsigned id;
    std::array<bool,256>& alive;
    Buffer(unsigned value,std::array<bool,256>& owners):id(value),alive(owners) {
        Require(!alive.at(id),"Duplicate fake buffer ownership");alive.at(id)=true;
    }
    ~Buffer(){alive.at(id)=false;}
};
}
int main() try {
    using Pool=RetiredBufferPool<Buffer>;
    static_assert(Pool::kMaxBuffers==128&&Pool::kMaxBytes==16ull*1024*1024);
    int device_a,device_b;std::array<bool,256> alive{};
    const RetiredBufferKey key{48,3,1,false,&device_a};
    auto make=[&](unsigned id){return std::make_unique<Buffer>(id,alive);};
    Pool pool(3,256);
    // An externally held live owner is never an idle candidate. The pool API
    // trusts the caller's full-fence proof; actual GPU coverage lives in Scene.
    auto active=make(1);
    Require(!pool.Take(key)&&alive[1],"Pool recycled an owner it never received");
    Require(pool.ReturnAfterFence(make(2),key,64),"Fenced exact buffer refused");
    auto incompatible=key;
    for(unsigned field=0;field<5;++field) {
        incompatible=key;
        if(field==0)++incompatible.size;
        if(field==1)++incompatible.flags;
        if(field==2)++incompatible.heap;
        if(field==3)incompatible.committed=true;
        if(field==4)incompatible.device=&device_b;
        Require(!pool.Take(incompatible)&&alive[2]&&pool.statistics().idle_bytes==64,
            "Descriptor/device mismatch removed or reused an idle owner");
    }
    auto first=pool.Take(key);
    Require(first&&first->id==2&&pool.statistics().idle_buffers==0&&alive[1],
        "Exact descriptor failed to move only the retired owner");
    first.reset();
    // Charge actual allocation extent: three 48-byte payloads occupy192 bytes.
    Require(pool.ReturnAfterFence(make(3),key,64)&&pool.ReturnAfterFence(make(4),key,64)&&
        pool.ReturnAfterFence(make(5),key,64),"Small idle pool admission failed");
    Require(pool.statistics().idle_bytes==192&&pool.statistics().idle_buffers==3,
        "Idle budget charged payload bytes instead of actual allocation bytes");
    const bool count_admitted=pool.ReturnAfterFence(make(6),key,64);
    Require(count_admitted&&!alive[3]&&alive[4]&&alive[5]&&alive[6]&&alive[1],
        "Count pressure evicted a live or newer idle allocation");
    const RetiredBufferKey larger{96,3,1,false,&device_a};
    const bool bytes_admitted=pool.ReturnAfterFence(make(7),larger,160);
    Require(bytes_admitted&&!alive[4]&&!alive[5]&&alive[6]&&alive[7]&&alive[1],
        "Byte pressure failed FIFO-only idle eviction");
    Require(pool.statistics().idle_bytes==224&&pool.statistics().evictions==3,
        "Actual idle extent or pressure accounting differs");
    // Oversized admission must leave existing idle and active owners intact.
    // A by-value unique_ptr argument may be destroyed by the caller only at
    // the end of the full expression. Observe destruction after that boundary.
    const bool oversized_admitted=pool.ReturnAfterFence(make(8),larger,257);
    Require(!oversized_admitted&&!alive[8]&&alive[6]&&alive[7]&&alive[1],
        "Rejected oversized allocation destroyed unrelated owners");
    Require(!pool.ReturnAfterFence({},key,64),"Null idle owner accepted");
    auto invalid=key;invalid.size=0;
    const bool zero_admitted=pool.ReturnAfterFence(make(9),invalid,64);
    Require(!zero_admitted&&!alive[9],"Zero descriptor accepted");
    invalid=key;invalid.device=nullptr;
    const bool no_device_admitted=pool.ReturnAfterFence(make(10),invalid,64);
    Require(!no_device_admitted&&!alive[10],"Device-less allocation accepted");
    const bool undersized_admitted=pool.ReturnAfterFence(make(11),key,47);
    Require(!undersized_admitted&&!alive[11],"Undersized actual allocation accepted");
    const bool overflow_admitted=pool.ReturnAfterFence(make(12),key,std::numeric_limits<uint64_t>::max());
    Require(!overflow_admitted&&!alive[12],
        "Overflow-sized actual allocation accepted");
    // A refill/preparation failure destroys only the locally acquired idle
    // generation. It cannot overwrite the external active generation or put
    // uncertain bytes back in the pool under a new content key.
    auto failed=pool.Take(larger);Require(failed&&failed->id==7,"Refill fixture missed retired owner");
    failed.reset();Require(!alive[7]&&alive[1]&&pool.statistics().idle_buffers==1&&
        pool.statistics().idle_bytes==64,"Failed refill damaged accepted active ownership");
    auto later=pool.Take(key);Require(later&&later->id==6&&!pool.Take(key),
        "Idle lease duplicated a unique live buffer");
    const bool former_active_admitted=pool.ReturnAfterFence(std::move(active),key,64);
    Require(former_active_admitted&&!active,
        "Explicit fenced return failed to transfer the former active owner");
    auto previous=pool.Take(key);Require(previous&&previous->id==1,"Fenced active owner was not reusable");
    Pool disabled(0,0);
    const bool disabled_admitted=disabled.ReturnAfterFence(make(13),key,64);
    Require(!disabled_admitted&&!alive[13]&&disabled.statistics().idle_bytes==0,
        "Disabled pool retained idle memory");
    bool rejected_count=false,rejected_bytes=false;
    try{Pool invalid_count(129);}catch(const std::invalid_argument&){rejected_count=true;}
    try{Pool invalid_bytes(128,Pool::kMaxBytes+1);}catch(const std::invalid_argument&){rejected_bytes=true;}
    Require(rejected_count&&rejected_bytes,"Hard pool bound widened");
    // Exercise the hard count independently of payload size and the byte bound.
    Pool hard;
    for(unsigned id=20;id<149;++id)Require(hard.ReturnAfterFence(make(id),key,64),"Hard count admission failed");
    Require(!alive[20]&&alive[21]&&hard.statistics().idle_buffers==128&&
        hard.statistics().idle_bytes==128*64&&hard.statistics().peak_idle_buffers==128&&
        hard.statistics().evictions==1,"Hard count exceeded 128 owners or evicted the wrong owner");
    Pool byte_hard;
    const RetiredBufferKey megabyte{1024*1024,3,1,false,&device_a};
    const bool hard_bytes_first=byte_hard.ReturnAfterFence(make(150),megabyte,15ull*1024*1024);
    const bool hard_bytes_second=byte_hard.ReturnAfterFence(make(151),megabyte,2ull*1024*1024);
    Require(hard_bytes_first&&hard_bytes_second&&!alive[150]&&alive[151]&&
        byte_hard.statistics().idle_bytes==2ull*1024*1024&&byte_hard.statistics().evictions==1,
        "Hard actual allocation byte pressure exceeded 16 MiB or retained the wrong idle owner");
    const bool hard_oversized_admitted=byte_hard.ReturnAfterFence(make(152),megabyte,Pool::kMaxBytes+1);
    Require(!hard_oversized_admitted&&!alive[152]&&alive[151],
        "Oversized actual allocation bypassed the hard idle byte bound");
    const auto stats=pool.statistics();
    Require(stats.hits==4&&stats.misses==7&&stats.returns==13&&stats.rejected==6&&
        stats.idle_bytes==0&&stats.idle_buffers==0&&stats.peak_idle_bytes==224,
        "Separate hit/miss/return/refusal/actual extent counters differ");
    std::cout<<"PASS: exact descriptor/device compatibility, unique fenced ownership, actual allocation budgets, bounded FIFO idle pressure, refusal and failed-refill isolation\n";
    return 0;
} catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
