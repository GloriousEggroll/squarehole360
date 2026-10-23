#pragma once
#include <cstdint>
namespace gow3::renderer {
enum class NativeRetainedOpaquePressureDecision { Skip, Poll, InvalidBudget };
// The previous actual pair size is only a poll hint, never allocation credit.
// The caller always performs the original exact allocation admission afterward.
constexpr NativeRetainedOpaquePressureDecision NativeRetainedOpaquePressurePoll(
        bool enabled,uint64_t held,uint64_t last_pair,uint64_t maximum,uint32_t inflight) noexcept {
    if(!enabled)return NativeRetainedOpaquePressureDecision::Skip;
    if(!maximum||held>maximum||last_pair>maximum)return NativeRetainedOpaquePressureDecision::InvalidBudget;
    if(!last_pair||!inflight||held<=maximum-last_pair)return NativeRetainedOpaquePressureDecision::Skip;
    return NativeRetainedOpaquePressureDecision::Poll;
}
}
