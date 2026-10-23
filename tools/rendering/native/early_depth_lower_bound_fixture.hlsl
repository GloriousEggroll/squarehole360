struct PushConstants {uint64_t VertexShaderConstants,PixelShaderConstants,SharedConstants;};
[[vk::push_constant]] ConstantBuffer<PushConstants> g_PushConstants;
// GOW3_INSERT_FSI_HELPER
[[vk::binding(1,0)]] ByteAddressBuffer fragments;
void main(float4 position:SV_Position,[[vk::location(0)]] nointerpolation uint ordinal:TEXCOORD0) {
    float4 color=asfloat(fragments.Load4(ordinal*32));
    // Preserve the unchanged canonical raw-depth/bias/pack/interlock epilogue.
    float2 slope=float2(ddx_coarse(position.z),ddy_coarse(position.z));
    gow3NativeRasterOutput(position.xy,position.z,true,color,color,color,color,1u,false,slope);
}
