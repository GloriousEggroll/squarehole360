#pragma once

#include "content_hash.h"
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace gow3::renderer {
inline constexpr std::string_view kNativeReadonlyDepthLessIdentity =
    "native-readonly-d32-depth-less-v1";
inline constexpr std::string_view kNativeReadonlyDepthLessSourceSpirv =
    "537372feea78ed2b42248073439ca90cedd098502862ca02c6f7f40123d7e61a";

// The current zero-bias readonly D32 fragment encodes original FragCoord.z to
// FP24 then decodes at half scale. For defined finite unit-range raster depth,
// this is <=FragCoord.z, including subnormal truncation/ties and NaN bias->0.
// Preserve all instructions and the late exact depth/query behavior. Only add
// DepthLess; never EarlyFragmentTests or unquantized hardware depth. The caller
// must retain the original per-draw admitted state and GREATER_EQUAL compare.
inline std::optional<std::vector<uint32_t>> TryNativeReadonlyDepthLess(
        std::span<const uint32_t> words) {
    if(words.size()!=1175||ContentHash({reinterpret_cast<const uint8_t*>(words.data()),
                                      words.size_bytes()})!=kNativeReadonlyDepthLessSourceSpirv)
        return std::nullopt;
    uint32_t origin=0,replacing=0;size_t insert=0;
    for(size_t at=5;at<words.size();) {
        const uint32_t count=words[at]>>16,opcode=words[at]&65535u;
        if(!count||count>words.size()-at)return std::nullopt;
        if(opcode==16) {
            if(count!=3||words[at+1]!=4)return std::nullopt;
            if(words[at+2]==7)++origin;
            else if(words[at+2]==12){++replacing;insert=at+count;}
            else return std::nullopt;
        }
        if(opcode==331)return std::nullopt;
        at+=count;
    }
    if(origin!=1||replacing!=1||!insert)return std::nullopt;
    std::vector<uint32_t> result;result.reserve(words.size()+3);
    result.insert(result.end(),words.begin(),words.begin()+insert);
    result.insert(result.end(),{(3u<<16)|16u,4u,15u}); // OpExecutionMode %main DepthLess.
    result.insert(result.end(),words.begin()+insert,words.end());
    return result;
}
} // namespace gow3::renderer
