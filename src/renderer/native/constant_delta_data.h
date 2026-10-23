#pragma once
#include "constant_data.h"
#include <array>
#include <utility>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#define GOW3_CONSTANT_DELTA_X86 1
#include <immintrin.h>
#if defined(_MSC_VER)
#include <intrin.h>
#endif
#if defined(__GNUC__) || defined(__clang__)
#define GOW3_CONSTANT_DELTA_TARGET(isa) __attribute__((target(isa)))
#else
#define GOW3_CONSTANT_DELTA_TARGET(isa)
#endif
#endif

namespace gow3::renderer {
namespace detail {
inline void CopyConstantBlockScalar(const uint8_t* source,uint8_t* destination) {
    for(size_t offset=0;offset<64;offset+=4) {
        uint32_t value;std::memcpy(&value,source+offset,4);
        if constexpr(std::endian::native==std::endian::little)value=std::byteswap(value);
        std::memcpy(destination+offset,&value,4);
    }
}
#if GOW3_CONSTANT_DELTA_X86
GOW3_CONSTANT_DELTA_TARGET("ssse3")
inline void CopyConstantBlockShuffle(const uint8_t* source,uint8_t* destination) {
    const auto order=_mm_setr_epi8(3,2,1,0,7,6,5,4,11,10,9,8,15,14,13,12);
    for(size_t offset=0;offset<64;offset+=16) {
        __m128i value;std::memcpy(&value,source+offset,16);
        value=_mm_shuffle_epi8(value,order);std::memcpy(destination+offset,&value,16);
    }
}
#endif
inline void CopyConstantBlock(const uint8_t* source,uint8_t* destination) {
    using Copy=void(*)(const uint8_t*,uint8_t*);
    static const Copy copy=[]()->Copy {
#if GOW3_CONSTANT_DELTA_X86
        if(HasConstantShuffle())return CopyConstantBlockShuffle;
#endif
        return CopyConstantBlockScalar;
    }();
    copy(source,destination);
}

// Fused delta kernels over the 8192 float bytes. For each 64-byte block,
// compare the guest source with the CPU mirror of the bits last written to this
// slot. A differing block is written to the destination (byte swapped per
// 32-bit word) and refreshes exactly that mirror block. With WriteAll every
// block is written and mirrored unconditionally (branch-free full pack) while
// still counting unchanged blocks, which drives the next pack's choice. The
// destination is never read. One dispatch per pack replaces one indirect call
// per block plus a separate full-mirror memcpy. Returns unchanged blocks.
inline constexpr size_t kConstantDeltaBlockBytes=64;
inline constexpr size_t kConstantDeltaBlocks=kFloatConstantBytes/kConstantDeltaBlockBytes;
template<bool WriteAll>
inline size_t DeltaFloatConstantsScalar(const uint8_t* source,uint8_t* mirror,uint8_t* destination) {
    size_t unchanged=0;
    for(size_t offset=0;offset<kFloatConstantBytes;offset+=kConstantDeltaBlockBytes) {
        const bool same=std::memcmp(source+offset,mirror+offset,kConstantDeltaBlockBytes)==0;
        unchanged+=same;
        if(!WriteAll&&same)continue;
        CopyConstantBlockScalar(source+offset,destination+offset);
        std::memcpy(mirror+offset,source+offset,kConstantDeltaBlockBytes);
    }
    return unchanged;
}
#if GOW3_CONSTANT_DELTA_X86
// SSE2 compare + SSSE3 pshufb (runtime-checked, like CopyFloatConstants).
template<bool WriteAll>
GOW3_CONSTANT_DELTA_TARGET("ssse3")
inline size_t DeltaFloatConstantsShuffle(const uint8_t* source,uint8_t* mirror,uint8_t* destination) {
    const auto order=_mm_setr_epi8(3,2,1,0,7,6,5,4,11,10,9,8,15,14,13,12);
    size_t unchanged=0;
    for(size_t offset=0;offset<kFloatConstantBytes;offset+=kConstantDeltaBlockBytes) {
        const auto* in=reinterpret_cast<const __m128i*>(source+offset);
        auto* cached=reinterpret_cast<__m128i*>(mirror+offset);
        const auto a=_mm_loadu_si128(in+0),b=_mm_loadu_si128(in+1);
        const auto c=_mm_loadu_si128(in+2),d=_mm_loadu_si128(in+3);
        const auto diff=_mm_or_si128(
            _mm_or_si128(_mm_xor_si128(a,_mm_loadu_si128(cached+0)),_mm_xor_si128(b,_mm_loadu_si128(cached+1))),
            _mm_or_si128(_mm_xor_si128(c,_mm_loadu_si128(cached+2)),_mm_xor_si128(d,_mm_loadu_si128(cached+3))));
        const bool same=_mm_movemask_epi8(_mm_cmpeq_epi8(diff,_mm_setzero_si128()))==0xFFFF;
        unchanged+=same;
        if(!WriteAll&&same)continue;
        auto* out=reinterpret_cast<__m128i*>(destination+offset);
        _mm_storeu_si128(out+0,_mm_shuffle_epi8(a,order));_mm_storeu_si128(out+1,_mm_shuffle_epi8(b,order));
        _mm_storeu_si128(out+2,_mm_shuffle_epi8(c,order));_mm_storeu_si128(out+3,_mm_shuffle_epi8(d,order));
        _mm_storeu_si128(cached+0,a);_mm_storeu_si128(cached+1,b);
        _mm_storeu_si128(cached+2,c);_mm_storeu_si128(cached+3,d);
    }
    return unchanged;
}
// AVX2 needs CPU support plus OS-enabled YMM state (OSXSAVE + XCR0[2:1]).
inline bool HasConstantDeltaAvx2() {
#if defined(_MSC_VER)
    int registers[4];__cpuid(registers,0);
    if(registers[0]<7)return false;
    __cpuid(registers,1);
    if((registers[2]&(1<<27))==0||(registers[2]&(1<<28))==0)return false;
    if((_xgetbv(0)&6)!=6)return false;
    __cpuidex(registers,7,0);
    return (registers[1]&(1<<5))!=0;
#elif defined(__GNUC__) || defined(__clang__)
    return __builtin_cpu_supports("avx2");
#else
    return false;
#endif
}
template<bool WriteAll>
GOW3_CONSTANT_DELTA_TARGET("avx2")
inline size_t DeltaFloatConstantsAvx2(const uint8_t* source,uint8_t* mirror,uint8_t* destination) {
    const auto order=_mm256_setr_epi8(3,2,1,0,7,6,5,4,11,10,9,8,15,14,13,12,
                                      3,2,1,0,7,6,5,4,11,10,9,8,15,14,13,12);
    size_t unchanged=0;
    for(size_t offset=0;offset<kFloatConstantBytes;offset+=kConstantDeltaBlockBytes) {
        const auto* in=reinterpret_cast<const __m256i*>(source+offset);
        auto* cached=reinterpret_cast<__m256i*>(mirror+offset);
        const auto a=_mm256_loadu_si256(in+0),b=_mm256_loadu_si256(in+1);
        const auto diff=_mm256_or_si256(_mm256_xor_si256(a,_mm256_loadu_si256(cached+0)),
                                        _mm256_xor_si256(b,_mm256_loadu_si256(cached+1)));
        const bool same=_mm256_testz_si256(diff,diff)!=0;
        unchanged+=same;
        if(!WriteAll&&same)continue;
        auto* out=reinterpret_cast<__m256i*>(destination+offset);
        _mm256_storeu_si256(out+0,_mm256_shuffle_epi8(a,order));
        _mm256_storeu_si256(out+1,_mm256_shuffle_epi8(b,order));
        _mm256_storeu_si256(cached+0,a);_mm256_storeu_si256(cached+1,b);
    }
    // Avoid AVX->SSE transition penalties in non-VEX callers.
    _mm256_zeroupper();
    return unchanged;
}
#endif
template<bool WriteAll>
inline size_t DeltaFloatConstants(const uint8_t* source,uint8_t* mirror,uint8_t* destination) {
    using Delta=size_t(*)(const uint8_t*,uint8_t*,uint8_t*);
    static const Delta delta=[]()->Delta {
#if GOW3_CONSTANT_DELTA_X86
        if(HasConstantDeltaAvx2())return DeltaFloatConstantsAvx2<WriteAll>;
        if(HasConstantShuffle())return DeltaFloatConstantsShuffle<WriteAll>;
#endif
        return DeltaFloatConstantsScalar<WriteAll>;
    }();
    return delta(source,mirror,destination);
}
} // namespace detail

// One object belongs to one actual DrawUpload allocation. The caller may
// overwrite that buffer only after its previous real submission fence, and
// must invalidate on recreation or after any other writer touches the float
// range. No guest pointer or upload-memory read is retained. The cached array
// is a CPU mirror of the float bits last written to the slot; it is trusted
// only while valid_, which is set after a successful flush.
class ConstantDeltaPacker {
    std::array<uint8_t,kFloatConstantBytes> previous_{};
    // Branchy skipping is slower than a full write when many blocks change
    // (mispredicts). Above this many changed blocks in the previous pack of
    // this slot, the next pack writes every block (still exact, still counted).
    static constexpr size_t kFullWriteChangedBlocks=40;
    uint8_t last_changed_blocks_=uint8_t(detail::kConstantDeltaBlocks);
    bool valid_=false;
public:
    struct Writes {uint64_t float_bytes=0,shared_bytes=0,unchanged_blocks=0;};
    static constexpr size_t kBlockBytes=detail::kConstantDeltaBlockBytes;
    void Invalidate()noexcept {valid_=false;}
    bool valid()const noexcept {return valid_;}
    template<class Flush>
    Writes Pack(std::span<const uint8_t> guest,std::span<const uint8_t> shared,
                std::span<uint8_t> destination,Flush&& flush) {
        const auto bytes=PackedDrawConstantSize(guest,shared);
        if(destination.size()!=bytes)throw std::runtime_error("Invalid shader constant destination extent");
        const auto overlaps=[&](std::span<const uint8_t> input) {
            const std::less<const uint8_t*> less;
            return less(input.data(),destination.data()+destination.size())&&
                less(destination.data(),input.data()+input.size());
        };
        if(overlaps(guest)||overlaps(shared))throw std::runtime_error("Aliased shader constant packing storage");
        const auto* source=guest.data()+kGuestFloatConstantOffset;
        const bool previous_valid=valid_;
        // After the first destination/mirror write an exception must force the
        // next attempt to overwrite all float words, including a failed flush.
        valid_=false;
        Writes writes;
        if(!previous_valid) {
            detail::CopyFloatConstants(source,destination.data());
            std::memcpy(previous_.data(),source,kFloatConstantBytes);
            last_changed_blocks_=uint8_t(detail::kConstantDeltaBlocks);
            writes.float_bytes=kFloatConstantBytes;
        } else {
            // Exact byte equality against only cached CPU bytes. Never read
            // mapped upload storage (which may have write-combining rules).
            const bool write_all=last_changed_blocks_>kFullWriteChangedBlocks;
            const auto unchanged=write_all?
                detail::DeltaFloatConstants<true>(source,previous_.data(),destination.data()):
                detail::DeltaFloatConstants<false>(source,previous_.data(),destination.data());
            last_changed_blocks_=uint8_t(detail::kConstantDeltaBlocks-unchanged);
            // Reported counters keep their meaning: blocks whose bits changed.
            writes.unchanged_blocks=unchanged;
            writes.float_bytes=last_changed_blocks_*kBlockBytes;
        }
        // Shared constants depend on current draw/FSI bindings. Always write
        // their complete original ABI and retain the original full-range flush.
        std::memcpy(destination.data()+kFloatConstantBytes,shared.data(),shared.size());
        writes.shared_bytes=shared.size();
        flush();
        valid_=true;
        return writes;
    }
};
static_assert(kFloatConstantBytes%ConstantDeltaPacker::kBlockBytes==0);
static_assert(detail::kConstantDeltaBlocks<=255);
} // namespace gow3::renderer
#undef GOW3_CONSTANT_DELTA_TARGET
#undef GOW3_CONSTANT_DELTA_X86
