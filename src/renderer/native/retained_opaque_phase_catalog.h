#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace gow3::renderer {
// Independent of the legacy retained-opaque catalogue. This closed inventory
// describes only the two authenticated 198/201-draw captured opaque phases.
// Inventory SHA256: b4afd5374f8c754ff5769d0188ea5ab513d0408835bcf0611cae6b1fbb778644
// A pixel source can occur with several vertices; the module identity never
// substitutes for the exact vertex/pixel/raster recipe or actual module proof.
inline constexpr size_t kRetainedOpaquePhaseMaxPrograms = 53;
inline constexpr size_t kRetainedOpaquePhaseMaxPipelines = 65;
// Measured sum of all 53 compiled candidate SPIR-V modules. Actual-byte
// certificates remain separately required at admission; additions or compiler
// changes need explicit recertification and rebudgeting.
inline constexpr uint64_t kRetainedOpaquePhaseMaxModuleBytes = 3432836;

struct NativeRetainedOpaquePhaseProgram {
    const std::string source_sha256;
    const std::string baseline_version;
    const std::string variant_version;
    const std::string spirv_sha256;
    const uint32_t specialization_mask;
    const std::vector<uint32_t> spirv;
};
struct NativeRetainedOpaquePhaseCatalog {
    std::vector<std::shared_ptr<const NativeRetainedOpaquePhaseProgram>> programs;
};

struct NativeRetainedOpaquePhaseRecipeEntry {
    std::string_view vertex, pixel;
    std::array<uint32_t, 6> raster;
    // Guest formats, not plume's host format enum or attachment descriptor.
    uint32_t guest_color_format = 3, guest_depth_format = 1;
    constexpr bool operator==(const NativeRetainedOpaquePhaseRecipeEntry&) const = default;
};

