// Exact Xenos float24 encode/decode. The1/2 factor is a lossless order embedding
// of all24-bit codes into[0,1), not replacement by unquantized Vulkan depth.
float nativeReadonlyD32(uint code) {
    uint exponent=code>>20,mantissa=code&0xfffffu;
    if(exponent!=0)return uintBitsToFloat(((exponent+111u)<<23)|(mantissa<<3));
    return float(mantissa)*2.910383045673370361328125e-11; // Exact2^-35.
}
uint nativeReadonlyDepthCode(float value) {
    float depth=clamp(isnan(value)?0.0:value,0.0,1.0);
    if(!(depth>0.0))return 0u;
    uint bits=floatBitsToUint(depth);
    uint p=bits<0x38800000u?((0x800000u|(bits&0x7fffffu))>>min(113u-(bits>>23),24u)):bits+0xc8000000u;
    return min((p+3u+((p>>3)&1u))>>3,0xffffffu);
}
