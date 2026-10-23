#pragma once
#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <utility>
#include <vector>

namespace gow3::renderer {
// Exact equality only. Every byte of an equal range is read on every call;
// physical writes, address reuse and missing notifications never become hits.
// Workers borrow disjoint spans synchronously and release them before Equal
// returns, including when another comparison range throws. They never own
// graphics state, guest memory, a runtime bridge, or an immutable snapshot.
class ParallelSnapshotCompare {
public:
    using ByteSpan=std::span<const uint8_t>;
    static constexpr size_t kThreshold=256u*1024;
    static constexpr size_t kCoarseRangeBytes=128u*1024;
    static constexpr unsigned kLegacyHelpers=2;
    static unsigned HardwareHelperLimit(unsigned threads=std::thread::hardware_concurrency()) noexcept {
        return threads>2?threads-2:0;
    }
    // A request>2 never wakes a worker for a tiny partition. The caller also
    // owns at least one coarse range, and the final helper owns the entire tail.
    static unsigned ActiveHelperCount(size_t bytes,unsigned helpers,bool coarse) noexcept {
        if(!coarse)return helpers;
        const size_t ranges=bytes/kCoarseRangeBytes;
        return ranges>1?unsigned(std::min(size_t(helpers),ranges-1)):0;
    }
    struct TestHooks {
        // Startup/failure seams only; defaults use std::thread and memcmp.
        // A launcher must return its started thread owner, never detach or retain
        // an entry on failure. A compare must be thread-safe and not retain spans.
        std::function<std::thread(std::function<void()>)> launch;
        std::function<bool(ByteSpan,ByteSpan)> compare;
        // Pure fixtures can exercise partial/large pools on a small machine.
        // Production never supplies an override and always clamps before alloc.
        std::optional<unsigned> hardware_threads;
    };
    explicit ParallelSnapshotCompare(unsigned requested_helpers=0,
                                     size_t threshold=kThreshold,TestHooks hooks={})
        :threshold_(std::max(size_t(128),threshold)),coarse_(requested_helpers>kLegacyHelpers),hooks_(std::move(hooks)) {
        const auto available=HardwareHelperLimit(hooks_.hardware_threads?
            *hooks_.hardware_threads:std::thread::hardware_concurrency());
        const auto limit=std::min(requested_helpers,available);
        // Allocation and launch failures leave an exact scalar/partial pool.
        // Each worker has a stable object address independent of vector growth.
        // No entry captures or reads the vector while startup may mutate it.
        for(unsigned i=0;i<limit;++i) {
            try {
                workers_.push_back(std::make_unique<WorkerState>());
                auto* worker=workers_.back().get();
                try {
                    auto entry=[this,worker]{Worker(*worker);};
                    worker->thread=hooks_.launch?hooks_.launch(entry):std::thread(entry);
                    if(!worker->thread.joinable()){workers_.pop_back();break;}
                } catch(...) {workers_.pop_back();break;}
            } catch(...) {break;}
        }
        helpers_=unsigned(workers_.size());
    }
    ~ParallelSnapshotCompare(){StopAndDrain();}
    ParallelSnapshotCompare(const ParallelSnapshotCompare&)=delete;
    ParallelSnapshotCompare& operator=(const ParallelSnapshotCompare&)=delete;
    ParallelSnapshotCompare(ParallelSnapshotCompare&&)=delete;
    ParallelSnapshotCompare& operator=(ParallelSnapshotCompare&&)=delete;

    unsigned helpers()const noexcept{return helpers_;}
    size_t threshold()const noexcept{return threshold_;}