inline constexpr std::array<std::string_view, kRetainedOpaquePhaseMaxPrograms>
kRetainedOpaquePhasePixels{{
    "055dbdf3694342aed020cd27c9426fb71c57d1e60fe5267ce9c3cb538fb8a7d0",
    "07297a57c1d500b10d63b3eaf81a57e58ab0f0f2f9cf596f52ddad1c51ac83d9",
    "07ab01db76715d2b37b64273821577c3b22c3f1ff752981b5d5559fc9c443cb1",
    "0de61d56c71cf0ee32294c3f2f4c69dbbd154c35c7147af905b75dea16377af1",
    "1010f9623d6f15d20235c86a20fe9de3eb6625ae384c4fb0c884af1d4fbfc468",
    "16139b306f267b2efc5cf8524703f0853eca82e5711f79547e3e095d983de03f",
    "196cc79577c42aae9069a265d89cd0a672e7fcc9e54583af8563eed8bb0c1fa0",
    "1e3be76bf5f9ca19116d005818c83497d25b7791142af56babff056cd95acc43",
    "2187635535de20051571e43cf5d22aa78ea18167cba792dce95058de36868fa1",
    "27605aa5c6eac27fe3978faa61f0ee40fcbf13433d232d6b16087807322e6a05",
    "2852b1b3aa1faae6e25f03ed8af0697e96c7079879d7d51a328f47e8d0495eb8",
    "28b63a43c49413829c4059148707f9b8456c28926cd22d3ceec9f7e9f18cd286",
    "2d46b4519eb56dffe719e38d827856a8d05303c8d77139a459d85d5f4751fe6f",
    "2f171c4bbda2315d2363bc63dec3b8b5256dce0c6fc223705d46b57c96fa42dc",
    "300a99f2e7c4cfc50be61d85a8365526e4ff83f1bacf723cff4f4dc44d706fe0",
    "333ce299839d3d857f8829e4249516ce823ea95e86fcf82473b81c068bd3c79f",
    "3d564f1e649a2e5ebae927cc7dc715b496f40965b83351809f2f3adb80d009b8",
    "3f4a745859e70c0fba277793adb4bfad82ee11756fb5d97ee5df6bbd6633880c",
    "4656f12e23c08ae036872f12f325d83d279a919824ba1fae70c56dcddac05129",
    "472d1f7c9fad138c0dc86011ee5ac9e158c65fda70c0a2d55fbfe1328e66e809",
    "49f26283e1436f4cce68ac4e75695c6375176768b1dbfbf6c1f79d9bef434ced",
    "559b4677ef84fd5cf5646553ba1cb54f3ab71d3cfdeb30cf057a750c9a4346df",
    "55c79ab30e62247886a8c74dc440f04695ffd0f6f88842ffa1aea0b6829ee6c6",
    "60f06efdd83a7c0f263fa6e735b37e103c99dac006d833f7fde985142019e9b0",
    "67345542a0b3a75a3e5bcc75c818e099bc7a5a3387884d0bd13114dfa39167d7",
    "6e32849817c598e7294767382d90c345de2523013cbc86748489c25647886152",
    "72478105be8ca45857d9fcf5e8d812a7276755d8dbf60f3c46175cda2206f4e6",
    "779faa05c69ed3950abea4e20368f3fa6ca965da9755f2eba67a59ef37b0abf5",
    "799fd4625a107d3223fdcd799185c81194380f321de41eacc3c413111deec29c",
    "7e52d8ce2caf241b62f3816841fbae38de30394ded4a72b023ecffea46f99fd2",
    "88b188111071ce7552c98455d5fcfd3e85047d3fa968b6f8a74f57f0f78aeac6",
    "8de5f9c029b86d1b3ec7e61bb67bde0d38cee5c37a269c597a673f6fe3c8bce9",
    "95d5ece6727854fb21786dde9a002f9cd0f8eb09f8fad4b98b07668e025e8589",
    "9e4f58f4be540b9ca2a30d94463febf6504aa8a8dd88255097b1acb5b82122d1",
    "a1b82a360d85cee56b9b4422dcb03ad2b9b6cd8da9f20c1167dede9ab88b5be3",
    "a3c9ae7e8cebe9024d1e53447bf8883ed57a9013fda97ef08a7d9f7fc04a145d",
    "ac507cb4e5884dd0386eb7ea5e07303bd7023777ee12d4f816e7eb999fa76da6",
    "b105b7607eb32c92d461b3b6cf6ee6de4b5cd517b46e7284105f62ae100e3b96",
    "b129409c8c06202f8c89e1fc57bd4f1c861b74dfeee4acf07ec3cb4f784382a4",
    "b5079c079c1bdc4975c3fe08686a0ee563b9cb61ddbf182048c896f2b194e27f",
    "c26864d3b10cf5b54164983e70df5f5a95ef2da4cf48d0ca53777990214e1904",
    "c390ca9fc909fb4da868b27d0982b69c42ab8dd34137b34d88d28517eebfb61a",
    "cdf9bcd4257edc209b174a1925f0ee6557ca1500fab1f2fbf2795304d7441036",
    "cff2cd752dcbc7e7e53e8624938dc261c915b965180554cc439f446fc4c4d85f",
    "d24b8b31644fc8651f6ec59544f82ec37a037826fa296ba5593e1002b5a033d8",
    "de200d5611337b47a52770fa0ad910ce8a9908fc10abd5325dfab894b8b5d4a1",
    "e413a5d1f8ecffb4f709964a31952aa5e377bb4a331dcc399dd1f28468401c87",
    "eb460c777cb877ca1457d66ffc93f8dfbfcbb68d8518b333cd61e5640230b04d",
    "ecabf2e766d3cc1e5bdacc6e1d9a77ac238b09bb64780fd22bad1ef78b92a18b",
    "efc44ab2161d8e8d069c304b2b8453057362c6ef0ce2af2c14f63d1a680d9957",
    "f4ceedff87fc2ed0562bfe4ea50c28fc2d31957a460c79145ad5d81f07935b9a",
    "f8caccd2b650765f328d5000d3254c35c0cb362abf99a0a0ebe21c9b3dfd0b86",
    "fff7edec7361f0692f8a29d7c43ca94eaf299aa6c51327433207f21c6bf24c6e",
}};
inline constexpr std::array<std::string_view, 20> kRetainedOpaquePhaseVertices{{
    "14c2c0fe0615d91dda70a471287a8487519c4661f6d96ed177c781b76af2523b",
    "20d0f93a552fb740d8f3e650b3ceed46b00284cd7c33017a5340c51506c79a49",
    "30adfae224a62d6a9e9dc8b582d49f214d9a4fae2d232dd65f027a15ea893a79",
    "47083b979798fe184e0e181831d7215710d7d75b51ab57a5133589a9f8a0f1dc",
    "48db6c2ce179cb93fb70a99146a8e7dd05f8db2772c014b1481288b98b071739",
    "66a4ed3ea019c6f09e5bacb170bbbe41221aec3a4cdf487957a3bea9428deba4",
    "66f9e0b1c0956d170b63bf4a8dbc604e702193919cd7775f39482c8c0cb8fa6c",
    "6e8c854f26c37f33cbb0cc2cdb4cb73d17c52520ed93f79b4f3393b39fcb8698",
    "72d8eda2e9b6367d3c236846fcfe3d9363cfd068917ac3cf620cbee9ed3488fd",
    "75d5a00f6d18df60d999164bb9374eabfde56d1b48fc3f7efea5f57ed634f802",
    "76d6b36cd92e9f85cbb76d2a29cd5f99b51874399875745d9c0a4886d184212e",
    "813abea09ff425455f7d4f6bdec434434b8a0242a0386eed776a8e67fbb75092",
    "9095984252fdb53f46afe53384dd6bf460be2c4d95c9754953ee61af4266d12d",
    "960cd960c6d68a130660bf917223e918b6ef420a391048352f1f21da566a4ef6",
    "a6569d35b4066d36fb4aba208cf869ee00c3eddf82c896c7b280c89e8899c56d",
    "c268a28ab5b5531f4176c6bb8b60e65d998b0548ca07fb8f7ab27eb9ec73d2ac",
    "c4f02e79785bbc0b0cad1e661a86bf38755f929ebbc1b6b2ec20479ce26947af",
    "cb80c53dd19538df706f90cdd2d962395c81306efbf5b89e6ba4cd0cfff2bea5",
    "e4ad08e969cfa2097429156209284fcdcd5d820f13764a8d93010b5512182114",
    "f7dba9374d4cba41785f4e2810d881f559a2f3b0cbc40fc8a1f58ee49f4eb969",
}};
inline constexpr std::array<NativeRetainedOpaquePhaseRecipeEntry, kRetainedOpaquePhaseMaxPipelines>
kRetainedOpaquePhaseRecipes{{
    {"75d5a00f6d18df60d999164bb9374eabfde56d1b48fc3f7efea5f57ed634f802",
     "55c79ab30e62247886a8c74dc440f04695ffd0f6f88842ffa1aea0b6829ee6c6", {3, 2, 1, 4, 0, 0}, 3, 1},
    {"75d5a00f6d18df60d999164bb9374eabfde56d1b48fc3f7efea5f57ed634f802",
     "07297a57c1d500b10d63b3eaf81a57e58ab0f0f2f9cf596f52ddad1c51ac83d9", {1, 2, 1, 4, 0, 0}, 3, 1},
    {"75d5a00f6d18df60d999164bb9374eabfde56d1b48fc3f7efea5f57ed634f802",
     "333ce299839d3d857f8829e4249516ce823ea95e86fcf82473b81c068bd3c79f", {3, 2, 1, 4, 0, 0}, 3, 1},
    {"f7dba9374d4cba41785f4e2810d881f559a2f3b0cbc40fc8a1f58ee49f4eb969",
     "779faa05c69ed3950abea4e20368f3fa6ca965da9755f2eba67a59ef37b0abf5", {3, 2, 1, 4, 0, 0}, 3, 1},
    {"f7dba9374d4cba41785f4e2810d881f559a2f3b0cbc40fc8a1f58ee49f4eb969",
     "28b63a43c49413829c4059148707f9b8456c28926cd22d3ceec9f7e9f18cd286", {1, 2, 1, 4, 0, 0}, 3, 1},
    {"f7dba9374d4cba41785f4e2810d881f559a2f3b0cbc40fc8a1f58ee49f4eb969",
     "b129409c8c06202f8c89e1fc57bd4f1c861b74dfeee4acf07ec3cb4f784382a4", {3, 2, 1, 4, 0, 0}, 3, 1},
    {"f7dba9374d4cba41785f4e2810d881f559a2f3b0cbc40fc8a1f58ee49f4eb969",
     "472d1f7c9fad138c0dc86011ee5ac9e158c65fda70c0a2d55fbfe1328e66e809", {3, 2, 1, 4, 0, 0}, 3, 1},
    {"813abea09ff425455f7d4f6bdec434434b8a0242a0386eed776a8e67fbb75092",
     "ac507cb4e5884dd0386eb7ea5e07303bd7023777ee12d4f816e7eb999fa76da6", {3, 2, 1, 4, 0, 0}, 3, 1},
    {"48db6c2ce179cb93fb70a99146a8e7dd05f8db2772c014b1481288b98b071739",
     "88b188111071ce7552c98455d5fcfd3e85047d3fa968b6f8a74f57f0f78aeac6", {3, 1, 1, 4, 0, 0}, 3, 1},
    {"30adfae224a62d6a9e9dc8b582d49f214d9a4fae2d232dd65f027a15ea893a79",
     "ecabf2e766d3cc1e5bdacc6e1d9a77ac238b09bb64780fd22bad1ef78b92a18b", {3, 2, 1, 4, 0, 0}, 3, 1},
    {"75d5a00f6d18df60d999164bb9374eabfde56d1b48fc3f7efea5f57ed634f802",
     "95d5ece6727854fb21786dde9a002f9cd0f8eb09f8fad4b98b07668e025e8589", {3, 2, 1, 4, 0, 0}, 3, 1},
    {"75d5a00f6d18df60d999164bb9374eabfde56d1b48fc3f7efea5f57ed634f802",
     "8de5f9c029b86d1b3ec7e61bb67bde0d38cee5c37a269c597a673f6fe3c8bce9", {3, 2, 1, 4, 0, 0}, 3, 1},
    {"75d5a00f6d18df60d999164bb9374eabfde56d1b48fc3f7efea5f57ed634f802",
     "f4ceedff87fc2ed0562bfe4ea50c28fc2d31957a460c79145ad5d81f07935b9a", {3, 2, 1, 4, 0, 0}, 3, 1},
    {"75d5a00f6d18df60d999164bb9374eabfde56d1b48fc3f7efea5f57ed634f802",
     "a3c9ae7e8cebe9024d1e53447bf8883ed57a9013fda97ef08a7d9f7fc04a145d", {3, 1, 1, 4, 0, 0}, 3, 1},
    {"75d5a00f6d18df60d999164bb9374eabfde56d1b48fc3f7efea5f57ed634f802",
     "a3c9ae7e8cebe9024d1e53447bf8883ed57a9013fda97ef08a7d9f7fc04a145d", {3, 2, 1, 4, 0, 0}, 3, 1},
    {"30adfae224a62d6a9e9dc8b582d49f214d9a4fae2d232dd65f027a15ea893a79",
     "1010f9623d6f15d20235c86a20fe9de3eb6625ae384c4fb0c884af1d4fbfc468", {3, 2, 1, 4, 0, 0}, 3, 1},
    {"72d8eda2e9b6367d3c236846fcfe3d9363cfd068917ac3cf620cbee9ed3488fd",
     "7e52d8ce2caf241b62f3816841fbae38de30394ded4a72b023ecffea46f99fd2", {3, 2, 1, 4, 0, 0}, 3, 1},
    {"75d5a00f6d18df60d999164bb9374eabfde56d1b48fc3f7efea5f57ed634f802",
     "cff2cd752dcbc7e7e53e8624938dc261c915b965180554cc439f446fc4c4d85f", {3, 2, 1, 4, 0, 0}, 3, 1},
    {"75d5a00f6d18df60d999164bb9374eabfde56d1b48fc3f7efea5f57ed634f802",
     "3f4a745859e70c0fba277793adb4bfad82ee11756fb5d97ee5df6bbd6633880c", {3, 2, 1, 4, 0, 0}, 3, 1},
    {"75d5a00f6d18df60d999164bb9374eabfde56d1b48fc3f7efea5f57ed634f802",
     "07ab01db76715d2b37b64273821577c3b22c3f1ff752981b5d5559fc9c443cb1", {3, 2, 1, 4, 0, 0}, 3, 1},
    {"75d5a00f6d18df60d999164bb9374eabfde56d1b48fc3f7efea5f57ed634f802",
     "eb460c777cb877ca1457d66ffc93f8dfbfcbb68d8518b333cd61e5640230b04d", {3, 2, 1, 4, 0, 0}, 3, 1},
    {"cb80c53dd19538df706f90cdd2d962395c81306efbf5b89e6ba4cd0cfff2bea5",
     "c26864d3b10cf5b54164983e70df5f5a95ef2da4cf48d0ca53777990214e1904", {3, 1, 1, 4, 0, 0}, 3, 1},
    {"75d5a00f6d18df60d999164bb9374eabfde56d1b48fc3f7efea5f57ed634f802",
     "4656f12e23c08ae036872f12f325d83d279a919824ba1fae70c56dcddac05129", {3, 2, 1, 4, 0, 0}, 3, 1},
    {"75d5a00f6d18df60d999164bb9374eabfde56d1b48fc3f7efea5f57ed634f802",
     "fff7edec7361f0692f8a29d7c43ca94eaf299aa6c51327433207f21c6bf24c6e", {1, 2, 1, 4, 0, 0}, 3, 1},
    {"75d5a00f6d18df60d999164bb9374eabfde56d1b48fc3f7efea5f57ed634f802",
     "799fd4625a107d3223fdcd799185c81194380f321de41eacc3c413111deec29c", {3, 2, 1, 4, 0, 0}, 3, 1},
    {"75d5a00f6d18df60d999164bb9374eabfde56d1b48fc3f7efea5f57ed634f802",
     "cdf9bcd4257edc209b174a1925f0ee6557ca1500fab1f2fbf2795304d7441036", {3, 2, 1, 4, 0, 0}, 3, 1},
    {"75d5a00f6d18df60d999164bb9374eabfde56d1b48fc3f7efea5f57ed634f802",
     "b105b7607eb32c92d461b3b6cf6ee6de4b5cd517b46e7284105f62ae100e3b96", {3, 2, 1, 4, 0, 0}, 3, 1},
    {"76d6b36cd92e9f85cbb76d2a29cd5f99b51874399875745d9c0a4886d184212e",
     "16139b306f267b2efc5cf8524703f0853eca82e5711f79547e3e095d983de03f", {3, 2, 1, 4, 0, 0}, 3, 1},
    {"75d5a00f6d18df60d999164bb9374eabfde56d1b48fc3f7efea5f57ed634f802",
     "055dbdf3694342aed020cd27c9426fb71c57d1e60fe5267ce9c3cb538fb8a7d0", {3, 2, 1, 4, 0, 0}, 3, 1},
    {"75d5a00f6d18df60d999164bb9374eabfde56d1b48fc3f7efea5f57ed634f802",
     "055dbdf3694342aed020cd27c9426fb71c57d1e60fe5267ce9c3cb538fb8a7d0", {3, 1, 1, 4, 0, 0}, 3, 1},
    {"20d0f93a552fb740d8f3e650b3ceed46b00284cd7c33017a5340c51506c79a49",
     "e413a5d1f8ecffb4f709964a31952aa5e377bb4a331dcc399dd1f28468401c87", {3, 1, 1, 4, 0, 0}, 3, 1},
    {"20d0f93a552fb740d8f3e650b3ceed46b00284cd7c33017a5340c51506c79a49",
     "e413a5d1f8ecffb4f709964a31952aa5e377bb4a331dcc399dd1f28468401c87", {3, 2, 1, 4, 0, 0}, 3, 1},
    {"9095984252fdb53f46afe53384dd6bf460be2c4d95c9754953ee61af4266d12d",
     "fff7edec7361f0692f8a29d7c43ca94eaf299aa6c51327433207f21c6bf24c6e", {1, 2, 1, 4, 0, 0}, 3, 1},
    {"f7dba9374d4cba41785f4e2810d881f559a2f3b0cbc40fc8a1f58ee49f4eb969",
     "9e4f58f4be540b9ca2a30d94463febf6504aa8a8dd88255097b1acb5b82122d1", {3, 2, 1, 4, 0, 0}, 3, 1},
    {"f7dba9374d4cba41785f4e2810d881f559a2f3b0cbc40fc8a1f58ee49f4eb969",
     "49f26283e1436f4cce68ac4e75695c6375176768b1dbfbf6c1f79d9bef434ced", {3, 2, 1, 4, 0, 0}, 3, 1},
    {"f7dba9374d4cba41785f4e2810d881f559a2f3b0cbc40fc8a1f58ee49f4eb969",
     "49f26283e1436f4cce68ac4e75695c6375176768b1dbfbf6c1f79d9bef434ced", {3, 1, 1, 4, 0, 0}, 3, 1},
    {"66f9e0b1c0956d170b63bf4a8dbc604e702193919cd7775f39482c8c0cb8fa6c",
     "2187635535de20051571e43cf5d22aa78ea18167cba792dce95058de36868fa1", {3, 2, 1, 4, 0, 0}, 3, 1},
    {"66f9e0b1c0956d170b63bf4a8dbc604e702193919cd7775f39482c8c0cb8fa6c",
     "72478105be8ca45857d9fcf5e8d812a7276755d8dbf60f3c46175cda2206f4e6", {3, 2, 1, 4, 0, 0}, 3, 1},
    {"a6569d35b4066d36fb4aba208cf869ee00c3eddf82c896c7b280c89e8899c56d",
     "6e32849817c598e7294767382d90c345de2523013cbc86748489c25647886152", {3, 2, 1, 4, 0, 0}, 3, 1},
    {"f7dba9374d4cba41785f4e2810d881f559a2f3b0cbc40fc8a1f58ee49f4eb969",
     "60f06efdd83a7c0f263fa6e735b37e103c99dac006d833f7fde985142019e9b0", {3, 2, 1, 4, 0, 0}, 3, 1},
    {"f7dba9374d4cba41785f4e2810d881f559a2f3b0cbc40fc8a1f58ee49f4eb969",
     "60f06efdd83a7c0f263fa6e735b37e103c99dac006d833f7fde985142019e9b0", {3, 1, 1, 4, 0, 0}, 3, 1},
    {"f7dba9374d4cba41785f4e2810d881f559a2f3b0cbc40fc8a1f58ee49f4eb969",
     "b5079c079c1bdc4975c3fe08686a0ee563b9cb61ddbf182048c896f2b194e27f", {3, 1, 1, 4, 0, 0}, 3, 1},
    {"f7dba9374d4cba41785f4e2810d881f559a2f3b0cbc40fc8a1f58ee49f4eb969",
     "c390ca9fc909fb4da868b27d0982b69c42ab8dd34137b34d88d28517eebfb61a", {3, 2, 1, 4, 0, 0}, 3, 1},
    {"f7dba9374d4cba41785f4e2810d881f559a2f3b0cbc40fc8a1f58ee49f4eb969",
     "196cc79577c42aae9069a265d89cd0a672e7fcc9e54583af8563eed8bb0c1fa0", {1, 2, 1, 4, 0, 0}, 3, 1},
    {"6e8c854f26c37f33cbb0cc2cdb4cb73d17c52520ed93f79b4f3393b39fcb8698",
     "27605aa5c6eac27fe3978faa61f0ee40fcbf13433d232d6b16087807322e6a05", {3, 1, 1, 4, 0, 0}, 3, 1},
    {"6e8c854f26c37f33cbb0cc2cdb4cb73d17c52520ed93f79b4f3393b39fcb8698",
     "27605aa5c6eac27fe3978faa61f0ee40fcbf13433d232d6b16087807322e6a05", {3, 2, 1, 4, 0, 0}, 3, 1},
    {"f7dba9374d4cba41785f4e2810d881f559a2f3b0cbc40fc8a1f58ee49f4eb969",
     "2d46b4519eb56dffe719e38d827856a8d05303c8d77139a459d85d5f4751fe6f", {3, 2, 1, 4, 0, 0}, 3, 1},
    {"c268a28ab5b5531f4176c6bb8b60e65d998b0548ca07fb8f7ab27eb9ec73d2ac",
     "67345542a0b3a75a3e5bcc75c818e099bc7a5a3387884d0bd13114dfa39167d7", {3, 2, 1, 4, 0, 0}, 3, 1},
    {"f7dba9374d4cba41785f4e2810d881f559a2f3b0cbc40fc8a1f58ee49f4eb969",
     "a1b82a360d85cee56b9b4422dcb03ad2b9b6cd8da9f20c1167dede9ab88b5be3", {3, 2, 1, 4, 0, 0}, 3, 1},
    {"f7dba9374d4cba41785f4e2810d881f559a2f3b0cbc40fc8a1f58ee49f4eb969",
     "de200d5611337b47a52770fa0ad910ce8a9908fc10abd5325dfab894b8b5d4a1", {3, 2, 1, 4, 0, 0}, 3, 1},
    {"f7dba9374d4cba41785f4e2810d881f559a2f3b0cbc40fc8a1f58ee49f4eb969",
     "de200d5611337b47a52770fa0ad910ce8a9908fc10abd5325dfab894b8b5d4a1", {3, 1, 1, 4, 0, 0}, 3, 1},
    {"47083b979798fe184e0e181831d7215710d7d75b51ab57a5133589a9f8a0f1dc",
     "2f171c4bbda2315d2363bc63dec3b8b5256dce0c6fc223705d46b57c96fa42dc", {3, 2, 1, 4, 0, 0}, 3, 1},
    {"47083b979798fe184e0e181831d7215710d7d75b51ab57a5133589a9f8a0f1dc",
     "2f171c4bbda2315d2363bc63dec3b8b5256dce0c6fc223705d46b57c96fa42dc", {3, 1, 1, 4, 0, 0}, 3, 1},
    {"47083b979798fe184e0e181831d7215710d7d75b51ab57a5133589a9f8a0f1dc",
     "3d564f1e649a2e5ebae927cc7dc715b496f40965b83351809f2f3adb80d009b8", {3, 2, 1, 4, 0, 0}, 3, 1},
    {"47083b979798fe184e0e181831d7215710d7d75b51ab57a5133589a9f8a0f1dc",
     "3d564f1e649a2e5ebae927cc7dc715b496f40965b83351809f2f3adb80d009b8", {3, 1, 1, 4, 0, 0}, 3, 1},
    {"960cd960c6d68a130660bf917223e918b6ef420a391048352f1f21da566a4ef6",
     "f8caccd2b650765f328d5000d3254c35c0cb362abf99a0a0ebe21c9b3dfd0b86", {3, 2, 1, 4, 0, 0}, 3, 1},
    {"66a4ed3ea019c6f09e5bacb170bbbe41221aec3a4cdf487957a3bea9428deba4",
     "300a99f2e7c4cfc50be61d85a8365526e4ff83f1bacf723cff4f4dc44d706fe0", {3, 2, 1, 4, 0, 0}, 3, 1},
    {"e4ad08e969cfa2097429156209284fcdcd5d820f13764a8d93010b5512182114",
     "559b4677ef84fd5cf5646553ba1cb54f3ab71d3cfdeb30cf057a750c9a4346df", {3, 2, 1, 4, 0, 0}, 3, 1},
    {"14c2c0fe0615d91dda70a471287a8487519c4661f6d96ed177c781b76af2523b",
     "d24b8b31644fc8651f6ec59544f82ec37a037826fa296ba5593e1002b5a033d8", {3, 1, 1, 4, 0, 0}, 3, 1},
    {"14c2c0fe0615d91dda70a471287a8487519c4661f6d96ed177c781b76af2523b",
     "d24b8b31644fc8651f6ec59544f82ec37a037826fa296ba5593e1002b5a033d8", {3, 2, 1, 4, 0, 0}, 3, 1},
    {"c4f02e79785bbc0b0cad1e661a86bf38755f929ebbc1b6b2ec20479ce26947af",
     "2852b1b3aa1faae6e25f03ed8af0697e96c7079879d7d51a328f47e8d0495eb8", {3, 2, 1, 4, 0, 0}, 3, 1},
    {"c4f02e79785bbc0b0cad1e661a86bf38755f929ebbc1b6b2ec20479ce26947af",
     "0de61d56c71cf0ee32294c3f2f4c69dbbd154c35c7147af905b75dea16377af1", {3, 2, 1, 4, 0, 0}, 3, 1},
    {"960cd960c6d68a130660bf917223e918b6ef420a391048352f1f21da566a4ef6",
     "300a99f2e7c4cfc50be61d85a8365526e4ff83f1bacf723cff4f4dc44d706fe0", {3, 2, 1, 4, 0, 0}, 3, 1},
    {"e4ad08e969cfa2097429156209284fcdcd5d820f13764a8d93010b5512182114",
     "efc44ab2161d8e8d069c304b2b8453057362c6ef0ce2af2c14f63d1a680d9957", {3, 2, 1, 4, 0, 0}, 3, 1},
    {"e4ad08e969cfa2097429156209284fcdcd5d820f13764a8d93010b5512182114",
     "1e3be76bf5f9ca19116d005818c83497d25b7791142af56babff056cd95acc43", {1, 2, 1, 4, 0, 0}, 3, 1},
}};

