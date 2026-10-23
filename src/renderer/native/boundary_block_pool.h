#pragma once
#include "renderer/gears_bridge.h"
#include <algorithm>
#include <array>
#include <bit>
#include <utility>

namespace gow3::renderer {
// Only scratch vectors left in a synchronous boundary event may return here.
// A vector moved into an immutable frame owner is empty and is never reclaimed.
// The bridge's capture mutex serializes every call; no guest pointer is cached.
class NativeBoundaryBlockPool {
public:
    static constexpr size_t kKinds=105, kMaximumBufferBytes=64*1024,
                            kMaximumRetainedBytes=1024*1024;
    struct Statistics {
        uint64_t events=0,outer_reuses=0,buffer_requests=0,buffer_reuses=0,
                 copies=0,copied_bytes=0,oversized_refusals=0,budget_refusals=0;
        size_t retained_bytes=0,peak_retained_bytes=0;
    };
    void Begin(GraphicsBoundaryEvent& event) {
        // Begin is called on a new empty event, before its original reserve.
        event.blocks.swap(blocks_);
        copied_blocks_={};
        if(event.blocks.capacity())++stats_.outer_reuses;
        ++stats_.events;
    }
    void Prepare(uint32_t kind,size_t bytes,std::vector<uint8_t>& destination,size_t block_index) {
        ++stats_.buffer_requests;
        if(bytes&&block_index<kKinds)copied_blocks_[block_index/64]|=uint64_t(1)<<(block_index%64);
        if(kind>=kKinds||bytes>kMaximumBufferBytes||destination.capacity())return;
        auto& buffer=buffers_[kind];
        if(buffer.capacity()<bytes)return;
        stats_.retained_bytes-=buffer.capacity();
        destination.swap(buffer);
        ++stats_.buffer_reuses;
    }
    void Copied(size_t bytes) {++stats_.copies;stats_.copied_bytes+=bytes;}
    void Reclaim(GraphicsBoundaryEvent& event) noexcept {
        // Most of the103 envelope entries are empty. Visit only the entries
        // that actually copied bytes; whole-event moves simply leave no slots.
        for(size_t word=0;word<copied_blocks_.size();++word) {
          auto mask=copied_blocks_[word];
          while(mask) {
            const size_t index=word*64+std::countr_zero(mask);mask&=mask-1;
            if(index>=event.blocks.size())continue;
            auto& block=event.blocks[index];
            const auto bytes=block.bytes.capacity();
            if(!bytes||block.kind>=kKinds)continue;
            if(bytes>kMaximumBufferBytes) {++stats_.oversized_refusals;continue;}
            auto& buffer=buffers_[block.kind];
            if(bytes<=buffer.capacity())continue;
            const auto retained=stats_.retained_bytes-buffer.capacity()+bytes;
            if(retained>kMaximumRetainedBytes) {++stats_.budget_refusals;continue;}
            buffer.swap(block.bytes);buffer.clear();
            stats_.retained_bytes=retained;
            stats_.peak_retained_bytes=std::max(stats_.peak_retained_bytes,retained);
          }
        }
        // Release shared snapshots and deferred descriptors now, at exactly the
        // old event-destruction boundary. Only empty vector storage survives.
        event.blocks.clear();
        if(event.blocks.capacity()<=kKinds)blocks_.swap(event.blocks);
    }
    void Clear() { *this=NativeBoundaryBlockPool{}; }
    const Statistics& statistics() const {return stats_;}
private:
    std::array<std::vector<uint8_t>,kKinds> buffers_;
    std::array<uint64_t,2> copied_blocks_{};
    std::vector<GraphicsBoundaryBlock> blocks_;
    Statistics stats_;
};
}
