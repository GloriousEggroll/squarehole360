#pragma once
#include "constant_data.h"
#include "null_depth_constant_footprint.h"

namespace gow3::renderer {
// Caller must have certified BOTH final selected modules. Preserve allocation
// size and all three push addresses; only bytes proved unread remain stale.
inline void PackPinnedNullDrawConstants(std::span<const uint8_t> guest,
                                       std::span<const uint8_t> shared,
                                       std::span<uint8_t> destination) {
    const auto bytes=PackedDrawConstantSize(guest,shared);
    if(shared.size()!=sizeof(NativeFsiShaderConstants)||destination.size()!=bytes)
        throw std::runtime_error("Invalid pinned null shader constant ABI extent");
    const auto overlaps=[&](std::span<const uint8_t> input) {
        const std::less<const uint8_t*> less;
        return less(input.data(),destination.data()+destination.size())&&
            less(destination.data(),input.data()+input.size());
    };
    if(overlaps(guest)||overlaps(shared))
        throw std::runtime_error("Aliased pinned null shader constant packing storage");
    static_assert(kNativeNullDepthFloatPrefixBytes==176);
    for(size_t offset=0;offset<kNativeNullDepthFloatPrefixBytes;offset+=4) {
        uint32_t value;
        std::memcpy(&value,guest.data()+kGuestFloatConstantOffset+offset,4);
        if constexpr(std::endian::native==std::endian::little)value=std::byteswap(value);
        std::memcpy(destination.data()+offset,&value,4);
    }
    std::memcpy(destination.data()+kFloatConstantBytes,shared.data(),shared.size());
}
} // namespace gow3::renderer
