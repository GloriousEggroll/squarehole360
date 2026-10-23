#version 450
#extension GL_ARB_fragment_shader_interlock : require
#extension GL_EXT_samplerless_texture_functions : require
layout(pixel_interlock_ordered) in;
layout(set=0,binding=0) uniform texture2D images[32];
layout(set=4,binding=0,std430) coherent buffer NativeEdram {uint words[];} edram;
layout(location=0) out vec4 dummy_color;
void main() {
    vec4 value=texelFetch(images[16],ivec2(0),0);
    // Signed float channels must preserve their values even when the guest
    // sign fields all select signed and number_format is integer1.
    if(any(notEqual(value,vec4(1,-2,3,-4))))discard;
    uvec2 pixel=uvec2(gl_FragCoord.xy);
    beginInvocationInterlockARB();memoryBarrierBuffer();
    if(!gl_HelperInvocation&&pixel.x<16&&pixel.y<16) {
        uint at=pixel.y*16+pixel.x;edram.words[at]=edram.words[at]+1u;
    }
    memoryBarrierBuffer();endInvocationInterlockARB();dummy_color=vec4(0);
}
