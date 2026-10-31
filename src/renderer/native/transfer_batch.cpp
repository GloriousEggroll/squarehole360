#include "transfer_batch.h"
#include "gpu_work_queue.h"
#include <algorithm>
#include <exception>
namespace gow3::renderer {
struct NativeTransferBatch::RecyclePool {
    std::vector<std::unique_ptr<plume::RenderCommandList>> commands;
    RecyclePool(){commands.reserve(NativeGpuWorkQueue::kMaxLists);}
};
struct NativeTransferBatch::Packet {
    std::shared_ptr<RecyclePool> recycle;
    std::unique_ptr<plume::RenderCommandList> commands;
    std::vector<std::shared_ptr<void>> resources;
    std::vector<std::shared_ptr<plume::RenderTexture>> images;
    ~Packet() {
        resources.clear();images.clear();
        if(commands)recycle->commands.push_back(std::move(commands));
    }
};
NativeTransferBatch::NativeTransferBatch(plume::RenderDevice& device,plume::RenderCommandQueue& queue,
    NativeGpuWorkQueue* work_queue):work_queue_(work_queue),recycle_pool_(std::make_shared<RecyclePool>()),queue_(queue) {
    commands_=queue.createCommandList();fence_=device.createCommandFence();
    resources_.reserve(kMaxStages*8);images_.reserve(kMaxStages);
}
NativeTransferBatch::~NativeTransferBatch() {
    // Scene closes scopes explicitly; never release live references after a
    // failed submission/fence or an accidentally abandoned recorded operation.
    if(pending_stages_||failed_)std::terminate();
}
plume::RenderCommandList& NativeTransferBatch::BeginStage() {
    if(!active()||failed_)throw NativeTransferBatchError("Cannot record native transfer outside an active scope");
    if(pending_stages_==kMaxStages){if(work_queue_)Seal();else Flush();}
    if(!pending_stages_) {
        if(work_queue_)work_queue_->Reserve(image_bytes_);
        if(!commands_) {
            if(recycle_pool_->commands.empty())commands_=queue_.createCommandList();
            else {commands_=std::move(recycle_pool_->commands.back());recycle_pool_->commands.pop_back();}
        }
        commands_->begin();
    }
    ++pending_stages_;++statistics_.stages;
    statistics_.maximum_stages=std::max(statistics_.maximum_stages,uint64_t(pending_stages_));
    return *commands_;
}
void NativeTransferBatch::KeepImage(std::shared_ptr<plume::RenderTexture> image,uint64_t bytes) {
    if(!active())return;
    if(!image||!bytes)throw std::invalid_argument("Invalid native transfer scratch image");
    if(bytes>kMaxImageBytes||image_bytes_>kMaxImageBytes-bytes||images_.size()==kMaxStages) {
        Flush();images_.clear();image_bytes_=0;synchronous_=true;++statistics_.synchronous_fallbacks;return;
    }
    if(work_queue_)work_queue_->Reserve(image_bytes_+bytes);
    images_.push_back(std::move(image));image_bytes_+=bytes;
}
bool NativeTransferBatch::ReserveUploadBytes(uint64_t bytes) {
    if(!active())return false;
    if(!bytes)throw std::invalid_argument("Invalid native transfer upload size");
    if(bytes>kMaxImageBytes||image_bytes_>kMaxImageBytes-bytes) {
        Flush();images_.clear();image_bytes_=0;synchronous_=true;
        ++statistics_.synchronous_fallbacks;return false;
    }
    if(work_queue_)work_queue_->Reserve(image_bytes_+bytes);
    image_bytes_+=bytes;return true;
}
void NativeTransferBatch::Seal() {
    if(!work_queue_)return;
    if(failed_)throw NativeTransferBatchError("Native transfer batch previously failed");
    if(!pending_stages_)return;
    try {
        work_queue_->Reserve(image_bytes_);
        auto packet=std::make_shared<Packet>();packet->recycle=recycle_pool_;
        // Allocate/copy ownership before transferring the command list; the
        // fixed-capacity enqueue itself cannot allocate after reservation.
        packet->images=images_;
        commands_->end();packet->commands=std::move(commands_);packet->resources=std::move(resources_);
        auto* commands=packet->commands.get();
        try {work_queue_->EnqueueTransfer(*commands,packet,image_bytes_,event_,attribution_tag_?&*attribution_tag_:nullptr);}
        catch(...) {
            commands_=std::move(packet->commands);resources_=std::move(packet->resources);
            throw;
        }
        pending_stages_=0;++statistics_.queued_lists;
        resources_.clear();resources_.reserve(kMaxStages*8);
    } catch(const std::exception& error){failed_=true;throw NativeTransferBatchError(error.what());}
    catch(...){failed_=true;throw NativeTransferBatchError("Native transfer packet ownership failed");}
}
void NativeTransferBatch::Flush() {
    if(failed_)throw NativeTransferBatchError("Native transfer batch previously failed");
    if(work_queue_) {
        Seal();
        try {work_queue_->Drain();}
        catch(const std::exception& error){failed_=true;throw NativeTransferBatchError(error.what());}
        return;
    }
    if(!pending_stages_)return;
    try {
        commands_->end();queue_.executeCommandLists(commands_.get(),fence_.get());++statistics_.submissions;
        queue_.waitForCommandFence(fence_.get());++statistics_.waits;
        // Views/descriptors/framebuffers may now be destroyed. Scratch images
        // also needed by later stages remain retained through the scope.
        resources_.clear();pending_stages_=0;
    } catch(const std::exception& error) {
        failed_=true;throw NativeTransferBatchError(error.what());
    } catch(...) {failed_=true;throw NativeTransferBatchError("Native transfer completion failed");}
}
void NativeTransferBatch::Finish() {
    if(work_queue_)Seal();else Flush();
    images_.clear();image_bytes_=0;in_scope_=false;synchronous_=false;
}
void NativeTransferBatch::BeginScope(uint64_t event) {
    if(in_scope_||failed_)throw NativeTransferBatchError("Nested or failed native transfer scope");
    attribution_tag_.reset();in_scope_=true;event_=event;
}
void NativeTransferBatch::EndScope() {
    if(!in_scope_)throw NativeTransferBatchError("No native transfer scope to end");
    Finish();
}
void NativeTransferBatch::AbortScope() {
    if(!in_scope_||failed_)return;
    Flush();Finish();
}
void NativeTransferBatch::Run(const std::function<void()>& operation,uint64_t event,const NativeGpuWorkTag* tag) {
    if(in_scope_||failed_)throw NativeTransferBatchError("Nested or failed native transfer scope");
    if(tag)attribution_tag_=*tag;else attribution_tag_.reset();
    in_scope_=true;event_=event;const auto stages_before=statistics_.stages;
    try {operation();Finish();}
    catch(...) {
        const auto original=std::current_exception();
        if(failed_)std::rethrow_exception(original);
        // Plume tracks layouts while recording. Submit the accepted prefix
        // even on a later validation/allocation error; dropping it would leave
        // host layout tracking inconsistent with the actual images.
        Flush();Finish();
        if(statistics_.stages!=stages_before) {
            try {std::rethrow_exception(original);}
            catch(const std::exception& error) {throw NativeTransferBatchError(error.what());}
            catch(...) {throw NativeTransferBatchError("Native transfer recording failed after accepted work");}
        }
        std::rethrow_exception(original);
    }
}
}
