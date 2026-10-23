#pragma once
#include "fsi_state.h"
#include <bit>
#include <cstdint>
#include <cstring>
#include <functional>
#include <span>
#include <stdexcept>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <tmmintrin.h>
#if defined(_MSC_VER)
#include <intrin.h>
#endif
#define GOW3_CONSTANTS_X86 1
#endif

namespace gow3::renderer {
inline constexpr size_t kGuestFloatConstantOffset=0x780;
inline constexpr size_t kFloatConstantBytes=8192;

namespace detail {
inline void CopyFloatConstantsScalar(const uint8_t* source,uint8_t* destination) {
    for(size_t offset=0;offset<kFloatConstantBytes;offset+=sizeof(uint32_t)) {
        uint32_t value;
        std::memcpy(&value,source+offset,sizeof(value));
        if constexpr(std::endian::native==std::endian::little)value=std::byteswap(value);
        std::memcpy(destination+offset,&value,sizeof(value));
    }
}
#if GOW3_CONSTANTS_X86
inline bool HasConstantShuffle() {
#if defined(_MSC_VER)
    int registers[4];__cpuid(registers,1);
    return (registers[2]&(1<<9))!=0;
#elif defined(__GNUC__) || defined(__clang__)
    return __builtin_cpu_supports("ssse3");
#else
    return false;
#endif
}
#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("ssse3")))
#endif
inline void CopyFloatConstantsShuffle(const uint8_t* source,uint8_t* destination) {
    const auto order=_mm_setr_epi8(3,2,1,0,7,6,5,4,11,10,9,8,15,14,13,12);
    for(size_t offset=0;offset<kFloatConstantBytes;offset+=16) {
        __m128i value;
        std::memcpy(&value,source+offset,sizeof(value));
        value=_mm_shuffle_epi8(value,order);
        std::memcpy(destination+offset,&value,sizeof(value));
    }
}
#endif
inline void CopyFloatConstants(const uint8_t* source,uint8_t* destination) {
    // Dispatch once, without raising the executable's baseline CPU requirement.
    // SSSE3 shuffles preserve every bit (including NaNs) and never read the
    // mapped upload destination. The scalar path also supports non-x86 hosts.
    using Copy=void(*)(const uint8_t*,uint8_t*);
    static const Copy copy=[]()->Copy {
#if GOW3_CONSTANTS_X86
        if(HasConstantShuffle())return CopyFloatConstantsShuffle;
#endif
        return CopyFloatConstantsScalar;
    }();
    copy(source,destination);
}
} // namespace detail

// Validate all input extents before allocating/mapping the upload slot. The
// shared block already uses the host ABI; only the guest float register bits
// are big endian. No floating-point conversion may canonicalize their values.
inline size_t PackedDrawConstantSize(std::span<const uint8_t> guest,
                                     std::span<const uint8_t> shared) {
    if(guest.size()<kGuestFloatConstantOffset+kFloatConstantBytes)
        throw std::runtime_error("Truncated guest shader constants");
    if(shared.size()!=sizeof(SharedShaderConstants)&&shared.size()!=sizeof(NativeFsiShaderConstants))
        throw std::runtime_error("Invalid shared shader constant ABI extent");
    return kFloatConstantBytes+shared.size();
}

// The caller supplies a retired mapped upload slot. Validate first, then write
// every byte exactly once, including the complete unchanged shared/FSI ABI.
inline void PackDrawConstants(std::span<const uint8_t> guest,
                              std::span<const uint8_t> shared,
                              std::span<uint8_t> destination) {
    const auto size=PackedDrawConstantSize(guest,shared);
    if(destination.size()!=size)throw std::runtime_error("Invalid shader constant destination extent");
    const auto overlaps=[&](std::span<const uint8_t> input) {
        const std::less<const uint8_t*> less;
        return less(input.data(),destination.data()+destination.size())&&
            less(destination.data(),input.data()+input.size());
    };
    if(overlaps(guest)||overlaps(shared))throw std::runtime_error("Aliased shader constant packing storage");
    static_assert(std::endian::native==std::endian::little||std::endian::native==std::endian::big);
    detail::CopyFloatConstants(guest.data()+kGuestFloatConstantOffset,destination.data());
    std::memcpy(destination.data()+kFloatConstantBytes,shared.data(),shared.size());
}
#undef GOW3_CONSTANTS_X86
}
