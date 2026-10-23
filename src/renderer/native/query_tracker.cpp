#include "query_tracker.h"
#include <array>
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace gow3::renderer {
NativeQueryTracker::NativeQueryTracker(plume::RenderDevice& device,uint32_t capacity,bool host_reset)
    :capacity_(capacity),device_(static_cast<plume::VulkanDevice&>(device).vk) {
    VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    info.queryType=VK_QUERY_TYPE_OCCLUSION;info.queryCount=capacity_.lists;
    if(vkCreateQueryPool(device_,&info,nullptr,&pool_)!=VK_SUCCESS)
        throw std::runtime_error("Cannot create native occlusion query pool");
    // A function pointer alone is not proof that hostQueryReset was enabled.
    // The borrowed adapter publishes the actual vkCreateDevice feature chain.
    if(host_reset&&static_cast<plume::VulkanDevice&>(device).hostQueryResetEnabled)
        host_reset_=reinterpret_cast<PFN_vkResetQueryPool>(vkGetDeviceProcAddr(device_,"vkResetQueryPool"));
    if(host_reset_)ResetRetiredRange(0,capacity_.lists); // No work has referenced this pool.
}
void NativeQueryTracker::ResetRetiredRange(uint32_t first,uint32_t count) {
    if(!count)return;
    host_reset_(device_,pool_,first,count);
    ++reset_statistics_.host_calls;reset_statistics_.host_slots+=count;
}
NativeQueryTracker::~NativeQueryTracker() {
    if(pool_)vkDestroyQueryPool(device_,pool_,nullptr);
}
void NativeQueryTracker::Begin(uint32_t handle,uint32_t address) {
    if(active_record_||prepared_||recording_)throw std::runtime_error("Nested native occlusion query");
    if(!handle||!address||(address&63)!=32)throw std::runtime_error("Invalid native query begin report");
    for(uint32_t i=0;i<records_.size();++i)if(!records_[i].used) {
        records_[i]={{handle,address,0,0,0},0,true};active_record_=i;return;
    }
    throw std::runtime_error("Native logical query pool is full");
}
NativeQueryTracker::Record& NativeQueryTracker::CheckedEnd(uint32_t handle,uint32_t address) {
    if(!active_record_||prepared_||recording_)
        throw std::runtime_error("Unmatched native occlusion query end");
    auto& record=records_[*active_record_];
    if(handle!=record.result.handle||address!=record.result.begin_address-32)
        throw std::runtime_error("Unmatched native occlusion query end");
    return record;
}
NativeQueryTracker::Result NativeQueryTracker::End(uint32_t handle,uint32_t address) {
    auto& record=CheckedEnd(handle,address);
    if(pending_count_)throw std::runtime_error("Native occlusion query end before draw completion");
    auto result=record.result;result.end_address=address;
    record={};active_record_.reset();return result;
}
void NativeQueryTracker::EndDeferred(uint32_t handle,uint32_t address) {
    auto& record=CheckedEnd(handle,address);
    if(closed_count_==kMaxDeferredQueries)
        throw std::runtime_error("Native deferred query queue is full; retire before another end");
    record.result.end_address=address;
    closed_[closed_count_++]=*active_record_;active_record_.reset();
}
std::vector<NativeQueryTracker::Result> NativeQueryTracker::TakeDeferredResults() {
    if(pending_count_||prepared_||recording_)
        throw std::runtime_error("Native deferred query results before draw completion");
    return TakeDeferredPrefix(closed_count_);
}
std::vector<NativeQueryTracker::Result> NativeQueryTracker::TakeDeferredPrefix(uint32_t count) {
    if(prepared_||recording_||count>closed_count_)
        throw std::runtime_error("Invalid native query completed prefix");
    if(!count)return {};
    std::vector<Result> results;results.reserve(count);
    for(uint32_t i=0;i<count;++i) {
        const auto& record=records_[closed_[i]];
        if(!record.used||record.pending_draws)
            throw std::runtime_error("Invalid completed native query ownership");
        results.push_back(record.result);
    }
    for(uint32_t i=0;i<count;++i)records_[closed_[i]]={};
    std::move(closed_.begin()+count,closed_.begin()+closed_count_,closed_.begin());
    closed_count_-=count;return results;
}
std::optional<NativeQueryTracker::Result> NativeQueryTracker::Snapshot() const {
    if(pending_count_||prepared_||recording_)
        throw std::runtime_error("Native query snapshot before draw completion");
    if(!active_record_)return std::nullopt;
    auto result=records_[*active_record_].result;
    result.end_address=result.begin_address-32;
    return result;
}
std::optional<NativeQueryTracker::Metadata> NativeQueryTracker::MetadataSnapshot() const {
    if(!active_record_)return std::nullopt;
    const auto& record=records_[*active_record_];const auto& result=record.result;
    return Metadata{result.handle,result.begin_address,result.begin_address-32,
        result.draws+record.pending_draws,record.pending_draws,recording_};
}
void NativeQueryTracker::ResetDraw(plume::RenderCommandList& commands) {
    if(!active_record_)return;
    if(prepared_||recording_)throw std::runtime_error("Native query draw already prepared");
    if(pending_count_>=capacity_.lists)throw std::runtime_error("Native query batch is full; submit before recording another draw");
    const auto& record=records_[*active_record_];
    if(record.result.draws>std::numeric_limits<uint64_t>::max()-record.pending_draws-1)
        throw std::runtime_error("Native occlusion draw count overflow");
    auto& command=static_cast<plume::VulkanCommandList&>(commands);
    if(command.queue->device->vk!=device_)throw std::runtime_error("Foreign native query command buffer");
    if(host_reset_) {
        // Free slots are reset at construction or after their completion fence
        // and result read. Never reset the pending tail (including other batches).
        ++reset_statistics_.gpu_calls_avoided;
    } else {
        command.endActiveRenderPass();
        vkCmdResetQueryPool(command.vk,pool_,(pending_head_+pending_count_)%capacity_.lists,1);
        ++reset_statistics_.gpu_calls;
    }
    prepared_=true;prepared_command_=command.vk;
}
void NativeQueryTracker::BeginDraw(plume::RenderCommandList& commands) {
    if(!active_record_)return;
    auto& command=static_cast<plume::VulkanCommandList&>(commands);
    if(!prepared_||recording_||command.vk!=prepared_command_)
        throw std::runtime_error("Native occlusion draw must begin in its reset command buffer");
    command.checkActiveRenderPass();
    // Match the reference backend's query flags (it does not request PRECISE).
    vkCmdBeginQuery(command.vk,pool_,(pending_head_+pending_count_)%capacity_.lists,0);recording_=true;
}
void NativeQueryTracker::EndDraw(plume::RenderCommandList& commands) {
    if(!active_record_)return;
    const auto command=static_cast<plume::VulkanCommandList&>(commands).vk;
    if(!recording_||command!=prepared_command_)
        throw std::runtime_error("Native query draw must end in its begin command buffer");
    const auto slot=(pending_head_+pending_count_)%capacity_.lists;
    vkCmdEndQuery(command,pool_,slot);
    slot_owners_[slot]=*active_record_;
    ++records_[*active_record_].pending_draws;
    recording_=false;prepared_=false;prepared_command_=VK_NULL_HANDLE;++pending_count_;
}
void NativeQueryTracker::CompletePrefix(uint32_t count) {
    if(prepared_||recording_)throw std::runtime_error("Native query retirement during draw recording");
    if(count>pending_count_)throw std::runtime_error("Native query retirement exceeds its pending prefix");
    if(!count)return;
    std::array<uint64_t,kMaxPendingDraws> values{};
    const auto first=std::min(count,capacity_.lists-pending_head_);
    auto read=[&](uint32_t slot,uint32_t size,uint64_t* output) {
        if(size&&vkGetQueryPoolResults(device_,pool_,slot,size,sizeof(uint64_t)*size,
            output,sizeof(uint64_t),VK_QUERY_RESULT_64_BIT)!=VK_SUCCESS)
            throw std::runtime_error("Native query unavailable after submission fence");
    };
    read(pending_head_,first,values.data());read(0,count-first,values.data()+first);
    // Validate every owner and subtotal in a copy before committing anything.
    // Slots from multiple closed queries and the current query share this fence.
    auto completed=records_;
    for(uint32_t i=0;i<count;++i) {
        const auto owner=slot_owners_[(pending_head_+i)%capacity_.lists];
        if(owner>=completed.size()||!completed[owner].used||!completed[owner].pending_draws)
            throw std::runtime_error("Invalid pending native query slot owner");
        auto& record=completed[owner];
        if(values[i]>std::numeric_limits<uint64_t>::max()-record.result.samples)
            throw std::runtime_error("Native occlusion sample count overflow");
        if(record.result.draws==std::numeric_limits<uint64_t>::max())
            throw std::runtime_error("Native occlusion draw count overflow");
        record.result.samples+=values[i];++record.result.draws;--record.pending_draws;
    }
    // CompletePrefix's caller supplies a fence covering precisely this prefix.
    // Copy and validate all results first. Retained query slots in a later
    // in-flight submission are disjoint and remain untouched. Calls are on the
    // same rendering thread as ResetDraw/GetQueryPoolResults.
    if(host_reset_) {
        ResetRetiredRange(pending_head_,first);
        ResetRetiredRange(0,count-first);
    }
    records_=completed;pending_head_=(pending_head_+count)%capacity_.lists;pending_count_-=count;
}
void NativeQueryTracker::CompletePending() {CompletePrefix(pending_count_);}
void NativeQueryTracker::CompleteDraw() {CompletePending();}
}