    bool Equal(ByteSpan a,ByteSpan b) {
        if(a.size()!=b.size())return false;
        if(a.empty())return true;
        if(!helpers_||a.size()<threshold_)return Compare(a,b);
        const unsigned active=ActiveHelperCount(a.size(),helpers_,coarse_);
        if(!active)return Compare(a,b);
        // Preserve the original default<=2 threshold/probes/64-byte partition.
        // Successful probes never skip any byte in the complete comparison.
        if(std::memcmp(a.data(),b.data(),64)||
           std::memcmp(a.data()+a.size()-64,b.data()+b.size()-64,64))return false;
        const size_t chunk=(a.size()/(size_t(active)+1))&~size_t(63);
        if(!chunk)return Compare(a,b);
        std::unique_lock admission(admission_mutex_);
        if(stopped_)return Compare(a,b);
        {
            std::lock_guard state(state_mutex_);
            for(unsigned i=0;i<active;++i) {
                auto& worker=*workers_[i];
                const size_t offset=chunk*(i+1);
                worker.job={a.data()+offset,b.data()+offset,
                    i+1==active?a.size()-offset:chunk};
                worker.equal=false;worker.error={};worker.assigned=true;
            }
            remaining_=active;
        }
        // Sleeping inactive workers never observe a shared job generation and
        // cannot consume an empty/stale span even after a spurious wakeup.
        for(unsigned i=0;i<active;++i)workers_[i]->available.notify_one();
        bool equal=false;std::exception_ptr error;
        try {equal=Compare(a.first(chunk),b.first(chunk));}
        catch(...) {error=std::current_exception();}
        {
            std::unique_lock state(state_mutex_);
            completed_.wait(state,[this]{return !remaining_;});
            for(unsigned i=0;i<active;++i) {
                auto& worker=*workers_[i];
                equal=equal&&worker.equal;
                if(!error&&worker.error)error=worker.error;
                worker.error={};
            }
        }
        if(error)std::rethrow_exception(error);
        return equal;
    }

    // May race with a still-running Equal while this object remains alive.
    // Admission serializes shutdown behind every dispatched borrowed range.
    // Later calls remain synchronous. Short scalar calls retain the original
    // synchronous caller/object-lifetime contract; there is no queued work.
    void StopAndDrain() {
        std::lock_guard admission(admission_mutex_);
        if(stopped_)return;
        {
            std::lock_guard state(state_mutex_);stopped_=true;
        }
        for(auto& worker:workers_)worker->available.notify_one();
        for(auto& worker:workers_)if(worker->thread.joinable())worker->thread.join();
    }
    // Explicit lifecycle assertion for the focused test, not a hot-path poll.
    bool Idle()const {
        std::lock_guard state(state_mutex_);
        return !remaining_&&std::all_of(workers_.begin(),workers_.end(),[](const auto& worker) {
            return !worker->assigned&&!worker->job.a&&!worker->job.b&&!worker->job.bytes;
        });
    }
private:
    struct Job {const uint8_t* a=nullptr;const uint8_t* b=nullptr;size_t bytes=0;};
    struct WorkerState {
        std::condition_variable available;
        std::thread thread;
        Job job;
        std::exception_ptr error;
        bool assigned=false,equal=false;
    };
    bool Compare(ByteSpan a,ByteSpan b)const {
        return hooks_.compare?hooks_.compare(a,b):!std::memcmp(a.data(),b.data(),a.size());
    }
    void Worker(WorkerState& worker) {
        for(;;) {
            bool equal=false;std::exception_ptr error;
            {
                Job job;
                {
                    std::unique_lock state(state_mutex_);
                    worker.available.wait(state,[&]{return stopped_||worker.assigned;});
                    if(stopped_)return;
                    job=worker.job;worker.assigned=false;
                }
                try {equal=Compare({job.a,job.bytes},{job.b,job.bytes});}
                catch(...) {error=std::current_exception();}
                // Local borrowed pointers die before completion can allow
                // caller destruction, reuse or unmapping of the source bytes.
            }
            {
                std::lock_guard state(state_mutex_);
                worker.job={};worker.equal=equal;worker.error=std::move(error);
                if(!--remaining_)completed_.notify_one();
            }
        }
    }
    const size_t threshold_;
    const bool coarse_;
    TestHooks hooks_;
    unsigned helpers_=0;
    std::mutex admission_mutex_;
    mutable std::mutex state_mutex_;
    std::condition_variable completed_;
    unsigned remaining_=0;
    bool stopped_=false;
    std::vector<std::unique_ptr<WorkerState>> workers_;
};
} // namespace gow3::renderer
