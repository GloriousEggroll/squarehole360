#pragma once
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace gow3::renderer {
// All accepted lists, including unqueried draws and transfer-only work, share
// one ordered sequence. A query end snapshots accepted(); only actual FIFO
// fence retirement advances completed(). API event IDs are not sufficient:
// several independently owned lists can belong to the same event.
class NativeGpuWorkFrontier {
public:
    uint64_t accepted() const { return accepted_; }
    uint64_t completed() const { return completed_; }
    void CheckAccept() const {
        if (accepted_ == std::numeric_limits<uint64_t>::max())
            throw std::overflow_error("Native GPU accepted-work serial overflow");
    }
    void Accept() { CheckAccept(); ++accepted_; }
    void Complete(uint32_t lists) {
        if (lists > accepted_ - completed_)
            throw std::logic_error("Native GPU completion exceeds accepted work");
        completed_ += lists;
    }
    bool Reached(uint64_t required) const {
        if (required > accepted_)
            throw std::logic_error("Native query depends on unaccepted GPU work");
        return required <= completed_;
    }
private:
    uint64_t accepted_ = 0, completed_ = 0;
};
}
