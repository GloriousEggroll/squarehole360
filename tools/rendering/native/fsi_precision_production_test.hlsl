struct PushConstants {uint64_t VertexShaderConstants,PixelShaderConstants,SharedConstants;};
[[vk::push_constant]] ConstantBuffer<PushConstants> g_PushConstants;
#include "fsi_precision.hlsl"
[[vk::binding(1,0)]] ByteAddressBuffer fragments;
void main(float4 position:SV_Position,[[vk::location(0)]] nointerpolation uint ordinal:TEXCOORD0) {
    float4 color=asfloat(fragments.Load4(ordinal*32));
    float4 properties=asfloat(fragments.Load4(ordinal*32+16));
    float depth=properties.x+position.x*properties.w;
    float2 slope=float2(ddx_coarse(depth),ddy_coarse(depth));
    gow3NativeRasterOutput(position.xy,depth,properties.z==0,color,0,0,0,1,properties.y!=0,slope);
}
