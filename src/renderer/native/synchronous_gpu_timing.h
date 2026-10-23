#pragma once
#include "gpu_timestamp.h"
#include "plume_vulkan.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cstddef>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>

namespace gow3::renderer {
// Presentation-only diagnostics. All markers enter existing command lists;
// results are read only after their original synchronous completion fences.
// This helper submits no work and never waits, changes barriers or owns images.
enum class NativeSynchronousGpuPhase : uint32_t { FrontbufferDecode, FrontbufferSwizzle, PresentTransition, Count };
class NativeSynchronousGpuTimings {
public:
    using Clock=std::chrono::steady_clock;
    static constexpr size_t kMaximumSamples=128;
    struct Sample {
        NativeSynchronousGpuPhase phase{};
        uint32_t frame=UINT32_MAX,width=0,height=0;
        uint64_t generation=0,begin_tick=0,end_tick=0;
        double gpu_ms=0,cpu_submit_ms=0,cpu_fence_wait_ms=0;
    };
    struct PhaseStatistics {
        uint64_t intervals=0;
        double gpu_ms=0,maximum_gpu_ms=0,cpu_submit_ms=0,cpu_fence_wait_ms=0,maximum_cpu_fence_wait_ms=0;
        Sample last{};
    };
    enum class Status { UnavailableDevice, UnavailableFamily, UnavailableProperties, UnavailablePool,
                        Enabled, ForeignCommands, InvalidScope, InterruptedScope, InvalidCompletion, UnavailableResult, WrappedResult };
    explicit NativeSynchronousGpuTimings(plume::RenderDevice& device,plume::RenderCommandQueue& queue) noexcept {
        auto* d=dynamic_cast<plume::VulkanDevice*>(&device);
        auto* q=dynamic_cast<plume::VulkanCommandQueue*>(&queue);
        if(!d||!q||q->device!=d||!d->vk||!d->physicalDevice)return;
        uint32_t count=0;vkGetPhysicalDeviceQueueFamilyProperties(d->physicalDevice,&count,nullptr);
        // No unbounded startup allocation, even for unexpected driver metadata.
        std::array<VkQueueFamilyProperties,64> families{};
        if(!count||count>families.size()){status_=Status::UnavailableFamily;return;}
        vkGetPhysicalDeviceQueueFamilyProperties(d->physicalDevice,&count,families.data());
        if(q->familyIndex>=count||!families[q->familyIndex].timestampValidBits){status_=Status::UnavailableFamily;return;}
        valid_bits_=families[q->familyIndex].timestampValidBits;
        period_ns_=d->physicalDeviceProperties.limits.timestampPeriod;
        if(!DecodeNativeGpuTimestamps(0,0,valid_bits_,period_ns_)){status_=Status::UnavailableProperties;return;}
        device_=d->vk;queue_=&queue;family_=q->familyIndex;
        VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        info.queryType=VK_QUERY_TYPE_TIMESTAMP;info.queryCount=2;
        if(vkCreateQueryPool(device_,&info,nullptr,&pool_)!=VK_SUCCESS){status_=Status::UnavailablePool;return;}
        status_=Status::Enabled;
    }
    ~NativeSynchronousGpuTimings(){if(pool_)vkDestroyQueryPool(device_,pool_,nullptr);}
    NativeSynchronousGpuTimings(const NativeSynchronousGpuTimings&)=delete;
    NativeSynchronousGpuTimings& operator=(const NativeSynchronousGpuTimings&)=delete;
    bool enabled()const noexcept{return status_==Status::Enabled;}
    uint64_t Begin(plume::RenderCommandList& commands,NativeSynchronousGpuPhase phase,uint32_t frame,uint32_t width,uint32_t height) noexcept {
        if(!enabled())return 0;
        auto* c=Commands(commands);
        if(!c)return 0;
        if(busy_||uint32_t(phase)>=uint32_t(NativeSynchronousGpuPhase::Count)||frame==UINT32_MAX||!width||!height||generation_==UINT64_MAX) {
            status_=Status::InvalidScope;++failures_;return 0;
        }
        busy_=true;ended_=false;active_commands_=c->vk;active_={phase,frame,width,height,++generation_};recorded_=Clock::now();
        vkCmdResetQueryPool(c->vk,pool_,0,2);
        vkCmdWriteTimestamp(c->vk,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,pool_,0);
        return generation_;
    }
    bool End(plume::RenderCommandList& commands,uint64_t generation) noexcept {
        if(!enabled())return false;
        auto* c=Commands(commands);
        if(!c)return false;
        if(!busy_||ended_||generation!=active_.generation||c->vk!=active_commands_){status_=Status::InvalidScope;++failures_;return false;}
        vkCmdWriteTimestamp(c->vk,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,pool_,1);ended_=true;return true;
    }
    // Called only after the caller's original waitForCommandFence returned.
    // Diagnostic failure disables reuse; pool remains owned until Scene's drain.
    void FenceCompleted(uint64_t generation,double submit_ms,double wait_ms) noexcept {
        if(!enabled())return;
        if(!busy_||!ended_||generation!=active_.generation||!std::isfinite(submit_ms)||submit_ms<0||!std::isfinite(wait_ms)||wait_ms<0) {
            status_=Status::InvalidCompletion;++failures_;return;
        }
        const auto before=Clock::now();
        struct Value {uint64_t ticks=0,available=0;};std::array<Value,2> values{};
        const auto result=vkGetQueryPoolResults(device_,pool_,0,2,sizeof(values),values.data(),sizeof(Value),
            VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        const auto after=Clock::now();++query_reads_;query_read_ms_+=Milliseconds(before,after);
        if(result!=VK_SUCCESS||!values[0].available||!values[1].available) {status_=Status::UnavailableResult;++failures_;return;}
        const auto host_ns=std::chrono::duration<double,std::nano>(after-recorded_).count();
        // Conservatively refuse an age that could contain a counter wrap.
        const auto wrap_ns=std::ldexp(period_ns_,int(valid_bits_));
        if(!std::isfinite(host_ns)||host_ns<0||host_ns>=wrap_ns){status_=Status::WrappedResult;++failures_;return;}
        const auto decoded=DecodeNativeGpuTimestamps(values[0].ticks,values[1].ticks,valid_bits_,period_ns_);
        if(!decoded){status_=Status::UnavailableResult;++failures_;return;}
        active_.begin_tick=decoded->begin_tick;active_.end_tick=decoded->end_tick;active_.gpu_ms=decoded->milliseconds;
        active_.cpu_submit_ms=submit_ms;active_.cpu_fence_wait_ms=wait_ms;
        auto& s=statistics_[size_t(active_.phase)];++s.intervals;s.gpu_ms+=active_.gpu_ms;
        s.maximum_gpu_ms=std::max(s.maximum_gpu_ms,active_.gpu_ms);s.cpu_submit_ms+=submit_ms;s.cpu_fence_wait_ms+=wait_ms;
        s.maximum_cpu_fence_wait_ms=std::max(s.maximum_cpu_fence_wait_ms,wait_ms);s.last=active_;
        samples_[next_sample_]=active_;next_sample_=(next_sample_+1)%samples_.size();
        sample_count_=std::min(sample_count_+1,samples_.size());busy_=false;ended_=false;
    }
    void Interrupt(uint64_t generation) noexcept {
        if(enabled()&&busy_&&generation==active_.generation){status_=Status::InterruptedScope;++failures_;}
    }
    void RejectOrdering(uint64_t generation)noexcept {
        if(enabled()&&busy_&&generation==active_.generation){status_=Status::InvalidCompletion;++failures_;}
    }
    nlohmann::json Report(bool detail=false)const {
        using Json=nlohmann::json;
        Json report={{"requested",true},{"supported",pool_!=VK_NULL_HANDLE},{"enabled",enabled()},
            {"status",StatusText(status_)},{"queue_family",family_},{"valid_bits",valid_bits_},{"period_ns",period_ns_},
            {"query_slots",2},{"query_reads",query_reads_},{"query_read_ms",query_read_ms_},{"failures",failures_},
            {"maximum_retained_samples",kMaximumSamples},{"retained_samples",sample_count_},{"phases",Json::object()},
            {"scope","Existing synchronous Native frontbuffer decode, optional swizzle and final transition only; GPU intervals include their recorded barriers/dependencies"},
            {"excluded",{"Prior Native draw/transfer FIFO drain","Host CPU resource allocation and descriptor preparation","SDK gamma/FXAA submission","UI bilinear/FSR/HDR paint and swapchain acquire/present","Compositor/display work"}}};
        for(size_t i=0;i<statistics_.size();++i) {
            const auto& s=statistics_[i];
            report["phases"][PhaseText(NativeSynchronousGpuPhase(i))]={{"intervals",s.intervals},
                {"gpu_interval_sum_ms",s.gpu_ms},{"maximum_gpu_ms",s.maximum_gpu_ms},{"cpu_submit_ms",s.cpu_submit_ms},
                {"cpu_fence_wait_ms",s.cpu_fence_wait_ms},{"maximum_cpu_fence_wait_ms",s.maximum_cpu_fence_wait_ms},
                {"last",s.intervals?SampleJson(s.last):Json()}};
        }
        if(detail) {
            report["samples"]=Json::array();
            for(size_t i=0;i<sample_count_;++i)report["samples"].push_back(SampleJson(samples_[(next_sample_+samples_.size()-sample_count_+i)%samples_.size()]));
        }
        return report;
    }
    static double Milliseconds(Clock::time_point begin,Clock::time_point end)noexcept{return std::chrono::duration<double,std::milli>(end-begin).count();}
private:
    plume::VulkanCommandList* Commands(plume::RenderCommandList& commands)noexcept {
        auto* c=dynamic_cast<plume::VulkanCommandList*>(&commands);
        if(!c||c->queue!=queue_||!c->vk||c->activeRenderPass!=VK_NULL_HANDLE) {status_=Status::ForeignCommands;++failures_;return nullptr;}
        return c;
    }
    static const char* PhaseText(NativeSynchronousGpuPhase phase)noexcept {
        constexpr const char* names[]{"frontbuffer_decode","frontbuffer_swizzle","present_transition"};
        return names[size_t(phase)];
    }
    static const char* StatusText(Status s)noexcept {
        constexpr const char* names[]{"unavailable device","unavailable queue family","unavailable timestamp properties","unavailable query pool",
            "enabled","disabled: foreign/active-render-pass commands","disabled: invalid scope","disabled: interrupted scope",
            "disabled: invalid fence completion","disabled: unavailable fenced result","disabled: possible counter wrap"};
        return names[size_t(s)];
    }
    static nlohmann::json SampleJson(const Sample& s) {
        return {{"phase",PhaseText(s.phase)},{"frame",s.frame},{"width",s.width},{"height",s.height},{"generation",s.generation},
            {"begin_tick",s.begin_tick},{"end_tick",s.end_tick},{"gpu_ms",s.gpu_ms},{"cpu_submit_ms",s.cpu_submit_ms},{"cpu_fence_wait_ms",s.cpu_fence_wait_ms}};
    }
    VkDevice device_=VK_NULL_HANDLE;VkQueryPool pool_=VK_NULL_HANDLE;plume::RenderCommandQueue* queue_=nullptr;
    Status status_=Status::UnavailableDevice;uint32_t family_=UINT32_MAX,valid_bits_=0;double period_ns_=0;
    VkCommandBuffer active_commands_=VK_NULL_HANDLE;
    bool busy_=false,ended_=false;uint64_t generation_=0,query_reads_=0,failures_=0;double query_read_ms_=0;
    Clock::time_point recorded_{};Sample active_{};
    std::array<PhaseStatistics,size_t(NativeSynchronousGpuPhase::Count)> statistics_{};
    std::array<Sample,kMaximumSamples> samples_{};size_t next_sample_=0,sample_count_=0;
};
struct NativeSynchronousGpuTimingContext {NativeSynchronousGpuTimings* timer=nullptr;uint32_t frame=UINT32_MAX;};
class NativeSynchronousGpuTimingScope {
public:
    using Clock=NativeSynchronousGpuTimings::Clock;
    NativeSynchronousGpuTimingScope(const NativeSynchronousGpuTimingContext* context,plume::RenderCommandList& commands,
        NativeSynchronousGpuPhase phase,uint32_t width,uint32_t height)noexcept :timer_(context?context->timer:nullptr) {
        if(timer_)generation_=timer_->Begin(commands,phase,context->frame,width,height);
    }
    ~NativeSynchronousGpuTimingScope(){if(generation_&&!completed_)timer_->Interrupt(generation_);}
    NativeSynchronousGpuTimingScope(const NativeSynchronousGpuTimingScope&)=delete;
    NativeSynchronousGpuTimingScope& operator=(const NativeSynchronousGpuTimingScope&)=delete;
    bool active()const noexcept{return generation_&&timer_->enabled();}
    void End(plume::RenderCommandList& commands)noexcept{if(active())ended_=timer_->End(commands,generation_);}
    void BeforeSubmit()noexcept {
        if(!active())return;
        if(!ended_||submit_started_){timer_->RejectOrdering(generation_);return;}
        submit_started_=true;submit_start_=Clock::now();
    }
    void BeforeFenceWait()noexcept {
        if(!active())return;
        if(!submit_started_||wait_started_){timer_->RejectOrdering(generation_);return;}
        wait_started_=true;wait_start_=Clock::now();submit_ms_=NativeSynchronousGpuTimings::Milliseconds(submit_start_,wait_start_);
    }
    void FenceCompleted()noexcept {
        if(active()) {
            if(!wait_started_)timer_->RejectOrdering(generation_);
            else timer_->FenceCompleted(generation_,submit_ms_,NativeSynchronousGpuTimings::Milliseconds(wait_start_,Clock::now()));
        }
        completed_=true;
    }
private:
    NativeSynchronousGpuTimings* timer_=nullptr;uint64_t generation_=0;bool completed_=false,ended_=false,submit_started_=false,wait_started_=false;
    Clock::time_point submit_start_{},wait_start_{};double submit_ms_=0;
};
}
