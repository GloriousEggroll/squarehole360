#version 450
layout(set=0,binding=0,std430) readonly buffer PackedAtlas {uint words[];} packed;
layout(location=0) out float resolved_depth;
layout(location=1) out vec4 stencil_carrier;
void main() {
    uvec2 p=uvec2(gl_FragCoord.xy);
    // Exact 1120-pitch/base1440 depth EDRAM addressing, including the depth
    // half-tile column permutation. Scene/primitive reject every other layout.
    uint tile=1440u+(p.y/16u)*14u+p.x/80u;
    uint word=packed.words[tile*1280u+(p.y%16u)*80u+(p.x%80u+40u)%80u];
    uint d=word>>8u;
    // Canonical LoadSurfaceShared UNORM24 decode, followed by the ORIGINAL
    // Resolve quantizer. Keep both expressions: encode/decode is not identity.
    precise float decoded=float(d+(d>>23u))/16777216.0;
    precise float scaled=clamp(decoded,0.0,1.0)*16777215.0;
    uint resolved=uint(roundEven(scaled));
    resolved_depth=float(resolved+(resolved>>23u))/16777216.0;
    // The unchanged physical StoreDepthFromRgbaStencil applies its second
    // depth quantizer and the original alpha*255 roundEven stencil conversion.
    stencil_carrier=vec4(0.0,0.0,0.0,float(word&255u)/255.0);
}
