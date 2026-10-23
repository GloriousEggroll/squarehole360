#include "draw_state.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>

namespace gow3::renderer {
using namespace plume;
namespace {
int32_t Signed15(uint32_t n) { return int32_t((n & 0x7fff) ^ 0x4000) - 0x4000; }
RenderBlend Factor(uint32_t n, bool alpha, bool programmable) {
    switch (n) {
    case 0: return RenderBlend::ZERO;
    case 1: return RenderBlend::ONE;
    case 4: return alpha ? RenderBlend::SRC_ALPHA : RenderBlend::SRC_COLOR;
    case 5: return alpha ? RenderBlend::INV_SRC_ALPHA : RenderBlend::INV_SRC_COLOR;
    case 6: return RenderBlend::SRC_ALPHA;
    case 7: return RenderBlend::INV_SRC_ALPHA;
    case 8: return alpha ? RenderBlend::DEST_ALPHA : RenderBlend::DEST_COLOR;
    case 9: return alpha ? RenderBlend::INV_DEST_ALPHA : RenderBlend::INV_DEST_COLOR;
    case 10: return RenderBlend::DEST_ALPHA;
    case 11: return RenderBlend::INV_DEST_ALPHA;
    case 16: return alpha ? RenderBlend::ONE : RenderBlend::SRC_ALPHA_SAT;
    case 12: case 13: case 14: case 15:
        // The packed EDRAM epilogue consumes the original blend register and
        // constants. Its host attachment has blending disabled entirely.
        if(programmable)return RenderBlend::ONE;
        [[fallthrough]];
    default: throw std::runtime_error("Unsupported native blend factor (constant factors require programmable blending)");
    }
}
RenderBlendOperation Operation(uint32_t n) {
    constexpr RenderBlendOperation ops[]{RenderBlendOperation::ADD, RenderBlendOperation::SUBTRACT,
        RenderBlendOperation::MIN, RenderBlendOperation::MAX, RenderBlendOperation::REV_SUBTRACT};
    if (n >= std::size(ops)) throw std::runtime_error("Invalid native blend operation");
    return ops[n];
}
RenderStencilFaceDesc Stencil(uint32_t n) {
    RenderStencilFaceDesc out;
    out.compareFunction = RenderComparisonFunction((n & 7) + 1);
    out.failOp = RenderStencilOp(((n >> 3) & 7) + 1);
    out.passOp = RenderStencilOp(((n >> 6) & 7) + 1);
    out.depthFailOp = RenderStencilOp(((n >> 9) & 7) + 1);
    return out;
}
}
DrawState DecodeDrawState(std::span<const uint32_t, kDrawStateWords> words,
                          uint32_t width, uint32_t height, uint32_t primitive,
                          bool programmable_blending) {
    if (!width || !height || width > 8192 || height > 8192) throw std::runtime_error("Invalid native render extent");
    auto reg = [&](uint32_t offset) { return words[(offset - kDrawStateOffset) / 4]; };
    auto number = [&](uint32_t offset) {
        const float v = std::bit_cast<float>(reg(offset));
        if (!std::isfinite(v)) throw std::runtime_error("Non-finite viewport state");
        return v;
    };
    DrawState state;
    auto& p = state.pipeline;
    switch (primitive) {
    case 1: p.primitiveTopology = RenderPrimitiveTopology::POINT_LIST; break;
    case 2: p.primitiveTopology = RenderPrimitiveTopology::LINE_LIST; break;
    case 3: p.primitiveTopology = RenderPrimitiveTopology::LINE_STRIP; break;
    case 4: case 8: p.primitiveTopology = RenderPrimitiveTopology::TRIANGLE_LIST; break;
    case 6: p.primitiveTopology = RenderPrimitiveTopology::TRIANGLE_STRIP; break;
    default: throw std::runtime_error("Unsupported native primitive topology");
    }
    const uint32_t mode = reg(0x2948), vte = reg(0x294c), clip = reg(0x2944);
    if ((vte & 0x73f) != 0x43f) throw std::runtime_error("Unsupported native vertex coordinate transform");
    if ((clip & 0x3f) || !(clip & (1u << 19))) throw std::runtime_error("Unsupported native clip planes/space");
    if (((mode >> 3) & 3) == 1 || (mode & (1u << 20)))
        throw std::runtime_error("Unsupported native polygon mode/interpolation");
    // D3D's offset setters maintain the front/back GPU values at 0x2a50.
    // Plume uses one bias for both faces, so reject differing active faces.
    const bool polygon = primitive == 4 || primitive == 6;
    const bool front_bias = polygon ? ((mode & (1u<<11)) && !(mode&1)) : bool(mode&(1u<<13));
    const bool back_bias = polygon && (mode&(1u<<12)) && !(mode&2);
    const std::array<float,2> front_offset{front_bias?number(0x2a50):0,front_bias?number(0x2a54):0};
    const std::array<float,2> back_offset{back_bias?number(0x2a58):0,back_bias?number(0x2a5c):0};
    if(polygon && !(mode&3) && front_offset!=back_offset)
        throw std::runtime_error("Distinct front/back polygon offsets require a native pipeline variant");
    const auto bias=(mode&1)?back_offset:front_offset;
    // Float24 has three fewer mantissa bits than float32. Match the reference
    // host path's bias scale; exact per-fragment 24-bit quantization is separate.
    state.depth_bias=bias[1]*((reg(0x2888)&0x10000)?16777216.f:16777215.f);
    state.slope_depth_bias=bias[0]/16.f;
    p.dynamicDepthBiasEnabled=front_bias||back_bias;
    if ((reg(0x2880) >> 16) & 3) throw std::runtime_error("Native multisampled targets not implemented");
    if (primitive == 6 && !(mode & (1u << 21)))
        throw std::runtime_error("Triangle strip without restart requires a native pipeline variant");
    state.discard = polygon && (mode & 3) == 3;
    p.cullMode = !polygon ? RenderCullMode::NONE : mode & 1 ? RenderCullMode::FRONT : mode & 2 ? RenderCullMode::BACK : RenderCullMode::NONE;
    p.frontFace = mode & 4 ? RenderFrontFace::CLOCKWISE : RenderFrontFace::COUNTER_CLOCKWISE;
    p.depthClipEnabled = !(clip & (1u << 16));
    const float xs = number(0x2908), xo = number(0x290c), ys = number(0x2910), yo = number(0x2914);
    const float zs = number(0x2918), zo = number(0x291c);
    if (xs <= 0 || ys >= 0 || zo < 0 || zo > 1 || zo + zs < 0 || zo + zs > 1)
        throw std::runtime_error("Unsupported native viewport orientation/depth range");
    // DXC flips clip-space Y. Positive Vulkan height then restores D3D's
    // top-left origin. Vulkan permits minDepth > maxDepth (reversed Z).
    const uint32_t window = reg(0x28c0);
    const int32_t wx = Signed15(window), wy = Signed15(window >> 16);
    state.viewport = RenderViewport(xo - xs + ((mode >> 16) & 1 ? wx : 0),
        yo + ys + ((mode >> 16) & 1 ? wy : 0), 2 * xs, -2 * ys, zo, zo + zs);
    const uint32_t stl = reg(0x28b8), sbr = reg(0x28bc), wtl = reg(0x28c4), wbr = reg(0x28c8);
    const int32_t ox = wtl & 0x80000000u ? 0 : wx, oy = wtl & 0x80000000u ? 0 : wy;
    state.scissor = RenderRect(
        std::max({0, Signed15(stl), int32_t(wtl & 0x3fff) + ox}),
        std::max({0, Signed15(stl >> 16), int32_t((wtl >> 16) & 0x3fff) + oy}),
        std::min({int32_t(width), Signed15(sbr), int32_t(wbr & 0x3fff) + ox}),
        std::min({int32_t(height), Signed15(sbr >> 16), int32_t((wbr >> 16) & 0x3fff) + oy}));
    state.discard |= state.scissor.isEmpty();
    const uint32_t depth = reg(0x2934), front = reg(0x2900), back = reg(0x28fc);
    p.depthEnabled = depth & 2;
    p.depthWriteEnabled = p.depthEnabled && (depth & 4);
    p.depthFunction = RenderComparisonFunction(((depth >> 4) & 7) + 1);
    p.stencilEnabled = depth & 1;
    p.stencilFrontFace = Stencil(depth >> 8);
    p.stencilBackFace = depth & 0x80 ? Stencil(depth >> 20) : p.stencilFrontFace;
    if (p.stencilEnabled && (depth & 0x80) && (front & 0xffffff) != (back & 0xffffff))
        throw std::runtime_error("Distinct front/back stencil masks require a native pipeline variant");
    p.stencilReference = front & 255; p.stencilReadMask = (front >> 8) & 255; p.stencilWriteMask = (front >> 16) & 255;
    constexpr uint32_t blend_offsets[]{0x2938, 0x2958, 0x295c, 0x2960};
    // D3D blend-factor setter 0x82AEABB8 expands ARGB into RGBA floats here.
    // Preserve float bit patterns; the raster epilogue applies format-specific
    // clamping and NaN handling just like the source/destination blend terms.
    for(uint32_t i=0;i<4;++i)state.blend_constant[i]=std::bit_cast<float>(reg(0x28e0+i*4));
    for (uint32_t i = 0; i < 4; ++i) {
        auto& blend = p.renderTargetBlend[i];
        const uint32_t b = reg(blend_offsets[i]);
        blend.srcBlend = Factor(b & 31, false, programmable_blending); blend.dstBlend = Factor((b >> 8) & 31, false, programmable_blending);
        blend.blendOp = Operation((b >> 5) & 7);
        blend.srcBlendAlpha = Factor((b >> 16) & 31, true, programmable_blending); blend.dstBlendAlpha = Factor((b >> 24) & 31, true, programmable_blending);
        blend.blendOpAlpha = Operation((b >> 21) & 7);
        blend.blendEnabled = (b & 0x1fff1fff) != 0x00010001;
        blend.renderTargetWriteMask = (reg(0x28dc) >> (i * 4)) & 15;
    }
    const uint32_t color = reg(0x293c);
    if (color & 0x10) throw std::runtime_error("Native alpha-to-coverage not implemented");
    if ((color & 8) && (color & 7) != 7) {
        // Preserve the original >= encoding (2) while representing the full
        // console comparison function without changing the shared-buffer ABI.
        state.specialization = 2 | (((color & 7) ^ 6) << 8);
    }
    state.alpha_reference = number(0x2904);
    state.index_offset = std::bit_cast<int32_t>(reg(0x28d4));
    // Console center convention is selected by PA_SU_VTX_CNTL.pix_center.
    // +0.5 in window space when console centers are at integer coordinates.
    if (!(reg(0x29c0) & 1)) state.half_pixel = {0.5f / xs, 0.5f / ys};
    return state;
}
}
