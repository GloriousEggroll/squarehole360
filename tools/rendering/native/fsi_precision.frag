#version 450
#extension GL_ARB_fragment_shader_interlock : require
#extension GL_GOOGLE_include_directive : require
layout(pixel_interlock_ordered) in;
layout(location=0) flat in uint ordinal;
#include "fsi_precision.glsl"
struct Fragment {vec4 color;vec4 depth;};
layout(set=0,binding=1,std430) readonly buffer Inputs {Fragment fragments[];} inputs;
void main() {
    vec4 color=inputs.fragments[ordinal].color;
    float depth=inputs.fragments[ordinal].depth.x;
    beginInvocationInterlockARB();
    memoryBarrierBuffer();
    nativeRasterOutput(color,depth,gl_FrontFacing);
    memoryBarrierBuffer();
    endInvocationInterlockARB();
}
