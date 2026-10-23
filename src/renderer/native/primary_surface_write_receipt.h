#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
namespace gow3::renderer {
// Metadata proof only. ResolveMemory privately mints actual receipts after its
// original Assign loop succeeds. No page version, owner or GPU work is cached.
struct NativePrimarySurfaceWriteStamp {
    const void* owner=nullptr;
    uint64_t serial=0,epoch=0;
    uint32_t first_page=0,end_page=0;
};
inline std::optional<std::pair<uint32_t,uint32_t>> NativePrimarySurfaceWritePages(
        const std::array<uint32_t,6>& layout,
        std::span<const std::pair<uint32_t,uint32_t>> actual_ranges,uint32_t page_count) noexcept {
    constexpr std::array<uint32_t,6> color{1280,720,1280,720,3,0},depth{1280,720,1280,0,1,1};
    if(page_count<2560||actual_ranges.size()!=1)return std::nullopt;
    const auto expected=layout==color?std::pair<uint32_t,uint32_t>{3686400,7372800}:
        layout==depth?std::pair<uint32_t,uint32_t>{0,3686400}:std::pair<uint32_t,uint32_t>{0,0};
    if(expected.first==expected.second||actual_ranges[0]!=expected)return std::nullopt;
    return std::pair{expected.first/4096,expected.second/4096};
}
inline std::optional<std::array<uint64_t,2>> NativePrimarySurfaceWriteVersions(
        const NativePrimarySurfaceWriteStamp& color,const NativePrimarySurfaceWriteStamp& depth,
        const void* owner,uint64_t current_serial,uint64_t current_epoch) noexcept {
    // Adjacent completed writes, no wrap between them, and exactly disjoint
    // primary page coverage. A later physical write invalidates this shortcut.
    if(!owner||color.owner!=owner||depth.owner!=owner||
       color.first_page!=900||color.end_page!=1800||depth.first_page!=0||depth.end_page!=900||
       !color.serial||color.serial==UINT64_MAX||!depth.serial||
       color.serial+1!=depth.serial||depth.serial!=current_serial||
       color.epoch!=current_epoch||depth.epoch!=current_epoch)return std::nullopt;
    return std::array<uint64_t,2>{color.serial,depth.serial};
}
} // namespace gow3::renderer
