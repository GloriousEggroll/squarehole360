#ifndef GOW3_RETAINED_OPAQUE_PHASE_DEPTH_GLSL
#define GOW3_RETAINED_OPAQUE_PHASE_DEPTH_GLSL
// Exact integer encoding of every 24-bit stored code into a distinct finite
// D32 value in [0,1]. For canonical Q(z), E(Q(z)) <= z on the admitted raster
// domain. This is a representation, not an arbitrary-float depth quantizer.
// Identity: fp24-cellfloor-unit-tail-v1. Keep V4 material outputs in agreement.
uint nativePhaseDepthBits(uint code) {
    if(code==0u)return 0u;
    uint lowerBits;
    if(code<=0x100000u) {
        uint threshold=8u*code-4u+(code&1u);
        uint leading=uint(findMSB(threshold));
        lowerBits=((leading+90u)<<23u)|((threshold<<(23u-leading))&0x7fffffu);
    } else lowerBits=0x38000000u+8u*code-4u+(code&1u);
    return min(lowerBits,0x3e800001u+code);
}
// Inverse only on E's image: canonical restore and certified V4 material
// outputs are the sole producers. Preserve the extended stored-code tail.
uint nativePhaseDepthCode(uint bits) {
    if(bits>=0x3f6db6deu)return bits-0x3e800001u;
    if(bits==0u)return 0u;
    if(bits<0x38800000u) {
        uint shift=min(113u-(bits>>23u),24u);
        uint threshold=(0x800000u|(bits&0x7fffffu))>>shift;
        return (threshold+4u)>>3u;
    }
    return (bits-0x38000000u+4u)>>3u;
}
#endif
