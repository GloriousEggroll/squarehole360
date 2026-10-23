#pragma once
#include "renderer/gears_bridge.h"
#include <stdexcept>
#include <thread>
#include <utility>

namespace gow3::renderer {
// A lazy texture descriptor is not a queued guest-memory pointer. Its capture
// capability expires with the synchronous D3D observation and may only be used
// by that caller. Only the resulting immutable bytes enter a native frame.
class DeferredTextureSnapshotScope {
public:
    using Bytes=std::shared_ptr<const std::vector<uint8_t>>;
    using Capture=std::function<Bytes(uint32_t,uint32_t)>;
    struct Request {uint32_t address=0,bytes=0;};
    using CaptureBatch=std::function<std::vector<Bytes>(std::span<const Request>)>;
    DeferredTextureSnapshotScope():owner_(std::make_shared<const std::thread::id>(std::this_thread::get_id())){}
    DeferredTextureSnapshotScope(const DeferredTextureSnapshotScope&)=delete;
    DeferredTextureSnapshotScope& operator=(const DeferredTextureSnapshotScope&)=delete;
    std::function<void(GraphicsBoundaryBlock&)> Bind(Capture capture) const {
        if(!capture)throw std::runtime_error("Missing deferred texture snapshot provider");
        return [owner=std::weak_ptr(owner_),capture=std::move(capture)](GraphicsBoundaryBlock& block) {
            const auto lease=owner.lock();
            if(!lease||*lease!=std::this_thread::get_id())
                throw std::runtime_error("Deferred texture snapshot escaped its D3D caller");
            const auto count=block.deferred_physical_bytes;
            if(block.kind<32||block.kind>95||!count||count>8u*1024*1024||
               uint64_t(block.address)+count>0x20000000ull||!block.bytes.empty()||block.shared_bytes)
                throw std::runtime_error("Invalid deferred texture snapshot descriptor");
            auto bytes=capture(block.address,count);
            // An unreadable allocation remains explicitly missing, as in the
            // eager capture path. A partial snapshot must never masquerade as
            // the captured allocation extent.
            if(bytes&&!bytes->empty()&&bytes->size()!=count)
                throw std::runtime_error("Deferred texture snapshot extent mismatch");
            block.shared_bytes=std::move(bytes);
            block.deferred_physical_bytes=0;
        };
    }
    std::function<void(std::span<GraphicsBoundaryBlock* const>)> BindBatch(CaptureBatch capture) const {
        if(!capture)throw std::runtime_error("Missing deferred texture batch provider");
        return [owner=std::weak_ptr(owner_),capture=std::move(capture)](std::span<GraphicsBoundaryBlock* const> blocks) {
            const auto lease=owner.lock();
            if(!lease||*lease!=std::this_thread::get_id())
                throw std::runtime_error("Deferred texture batch escaped its D3D caller");
            if(blocks.empty()||blocks.size()>64)throw std::runtime_error("Invalid deferred texture batch size");
            std::array<Request,64> requests{};
            for(size_t i=0;i<blocks.size();++i) {
                const auto* block=blocks[i];
                if(!block||block->kind<32||block->kind>95||!block->deferred_physical_bytes||
                   block->deferred_physical_bytes>8u*1024*1024||
                   uint64_t(block->address)+block->deferred_physical_bytes>0x20000000ull||
                   !block->bytes.empty()||block->shared_bytes)
                    throw std::runtime_error("Invalid deferred texture batch descriptor");
                for(size_t j=0;j<i;++j)if(blocks[j]==block)
                    throw std::runtime_error("Repeated deferred texture block");
                requests[i]={block->address,block->deferred_physical_bytes};
            }
            auto bytes=capture(std::span<const Request>(requests).first(blocks.size()));
            if(bytes.size()!=blocks.size())throw std::runtime_error("Deferred texture batch result mismatch");
            // Validate EVERY result before mutating ANY descriptor. Unreadable
            // views stay explicitly missing, as with the original single path.
            for(size_t i=0;i<blocks.size();++i)
                if(bytes[i]&&!bytes[i]->empty()&&bytes[i]->size()!=requests[i].bytes)
                    throw std::runtime_error("Deferred texture batch extent mismatch");
            for(size_t i=0;i<blocks.size();++i) {
                blocks[i]->shared_bytes=std::move(bytes[i]);blocks[i]->deferred_physical_bytes=0;
            }
        };
    }
private:
    std::shared_ptr<const std::thread::id> owner_;
};
}
