#pragma once
#include "packed_color_variant8_catalog.h"
#include <optional>

namespace gow3::renderer {
// Separate, closed blend cohort. Original canonical variant8 bytes and exact
// 17 specialization values were proved; six unresolved pixel sources stay out.
// This policy never certifies monotonic depth or retained material ownership.
struct NativePackedColorBlendVariant8Recipe {
    std::string_view vertex,pixel;
    std::array<uint32_t,6> raster;
    std::array<uint32_t,15> state;
};
inline constexpr std::array<NativePackedColorBlendVariant8Recipe,19> kNativePackedColorBlendVariant8Recipes{{
    {"2c1688b2bde3f6e1612051f482e4cec1051f60df534a47dfc4138f3655b262d3","16b6fc585afa448a20e9230ae9e340d600ce7ade97ce56493523a548ff56d82c",{{3u,2u,1u,4u,0u,0u}},{{7341922u,1u,1u,3u,15u,16779014u,0u,0u,65537u,0u,0u,65537u,0u,0u,65537u}}},
    {"4a0683804f6243708850e6e397ec7893ab220e7cbfe93ae5e8cb28f8e0425cb1","484ebcd117e060e167f707206a9de7b8c8bf3558a091fc3ee47874619ad186e1",{{3u,2u,1u,4u,0u,0u}},{{7341922u,1u,1u,3u,15u,16777473u,0u,0u,65537u,0u,0u,65537u,0u,0u,65537u}}},
    {"679b080ad5278c495fbefa8ec6aee55b088b648e99f60fec8560b3fe4d0485db","24741c58b4d1d29312a50c485a36c60be998cd38fd675e0e0914c9f3dc8405b1",{{3u,2u,1u,4u,0u,0u}},{{7341922u,1u,1u,3u,15u,16779014u,0u,0u,65537u,0u,0u,65537u,0u,0u,65537u}}},
    {"679b080ad5278c495fbefa8ec6aee55b088b648e99f60fec8560b3fe4d0485db","68034564a837efb5f50142b31a439e24fd781447f808993d6c049338018a4c9a",{{3u,2u,1u,4u,0u,0u}},{{7341922u,1u,1u,3u,15u,16779014u,0u,0u,65537u,0u,0u,65537u,0u,0u,65537u}}},
    {"679b080ad5278c495fbefa8ec6aee55b088b648e99f60fec8560b3fe4d0485db","8161dce72f3f9eb44969f0a47d06b7375deb3b3d79375f4934a70d5660be7c40",{{3u,2u,1u,4u,0u,0u}},{{7341922u,1u,1u,3u,15u,16779014u,0u,0u,65537u,0u,0u,65537u,0u,0u,65537u}}},
    {"6cf1a8f547bbc850f8004a468a4c4b1e67eb8c85b5adbedeb248fa03d958bd84","0a382f5fc172e9ebd21743edc4252104961d36309acf2d156afcd4ca1254b81c",{{3u,1u,1u,4u,0u,0u}},{{7341922u,1u,1u,3u,15u,16779014u,0u,0u,65537u,0u,0u,65537u,0u,0u,65537u}}},
    {"7651f502a2455620ecf0796ca227725bffe1ef092f8576ac00007b57a10f486a","4f6015b37b5ab99126060707fbde199331e2e49a97613222f1501b2e8d1c9beb",{{3u,2u,1u,4u,0u,0u}},{{7341922u,1u,1u,3u,15u,16777473u,0u,0u,65537u,0u,0u,65537u,0u,0u,65537u}}},
    {"7651f502a2455620ecf0796ca227725bffe1ef092f8576ac00007b57a10f486a","8d7d720a3aa080a1b62ca773a704601862dc6737e9a1d3ad59495dce6a4292a5",{{3u,2u,1u,4u,0u,0u}},{{7341922u,1u,1u,3u,15u,16777473u,0u,0u,65537u,0u,0u,65537u,0u,0u,65537u}}},
    {"8481e1ad2902eb836ac9f35912140c17dd250fa5127c4f540d0cf8624e168055","0a674266d6cbdded11253ca5e9e5f8ad356af447f3196fe9b0d3c504ef1a8440",{{3u,1u,1u,4u,0u,0u}},{{7341922u,1u,1u,3u,15u,16779014u,0u,0u,65537u,0u,0u,65537u,0u,0u,65537u}}},
    {"8481e1ad2902eb836ac9f35912140c17dd250fa5127c4f540d0cf8624e168055","0a674266d6cbdded11253ca5e9e5f8ad356af447f3196fe9b0d3c504ef1a8440",{{3u,2u,1u,4u,0u,0u}},{{7341922u,1u,1u,3u,15u,16779014u,0u,0u,65537u,0u,0u,65537u,0u,0u,65537u}}},
    {"8481e1ad2902eb836ac9f35912140c17dd250fa5127c4f540d0cf8624e168055","93c4338f9212a60d4b50112c5bb206e39d35192c062fe0db2d8324b2cb935fd1",{{3u,2u,1u,4u,0u,0u}},{{7341922u,1u,1u,3u,15u,16779014u,0u,0u,65537u,0u,0u,65537u,0u,0u,65537u}}},
    {"8682d758c0051a0f54e586e899d53c40faa884f00501b406857f86280d740c1b","47e0cd17d835e8c97e9e28e50a07a783fab4b3ba31db9083f7a9a3e6e6f195ef",{{3u,2u,1u,4u,0u,0u}},{{7341922u,1u,1u,3u,15u,16777473u,0u,0u,65537u,0u,0u,65537u,0u,0u,65537u}}},
    {"8682d758c0051a0f54e586e899d53c40faa884f00501b406857f86280d740c1b","64e6f2fe9a3638cbf479b01396bf481b87084ed15d8a1545fcf49cb6a1980529",{{3u,2u,1u,4u,0u,0u}},{{7341922u,1u,1u,3u,15u,16777473u,0u,0u,65537u,0u,0u,65537u,0u,0u,65537u}}},
    {"8682d758c0051a0f54e586e899d53c40faa884f00501b406857f86280d740c1b","ce22bfe59e7b51b5eaf4669554e03040fcbb5fb3f91d5796174e4e14e30087fa",{{3u,2u,1u,4u,0u,0u}},{{7341922u,1u,1u,3u,15u,16777473u,0u,0u,65537u,0u,0u,65537u,0u,0u,65537u}}},
    {"b81393e75259d97983438e3e30ca0ad8e70edf94d694ce13c30cee1c2dcd7562","9801dd2229710d40f50039d6de60b81f36bada0fe3d068d1d70cbacdb2e00240",{{3u,2u,1u,4u,0u,0u}},{{7341922u,1u,1u,3u,15u,16777473u,0u,0u,65537u,0u,0u,65537u,0u,0u,65537u}}},
    {"b81393e75259d97983438e3e30ca0ad8e70edf94d694ce13c30cee1c2dcd7562","b870919728f165877080a0b7f4032357891e3b2d65e15b18d3a50518065d7c93",{{3u,2u,1u,4u,0u,0u}},{{7341922u,1u,1u,3u,15u,16777473u,0u,0u,65537u,0u,0u,65537u,0u,0u,65537u}}},
    {"cd66fe55f328785ba625920e57eaf7a14e3695effc3fce23ee27b6f2eb6e6e55","b1e206701365bc7b4ba7d3196c192dc854700b825450d4e93bc91929c8455230",{{3u,2u,1u,4u,0u,0u}},{{7341922u,1u,1u,3u,15u,16777473u,0u,0u,65537u,0u,0u,65537u,0u,0u,65537u}}},
    {"f5628ce62e3a395f3a2ccb7bbdc7af5aa76f79412165dba9f57f45cc70b969f8","22de606147c967769f9ece0b6b46de8b554624b6d916cf52c25b18575c193736",{{1u,2u,1u,4u,0u,0u}},{{7341922u,1u,1u,3u,15u,16779014u,0u,0u,65537u,0u,0u,65537u,0u,0u,65537u}}},
    {"f5628ce62e3a395f3a2ccb7bbdc7af5aa76f79412165dba9f57f45cc70b969f8","cce0811d88c3dcb86c5553b23a96bbddcbe0c30e4c2e74be75597aee64c6681a",{{1u,2u,1u,4u,0u,0u}},{{7341922u,1u,1u,3u,15u,16779014u,0u,0u,65537u,0u,0u,65537u,0u,0u,65537u}}},
}};
inline constexpr std::optional<size_t> NativePackedColorBlendVariant8RecipeIndex(
        std::string_view vertex,std::string_view pixel,const std::array<uint32_t,6>& raster,
        const NativeFsiStateSpecialization& state,int32_t color_format,int32_t depth_format) noexcept {
    if(color_format!=20||depth_format!=-1||state.words[0]!=0x700762u||state.words[2]!=1u||
       (state.words[5]!=0x01000706u&&state.words[5]!=0x01000101u))return std::nullopt;
    const auto key=std::tie(vertex,pixel,raster,state.words);
    const auto found=std::lower_bound(kNativePackedColorBlendVariant8Recipes.begin(),kNativePackedColorBlendVariant8Recipes.end(),key,
        [](const auto& row,const auto& value){return std::tie(row.vertex,row.pixel,row.raster,row.state)<value;});
    if(found==kNativePackedColorBlendVariant8Recipes.end()||
       std::tie(found->vertex,found->pixel,found->raster,found->state)!=key)return std::nullopt;
    return size_t(found-kNativePackedColorBlendVariant8Recipes.begin());
}
inline constexpr bool NativePackedColorBlendVariant8Tuple(
        std::string_view vertex,std::string_view pixel,const std::array<uint32_t,6>& raster,
        const NativeFsiStateSpecialization& state,int32_t color_format,int32_t depth_format) noexcept {
    return NativePackedColorBlendVariant8RecipeIndex(vertex,pixel,raster,state,color_format,depth_format).has_value();
}
inline bool NativePackedColorBlendVariant8PassAllowed(const NativeFsiConstants& raster,
        const NativePackedColorVariant8PassProof& p,size_t recipe_index) noexcept {
    if(recipe_index>=kNativePackedColorBlendVariant8Recipes.size()||
       !p.exact_tuple||!p.vertex_module||p.vertex_module!=p.certified_vertex_module||
       !p.pixel_module||p.pixel_module!=p.certified_pixel_module||
       !p.pipeline||p.pipeline!=p.certified_pipeline||
       p.vertex_variant!=NativeShaderVariant::Unmodified||p.pixel_variant!=NativeShaderVariant::PipelineStateEpilogueV1||
       !p.cpu_geometry||!p.indexed_triangles||p.active_query||p.discarded||p.rectangle||
       !p.private_framebuffer||!p.bounded_scissor||!p.primary_surfaces||!p.single_sample_layer||
       raster.pitch!=1280||raster.depth_base!=0||raster.color[0].base!=720||
       NativeFsiStateSpecialization::From(raster).words!=kNativePackedColorBlendVariant8Recipes[recipe_index].state)return false;
    return true;
}
} // namespace gow3::renderer
