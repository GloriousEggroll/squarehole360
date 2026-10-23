#pragma once
#include "plume_render_interface.h"
#include "gpu_batch_attribution.h"
#include <functional>
#include <memory>
#include <stdexcept>
#include <vector>
namespace gow3::renderer {
class NativeGpuWorkQueue;
class NativeTransferBatchError : public std::runtime_error {
public: using std::runtime_error::runtime_error;
};
// A bounded, operation-local recorder. Standalone scope exit fences its work;
// with a shared queue it seals owned work, and explicit Flush provides the fence.
class NativeTransferBatch {
public:
    static constexpr uint32_t kMaxStages=64;
    static constexpr uint64_t kMaxImageBytes=128u*1024*1024;
    struct Statistics {
        uint64_t stages=0,submissions=0,waits=0,maximum_stages=0,synchronous_fallbacks=0,queued_lists=0;
    };
    NativeTransferBatch(plume::RenderDevice&,plume::RenderCommandQueue&,NativeGpuWorkQueue* work_queue=nullptr);
    ~NativeTransferBatch();
    bool in_scope() const {return in_scope_;}
    bool active() const {return in_scope_&&!synchronous_&&!suspensions_;}
    bool has_work_queue() const {return work_queue_!=nullptr;}
    const NativeGpuWorkQueue* work_queue_identity() const noexcept {return work_queue_;}
    void Run(const std::function<void()>& operation,uint64_t event=0,const NativeGpuWorkTag* tag=nullptr);
    bool has_recording() const {return pending_stages_!=0;}
    // Seal into a shared FIFO without completing it. No-op in standalone mode.
    void Seal();
    plume::RenderCommandList& BeginStage();
    // Retain before recording any command that references the resource.
    template<class T>T* Keep(std::unique_ptr<T> resource) {
        if(!active()||!pending_stages_)throw std::logic_error("Transfer resource outside recorded stage");
        if(resources_.size()>=kMaxStages*8)throw std::logic_error("Native transfer resource bound exceeded");
        auto* ptr=resource.get();resources_.push_back(std::shared_ptr<T>(std::move(resource)));return ptr;
    }
    // An exclusive pooled lease is retained by the same actual fence-owned
    // packet as unique scratch. The pool cannot reacquire it until ALL owners
    // (including generation reserves and DrawSlots) release the lease.
    void KeepLease(std::shared_ptr<void> lease) {
        if(!lease||!active()||!pending_stages_)throw std::logic_error("Transfer lease outside recorded stage");
        if(resources_.size()>=kMaxStages*8)throw std::logic_error("Native transfer resource bound exceeded");
        resources_.push_back(std::move(lease));
    }
    // Scratch ownership must be registered before its first recorded write.
    // On pressure, finish the prefix and use synchronous helpers for this scope.
    void KeepImage(std::shared_ptr<plume::RenderTexture> image,uint64_t bytes);
    // Charge immutable upload bytes against the same bounded scratch budget
    // before BeginStage, then retain the upload with Keep before recording it.
    // Pressure fences the prefix and selects synchronous execution this scope.
    bool ReserveUploadBytes(uint64_t bytes);
    void Flush();
    const Statistics& statistics() const {return statistics_;}
    class Suspension {
        NativeTransferBatch* batch_;
    public:
        explicit Suspension(NativeTransferBatch* batch):batch_(batch) {
            if(batch_){batch_->Flush();++batch_->suspensions_;}
        }
        ~Suspension(){if(batch_)--batch_->suspensions_;}
        Suspension(const Suspension&)=delete;
        Suspension& operator=(const Suspension&)=delete;
    };
private:
    struct RecyclePool;
    struct Packet;
    NativeGpuWorkQueue* work_queue_;
    std::optional<NativeGpuWorkTag> attribution_tag_;
    std::shared_ptr<RecyclePool> recycle_pool_;
    plume::RenderCommandQueue& queue_;
    std::unique_ptr<plume::RenderCommandList> commands_;
    std::unique_ptr<plume::RenderCommandFence> fence_;
    std::vector<std::shared_ptr<void>> resources_;
    std::vector<std::shared_ptr<plume::RenderTexture>> images_;
    uint64_t image_bytes_=0,event_=0;
    uint32_t pending_stages_=0,suspensions_=0;
    bool in_scope_=false,synchronous_=false,failed_=false;
    Statistics statistics_;
    void Finish();
};
}
