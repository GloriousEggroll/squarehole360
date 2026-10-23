struct PushConstants {uint64_t VertexShaderConstants,PixelShaderConstants,SharedConstants;};
[[vk::push_constant]] ConstantBuffer<PushConstants> g_PushConstants;
// The paired source-preparation fixture inserts the canonical FSI helper here.
// GOW3_INSERT_FSI_HELPER
[[vk::binding(1,0)]] ByteAddressBuffer fragments;
void main(float4 position:SV_Position,[[vk::location(0)]] nointerpolation uint ordinal:TEXCOORD0) {
    float4 color=asfloat(fragments.Load4(ordinal*32));
    float4 properties=asfloat(fragments.Load4(ordinal*32+16));
    float depth=properties.x+position.x*properties.w;
    float2 slope=float2(ddx_coarse(depth),ddy_coarse(depth));
    // Different color exports make aliased target ordering observable. The
    // same production alpha/depth/interlock/late-discard epilogue is used.
    gow3NativeRasterOutput(position.xy,depth,properties.z==0,
        color,color.zwxy,color.yxwz,color.wzyx,15u,properties.y!=0,slope);
}
