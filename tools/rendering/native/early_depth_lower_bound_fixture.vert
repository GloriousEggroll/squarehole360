#version 450
layout(location=0) flat out uint ordinal;
layout(set=0,binding=1,std430)readonly buffer Inputs {vec4 values[];} inputs;
void main() {
    vec2 p=vec2((gl_VertexIndex<<1)&2,gl_VertexIndex&2);
    uint id=uint(gl_InstanceIndex);
    gl_Position=vec4(p*2.0-1.0,inputs.values[id*2u+1u].x,1.0);
    ordinal=id;
}
