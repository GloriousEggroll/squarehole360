#pragma once
#include "content_hash.h"
#include "fsi_shader_variant.h"
#include <array>
#include <string_view>

namespace gow3::renderer {
struct NativePackedColorBlendVariant8ModulePin {
    std::string_view source,spirv;
    bool pixel;
    size_t bytes;
};
// Only immutable original module payloads. No shader rewriting or cache ABI
// change, and no admission into the separate monotonic-depth certificate.
inline constexpr std::array<NativePackedColorBlendVariant8ModulePin,28> kNativePackedColorBlendVariant8ModulePins{{
    {"2c1688b2bde3f6e1612051f482e4cec1051f60df534a47dfc4138f3655b262d3","214c2846634c4dac74974ff80b36ac65dec87b8e444650b8f176761c26bf5eb3",false,51124u},
    {"4a0683804f6243708850e6e397ec7893ab220e7cbfe93ae5e8cb28f8e0425cb1","e83f6adfd5406900cb45017ea3519160bec24d3013764e904b08d9c74c7438be",false,59528u},
    {"679b080ad5278c495fbefa8ec6aee55b088b648e99f60fec8560b3fe4d0485db","1f1db01409fd2b526fe54844c38de7e20792f0ccf22aac8190f23295e084ce19",false,45416u},
    {"6cf1a8f547bbc850f8004a468a4c4b1e67eb8c85b5adbedeb248fa03d958bd84","954757c536eb35754363350a88ab80dc2b015ba0c48832a2b51390480b01f6f0",false,45432u},
    {"7651f502a2455620ecf0796ca227725bffe1ef092f8576ac00007b57a10f486a","6dbb9398bccb93a7a062187579ad834978d40ab40c66bdbbbb2122a3057830f0",false,58872u},
    {"8481e1ad2902eb836ac9f35912140c17dd250fa5127c4f540d0cf8624e168055","fef8f2c1702fc6f06c8475031d337b376a16b1067958d520cec0802ed0611400",false,45360u},
    {"8682d758c0051a0f54e586e899d53c40faa884f00501b406857f86280d740c1b","b2e3747c79ce4575e5e5dffa881ee92ad762ef783ae32e4bcbfa58cabd6bfd8f",false,61360u},
    {"b81393e75259d97983438e3e30ca0ad8e70edf94d694ce13c30cee1c2dcd7562","d6f0e102dc3c53551b4ed3569089ac19fef69f67cb8386274b79830ed398b9aa",false,63788u},
    {"cd66fe55f328785ba625920e57eaf7a14e3695effc3fce23ee27b6f2eb6e6e55","cdf6ff6b08e978e5b8a25c9be0c2090f24bbef4643875192ee3a70dab97da3f8",false,59852u},
    {"f5628ce62e3a395f3a2ccb7bbdc7af5aa76f79412165dba9f57f45cc70b969f8","9662453464e5d451ae7f0b3c98e559427a2d58f9acc551356960f79011e1b052",false,45080u},
    {"0a382f5fc172e9ebd21743edc4252104961d36309acf2d156afcd4ca1254b81c","896cedc481338ed2f5551970272e68c8c312410c7f9b0a02890ce1472ab7337e",true,69636u},
    {"0a674266d6cbdded11253ca5e9e5f8ad356af447f3196fe9b0d3c504ef1a8440","3450135f2b8d1df1ea392a6cd633fa395034a5c3489cde2ca2dc6d292dea11aa",true,54396u},
    {"16b6fc585afa448a20e9230ae9e340d600ce7ade97ce56493523a548ff56d82c","42fed6cbe5f9d10a0a844a28e676ebb9bd7326faa3d0d63ec548f7bf4c565132",true,105744u},
    {"22de606147c967769f9ece0b6b46de8b554624b6d916cf52c25b18575c193736","41fb021bbb50c801b759800a2871423524fc91967d738d1880123ab628ae2d38",true,62236u},
    {"24741c58b4d1d29312a50c485a36c60be998cd38fd675e0e0914c9f3dc8405b1","f00b91d8bbf4949e8bab286344309bfedc44e05bd0902af0fa2419a0faa5bcad",true,56520u},
    {"47e0cd17d835e8c97e9e28e50a07a783fab4b3ba31db9083f7a9a3e6e6f195ef","cd80aae13d8c9f45b7c317140f79eb974f78f54070fbfd905bd56b207ec96740",true,79384u},
    {"484ebcd117e060e167f707206a9de7b8c8bf3558a091fc3ee47874619ad186e1","b147ac6bb902aba37b2864891f3c9f9a4324fcff76808d6509e1ec1cee6e2992",true,103304u},
    {"4f6015b37b5ab99126060707fbde199331e2e49a97613222f1501b2e8d1c9beb","aebcb7cd5dc1c19c711b9ffaa1e97a8f16fc49aa726fe4f1495adcd7c14f0d14",true,92544u},
    {"64e6f2fe9a3638cbf479b01396bf481b87084ed15d8a1545fcf49cb6a1980529","60b33bf75c3b3c87c7fdbd8867042fa250f72567f2cad6f9bc822c0f9fa708a7",true,79440u},
    {"68034564a837efb5f50142b31a439e24fd781447f808993d6c049338018a4c9a","ac23098aeb7b509332b1ba547a0143f4a91cb6f7ac313fe7485c94ab2a154783",true,62064u},
    {"8161dce72f3f9eb44969f0a47d06b7375deb3b3d79375f4934a70d5660be7c40","2c120ad63f5924d302105e5fd029a63a5b725ea1b33dbc06ebc985afa96fdc1d",true,68860u},
    {"8d7d720a3aa080a1b62ca773a704601862dc6737e9a1d3ad59495dce6a4292a5","ace4b366e29d21a6f470933933a928b68b3b3fcd1df52a3fa228bc83d1876117",true,108820u},
    {"93c4338f9212a60d4b50112c5bb206e39d35192c062fe0db2d8324b2cb935fd1","fbde251733ef6c792915c46814fbee73137147d2f7625df5e0eb2d93c938ee78",true,66340u},
    {"9801dd2229710d40f50039d6de60b81f36bada0fe3d068d1d70cbacdb2e00240","ac8711cf9e2cea3a81055964561ef70ba9aaeec6e5711887b81d4d17bbeda7f5",true,68828u},
    {"b1e206701365bc7b4ba7d3196c192dc854700b825450d4e93bc91929c8455230","5be9d53851272f6327ccf4381a857b50fba3e17a01c8d8ef066fbdf9429db01c",true,100220u},
    {"b870919728f165877080a0b7f4032357891e3b2d65e15b18d3a50518065d7c93","571399aa4bdd969eb0f72bb2979a549597c0fad05187b61c8c9dc03b99922424",true,82332u},
    {"cce0811d88c3dcb86c5553b23a96bbddcbe0c30e4c2e74be75597aee64c6681a","71a928abcc030d4370965f664cd6f0d8a27b14d7669e7867eac76ca78a0613cc",true,54184u},
    {"ce22bfe59e7b51b5eaf4669554e03040fcbb5fb3f91d5796174e4e14e30087fa","726d3c6030fbec8e8e9b62e47d6ffa4d927088467a3acafb080165f1c6ebd12e",true,79384u},
}};
inline bool NativePackedColorBlendVariant8ModuleCertified(std::string_view source,bool pixel,
        NativeShaderVariant variant,std::span<const uint8_t> bytes) noexcept {
    if(variant!=(pixel?NativeShaderVariant::PipelineStateEpilogueV1:NativeShaderVariant::Unmodified))return false;
    for(const auto& pin:kNativePackedColorBlendVariant8ModulePins)
        if(pin.source==source&&pin.pixel==pixel)
            try {return bytes.size()==pin.bytes&&ContentHash(bytes)==pin.spirv;}
            catch(...) {return false;}
    return false;
}
} // namespace gow3::renderer
