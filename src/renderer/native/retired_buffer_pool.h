#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <utility>
namespace gow3::renderer {
// An explicit fenced-return pool. This owns only idle unique buffers; callers
// must not return a buffer at a partial drain or merely because its key changed.
struct RetiredBufferKey {
    uint64_t size=0;
    uint32_t flags=0,heap=0;
    bool committed=false;
    const void* device=nullptr;
    bool operator==(const RetiredBufferKey&) const = default;
};
template<class Buffer>class RetiredBufferPool {
public:
    static constexpr uint32_t kMaxBuffers=128;
    static constexpr uint64_t kMaxBytes=16ull*1024*1024;
    struct Statistics {
        uint64_t hits=0,misses=0,returns=0,evictions=0,rejected=0;
        uint64_t idle_buffers=0,idle_bytes=0,peak_idle_buffers=0,peak_idle_bytes=0;
    };
private:
    struct Slot {std::unique_ptr<Buffer> buffer;RetiredBufferKey key;uint64_t bytes=0;};
    std::array<Slot,kMaxBuffers> idle_{};
    uint32_t count_=0,max_buffers_;
    uint64_t max_bytes_;
    Statistics stats_;
    std::unique_ptr<Buffer> Remove(uint32_t index) noexcept {
        auto result=std::move(idle_[index].buffer);stats_.idle_bytes-=idle_[index].bytes;
        for(uint32_t i=index+1;i<count_;++i)idle_[i-1]=std::move(idle_[i]);
        idle_[--count_]=Slot{};stats_.idle_buffers=count_;return result;
    }
public:
    explicit RetiredBufferPool(uint32_t count=kMaxBuffers,uint64_t bytes=kMaxBytes)
        :max_buffers_(count),max_bytes_(bytes) {
        if(count>kMaxBuffers||bytes>kMaxBytes)throw std::invalid_argument("Retired buffer pool exceeds hard limits");
    }
    RetiredBufferPool(const RetiredBufferPool&)=delete;
    RetiredBufferPool& operator=(const RetiredBufferPool&)=delete;
    std::unique_ptr<Buffer> Take(const RetiredBufferKey& key) noexcept {
        for(uint32_t i=0;i<count_;++i)if(idle_[i].key==key) {
            ++stats_.hits;return Remove(i);
        }
        ++stats_.misses;return {};
    }
    // This non-allocating, noexcept hook receives ONLY fully fenced owners.
    // Actual allocation bytes, not payload size, determine idle admission.
    bool ReturnAfterFence(std::unique_ptr<Buffer> buffer,RetiredBufferKey key,uint64_t bytes) noexcept {
        ++stats_.returns;
        if(!buffer||!key.size||!key.device||bytes<key.size||!max_buffers_||bytes>max_bytes_) {
            ++stats_.rejected;return false;
        }
        while(count_&&(count_==max_buffers_||bytes>max_bytes_-stats_.idle_bytes)) {
            auto retired=Remove(0);++stats_.evictions;
        }
        idle_[count_++]={std::move(buffer),key,bytes};stats_.idle_buffers=count_;stats_.idle_bytes+=bytes;
        if(stats_.idle_buffers>stats_.peak_idle_buffers)stats_.peak_idle_buffers=stats_.idle_buffers;
        if(stats_.idle_bytes>stats_.peak_idle_bytes)stats_.peak_idle_bytes=stats_.idle_bytes;
        return true;
    }
    const Statistics& statistics() const noexcept {return stats_;}
};
}
