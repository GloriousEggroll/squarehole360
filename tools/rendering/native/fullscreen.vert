#version 450
layout(location=0) out vec4 coordinates;
void main() {
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
    coordinates = vec4(2 * p.x + 3 * p.y, 5 * p.x + 7 * p.y, 0, 1);
}
