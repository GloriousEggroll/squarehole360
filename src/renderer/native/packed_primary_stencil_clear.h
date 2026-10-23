#pragma once
#include <array>
#include <cmath>
#include <cstdint>

namespace gow3::renderer {
inline constexpr uint64_t kNativePackedPrimaryStencilClearPixels=1280ull*720ull;
struct NativePackedPrimaryStencilClearInput {
    bool native_fsi=false,query_active=false,source_depth=false;
    uint32_t width=0,height=0,pitch=0,base=0,format=0,surface_info=0,flags=0;
    float depth=0.f;
    std::array<int32_t,4> rectangle{};
};
// The depth scalar remains validated even though flags32 changes only stencil,
// exactly as the original PassTransfer::Clear. No MSAA or extra attachments.
inline bool NativePackedPrimaryStencilClearLayoutAllowed(
    const NativePackedPrimaryStencilClearInput& input) noexcept {
    const auto& r=input.rectangle;
    return input.native_fsi&&input.source_depth&&input.width==1280&&input.height==720&&
        input.pitch==1280&&input.base==0&&input.format==1&&
        (input.surface_info&16383u)==input.pitch&&!(input.surface_info&0x30000u)&&
        input.flags==0x20&&std::isfinite(input.depth)&&input.depth>=0.f&&input.depth<=1.f&&
        r[0]>=0&&r[1]>=0&&r[2]>r[0]&&r[3]>r[1]&&r[2]<=1280&&r[3]<=720;
}
inline bool NativePackedPrimaryStencilClearAllowed(
    const NativePackedPrimaryStencilClearInput& input) noexcept {
    return !input.query_active&&NativePackedPrimaryStencilClearLayoutAllowed(input);
}
enum class NativePackedPrimaryStencilClearSource {Refuse,CurrentHost,Packed};
// Observe this AFTER the original overlapping dirty alias commits. A current
// host token can follow RestoreDepth without StoreSurface, so neither it nor
// dirty/pending host state authorizes replacing the original image path.
inline NativePackedPrimaryStencilClearSource NativePackedPrimaryStencilClearChooseSource(
    bool fully_defined,bool host_present,bool host_dirty,uint64_t host_version,
    uint64_t packed_version,bool pending_receipt,bool other_dirty) noexcept {
    if(!fully_defined||!packed_version||host_dirty||pending_receipt||other_dirty)
        return NativePackedPrimaryStencilClearSource::Refuse;
    if(host_present&&host_version==packed_version)
        return NativePackedPrimaryStencilClearSource::CurrentHost;
    return NativePackedPrimaryStencilClearSource::Packed;
}
} // namespace gow3::renderer
