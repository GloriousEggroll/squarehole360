#version 450
// Project-owned fixture input: one draw deliberately contains overlapping
// triangles with different/interpolated depth, rather than repeated instances.
layout(set=0,binding=0,std430) readonly buffer Positions { vec4 positions[]; } source;
void main() { gl_Position=source.positions[gl_VertexIndex]; }
