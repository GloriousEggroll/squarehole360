#pragma once
#include <cstdint>

namespace gow3::renderer {
// Every component remains float32. Flags mean an actual load pipeline exists
// and its storage/sampling/linear/transfer requirements were checked on the
// device. RG additionally requires proof of an enabled extended-format feature.
struct CompactAliasFormats {
    bool r32=false,rg32=false;
};
constexpr uint32_t DecodedAliasChannels(uint32_t dimension,uint32_t layers,
                                       uint32_t components,CompactAliasFormats formats) {
    if(dimension!=1||layers!=1)return 4;
    if(components==1&&formats.r32)return 1;
    if(components==2&&formats.rg32)return 2;
    return 4;
}
}
