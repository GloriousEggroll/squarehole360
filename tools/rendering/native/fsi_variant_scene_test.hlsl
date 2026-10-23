struct PushConstants {uint64_t VertexShaderConstants,PixelShaderConstants,SharedConstants;};
[[vk::push_constant]] ConstantBuffer<PushConstants> g_PushConstants;
#include "fsi_precision.hlsl"
void main(float4 position:SV_Position,bool face:SV_IsFrontFace) {
    float2 slope=float2(ddx_coarse(position.z),ddy_coarse(position.z));
    gow3NativeRasterOutput(position.xy,position.z,face,float4(1,0,0,1),0,0,0,1u,false,slope);
}
