#pragma once
#include <cstdint>
namespace gow3::renderer {
// Admission only: the actual transfer packet owns the scratch lease, and its
// existing barriers order accesses through one fixed accepted Native FIFO.
// No completion inference, descriptor mutation, resource recycling or waits.
class NativeRetainedOpaqueScratchSequence {
    const void* batch_;
    const void* canonical_=nullptr;
    bool failed_=false;
public:
    explicit NativeRetainedOpaqueScratchSequence(const void* batch):batch_(batch) {}
    bool CanPrepare(const void* canonical) const noexcept {
        return batch_&&!failed_&&canonical&&(!canonical_||canonical_==canonical);
    }
    bool BindCanonical(const void* canonical) noexcept {
        if(!CanPrepare(canonical))return false;
        canonical_=canonical;return true;
    }
    bool CanRecord(const void* batch,const void* canonical,bool active,bool queued,
                   bool immutable_allocation,bool owns_lease) const noexcept {
        return !failed_&&batch_&&batch_==batch&&canonical_&&canonical_==canonical&&
            active&&queued&&immutable_allocation&&owns_lease;
    }
    void FailRecording() noexcept {failed_=true;}
    bool failed() const noexcept {return failed_;}
};
}
