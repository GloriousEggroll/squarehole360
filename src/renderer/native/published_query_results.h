#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace gow3::renderer {
// This capability applies only to the title's blocking occlusion-result caller.
// It never authorizes a completion marker, memory reuse, or a synthetic result.
struct PublishedQueryHeader {
    uint32_t device=0, serial=0, report=0, count=0, type=0;
    uint8_t flags=0;
    uint32_t PhysicalReport() const {
        return (report&0x1fffffffu)+(((report>>20)+0x200)&0x1000);
    }
    bool Eligible() const {
        return device&&serial&&report&&type==9&&count==1&&(flags&0x40)&&
               PhysicalReport()&&!(PhysicalReport()&63);
    }
    bool SameIssue(const PublishedQueryHeader& other) const {
        return device==other.device&&serial==other.serial&&report==other.report&&
               count==other.count&&type==other.type&&(flags&0x7f)==(other.flags&0x7f);
    }
};

class PublishedQueryResults {
public:
    enum class PollStatus : uint8_t {Untracked,Unbound,WrongIssuer,Ineligible,Mismatch,Pending,Ready,Count};
    struct PollObservation {
        uint64_t sequence=0,generation=0;
        uint32_t frame=0,address=0;
        PollStatus status=PollStatus::Untracked;
    };
    struct PollCensus {
        std::array<uint64_t,size_t(PollStatus::Count)> counts{};
        PollObservation latest{};
    };
    explicit PublishedQueryResults(size_t capacity=8192,bool observe_polls=false)
        :capacity_(capacity),observe_polls_(observe_polls) {}
    bool observing_polls() const {return observe_polls_;}
    PollCensus ObservePolls() const {
        std::lock_guard lock(mutex_);return poll_census_;
    }
    // Called before the original Issue routine and on every accepted Begin.
    void Invalidate(uint32_t handle,uint32_t address=0) {
        std::lock_guard lock(mutex_);Erase(handle);EraseAddress(address);
    }
    void End(uint32_t handle,uint64_t generation,uint32_t frame,uint32_t address,
             std::thread::id issuer=std::this_thread::get_id()) {
        std::lock_guard lock(mutex_);Erase(handle);EraseAddress(address);
        if(!capacity_||!generation||!address||(address&63))return;
        if(handles_.size()>=capacity_) {handles_.clear();generations_.clear();addresses_.clear();}
        if(const auto found=generations_.find(generation);found!=generations_.end()) {
            Erase(found->second);return;
        }
        handles_.emplace(handle,Entry{generation,frame,address,issuer});
        generations_.emplace(generation,handle);addresses_.emplace(address,handle);
    }
    // The original Issue call has now written its actual device/serial/header.
    bool Bind(uint32_t handle,const PublishedQueryHeader& header,
              std::thread::id issuer=std::this_thread::get_id()) {
        std::lock_guard lock(mutex_);
        const auto found=handles_.find(handle);
        if(found==handles_.end())return false;
        auto& e=found->second;
        if(!header.Eligible()||header.PhysicalReport()!=e.address||issuer!=e.issuer||e.bound) {
            Erase(handle);return false;
        }
        e.header=header;e.bound=true;return true;
    }
    bool Publish(uint32_t frame,uint64_t generation,uint32_t address) {
        std::lock_guard lock(mutex_);
        const auto key=generations_.find(generation);if(key==generations_.end())return false;
        auto& e=handles_.at(key->second);
        if(e.frame!=frame||e.address!=address)return false;
        e.published=true;return true;
    }
    bool Ready(uint32_t handle,const PublishedQueryHeader& header,
               std::thread::id caller=std::this_thread::get_id()) const {
        std::lock_guard lock(mutex_);
        const auto found=handles_.find(handle);
        if(found==handles_.end()) {Observe(PollStatus::Untracked);return false;}
        const auto& e=found->second;
        if(!observe_polls_)return e.bound&&e.published&&caller==e.issuer&&header.Eligible()&&e.header.SameIssue(header);
        const auto status=!e.bound?PollStatus::Unbound:caller!=e.issuer?PollStatus::WrongIssuer:
            !header.Eligible()?PollStatus::Ineligible:!e.header.SameIssue(header)?PollStatus::Mismatch:
            !e.published?PollStatus::Pending:PollStatus::Ready;
        Observe(status,e.generation,e.frame,e.address);
        return status==PollStatus::Ready;
    }
private:
    struct Entry {
        uint64_t generation;uint32_t frame,address;std::thread::id issuer;
        PublishedQueryHeader header{};bool bound=false,published=false;
    };
    void Erase(uint32_t handle) {
        const auto found=handles_.find(handle);if(found==handles_.end())return;
        generations_.erase(found->second.generation);addresses_.erase(found->second.address);handles_.erase(found);
    }
    void EraseAddress(uint32_t address) {
        const auto found=addresses_.find(address);if(found!=addresses_.end())Erase(found->second);
    }
    size_t capacity_;
    const bool observe_polls_;
    mutable PollCensus poll_census_{};
    void Observe(PollStatus status,uint64_t generation=0,uint32_t frame=0,uint32_t address=0) const {
        if(!observe_polls_)return;
        ++poll_census_.counts[size_t(status)];
        auto& last=poll_census_.latest;
        last={last.sequence+1,generation,frame,address,status};
    }
    mutable std::mutex mutex_;
    std::unordered_map<uint32_t,Entry> handles_;
    std::unordered_map<uint64_t,uint32_t> generations_;
    std::unordered_map<uint32_t,uint32_t> addresses_;
};
}
