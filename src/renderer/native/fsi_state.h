#pragma once
#include "shader_abi.h"

namespace gow3::renderer {
// Native-only extension. Default shader variants keep the existing ABI exactly.
// Register values are decoded only at raster-operation time in the native
// epilogue, after the translated game pixel shader has produced its outputs.
struct alignas(16) NativeFsiConstants {
    uint32_t pitch=0,depth_base=0,depth_control=0,depth_format=0;
    uint32_t stencil_front=0,stencil_back=0;
    float front_slope_bias=0,front_constant_bias=0;
    struct ColorTarget {uint32_t base=0,format=0,mask=0,blend=0x10001;};
    std::array<ColorTarget,4> color{};
    std::array<float,4> blend_constant{};
    uint32_t blend_enabled=0;
    float back_slope_bias=0,back_constant_bias=0;
    uint32_t reserved=0;
};
struct alignas(16) NativeFsiShaderConstants {
    SharedShaderConstants shared;
    NativeFsiConstants raster;
};
static_assert(sizeof(NativeFsiConstants)==128);
static_assert(offsetof(NativeFsiConstants,color)==32);
static_assert(offsetof(NativeFsiConstants,blend_constant)==96);
static_assert(offsetof(NativeFsiConstants,blend_enabled)==112);
static_assert(offsetof(NativeFsiShaderConstants,raster)==2496);
static_assert(sizeof(NativeFsiShaderConstants)==2624);
}
