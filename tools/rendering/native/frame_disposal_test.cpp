#include "renderer/native/completed_frame_disposer.h"
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

using namespace gow3::renderer;
namespace {
void Require(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
bool RetireEventually(CompletedFrameDisposer& disposer,std::shared_ptr<LiveRenderFrame>& frame) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    // A try-lock rejection during worker startup is permitted. Keep ownership
    // and retry only in this test; the real presentation path releases inline.
    do {if(disposer.TryRetire(frame))return true;std::this_thread::yield();}
    while(std::chrono::steady_clock::now()<deadline);
    return false;
}
struct BlockedRelease {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered=false,released=false;
    std::thread::id thread;
    std::shared_ptr<LiveRenderFrame> Make(size_t retained) {
        auto frame=std::shared_ptr<LiveRenderFrame>(new LiveRenderFrame,[this](auto* value) {
            {
                std::unique_lock lock(mutex);thread=std::this_thread::get_id();entered=true;changed.notify_all();
                changed.wait(lock,[this]{return released;});
            }
            delete value;
        });
        frame->retained_bytes=retained;return frame;
    }
    void WaitEntered() {
        std::unique_lock lock(mutex);
        Require(changed.wait_for(lock,std::chrono::seconds(5),[this]{return entered;}),"Worker did not begin CPU release");
    }
    void Release() {{std::lock_guard lock(mutex);released=true;}changed.notify_all();}
};
void CheckLimits(size_t first_bytes,size_t rejected_bytes) {
    BlockedRelease blocked;
    CompletedFrameDisposer disposer;
    Require(disposer.statistics().worker_started,"Test worker did not start");
    auto first=blocked.Make(first_bytes);
    Require(RetireEventually(disposer,first)&&!first,"First frame was not transferred");
    blocked.WaitEntered();
    try {
        auto second=std::make_shared<LiveRenderFrame>();second->retained_bytes=rejected_bytes;
        const auto* identity=second.get();
        if(first_bytes+rejected_bytes>CompletedFrameDisposer::kMaxRetainedPayloadBytes) {
            Require(!disposer.TryRetire(second)&&second.get()==identity,"Retained-payload budget did not preserve caller ownership");
            second->retained_bytes=CompletedFrameDisposer::kMaxRetainedPayloadBytes-first_bytes;
        }
        Require(disposer.TryRetire(second)&&!second,"Second frame was not queued while worker was occupied");
        auto third=std::make_shared<LiveRenderFrame>();const auto* third_identity=third.get();
        Require(!disposer.TryRetire(third)&&third.get()==third_identity,"Two-frame limit excluded the frame being destroyed");
        const auto busy=disposer.statistics();
        Require(busy.outstanding_frames==2&&busy.peak_outstanding_frames==2&&
                busy.retained_payload_bytes==CompletedFrameDisposer::kMaxRetainedPayloadBytes,
                "Outstanding disposal accounting was not bounded");
        blocked.Release();disposer.StopAndDrain();
        const auto done=disposer.statistics();
        Require(done.accepted_frames==2&&done.completed_frames==2&&!done.outstanding_frames&&
                !done.retained_payload_bytes&&done.inline_frames>=1,"Shutdown did not drain queued CPU frames");
        Require(blocked.thread!=std::this_thread::get_id(),"Frame destruction ran on the presentation thread");
        Require(!disposer.TryRetire(third)&&third.get()==third_identity,"Closed disposer accepted a new frame");
    }catch(...) {blocked.Release();throw;}
}
void CheckLifetimes() {
    std::weak_ptr<LiveRenderFrame> frame_lifetime;
    std::weak_ptr<const std::vector<uint8_t>> unshared_lifetime;
    LiveOperations prefix;
    std::shared_ptr<const std::vector<uint8_t>> shared_payload;
    const nlohmann::json expected={{"type","draw"},{"nested",{{"texture","immutable"},{"values",{1,2,3}}}}};
    {
        CompletedFrameDisposer disposer;
        auto frame=std::make_shared<LiveRenderFrame>();frame_lifetime=frame;
        frame->operations.push_back(expected);prefix=frame->operations.Slice(0,1);
        shared_payload=std::make_shared<const std::vector<uint8_t>>(4096,0x58);
        frame->payloads.emplace("shared",shared_payload);
        frame->payloads.emplace("last_owned",std::make_shared<const std::vector<uint8_t>>(4096,0x99));
        unshared_lifetime=frame->payloads.at("last_owned");frame->retained_bytes=8192;
        Require(RetireEventually(disposer,frame)&&!frame,"CPU frame did not transfer");
        // Destructor must drain without an explicit StopAndDrain call.
    }
    Require(frame_lifetime.expired()&&unshared_lifetime.expired(),"Worker shutdown leaked frame or last-owned CPU bytes");
    Require(prefix.size()==1&&prefix.at(0).ToJson()==expected&&shared_payload->at(4095)==0x58,
            "Worker invalidated independently retained operation or payload references");
    CompletedFrameDisposer disposer;
    auto oversized=std::make_shared<LiveRenderFrame>();oversized->retained_bytes=CompletedFrameDisposer::kMaxRetainedPayloadBytes+1;
    Require(!disposer.TryRetire(oversized)&&oversized,"Oversized frame was retained by the worker");
    std::shared_ptr<LiveRenderFrame> empty;
    Require(disposer.TryRetire(empty)&&disposer.statistics().accepted_frames==0,"Empty retirement consumed worker capacity");
}
}
int main() try {
    constexpr size_t mib=1024*1024;
    CheckLimits(64*mib,64*mib);
    CheckLimits(96*mib,64*mib);
    CheckLifetimes();
    std::cout<<"PASS: CPU disposal thread, two outstanding frames, retained-payload budget, rejection ownership, shutdown drain, immutable reference lifetime\n";
}catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
