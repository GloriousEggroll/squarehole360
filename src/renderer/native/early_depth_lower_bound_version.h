#pragma once
#include "readonly_depth_version.h"
#include <cstdint>
#include <optional>
namespace gow3::renderer {
// The image is an immutable lower bound, never current canonical depth. Keep
// the anchor version fixed while separately certifying each accepted monotonic
// operation's exact physical-page version transition.
class NativeEarlyDepthLowerBoundVersion {
public:
    struct Ticket {uint64_t generation=0,anchor=0,current=0;};
    NativeEarlyDepthLowerBoundVersion(uint64_t generation,NativeReadonlyDepthLayoutKey layout,
        uint64_t source) noexcept: generation_(generation),layout_(layout),anchor_(source),current_(source),
        valid_(generation&&source) {}
    bool Reusable(const NativeReadonlyDepthLayoutKey& layout,uint64_t observed) const noexcept {
        return valid_&&observed&&layout_==layout&&current_==observed;
    }
    std::optional<Ticket> Capture(const NativeReadonlyDepthLayoutKey& layout,uint64_t observed) const noexcept {
        if(!Reusable(layout,observed)) return {};
        return Ticket{generation_,anchor_,current_};
    }
    // Call only at the original accepted Draw metadata transition, with a
    // independently authenticated GE/stencilOFF/no-alias/no-external-writes
    // proof. This never alters the image or advertises an exact readonly token.
    bool PromoteAcceptedMonotonic(const NativeReadonlyDepthLayoutKey& layout,Ticket ticket,
        uint64_t pre,uint64_t post,bool accepted,bool authenticated_monotonic,bool discarded) noexcept {
        if(!accepted||!authenticated_monotonic||discarded||!Reusable(layout,pre)||post<=pre||
           ticket.generation!=generation_||ticket.anchor!=anchor_||ticket.current!=current_)return false;
        current_=post;return true;
    }
    void Invalidate() noexcept {valid_=false;}
    uint64_t generation()const noexcept{return generation_;}
    uint64_t anchor()const noexcept{return anchor_;}
    uint64_t current()const noexcept{return current_;}
    const NativeReadonlyDepthLayoutKey& layout()const noexcept{return layout_;}
private:
    const uint64_t generation_;
    const NativeReadonlyDepthLayoutKey layout_;
    const uint64_t anchor_;
    uint64_t current_;
    bool valid_;
};
}
