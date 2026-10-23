#pragma once
#include "forward_control_flow.h"
#include <array>

namespace gow3::renderer {
inline constexpr std::string_view kForwardPixelCatalogIdentity="native-fsi-forward-color-catalog-v1";
// Only these observed color0 sources are candidates. Actual queried replay
// must cover every selected source before this diagnostic is benchmarked.
inline constexpr std::array<std::string_view,53> kForwardPixelCatalogSources{
    "0229a7d1a87202fc20113ece34875c53cfed521734402c6163fc8b73ea13aa8a",
    "042d9ac7624ed89e8c80a5c0e8cf00b563784c79395f3096a3142685f40fd7c8",
    "07ab01db76715d2b37b64273821577c3b22c3f1ff752981b5d5559fc9c443cb1",
    "0a94610849f6cdebfbfa41fb7c840852e2562bd1f39e1dedddb2275e4cfa0b63",
    "0f05dfa28809cda47c013207d01a050ee927f0e400ec4e80a5a141443edc8e3d",
    "1010f9623d6f15d20235c86a20fe9de3eb6625ae384c4fb0c884af1d4fbfc468",
    "1935752b20a26cf82d0cb876b81967b284e903afafc9acf037b43c92ffce8dc7",
    "1cf53b195f080c7840cf3c3a2bb39244a9c5512023781dbb5fc815ee75b117b8",
    "1fd971a51719f6f2ea2dbbceb7e847513cc126ed579ea15a92059bffb8f37b37",
    "201f1af4e52625af70f818521b4cffaaa1b1881a473ed0b27b50fb0c693c6313",
    "22de606147c967769f9ece0b6b46de8b554624b6d916cf52c25b18575c193736",
    "22f954ec98dd5731e7fbd2d7d98b9e7cd67ab866e1dd447c9869fc7ecf07945d",
    "279b1efb14ec5938507c28ee7e11ef94f48b1eab4320b0ed0b951066cd786fd4",
    "2d46b4519eb56dffe719e38d827856a8d05303c8d77139a459d85d5f4751fe6f",
    "2f171c4bbda2315d2363bc63dec3b8b5256dce0c6fc223705d46b57c96fa42dc",
    "355f521e0c4e25e691934d78c1dbf9e78223d99927bfa81140a6100d64c00eaa",
    "359243188e0f06971f2886df087e929785edc5fffae8cb338398bb85699dc404",
    "3f4a745859e70c0fba277793adb4bfad82ee11756fb5d97ee5df6bbd6633880c",
    "4656f12e23c08ae036872f12f325d83d279a919824ba1fae70c56dcddac05129",
    "49be8aa208898d6ac4860840a1780ad1f40672acfee7e94062e3d24534ba9228",
    "49f26283e1436f4cce68ac4e75695c6375176768b1dbfbf6c1f79d9bef434ced",
    "4b05cc90ed92d55b2bcaa74a4372218b121124cf287a342ce6245183b9af4db3",
    "4dae3ef2af763c08f8f8a21fa34749a2a8182c07abd615dfe62619b2202cf4d4",
    "56a6ae5c5227d83ed98d656776de0cc40253c4f411ee6c60ecef4c258d9a2fa6",
    "60f06efdd83a7c0f263fa6e735b37e103c99dac006d833f7fde985142019e9b0",
    "72478105be8ca45857d9fcf5e8d812a7276755d8dbf60f3c46175cda2206f4e6",
    "799fd4625a107d3223fdcd799185c81194380f321de41eacc3c413111deec29c",
    "7b0d28a1af61830f97317e0646b2915d79a7eb10123c59bdfabbc4aade9830b2",
    "83bec0a29f72990e7011938d1590b4c840f28a9bcc3303bc1e31aea9b15e7f43",
    "84e1b3967a2b905ba3927ed05180ac40fef86da3b43e98759994c0151ecfd6c3",
    "93c4338f9212a60d4b50112c5bb206e39d35192c062fe0db2d8324b2cb935fd1",
    "95d5ece6727854fb21786dde9a002f9cd0f8eb09f8fad4b98b07668e025e8589",
    "9c074792b97384f623472eb038f5d9f9a796683318a6b492dd88d9162ef4bc18",
    "a3c9ae7e8cebe9024d1e53447bf8883ed57a9013fda97ef08a7d9f7fc04a145d",
    "a720496d28568c333aab96a8172c8f8aecfaa38b35f0f7d78fd98272898f4efe",
    "accd65f3ad433580f3c8663b1c12a524b5fd95b7caf756a4c968d7ed17986e75",
    "adc47bcd990f137b6b4219adb512b1e24990379a0a5570d157e563415e250fc0",
    "af77232f70766c89d207f1ed1c1603f0d80f9458efa56a9c31fa4d397a60f961",
    "b02ec1327d2e57c518be5b663ba65e19e7f64b4c11344243fc2aab6a35cea026",
    "b105b7607eb32c92d461b3b6cf6ee6de4b5cd517b46e7284105f62ae100e3b96",
    "b1e206701365bc7b4ba7d3196c192dc854700b825450d4e93bc91929c8455230",
    "b5079c079c1bdc4975c3fe08686a0ee563b9cb61ddbf182048c896f2b194e27f",
    "cce0811d88c3dcb86c5553b23a96bbddcbe0c30e4c2e74be75597aee64c6681a",
    "cdf9bcd4257edc209b174a1925f0ee6557ca1500fab1f2fbf2795304d7441036",
    "cff2cd752dcbc7e7e53e8624938dc261c915b965180554cc439f446fc4c4d85f",
    "dcd22480d0a99f00d41c5bbb24adfe230e402eb33d965519c4f7a0d765f6369f",
    "e413a5d1f8ecffb4f709964a31952aa5e377bb4a331dcc399dd1f28468401c87",
    "e558dd03a681aa051d9a629432cb6102178015751d1127aa7b4a8f85bcb185a9",
    "ed3673ee73fb3ac02562a7e0d9777e0a39f604bc90e85d7528264cee97586e28",
    "f5e7c3c56f8df25eee998e136472cbec860d93cdc98175c655bec39aa1a0311c",
    "f8caccd2b650765f328d5000d3254c35c0cb362abf99a0a0ebe21c9b3dfd0b86",
    "fc84b0647506f195a1c7543eebc57280eaae04a9c7490811cb79a862b78999b0",
    "fd74d763e4eaabdfe6576c6302acc9e635ea0359beccc0736eac1823585bc3ee",
};
inline constexpr uint32_t kForwardPixelCatalogMaxPrograms=128,kForwardPixelCatalogMaxPipelines=256;
inline bool ForwardPixelCatalogSource(std::string_view source) noexcept {
    return std::binary_search(kForwardPixelCatalogSources.begin(),kForwardPixelCatalogSources.end(),source);
}
inline bool ForwardPixelCatalogRasterKey(const std::array<uint32_t,6>& key) noexcept {
    return key==std::array<uint32_t,6>{3,1,1,4,0,0}||key==std::array<uint32_t,6>{3,2,1,4,0,0}||key==std::array<uint32_t,6>{1,2,1,4,0,0};
}
inline bool ForwardPixelCatalogAllowed(std::string_view pixel,const std::array<uint32_t,6>& key) noexcept {
    return ForwardPixelCatalogSource(pixel)&&ForwardPixelCatalogRasterKey(key);
}
inline std::optional<ForwardCfProgram> AnalyzeForwardPixelCatalog(std::span<const uint8_t> source) {
    const auto program=AnalyzeForwardControlFlow(source);
    if(!program)return std::nullopt;
    const auto be=[&](size_t at) {return uint32_t(source[at])<<24|uint32_t(source[at+1])<<16|
        uint32_t(source[at+2])<<8|source[at+3];};
    // This bounded metadata excludes depth and other color outputs.
    // All pinned source ALUs were separately audited for absent memexports.
    if(be(size_t(be(24))+28)!=1u)return std::nullopt;
    return program;
}
inline std::optional<std::string> TryForwardPixelCatalogSource(std::string_view source,const ForwardCfProgram& program) {
    constexpr std::string_view wrapper="\n\tuint pc = 0;\n\twhile (true)\n\t{\n\t\tswitch (pc)\n\t\t{\n";
    constexpr std::string_view finish="\t\t\tbreak;\n\t\t}\n\t\tbreak;\n\t}\n";
    constexpr std::string_view derivatives="float2 gow3FsiDepthSlope = float2(ddx_coarse(iPos.z), ddy_coarse(iPos.z));\n";
    constexpr std::string_view output="gow3NativeRasterOutput(iPos.xy, iPos.z, iFace, oC0, float4(0,0,0,0), float4(0,0,0,0), float4(0,0,0,0), 1u, false, gow3FsiDepthSlope);\n";
    if(program.instructions.empty())return std::nullopt;
    const auto begin=source.find(wrapper),slope=source.find(derivatives);
    if(begin==source.npos||slope==source.npos||slope>=begin||ForwardTokenCount(source,derivatives)!=1)return std::nullopt;
    const auto label="\t\tcase "+std::to_string(program.instructions.size()-1)+":\n";
    const auto terminal=source.find(label,begin+wrapper.size()),end=source.find(finish,begin+wrapper.size());
    if(terminal==source.npos||end==source.npos||terminal>=end)return std::nullopt;
    const auto clause=source.substr(terminal+label.size(),end-terminal-label.size());
    if(ForwardTokenCount(clause,output)!=1||ForwardTokenCount(clause,"gow3NativeRasterOutput(")!=1||
       ForwardTokenCount(clause,"oC0 *= g_ColorScale[0];\n")!=1||ForwardTokenCount(clause,"return;")!=1)return std::nullopt;
    // Existing exact wrapper/jump validation retains all other clause bytes,
    // including material/discard branches and complete original late FSI.
    return TryForwardControlFlowSource(source,program);
}
} // namespace gow3::renderer
