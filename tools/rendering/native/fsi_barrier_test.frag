#version 450
#extension GL_ARB_fragment_shader_interlock : require
layout(pixel_interlock_ordered) in;
layout(location=0) out vec4 dummy_color;
layout(set=4,binding=0,std430) coherent buffer NativeEdram {uint words[];} edram;
void main() {
    uvec2 pixel=uvec2(gl_FragCoord.xy);
    beginInvocationInterlockARB();
    memoryBarrierBuffer();
    if(!gl_HelperInvocation&&pixel.x<16&&pixel.y<16) {
        uint at=pixel.y*16+pixel.x;
        edram.words[at]=edram.words[at]+1u;
    }
    memoryBarrierBuffer();
    endInvocationInterlockARB();
    dummy_color=vec4(0.0);
}
