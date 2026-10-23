struct PushConstants {uint64_t VertexShaderConstants,PixelShaderConstants,SharedConstants;};
[[vk::push_constant]] ConstantBuffer<PushConstants> g_PushConstants;
// GOW3_INSERT_FSI_HELPER
[[vk::binding(1,0)]] ByteAddressBuffer fragments;
void main(float4 position:SV_Position,[[vk::location(0)]] nointerpolation uint ordinal:TEXCOORD0) {
    float4 color=asfloat(fragments.Load4(ordinal*48));
    float4 properties=asfloat(fragments.Load4(ordinal*48+16));
    uint4 control=fragments.Load4(ordinal*48+32);
    float depth=properties.x+position.x*properties.w;
    float2 slope=float2(ddx_coarse(depth),ddy_coarse(depth));
    // Derivatives precede material kill, matching the production call order.
    // Killed invocations never enter the raster epilogue or its interlock.
    if(control.y!=0u&&color.w<asfloat(control.z))discard;
    gow3NativeRasterOutput(position.xy,depth,properties.z==0,
        color,color.zwxy,color.yxwz,color.wzyx,control.x,properties.y!=0,slope);
}
