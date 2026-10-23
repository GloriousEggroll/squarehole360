#version 450
#extension GL_EXT_samplerless_texture_functions : require
layout(set=0, binding=0) uniform texture2D source_image;
layout(push_constant) uniform Parameters { ivec2 offset; float scale; uint conversion; uint swizzle; } pc;
layout(location=0) out vec4 result;
float float24(float value) {
    if (!(value > 0.0)) return 0.0;
    uint bits = floatBitsToUint(min(value, uintBitsToFloat(0x3ffffff8u)));
    uint packed;
    if (bits < 0x38800000u) packed = (0x800000u | (bits & 0x7fffffu)) >> min(113u - (bits >> 23), 24u);
    else packed = bits + 0xc8000000u;
    packed = min((packed + 3u + ((packed >> 3) & 1u)) >> 3, 0xffffffu);
    uint e = packed >> 20, m = packed & 0xfffffu;
    return e == 0 ? float(m) * exp2(-34.0) : float(m | 0x100000u) * exp2(float(int(e) - 35));
}
// Xenos 7e3: unsigned, bias 3, seven mantissa bits, no infinities.
float pack7e3(float value) {
    value = clamp(value, 0.0, 31.875);
    if (value < 0.25) return roundEven(value * 512.0);
    uint bits = floatBitsToUint(value) - 0x3e000000u;
    return float((bits + 32767u + ((bits >> 16) & 1u)) >> 16);
}
float unpack7e3(float value) {
    uint bits = uint(roundEven(clamp(value, 0.0, 1.0) * 1023.0));
    uint e = bits >> 7, m = bits & 127u;
    return e == 0 ? float(m) / 512.0 : float(m + 128u) * exp2(float(int(e) - 10));
}
void main() {
    result = texelFetch(source_image, ivec2(gl_FragCoord.xy) + pc.offset, 0) * pc.scale;
    if (pc.conversion == 1) {
        uint d = uint(roundEven(clamp(result.r, 0.0, 1.0) * 16777215.0));
        result = vec4(float(d + (d >> 23)) / 16777216.0);
    } else if (pc.conversion == 2) {
        result = vec4(float24(result.r));
    } else if (pc.conversion == 3) {
        vec4 grid = vec4(1023.0, 1023.0, 1023.0, 3.0);
        result = roundEven(clamp(result, 0.0, 1.0) * grid) / grid;
    } else if (pc.conversion == 4) {
        result = vec4(pack7e3(result.r)/1023.0, pack7e3(result.g)/1023.0,
                      pack7e3(result.b)/1023.0, roundEven(clamp(result.a,0.0,1.0)*3.0)/3.0);
    } else if (pc.conversion == 5) {
        result = vec4(unpack7e3(result.r), unpack7e3(result.g), unpack7e3(result.b), result.a);
    } else if (pc.conversion == 6) {
        uvec4 c = uvec4(roundEven(clamp(result,0.0,1.0)*255.0));
        uint bits = c.r | (c.g<<8) | (c.b<<16) | (c.a<<24);
        result = vec4(float(bits&1023u)/1023.0,float((bits>>10)&1023u)/1023.0,
                      float((bits>>20)&1023u)/1023.0,float(bits>>30)/3.0);
    } else if (pc.conversion == 7) {
        uvec4 c = uvec4(roundEven(clamp(result,0.0,1.0)*vec4(1023,1023,1023,3)));
        uint bits = c.r | (c.g<<10) | (c.b<<20) | (c.a<<30);
        result = vec4(bits&255u,(bits>>8)&255u,(bits>>16)&255u,bits>>24)/255.0;
    } else if (pc.conversion == 8) {
        // The guest 8-bit resolve rounds positive halfway values upward.
        // Quantize before an R8 host attachment can round them to even. Keep
        // multiply and add distinct, matching the reference resolve packer.
        precise vec4 scaled = clamp(mix(result,vec4(0),isnan(result)),0.0,1.0)*255.0;
        precise vec4 rounded = scaled+vec4(0.5);
        result = vec4(uvec4(rounded))/255.0;
    }
    // Apply guest fetch channels explicitly, including constant zero/one. This
    // also works on Vulkan hosts without image-view component swizzles.
    vec4 decoded = result;
    for (uint c=0u;c<4u;++c) {
        uint component=(pc.swizzle>>(3u*c))&7u;
        result[c]=component<4u?decoded[component]:float(component-4u);
    }
}
