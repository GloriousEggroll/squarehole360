#version 450
#extension GL_ARB_fragment_shader_interlock : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
layout(pixel_interlock_ordered) in;
layout(location=0) in vec4 coordinates;
layout(location=0) out vec4 dummy_color;
layout(set=4,binding=0,std430) coherent buffer NativeEdram {uint words[];} edram;
layout(buffer_reference,std430,buffer_reference_align=4) readonly buffer Constants {uint words[];};
layout(push_constant,std430) uniform Addresses {
    uint64_t vertex_constants;
    uint64_t pixel_constants;
    uint64_t shared_constants;
} addresses;
void main() {
    // Derivatives are evaluated before any divergent control flow. Each draw
    // has a separate EDRAM region so later extents cannot hide earlier writes.
    vec4 gradient=vec4(dFdx(coordinates.xy),dFdy(coordinates.xy));
    uint pass=Constants(addresses.pixel_constants).words[0];
    uvec2 pixel=uvec2(gl_FragCoord.xy);
    beginInvocationInterlockARB();
    memoryBarrierBuffer();
    if(!gl_HelperInvocation&&pixel.x<64&&pixel.y<64) {
        uint first=(pass*64*64+pixel.y*64+pixel.x)*4;
        uvec4 bits=floatBitsToUint(gradient);
        for(uint i=0;i<4;++i)edram.words[first+i]=bits[i];
    }
    memoryBarrierBuffer();
    endInvocationInterlockARB();
    dummy_color=gradient;
}
