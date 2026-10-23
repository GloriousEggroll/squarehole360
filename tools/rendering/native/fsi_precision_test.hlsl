#define GOW3_FSI_TEST 1
#define GOW3_FSI_EDRAM_SET 0
#include "fsi_precision.hlsl"
[[vk::push_constant]] ConstantBuffer<Gow3FsiRasterState> testState;
[[vk::binding(1,0)]] ByteAddressBuffer fragments;
void main(float4 position:SV_Position,[[vk::location(0)]] nointerpolation uint ordinal:TEXCOORD0) {
    raster=testState;
    float4 color=asfloat(fragments.Load4(ordinal*32));
    float depth=asfloat(fragments.Load(ordinal*32+16));
    vk::ext_execution_mode(5366);
    gow3BeginInterlock();
    DeviceMemoryBarrier();
    if(nativeDepthStencil(uint2(position.xy),depth,true))nativeColorOutput(uint2(position.xy),color);
    DeviceMemoryBarrier();
    gow3EndInterlock();
}
