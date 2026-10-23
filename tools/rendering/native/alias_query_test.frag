#version 450
#extension GL_EXT_samplerless_texture_functions : require
layout(set=0,binding=0) uniform texture2D images[32];
layout(location=0) out vec4 color;
void main() {
    vec4 sampled=texelFetch(images[0],ivec2(0),0);
    if(sampled.r<0.5)discard;
    color=sampled;
}
