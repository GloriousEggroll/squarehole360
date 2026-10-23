#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>
namespace gow3::renderer {
struct NativeRetainedOpaqueProgram;
struct NativeRetainedOpaqueCatalogPair {
    std::string_view vertex,pixel;
    constexpr bool operator==(const NativeRetainedOpaqueCatalogPair&)const=default;
};
inline constexpr uint32_t kRetainedOpaqueCatalogMaxPrograms=3;
inline constexpr uint32_t kRetainedOpaqueCatalogMaxPipelines=6;
inline constexpr uint64_t kRetainedOpaqueCatalogMaxModuleBytes=16u*1024*1024;
inline constexpr std::array<NativeRetainedOpaqueCatalogPair,3> kRetainedOpaqueCatalogPairs{{
    {"f7dba9374d4cba41785f4e2810d881f559a2f3b0cbc40fc8a1f58ee49f4eb969",
     "49f26283e1436f4cce68ac4e75695c6375176768b1dbfbf6c1f79d9bef434ced"},
    {"75d5a00f6d18df60d999164bb9374eabfde56d1b48fc3f7efea5f57ed634f802",
     "a3c9ae7e8cebe9024d1e53447bf8883ed57a9013fda97ef08a7d9f7fc04a145d"},
    {"75d5a00f6d18df60d999164bb9374eabfde56d1b48fc3f7efea5f57ed634f802",
     "55c79ab30e62247886a8c74dc440f04695ffd0f6f88842ffa1aea0b6829ee6c6"}
}};
inline constexpr const NativeRetainedOpaqueCatalogPair* RetainedOpaqueCatalogPair(
        std::string_view vertex,std::string_view pixel) noexcept {
    for(const auto& pair:kRetainedOpaqueCatalogPairs)if(pair.vertex==vertex&&pair.pixel==pixel)return &pair;
    return nullptr;
}
// Exact original Native host raster recipes; no state is inferred from source identity.
inline constexpr bool RetainedOpaqueCatalogRaster(const std::array<uint32_t,6>& raster)noexcept {
    return raster[0]==3&&(raster[1]==1||raster[1]==2)&&raster[2]==1&&raster[3]==4&&!raster[4]&&!raster[5];
}
struct NativeRetainedOpaqueCatalog {
    // Immutable after startup. Entries authenticate each allowed vertex/pixel pair.
    // At most three programs, one module per exact pixel source, six PSOs.
    std::vector<std::shared_ptr<const NativeRetainedOpaqueProgram>> programs;
};
}
