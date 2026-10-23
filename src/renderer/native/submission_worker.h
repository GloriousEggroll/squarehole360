#pragma once
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <type_traits>

namespace gow3::renderer {
// Single producer, single ordered consumer. Slots belong to the caller's fixed
// fence ring; a receipt proves only that the driver call returned, NOT that GPU
// work completed. The caller retains all resource owners until both proofs.
// A failed driver call poisons the worker and cancels every later request. It
// never retries uncertain work. Destruction joins before borrowed objects die.
template<class Request, size_t Capacity>
class NativeSubmissionWorker {
    static_assert(Capacity>0&&std::is_trivially_copyable_v<Request>);
public:
    using Clock=std::chrono::steady_clock;
    struct Receipt {Clock::time_point began{};double submit_ms=0;};
    using Execute=std::function<void(const Request&)>;
    explicit NativeSubmissionWorker(Execute execute,bool timing=false)
        :execute_(std::move(execute)),timing_(timing) {
        if(!execute_)throw std::invalid_argument("Missing Native submission executor");
        worker_=std::thread([this]{Run();});
    }
    ~NativeSubmissionWorker() {
        {std::lock_guard lock(mutex_);stopping_=true;}
        ready_.notify_one();
        worker_.join();
    }
    NativeSubmissionWorker(const NativeSubmissionWorker&)=delete;
    NativeSubmissionWorker& operator=(const NativeSubmissionWorker&)=delete;
    void Submit(size_t index,uint64_t generation,const Request& request) {
        {
            std::lock_guard lock(mutex_);
            if(failure_)std::rethrow_exception(failure_);
            if(stopping_||!generation||index>=Capacity||slots_[index].generation||count_==Capacity)
                throw std::logic_error("Native submission slot reused before its driver receipt");
            auto& slot=slots_[index];
            slot.request=request;slot.receipt={};slot.done=false;slot.generation=generation;
            fifo_[(head_+count_)%Capacity]=index;++count_;
        }
        ready_.notify_one();
    }
    // Checking a receipt never polls or waits on a GPU fence. The producer must
    // collect it before accessing/resetting that fence or its command buffers.
    std::optional<Receipt> Collect(size_t index,uint64_t generation,bool wait) {
        std::unique_lock lock(mutex_);
        if(index>=Capacity||!generation||slots_[index].generation!=generation)
            throw std::logic_error("Native driver receipt generation mismatch");
        auto& slot=slots_[index];
        if(wait)completed_.wait(lock,[&]{return slot.done;});
        if(!slot.done)return {};
        if(failure_)std::rethrow_exception(failure_);
        const auto receipt=slot.receipt;
        slot.request={};slot.generation=0;slot.done=false;
        return receipt;
    }
private:
    struct Slot {Request request{};Receipt receipt{};uint64_t generation=0;bool done=false;};
    std::array<Slot,Capacity> slots_{};
    std::array<size_t,Capacity> fifo_{};
    size_t head_=0,count_=0;
    Execute execute_;
    bool timing_=false,stopping_=false;
    std::exception_ptr failure_;
    std::mutex mutex_;
    std::condition_variable ready_,completed_;
    std::thread worker_;
    void Run() noexcept {
        std::unique_lock lock(mutex_);
        for(;;) {
            ready_.wait(lock,[&]{return stopping_||count_;});
            if(!count_)return;
            auto& slot=slots_[fifo_[head_]];
            // Keep this entry occupied while outside the mutex. Submit cannot
            // overwrite it; Collect cannot release it until done is published.
            if(!failure_) {
                lock.unlock();
                Receipt receipt;
                if(timing_)receipt.began=Clock::now();
                std::exception_ptr error;
                try {execute_(slot.request);}catch(...){error=std::current_exception();}
                if(timing_)receipt.submit_ms=std::chrono::duration<double,std::milli>(Clock::now()-receipt.began).count();
                lock.lock();slot.receipt=receipt;
                if(error)failure_=error;
            }
            slot.done=true;head_=(head_+1)%Capacity;--count_;
            completed_.notify_all();
        }
    }
};
}
