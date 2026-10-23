#pragma once
#include <bit>
#include <cstdint>
#include <stdexcept>
namespace gow3::renderer::validation {
// Independent integer oracle for the two separately rounded IEEE binary32
// operations in canonical format22 load/store. In [2^22,2^23), the product
// rounds to d-1/2, then ties-to-even clears bit0. Below this range the final
// integer rounding recovers d; above it decode adds1 and product recovers d.
inline uint32_t AtlasPackedRoundTripOracle(uint32_t d) {
    if(d>0xffffffu)throw std::invalid_argument("Not a 24-bit depth");
    return d>=0x400000u&&d<0x800000u?d&~1u:d;
}
// Binary32 multiplication by2^24-1, then integer ties-to-even, implemented
// without floating arithmetic. This covers arbitrary finite host depth bits.
inline uint64_t AtlasRoundRightEven(uint64_t v,unsigned shift) {
    if(!shift)return v;
    if(shift>=64)return 0;
    const uint64_t q=v>>shift,r=v&((uint64_t(1)<<shift)-1),half=uint64_t(1)<<(shift-1);
    return q+(r>half||(r==half&&(q&1)));
}
inline uint32_t AtlasHostQuantizeOracle(uint32_t bits) {
    const unsigned exponent=(bits>>23)&255;
    if(exponent==255)throw std::invalid_argument("Nonfinite host oracle input");
    if(bits>>31||!(bits&0x7fffffffu))return 0;
    if(bits>=0x3f800000u)return 0xffffff;
    const uint64_t significand=(bits&0x7fffffu)|(exponent?0x800000u:0u);
    const int scale=exponent?int(exponent)-150:-149;
    const uint64_t exact=significand*0xffffffu;
    if(!exact)return 0;
    const unsigned top=63-std::countl_zero(exact);
    // Product cannot overflow and subnormals have zero integer result. Round
    // the exact product to24 significant bits first, then to an integer.
    const unsigned trim=top>23?top-23:0;
    const uint64_t rounded=AtlasRoundRightEven(exact,trim);
    const int power=scale+int(trim);
    return power>=0?uint32_t(rounded<<power):uint32_t(AtlasRoundRightEven(rounded,unsigned(-power)));
}
inline uint32_t AtlasDepthOffsetOracle(uint32_t x,uint32_t y) {
    if(x>=1120||y>=688)throw std::invalid_argument("Atlas oracle pixel out of bounds");
    const uint32_t tile=1440+(y/16)*14+x/80;
    return 4*(tile*1280+(y%16)*80+(x%80+40)%80);
}
} // namespace gow3::renderer::validation
