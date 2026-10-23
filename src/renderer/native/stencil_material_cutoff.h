#pragma once
#include "fsi_state.h"
#include <optional>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace gow3::renderer {
enum class NativeStencilMaterialMode : uint32_t {Disabled=0,Observe=1,Cutoff=2,Uninstrumented=3};
inline constexpr std::string_view kStencilMaterialVertex="3216ee59572855fc49298b760ec9eb95b7313dbb25341da49eb6f0d03b501e0c";
inline constexpr std::string_view kStencilMaterialPixel="5dcdf6e1b00fef55fcb866498e58586cd0eb6cb796311a54771435e7e8aac1f6";
inline constexpr std::string_view kStencilMaterialVertexSpirv="167b62800a57aee5be4f7df3dc2c7316c2eff93236c872af87669789af14d798";
inline constexpr std::string_view kStencilMaterialPixelSpirv="4da173957190fb1b016157d70dfc4f364842c749597798409ab6870e179761e9";
inline constexpr std::string_view kStencilMaterialCanonicalVersion="1c697af73d13496ae2424cfd4e65de2856f16bd612bd715b92160403c8bd762b";
inline constexpr std::string_view kStencilMaterialCommonSha="89de28c53ee17200c968c879a4cd9fde4b00727a3e561b007e0807ed86377b63";
inline constexpr std::string_view kStencilMaterialIndexSha="30a55215150d5fdcfb0da8862a78e05ebb69d882b491870015c3bc8d87c035a2";
inline constexpr uint32_t kStencilMaterialCounterLimit=12u*1280*720;
inline constexpr uint32_t kStencilMaterialMaximumSamples=4096;
struct NativeStencilMaterialCounts {
    uint32_t entered=0,rejected=0,survived=0,overflow=0;
    bool Valid()const noexcept {
        return !overflow&&entered<=kStencilMaterialCounterLimit&&
            uint64_t(rejected)+survived==entered;
    }
};
static_assert(sizeof(NativeStencilMaterialCounts)==16);
// The caller may Retire only from the existing submission completion callback.
// First-use and every reuse have the same clear/record/accept/fence discipline.
class NativeStencilMaterialCounterOwnership {
public:
    enum class Phase {Free,Preparing,Begun,Recorded,Accepted};
    Phase phase()const noexcept{return phase_;}
    bool pending()const noexcept{return phase_==Phase::Accepted;}
    void Prepare(uint64_t token) {
        Require(phase_==Phase::Free&&token&&token>token_,"Stencil counter premature/stale reuse");
        token_=token;phase_=Phase::Preparing;
    }
    void Begin(){Require(phase_==Phase::Preparing,"Stencil counter unmatched clear");phase_=Phase::Begun;}
    void End(){Require(phase_==Phase::Begun,"Stencil counter unmatched draw end");phase_=Phase::Recorded;}
    void Accept(uint64_t token){Require(phase_==Phase::Recorded&&token==token_,"Stencil counter wrong acceptance");phase_=Phase::Accepted;}
    void ValidateRetirement(uint64_t token,uint64_t submission) const {
        Require(phase_==Phase::Accepted&&token==token_&&submission&&submission>last_submission_,"Stencil counter unfenced/stale retirement");
    }
    void Retire(uint64_t token,uint64_t submission) {
        ValidateRetirement(token,submission);last_submission_=submission;phase_=Phase::Free;
    }
private:
    static void Require(bool ok,const char* text){if(!ok)throw std::logic_error(text);}
    Phase phase_=Phase::Free;uint64_t token_=0,last_submission_=0;
};
class NativeShaderCompiler;
struct NativeStencilMaterialProgram {
    NativeStencilMaterialMode mode=NativeStencilMaterialMode::Disabled;
    std::string source,canonical_version,derived_version;
    uint32_t specialization_mask=0;
    std::vector<uint32_t> spirv;
    // Full-byte provenance issued only after exact source restoration and DXC
    // validation. Copies preserve it; changing any public identity/code refuses.
    bool Authenticated() const;
private:
    std::string authentication_;
    void Authenticate();
    friend std::shared_ptr<const NativeStencilMaterialProgram> CompileNativeStencilMaterial(
        NativeShaderCompiler&,std::span<const uint8_t>,NativeStencilMaterialMode);
};
struct NativeStencilMaterialProof {
    bool original_modules=false,unmodified_variants=false,owned_up_cube=false;
    bool active_query=false,discarded=false,rectangle=false,primary_surfaces=false;
    bool private_framebuffer=false,bounded_scissor=false,packed_current=false,single_sample_single_view=false;
    uint32_t width=0,height=0,raw_color_mask=0,index_count=0;
    std::array<uint32_t,6> raster{};
};
inline bool NativeStencilMaterialAllowed(const NativeFsiConstants& r,const NativeStencilMaterialProof& p) {
    if(!p.original_modules||!p.unmodified_variants||!p.owned_up_cube||p.active_query||p.discarded||p.rectangle||
       !p.primary_surfaces||!p.private_framebuffer||!p.bounded_scissor||!p.packed_current||!p.single_sample_single_view||
       p.width!=1280||p.height!=720||p.raw_color_mask!=1||p.index_count!=36||
       p.raster!=std::array<uint32_t,6>{3,2,1,4,0,0}||
       r.pitch!=1280||r.depth_base!=0||r.depth_format!=1||r.depth_control!=0x00700571||
       r.stencil_front!=0x00ffff00||r.stencil_back!=0x00ffff00||
       r.color[0].base!=720||r.color[0].format!=0||r.color[0].mask!=1||
       (r.blend_enabled!=0&&r.blend_enabled!=1)||
       r.color[0].blend!=(r.blend_enabled?0x01000008u:0x00010001u))return false;
    for(unsigned i=1;i<4;++i)if(r.color[i].mask)return false;
    return true;
}
// Exact all-uint32 identity: no floating point decode/re-encode occurs on this
// depth-disabled, all-stencil-KEEP state, even for noncanonical high24 values.
inline constexpr uint32_t NativeStencilMaterialIdentityStore(uint32_t previous) {
    const uint32_t stencil=previous&255u,packed_depth=previous>>8;
    const uint32_t result=stencil; // Both front/back select KEEP for all outcomes.
    const uint32_t merged=(stencil&~255u)|(result&255u);
    return (packed_depth<<8)|merged;
}
inline constexpr bool NativeStencilMaterialPass(uint32_t previous) {return (previous&255u)!=0;}
bool NativeStencilMaterialCanonicalCertified(std::span<const uint32_t>,bool vertex);
// Candidate audit is structural in addition to authenticated whole-source
// restoration. There are no calls, hidden derivative/quad/subgroup operations,
// implicit-LOD samples or depth outputs. Both original derivatives must execute
// in the entry block before any branch, kill, material sample or counter atomic.
bool NativeStencilMaterialDerivedCertified(std::span<const uint32_t>);
bool NativeStencilMaterialDerivedCertified(std::span<const uint32_t>,NativeStencilMaterialMode);
std::optional<std::string> TryNativeStencilMaterialSource(std::string_view,NativeStencilMaterialMode);
std::optional<std::string> RestoreNativeStencilMaterialSource(std::string_view,NativeStencilMaterialMode);
inline constexpr std::string_view kStencilMaterialEntry=R"CUT(float2 gow3FsiDepthSlope = float2(ddx_coarse(iPos.z), ddy_coarse(iPos.z));)CUT";
inline constexpr std::string_view kStencilMaterialStore="        storeWord(at,(packed_depth<<8)|stencil);";
inline constexpr std::string_view kStencilMaterialStoreRemoved="        // StencilMaterial: exact depth-OFF/all-KEEP identity store omitted.";
inline constexpr std::string_view kStencilMaterialUninstrumentedHelper=R"CUT(
bool gow3StencilMaterialPrefix(uint2 p) {
    if(p.x>=1280u||p.y>=720u)return false;
    uint at=((p.y/16u)*16u+p.x/80u)*1280u+(p.y%16u)*80u+(p.x%80u+40u)%80u;
    return (loadWord(at)&255u)!=0u;
}
)CUT";
inline constexpr std::string_view kStencilMaterialHelper=R"CUT(
[[vk::binding(1,4)]] RWByteAddressBuffer gow3StencilMaterialCounters;
bool gow3StencilMaterialPrefix(uint2 p) {
    // Out-of-frame helpers must not form an unchecked packed address. Their
    // counter atomics have no memory effect under Vulkan helper semantics.
    bool pass=false;
    if(p.x<1280u&&p.y<720u) {
        uint at=((p.y/16u)*16u+p.x/80u)*1280u+(p.y%16u)*80u+(p.x%80u+40u)%80u;
        pass=(loadWord(at)&255u)!=0u;
    }
    uint old;
    gow3StencilMaterialCounters.InterlockedAdd(0u,1u,old);
    if(old>=11059200u)gow3StencilMaterialCounters.InterlockedOr(12u,1u);
    gow3StencilMaterialCounters.InterlockedAdd(pass?8u:4u,1u,old);
    if(old>=11059200u)gow3StencilMaterialCounters.InterlockedOr(12u,1u);
    return pass;
}
)CUT";
} // namespace gow3::renderer
