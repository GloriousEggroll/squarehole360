#pragma once
#include <cstdint>
namespace gow3::renderer {
// Live coherence captures one immutable physical page per update. Larger or
// malformed replay updates retain the original eager completion path.
constexpr bool NativeCpuWriteQueueEligible(bool enabled,bool unified,
                                           uint32_t address,uint32_t count) {
    return enabled&&unified&&count&&count<=4096&&
        uint64_t(address)+count<=0x20000000ull;
}
// SDK publication writes the end record, and the adjacent begin record belongs
// to the same query capability. Preserve eager publication for the whole pair.
// Generic CPU writes do not supersede NativeQueryTickets, unlike query BEGIN.
constexpr bool NativeCpuWriteOverlapsQueryReport(uint32_t address,uint32_t count,
                                                uint32_t end_address) {
    if(!count||uint64_t(address)+count>0x20000000ull||end_address%64||
       uint64_t(end_address)+64>0x20000000ull)return true;
    return uint64_t(address)<uint64_t(end_address)+64&&
        uint64_t(end_address)<uint64_t(address)+count;
}
}
