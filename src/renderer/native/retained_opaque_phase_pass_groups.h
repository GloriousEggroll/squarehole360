#pragma once
#include <cstdint>

namespace gow3::renderer {
// Borrowed identities only. Scene pins the logical generation in every draw
// slot and checks its weak open-pass owner. This helper cannot certify modules,
// GPU completion, image layouts or command-list recording state.
struct NativeRetainedOpaquePhasePassIdentity {
    const void* owner=nullptr;
    uint64_t generation=0;
    const void* framebuffer=nullptr;
    const void* color=nullptr;
    const void* depth=nullptr;
    bool valid()const noexcept {
        return owner&&generation&&framebuffer&&color&&depth&&color!=depth;
    }
    bool operator==(const NativeRetainedOpaquePhasePassIdentity&)const=default;
};
struct NativeRetainedOpaquePhasePassProof {
    NativeRetainedOpaquePhasePassIdentity identity;
    bool actual_pipeline_certified=false;
    bool actual_attachments=false;
    bool exact_variants=false;
    bool queried=false,discarded=false,rectangle=false;
};
inline bool NativeRetainedOpaquePhasePassAllowed(const NativeRetainedOpaquePhasePassProof& p)noexcept {
    return p.identity.valid()&&p.actual_pipeline_certified&&p.actual_attachments&&p.exact_variants&&
        !p.queried&&!p.discarded&&!p.rectangle;
}
inline bool NativeRetainedOpaquePhasePassContinues(const NativeRetainedOpaquePhasePassIdentity& a,
        const NativeRetainedOpaquePhasePassIdentity& b)noexcept {
    // The same reusable framebuffer/images are insufficient: a fresh logical
    // restore generation must start a new pass even if all GPU pointers repeat.
    return a.valid()&&b.valid()&&a==b;
}
}
