#pragma once
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

namespace gow3::renderer {
struct NativeGpuTimestampInterval {
    uint64_t begin_tick=0,end_tick=0,elapsed_ticks=0;
    double milliseconds=0;
};
// Subtract in the queue family's actual counter width. The caller rejects
// intervals whose host lifetime could contain more than one counter wrap.
inline std::optional<NativeGpuTimestampInterval> DecodeNativeGpuTimestamps(
    uint64_t begin,uint64_t end,uint32_t valid_bits,double period_ns) {
    if(!valid_bits||valid_bits>64||!std::isfinite(period_ns)||period_ns<=0)return {};
    const uint64_t mask=valid_bits==64?std::numeric_limits<uint64_t>::max():(uint64_t{1}<<valid_bits)-1;
    begin&=mask;end&=mask;
    const uint64_t ticks=(end-begin)&mask;
    const double milliseconds=double(ticks)*(period_ns/1000000.0);
    if(!std::isfinite(milliseconds))return {};
    return NativeGpuTimestampInterval{begin,end,ticks,milliseconds};
}
}
