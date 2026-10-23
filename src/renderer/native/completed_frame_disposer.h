#pragma once
#include "renderer/native/live_plan.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <system_error>
#include <thread>
#include <utility>

namespace gow3::renderer {
// Completed frames contain only immutable JSON and owned CPU byte vectors.
// Never put a renderer, guest-memory view, callback, or graphics object here.
// Admission cannot wait for worker capacity: the caller retains rejected frames
// and destroys them inline. Limits include the frame currently being destroyed.
class CompletedFrameDisposer {
public:
    static constexpr size_t kMaxOutstandingFrames=2;
    // This accounts retained payload bytes, not total allocations: JSON and map
    // metadata are additionally bounded by the fixed number of retired frames.
    static constexpr size_t kMaxRetainedPayloadBytes=128u*1024*1024;
    struct Statistics {
        bool worker_started=false;
        uint64_t accepted_frames=0,completed_frames=0,inline_frames=0;
        size_t outstanding_frames=0,retained_payload_bytes=0;
        size_t peak_outstanding_frames=0,peak_retained_payload_bytes=0;
    };
    CompletedFrameDisposer() {
        try {worker_=std::thread([this]{Run();});worker_started_=true;}
        catch(const std::system_error&) {} // Optional optimization; inline release remains correct.
    }
    ~CompletedFrameDisposer() {StopAndDrain();}
    CompletedFrameDisposer(const CompletedFrameDisposer&)=delete;
    CompletedFrameDisposer& operator=(const CompletedFrameDisposer&)=delete;

    bool TryRetire(std::shared_ptr<LiveRenderFrame>& frame) {
        if(!frame)return true;
        std::unique_lock lock(mutex_,std::try_to_lock);
        if(!lock.owns_lock()||!worker_started_||closing_||
           outstanding_>=kMaxOutstandingFrames||
           frame->retained_bytes>kMaxRetainedPayloadBytes-retained_) {
            ++inline_frames_;return false;
        }
        const auto bytes=frame->retained_bytes;
        queue_[write_]={std::move(frame),bytes};
        write_=(write_+1)%queue_.size();++queued_;++outstanding_;retained_+=bytes;
        ++accepted_frames_;
        peak_outstanding_=std::max(peak_outstanding_,outstanding_);
        peak_retained_=std::max(peak_retained_,retained_);
        lock.unlock();changed_.notify_one();return true;
    }

    // Called only by the owner, after its final retirement attempt. The worker
    // drains accepted CPU data before joining and never calls back into runtime.
    void StopAndDrain() {
        {std::lock_guard lock(mutex_);closing_=true;}
        changed_.notify_one();
        if(worker_.joinable())worker_.join();
    }
    Statistics statistics() const {
        std::lock_guard lock(mutex_);
        return {worker_started_,accepted_frames_,completed_frames_,inline_frames_.load(),
            outstanding_,retained_,peak_outstanding_,peak_retained_};
    }
private:
    struct Item {std::shared_ptr<LiveRenderFrame> frame;size_t retained_bytes=0;};
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::array<Item,kMaxOutstandingFrames> queue_{};
    size_t read_=0,write_=0,queued_=0,outstanding_=0,retained_=0;
    size_t peak_outstanding_=0,peak_retained_=0;
    uint64_t accepted_frames_=0,completed_frames_=0;
    std::atomic<uint64_t> inline_frames_{0};
    bool closing_=false,worker_started_=false;
    std::thread worker_;
    void Run() {
        for(;;) {
            Item item;
            {
                std::unique_lock lock(mutex_);
                changed_.wait(lock,[this]{return closing_||queued_;});
                if(!queued_)return;
                item=std::move(queue_[read_]);
                read_=(read_+1)%queue_.size();--queued_;
            }
            // JSON trees and potentially last-owned payload bytes are released
            // outside the queue mutex, without delaying the next presentation.
            item.frame.reset();
            {
                std::lock_guard lock(mutex_);
                --outstanding_;retained_-=item.retained_bytes;++completed_frames_;
            }
        }
    }
};
}
