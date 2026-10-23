struct PushConstants {uint64_t VertexShaderConstants,PixelShaderConstants,SharedConstants;};
[[vk::push_constant]] ConstantBuffer<PushConstants> g_PushConstants;
#include "fsi_precision.hlsl"
void main(float4 position:SV_Position,bool face:SV_IsFrontFace) {
    float4 color=vk::RawBufferLoad<float4>(g_PushConstants.SharedConstants+2592);
    gow3NativeRasterOutput(position.xy,0.5f,face,color,0,0,0,1u,false,float2(0,0));
}
