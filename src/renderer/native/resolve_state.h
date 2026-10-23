#pragma once
#include <array>
#include <cstdint>

namespace gow3::renderer {
// Gears D3D Resolve (82AF3598, 82AF36D4, 82AF3804): the first
// destination fetch selector controls RB_COPY_DEST_INFO.copy_dest_swap.
// This is a storage operation, independent of later sampling/presentation
// selectors. Depth copies have one component and ignore the swap bit.
constexpr bool ResolveDestinationSwap(const std::array<uint32_t,6>& fetch,
                                      uint32_t source_slot) {
    return source_slot!=4 && (fetch[3]&0xe)!=0;
}
}
