#pragma once
#include <algorithm>
#include <bit>
#include <cstdint>
#include <memory>
#include <vector>
namespace gow3::renderer {
// Recycles texture upload staging buffers by power-of-two size class. Each
// upload otherwise allocates and frees its own buffer, and the allocator then
// creates and frees whole 256 MiB host-visible blocks as the live staging total
// swings across a block boundary; the kernel clears every new block, which
// stalls the submitting thread for tens of milliseconds. Holding a bounded set
// of idle buffers keeps those blocks resident. Callers return only buffers
// whose last GPU use has retired.
template<class Buffer>class StagingBufferPool {
public:
    static constexpr uint64_t kMinClass=64*1024;
    static constexpr uint64_t kMaxClass=64ull*1024*1024;
    struct Statistics {uint64_t hits=0,misses=0,returns=0,drops=0,idle_buffers=0,idle_bytes=0,peak_idle_bytes=0;};
    explicit StagingBufferPool(uint64_t max_idle_bytes):max_idle_bytes_(max_idle_bytes) {}
    StagingBufferPool(const StagingBufferPool&)=delete;
    StagingBufferPool& operator=(const StagingBufferPool&)=delete;
    // The size to allocate for a request, so the buffer can be recycled; 0
    // when the request is too large to pool and should be allocated exactly.
    static uint64_t SizeClass(uint64_t bytes) noexcept {
        if(!bytes||bytes>kMaxClass)return 0;
        return std::max(kMinClass,std::bit_ceil(bytes));
    }
    std::unique_ptr<Buffer> Take(uint64_t bytes) {
        const uint64_t size=SizeClass(bytes);
        for(size_t i=0;size&&i<idle_.size();++i)if(idle_[i].size==size) {
            auto result=std::move(idle_[i].buffer);
            stats_.idle_bytes-=size;idle_.erase(idle_.begin()+ptrdiff_t(i));stats_.idle_buffers=idle_.size();
            ++stats_.hits;return result;
        }
        ++stats_.misses;return {};
    }
    // size is the buffer's requested size, which is its class when the pool
    // sized it. Other sizes, or a full pool, release the buffer instead.
    void Return(std::unique_ptr<Buffer> buffer,uint64_t size) {
        ++stats_.returns;
        if(!buffer||SizeClass(size)!=size||stats_.idle_bytes+size>max_idle_bytes_){++stats_.drops;return;}
        idle_.push_back({std::move(buffer),size});
        stats_.idle_bytes+=size;stats_.idle_buffers=idle_.size();
        stats_.peak_idle_bytes=std::max(stats_.peak_idle_bytes,stats_.idle_bytes);
    }
    const Statistics& statistics() const noexcept {return stats_;}
private:
    struct Idle {std::unique_ptr<Buffer> buffer;uint64_t size=0;};
    std::vector<Idle> idle_;
    uint64_t max_idle_bytes_;
    Statistics stats_;
};
} // namespace gow3::renderer
