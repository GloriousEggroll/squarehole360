#pragma once
#include "plume_vulkan.h"
#include "submission_capacity.h"
#include <array>
#include <cstdint>
#include <vector>
#include <optional>

namespace gow3::renderer {
// Measures native depth-passing samples. Guest memory is deliberately outside
// this class: while validating against the reference renderer, its result must
// not overwrite the reference query's report or completion sentinel.
class NativeQueryTracker {
public:
    static constexpr uint32_t kMaxPendingDraws=NativeSubmissionCapacity::kMaximum;
    static constexpr uint32_t kMaxDeferredQueries=128;
    struct Result {
        uint32_t handle, begin_address, end_address;
        uint64_t samples, draws;
    };
    // This metadata is safe before a submission completes. It deliberately
    // contains no sample count; draws includes completed and accepted draws.
    struct Metadata {
        uint32_t handle, begin_address, end_address;
        uint64_t draws;
        uint32_t pending_draws;
        bool recording;
    };
    explicit NativeQueryTracker(plume::RenderDevice& device,uint32_t capacity=NativeSubmissionCapacity::kDefault,
                                bool host_reset=false);
    ~NativeQueryTracker();
    void Begin(uint32_t handle, uint32_t begin_address);
    Result End(uint32_t handle, uint32_t end_address);
    // Seal a logical query while its draw slots remain unfenced. Closed records
    // are bounded independently from slots, including queries with no draws.
    void EndDeferred(uint32_t handle, uint32_t end_address);
    uint32_t deferred_queries() const { return closed_count_; }
    // The caller must also fence unrelated draw command lists. This class can
    // only reject outstanding query slots; SceneExecutor enforces the full drain.
    std::vector<Result> TakeDeferredResults();
    // The caller proves the all-work frontier at each consumed query end.
    // Later physical slots may remain pending, but no consumed record may own
    // one. Exactly this closed prefix is validated before any record is freed.
    std::vector<Result> TakeDeferredPrefix(uint32_t count);
    // Completed subtotal for a one-way transition to reference query ownership.
    // Recording or unfenced draws are deliberately unavailable.
    std::optional<Result> Snapshot() const;
    std::optional<Metadata> MetadataSnapshot() const;
    bool active() const { return active_record_.has_value(); }
    uint32_t pending_draws() const { return pending_count_; }
    uint32_t capacity() const { return capacity_.lists; }
    bool host_reset_enabled() const { return host_reset_!=nullptr; }
    struct ResetStatistics { uint64_t host_calls=0,host_slots=0,gpu_calls=0,gpu_calls_avoided=0; };
    const ResetStatistics& reset_statistics() const { return reset_statistics_; }
    // GPU resets close any active render pass. Host mode has already reset
    // every free slot after its fence and can keep a pass open. BeginDraw opens
    // the framebuffer's pass as needed; EndDraw stays inside that pass.
    void ResetDraw(plume::RenderCommandList& commands);
    void BeginDraw(plume::RenderCommandList& commands);
    void EndDraw(plume::RenderCommandList& commands);
    // Call only after the submission fence covering every accepted draw has
    // completed. No slot may be reset or reused before this retirement.
    void CompletePending();
    // Retire exactly the oldest queried draws covered by one completed fence.
    // Later slots/logical queries remain owned and unavailable for publication.
    void CompletePrefix(uint32_t queried_draws);
    // Compatibility for callers that still submit and fence one draw at a time.
    void CompleteDraw();
private:
    NativeSubmissionCapacity capacity_;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueryPool pool_ = VK_NULL_HANDLE;
    PFN_vkResetQueryPool host_reset_=nullptr;
    ResetStatistics reset_statistics_;
    void ResetRetiredRange(uint32_t first,uint32_t count);
    struct Record {
        Result result{};
        uint32_t pending_draws=0;
        bool used=false;
    };
    Record& CheckedEnd(uint32_t handle,uint32_t end_address);
    std::array<Record,kMaxDeferredQueries+1> records_{};
    std::optional<uint32_t> active_record_;
    std::array<uint32_t,kMaxDeferredQueries> closed_{};
    uint32_t closed_count_=0;
    std::array<uint32_t,kMaxPendingDraws> slot_owners_{};
    bool prepared_ = false, recording_ = false;
    uint32_t pending_count_ = 0;
    uint32_t pending_head_ = 0;
    VkCommandBuffer prepared_command_ = VK_NULL_HANDLE;
};
}
