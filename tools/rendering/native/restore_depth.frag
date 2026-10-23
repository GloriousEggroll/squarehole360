#version 450
#extension GL_EXT_samplerless_texture_functions : require
layout(set=0,binding=0) uniform texture2D source_image;
layout(push_constant) uniform Parameters {uvec3 unused;uint stencil_mask;} pc;
void main() {
    vec4 value=texelFetch(source_image,ivec2(gl_FragCoord.xy),0);
    uint stencil=uint(roundEven(clamp(value.a,0.0,1.0)*255.0));
    if(pc.stencil_mask!=0&&(stencil&pc.stencil_mask)==0)discard;
    gl_FragDepth=value.r;
}
