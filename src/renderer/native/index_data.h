#pragma once
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace gow3::renderer {
namespace detail {
template<unsigned Width,unsigned Swap>
inline void DecodeIndexWords(std::span<const uint8_t> bytes,std::span<uint32_t> indices) {
    // Width and byte permutation are selected once per draw. Fixed offsets let
    // the compiler load/vectorize whole words without mutating the snapshot.
    const auto* source=bytes.data();
    for(auto& index:indices) {
        index=uint32_t(source[0^Swap])|(uint32_t(source[1^Swap])<<8);
        if constexpr(Width==4)
            index|=(uint32_t(source[2^Swap])<<16)|(uint32_t(source[3^Swap])<<24);
        source+=Width;
    }
}
}

// Guest index bytes are immutable. UP draws may include padding to four bytes;
// ordinary indexed draws contain exactly count*width bytes. Padding is never an
// index. Endian modes match the guest 8-in-16, 8-in-32 and 16-in-32 permutations.
inline void ValidateIndexPayload(std::span<const uint8_t> bytes,
    uint32_t count,uint32_t width,uint32_t endian,bool up_padding) {
    if(!count||count>4u*1024*1024||(width!=2&&width!=4)||endian>3||(width==2&&endian>1))
        throw std::runtime_error("Invalid index payload");
    const uint64_t extent=uint64_t(count)*width;
    const uint64_t expected=up_padding?(extent+3)&~uint64_t(3):extent;
    if(bytes.size()!=expected)throw std::runtime_error("Invalid index payload");
}
inline std::vector<uint32_t> DecodeIndexPayload(std::span<const uint8_t> bytes,
    uint32_t count,uint32_t width,uint32_t endian,bool up_padding) {
    ValidateIndexPayload(bytes,count,width,endian,up_padding);
    std::vector<uint32_t> indices(count);
    if(width==2) {
        if(endian==0)detail::DecodeIndexWords<2,0>(bytes,indices);
        else detail::DecodeIndexWords<2,1>(bytes,indices);
    } else {
        switch(endian) {
            case 0:detail::DecodeIndexWords<4,0>(bytes,indices);break;
            case 1:detail::DecodeIndexWords<4,1>(bytes,indices);break;
            case 2:detail::DecodeIndexWords<4,3>(bytes,indices);break;
            case 3:detail::DecodeIndexWords<4,2>(bytes,indices);break;
        }
    }
    return indices;
}
}
