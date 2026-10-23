#pragma once
#include <cstdint>
#include <stdexcept>
namespace gow3::renderer {
// Physical ownership capacity is separate from logical query tickets and from
// the configurable draw batching target and fence scheduling window. The
// staging divisor stays two even when the queue permits four in-flight fences.
struct NativeSubmissionCapacity {
    static constexpr uint32_t kDefault=64,kMaximum=128,kFences=2;
    explicit NativeSubmissionCapacity(uint32_t requested=kDefault,bool asynchronous=true) {
        if(requested!=kDefault&&requested!=kMaximum)
            throw std::invalid_argument("Native submission capacity must be 64 or 128");
        lists=asynchronous?requested:kDefault;
    }
    uint32_t lists=kDefault;
    uint32_t AsyncBatchLists() const {return lists/kFences;}
};
}
