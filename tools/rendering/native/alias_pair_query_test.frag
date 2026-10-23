#version 450
#extension GL_EXT_samplerless_texture_functions : require
layout(set=0,binding=0) uniform texture2D images[32];
layout(location=0) out vec4 color;
void main() {
    vec4 first=texelFetch(images[0],ivec2(0),0);
    vec4 second=texelFetch(images[1],ivec2(0),0);
    if(first.r<0.5||second.r<0.5)discard;
    color=first*second;
}
