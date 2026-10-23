#version 450
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#include "readonly_depth_attachment.glsl"
layout(buffer_reference,std430,buffer_reference_align=4) readonly buffer Constants {uint words[];};
layout(push_constant,std430) uniform Addresses {
    uint64_t vertex_constants;
    uint64_t pixel_constants;
    uint64_t shared_constants;
} addresses;
// Intentionally no EarlyFragmentTests. Preserve the original coarse derivative
// and separate multiply/add bias operations even on the admitted zero-bias
// path, including nonfinite derivative behavior; do not fold uniform0*x away.
void main() {
    float depth=gl_FragCoord.z;
    vec2 derivatives=vec2(dFdxCoarse(depth),dFdyCoarse(depth));
    Constants raster=Constants(addresses.shared_constants+2496ul);
    vec2 bias=gl_FrontFacing?uintBitsToFloat(uvec2(raster.words[6],raster.words[7])):
                             uintBitsToFloat(uvec2(raster.words[29],raster.words[30]));
    precise float slope=max(abs(derivatives.x),abs(derivatives.y))*bias.x;
    precise float offset=slope+bias.y;
    depth+=offset;
    gl_FragDepth=nativeReadonlyD32(nativeReadonlyDepthCode(depth));
}
