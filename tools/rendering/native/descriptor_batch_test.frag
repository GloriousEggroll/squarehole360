#version 450
layout(set=0,binding=0) uniform texture2D images2d[32];
layout(set=1,binding=0) uniform texture3D images3d[32];
layout(set=2,binding=0) uniform textureCube imagesCube[32];
layout(set=3,binding=0) uniform sampler samplers[32];
layout(location=0) out vec4 color;
void main() {
    vec4 a=textureLod(sampler2D(images2d[31],samplers[31]),vec2(0.5),0.0);
    vec4 b=textureLod(sampler3D(images3d[17],samplers[17]),vec3(0.5),0.0);
    vec4 c=textureLod(samplerCube(imagesCube[11],samplers[11]),vec3(1.0,0.0,0.0),0.0);
    if(a.r<0.5||b.g<0.5||c.b<0.5)discard;
    color=vec4(a.r,b.g,c.b,1.0);
}
