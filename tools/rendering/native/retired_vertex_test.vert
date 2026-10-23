#version 450
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
struct VertexStream {uint64_t address;uint byte_size;uint endian;};
// Keep this native ABI offset explicit. The GPU fixture consumes buffer device
// addresses exactly like a recompiled guest vertex shader, not a dummy stream.
layout(buffer_reference,std430,buffer_reference_align=16) readonly buffer Shared {
    uvec4 prefix[44]; // SharedShaderConstants::vertex_streams is byte704.
    VertexStream streams[96];
};
layout(buffer_reference,std430,buffer_reference_align=4) readonly buffer Vertices {
    vec4 positions[];
};
layout(push_constant,std430) uniform Addresses {
    uint64_t vertex_constants,pixel_constants,shared_constants;
} addresses;
void main() {
    VertexStream stream=Shared(addresses.shared_constants).streams[0];
    gl_Position=stream.byte_size>=48u
        ?Vertices(stream.address).positions[gl_VertexIndex]:vec4(-9.0,0.0,0.0,1.0);
}
