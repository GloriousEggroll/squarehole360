#pragma once
#include <array>
#include <cmath>
#include <cstdint>

namespace gow3::renderer {
inline constexpr uint32_t kNativeFusedAtlasDepthClearPixels=1120u*688u;
struct NativeFusedAtlasDepthClearInput {
    bool native_fsi=false,query_active=false,source_depth=false;
    uint32_t width=0,height=0,pitch=0,base=0,format=0,surface_info=0,flags=0;
    float depth=0.f;
    std::array<int32_t,4> rectangle{};
};
inline bool NativeFusedAtlasDepthClearAllowed(const NativeFusedAtlasDepthClearInput& input) noexcept {
    const auto& r=input.rectangle;
    return input.native_fsi&&!input.query_active&&input.source_depth&&
        input.width==1120&&input.height==688&&input.pitch==1120&&input.base==1440&&
        input.format==0&&(input.surface_info&16383u)==input.pitch&&
        !(input.surface_info&0x30000u)&&input.flags==0x10&&
        std::isfinite(input.depth)&&input.depth==1.f&&
        r[0]>=0&&r[1]>=0&&r[2]>r[0]&&r[3]>r[1]&&r[2]<=1120&&r[3]<=688;
}
enum class NativeFusedAtlasDepthClearSource {Refuse,Packed,Host};
// Called AFTER the original overlapping dirty-alias commits. A current clean
// host contains the original pre-store floats; stale/absent hosts must first
// decode packed bytes in the original path. Never infer one from the other.
// Remaining dirty images refuse rather than adding a flush or changing the
// relative page-version publication of the original scope-tail stores.
inline NativeFusedAtlasDepthClearSource NativeFusedAtlasDepthClearChooseSource(
    bool fully_defined,bool host_present,bool host_dirty,uint64_t host_version,
    uint64_t packed_version,bool pending_receipt,bool other_dirty) noexcept {
    if(!fully_defined||!packed_version||host_dirty||pending_receipt||other_dirty)
        return NativeFusedAtlasDepthClearSource::Refuse;
    return host_present&&host_version==packed_version?
        NativeFusedAtlasDepthClearSource::Host:NativeFusedAtlasDepthClearSource::Packed;
}
inline bool NativeFusedAtlasDepthClearReceiptCurrent(bool present,uint64_t receipt_version,
                                                     uint64_t packed_version) noexcept {
    return present&&receipt_version&&receipt_version==packed_version;
}
} // namespace gow3::renderer