inline constexpr std::optional<size_t> NativeRetainedOpaquePhasePixelIndex(
        std::string_view source) noexcept {
    for (size_t i = 0; i < kRetainedOpaquePhasePixels.size(); ++i)
        if (kRetainedOpaquePhasePixels[i] == source) return i;
    return std::nullopt;
}
inline constexpr bool NativeRetainedOpaquePhasePixelSupported(std::string_view source) noexcept {
    return NativeRetainedOpaquePhasePixelIndex(source).has_value();
}
// The byte certificate remains valid, but the actual310/1556 producer has a
// proven packed-color mismatch. Keep its ordinary Native path until repaired.
inline constexpr std::string_view kRetainedOpaquePhaseExcludedPixel =
    "333ce299839d3d857f8829e4249516ce823ea95e86fcf82473b81c068bd3c79f";
inline constexpr bool NativeRetainedOpaquePhasePixelEligible(std::string_view source) noexcept {
    return NativeRetainedOpaquePhasePixelSupported(source) && source != kRetainedOpaquePhaseExcludedPixel;
}
inline constexpr bool NativeRetainedOpaquePhaseVertexSupported(std::string_view source) noexcept {
    for (const auto vertex : kRetainedOpaquePhaseVertices)
        if (vertex == source) return true;
    return false;
}
inline constexpr const NativeRetainedOpaquePhaseRecipeEntry* NativeRetainedOpaquePhaseCatalogRecipe(
        std::string_view vertex, std::string_view pixel,
        const std::array<uint32_t, 6>& raster) noexcept {
    for (const auto& recipe : kRetainedOpaquePhaseRecipes)
        if (recipe.vertex == vertex && recipe.pixel == pixel && recipe.raster == raster)
            return &recipe;
    return nullptr;
}
// Keep the complete captured inventory available for symmetric ordinary
// startup preparation. Execution and optional phase preparation use this gate.
inline constexpr const NativeRetainedOpaquePhaseRecipeEntry* NativeRetainedOpaquePhaseRecipe(
        std::string_view vertex, std::string_view pixel,
        const std::array<uint32_t, 6>& raster) noexcept {
    return NativeRetainedOpaquePhasePixelEligible(pixel) ?
        NativeRetainedOpaquePhaseCatalogRecipe(vertex, pixel, raster) : nullptr;
}
// Source membership and this recipe predicate are metadata only. Admission
// must separately prove actual module/PSO owners, the guest formats, complete
// draw state, resource authority and residency. No recipe is inferred by
// combining independently accepted source or raster lists.
} // namespace gow3::renderer

