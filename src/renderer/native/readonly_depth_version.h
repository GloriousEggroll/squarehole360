#pragma once
#include <array>
#include <cstdint>
#include <optional>
namespace gow3::renderer {
// Exact immutable-generation policy only. No GPU/resource owner or ordinary
// epoch certificate is retained. Callers compare current exact physical-page
// SurfaceVersion and publish a fresh conversion only after its FIFO seal.
using NativeReadonlyDepthLayoutKey=std::array<uint32_t,6>; // width,height,pitch,base,format,depth.
class NativeReadonlyDepthVersion {
public:
    struct Ticket {uint64_t generation=0,source=0;};
    NativeReadonlyDepthVersion(uint64_t generation,NativeReadonlyDepthLayoutKey layout,uint64_t source) noexcept
        :generation_(generation),layout_(layout),source_(source) {}
    bool Reusable(const NativeReadonlyDepthLayoutKey& layout,uint64_t current) const noexcept {
        return generation_&&source_&&current&&layout_==layout&&source_==current;
    }
    std::optional<Ticket> Capture(const NativeReadonlyDepthLayoutKey& layout,uint64_t current) const noexcept {
        if(!Reusable(layout,current))return {};return Ticket{generation_,source_};
    }
    // Invoke at the one accepted audited readonly draw's existing metadata
    // RecordSurfaceWrite call, with exact versions immediately before/after it.
    // Never use a captured stale 'pre', and never promote another generation.
    // Failure leaves the old token intact: next lookup must recheck/reconvert.
    bool PromoteAcceptedReadonly(const NativeReadonlyDepthLayoutKey& layout,Ticket ticket,
        uint64_t observed_pre,uint64_t observed_post,bool accepted,bool audited_no_write,bool discarded) noexcept {
        if(!accepted||!audited_no_write||discarded||ticket.generation!=generation_||ticket.source!=source_||
            !Reusable(layout,observed_pre)||observed_post<=observed_pre)return false;
        source_=observed_post;return true;
    }
    // No physical write occurred. Selection observed the exact current source
    // through Capture, and no intervening physical metadata writer is allowed.
    // Unlike PromoteAcceptedReadonly this preserves the original source token.
    bool AcceptUnchangedReadonly(const NativeReadonlyDepthLayoutKey& layout,Ticket ticket,
        uint64_t selection_serial,uint64_t current_serial,bool accepted,bool audited_no_write,bool discarded) const noexcept {
        return accepted&&audited_no_write&&!discarded&&selection_serial&&selection_serial==current_serial&&
            ticket.generation==generation_&&ticket.source==source_&&Reusable(layout,ticket.source);
    }
    uint64_t source() const noexcept {return source_;}
    uint64_t generation() const noexcept {return generation_;}
    const NativeReadonlyDepthLayoutKey& layout() const noexcept {return layout_;}
private:
    const uint64_t generation_;
    const NativeReadonlyDepthLayoutKey layout_;
    uint64_t source_;
};
}
