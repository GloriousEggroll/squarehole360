#version 450
#extension GL_EXT_samplerless_texture_functions : require
#extension GL_ARB_shader_stencil_export : require
layout(set=0,binding=0) uniform texture2D source_image;
// scale: destination pixels per source pixel (internal resolution scaling; 0 is 1).
layout(push_constant) uniform Parameters {uint scale;} pc;
void main() {
    vec4 value=texelFetch(source_image,ivec2(gl_FragCoord.xy)/int(max(pc.scale,1u)),0);
    // Identical conversion to the nine-draw restore shader. Both outputs are
    // assigned on every path; no early-fragment-test mode or discard is used.
    uint stencil=uint(roundEven(clamp(value.a,0.0,1.0)*255.0));
    gl_FragDepth=value.r;
    gl_FragStencilRefARB=int(stencil);
}
