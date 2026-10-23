#include "renderer/native/early_depth_material_retention.h"
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace gow3::renderer;
namespace {
uint64_t checks = 0;
int vertex_owner, pixel_owner, other_owner;
void Check(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
NativeEarlyDepthMaterialProof Proof(bool specialized = false) {
    NativeEarlyDepthMaterialProof p;
    p.enabled = p.native_fsi = p.private_framebuffer = true;
    p.indexed_triangles = p.bounded_scissor = p.single_sample = true;
    p.certified_pipeline = true;
    p.draw_identity = 91;
    p.vertex_module = p.certified_vertex_module = &vertex_owner;
    p.pixel_module = p.certified_pixel_module = &pixel_owner;
    p.depth_layout = kEarlyDepthMaterialDepthLayout;
    p.color_layout = kEarlyDepthMaterialColorLayout;
    auto& r = p.raster;
    r.pitch = 1280; r.depth_format = 1; r.depth_control = 0x00700766;
    r.color[0].base = 720; r.color[0].format = 3; r.color[0].mask = 15;
    r.front_constant_bias = r.back_constant_bias = -0.0f;
    if (specialized) {
        p.pixel_variant = NativeShaderVariant::PipelineStateEpilogueV1;
        p.selected_specialization = NativeFsiStateSpecialization::From(r);
    }
    return p;
}
bool Admitted(const NativeEarlyDepthMaterialProof& p) {
    return ClassifyNativeEarlyDepthMaterialRetention(p) == NativeEarlyDepthMaterialAdmission::Admitted;
}
void Refused(const NativeEarlyDepthMaterialProof& p) {
    Check(!Admitted(p), "Malformed proof admitted");
    NativeEarlyDepthLowerBoundVersion v(7, kEarlyDepthMaterialDepthLayout, 11);
    Check(!CaptureNativeEarlyDepthMaterialRetention(v, p, 11), "Malformed proof captured");
    Check(!v.Reusable(kEarlyDepthMaterialDepthLayout, 11), "Capture refusal did not invalidate");
}
template<class Mutate> void BadCommit(Mutate mutate) {
    auto p = Proof(true);
    NativeEarlyDepthLowerBoundVersion v(7, kEarlyDepthMaterialDepthLayout, 11);
    auto ticket = CaptureNativeEarlyDepthMaterialRetention(v, p, 11);
    Check(bool(ticket), "Valid proof capture failed");
    uint64_t pre = 11, post = 20, serial_pre = 19, serial_post = 20;
    bool accepted = true;
    mutate(v, *ticket, p, pre, post, serial_pre, serial_post, accepted);
    Check(!CommitNativeEarlyDepthMaterialRetention(v, *ticket, p,
        pre, post, serial_pre, serial_post, accepted), "Bad commit accepted");
    Check(v.current() == 11 && v.anchor() == 11, "Failed commit advanced token");
    Check(!v.Reusable(kEarlyDepthMaterialDepthLayout, 11), "Commit refusal did not invalidate");
}
// Independent expression of the original shader's unsigned FP24 value ordering.
// This tests the preservation premise, not hardware raster/sample equivalence.
double Fp24(uint32_t x) {
    const auto e = x >> 20;
    const auto m = x & 0xfffffu;
    return std::ldexp(double(e ? m + 0x100000u : m), e ? int(e)-35 : -34);
}
}

int main() {
    Check(!kNativeEarlyDepthMaterialRetentionDefaultEnabled, "Unexpected default ON");
    Refused({});
    for (bool state : {false, true}) {
        auto p = Proof(state);
        Check(Admitted(p), "Valid exact proof rejected");
        NativeEarlyDepthLowerBoundVersion v(7, kEarlyDepthMaterialDepthLayout, 11);
        auto ticket = CaptureNativeEarlyDepthMaterialRetention(v, p, 11);
        Check(bool(ticket), "Valid capture failed");
        // Disjoint color writes may have advanced the global serial from11to19.
        Check(CommitNativeEarlyDepthMaterialRetention(v, *ticket, p, 11, 20, 19, 20, true), "Valid commit failed");
        Check(v.anchor() == 11 && v.current() == 20, "Immutable anchor changed");
        Check(v.Reusable(kEarlyDepthMaterialDepthLayout, 20), "Promoted bound not reusable");
        Check(!CommitNativeEarlyDepthMaterialRetention(v, *ticket, p, 20, 21, 20, 21, true), "Stale ticket accepted twice");
        Check(!v.Reusable(kEarlyDepthMaterialDepthLayout, 20), "Duplicate commit not fail closed");
    }
    for (auto member : {&NativeEarlyDepthMaterialProof::enabled,
        &NativeEarlyDepthMaterialProof::native_fsi, &NativeEarlyDepthMaterialProof::private_framebuffer,
        &NativeEarlyDepthMaterialProof::indexed_triangles, &NativeEarlyDepthMaterialProof::bounded_scissor,
        &NativeEarlyDepthMaterialProof::single_sample, &NativeEarlyDepthMaterialProof::certified_pipeline}) {
        auto p = Proof(); p.*member = false; Refused(p);
    }
    for (auto member : {&NativeEarlyDepthMaterialProof::active_query,
        &NativeEarlyDepthMaterialProof::discarded, &NativeEarlyDepthMaterialProof::rectangle,
        &NativeEarlyDepthMaterialProof::gpu_geometry}) {
        auto p = Proof(); p.*member = true; Refused(p);
    }
    { auto p = Proof(); p.draw_identity = 0; Refused(p); }
    for (auto member : {&NativeEarlyDepthMaterialProof::vertex_module,
        &NativeEarlyDepthMaterialProof::pixel_module, &NativeEarlyDepthMaterialProof::certified_vertex_module,
        &NativeEarlyDepthMaterialProof::certified_pixel_module}) {
        auto p = Proof(); p.*member = nullptr; Refused(p);
        p = Proof(); p.*member = &other_owner; Refused(p);
    }
    for (unsigned variant = 0; variant < 256; ++variant) {
        auto p = Proof(); p.vertex_variant = NativeShaderVariant(variant);
        if (variant) Refused(p); else Check(Admitted(p), "Canonical vertex refused");
        p = Proof(); p.pixel_variant = NativeShaderVariant(variant);
        if (variant) Refused(p); else Check(Admitted(p), "Canonical pixel refused");
        p = Proof(true); p.pixel_variant = NativeShaderVariant(variant);
        if (variant != unsigned(NativeShaderVariant::PipelineStateEpilogueV1)) Refused(p);
        else Check(Admitted(p), "State pixel refused");
    }
    for (unsigned field = 0; field < 6; ++field) for (unsigned bit = 0; bit < 32; ++bit) {
        auto p = Proof(); p.depth_layout[field] ^= uint32_t(1) << bit; Refused(p);
        p = Proof(); p.color_layout[field] ^= uint32_t(1) << bit; Refused(p);
    }
    for (auto member : {&NativeFsiConstants::pitch, &NativeFsiConstants::depth_base,
        &NativeFsiConstants::depth_format, &NativeFsiConstants::depth_control}) {
        for (unsigned bit = 0; bit < 32; ++bit) {
            auto p = Proof(); p.raster.*member ^= uint32_t(1) << bit; Refused(p);
        }
    }
    for (auto member : {&NativeFsiConstants::ColorTarget::base,
        &NativeFsiConstants::ColorTarget::format, &NativeFsiConstants::ColorTarget::mask}) {
        for (unsigned bit = 0; bit < 32; ++bit) {
            auto p = Proof(); p.raster.color[0].*member ^= uint32_t(1) << bit; Refused(p);
        }
    }
    for (unsigned target = 1; target < 4; ++target) for (unsigned bit = 0; bit < 32; ++bit) {
        auto p = Proof(); p.raster.color[target].mask = uint32_t(1) << bit; Refused(p);
    }
    for (auto member : {&NativeFsiConstants::front_slope_bias, &NativeFsiConstants::front_constant_bias,
        &NativeFsiConstants::back_slope_bias, &NativeFsiConstants::back_constant_bias}) {
        for (float value : {0.0f, -0.0f}) {auto p = Proof(); p.raster.*member = value; Check(Admitted(p), "Signed zero bias refused");}
        for (float value : {1.0f, -1.0f, std::numeric_limits<float>::denorm_min(),
            std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(),
            std::numeric_limits<float>::quiet_NaN()}) {auto p = Proof(); p.raster.*member = value; Refused(p);}
    }
    { auto p = Proof(true); p.selected_specialization.reset(); Refused(p); }
    { auto p = Proof(); p.selected_specialization = NativeFsiStateSpecialization::From(p.raster); Refused(p); }
    for (unsigned word = 0; word < 15; ++word) for (unsigned bit = 0; bit < 32; ++bit) {
        auto p = Proof(true); p.selected_specialization->words[word] ^= uint32_t(1) << bit; Refused(p);
    }
    for (uint32_t blend : {0u, 1u, 0xffffffffu}) {
        auto p = Proof(true); p.raster.blend_enabled = blend;
        p.selected_specialization = NativeFsiStateSpecialization::From(p.raster);
        Check(Admitted(p), "Disjoint color blending unnecessarily rejected");
    }
    BadCommit([](auto&, auto&, auto&, auto&, auto&, auto&, auto&, bool& accepted){accepted=false;});
    BadCommit([](auto& v, auto&, auto&, auto&, auto&, auto&, auto&, auto&){v.Invalidate();});
    BadCommit([](auto&, auto& t, auto&, auto&, auto&, auto&, auto&, auto&){t.owner=nullptr;});
    BadCommit([](auto&, auto& t, auto&, auto&, auto&, auto&, auto&, auto&){++t.version.generation;});
    BadCommit([](auto&, auto& t, auto&, auto&, auto&, auto&, auto&, auto&){++t.version.anchor;});
    BadCommit([](auto&, auto& t, auto&, auto&, auto&, auto&, auto&, auto&){++t.version.current;});
    BadCommit([](auto&, auto&, auto& p, auto&, auto&, auto&, auto&, auto&){++p.draw_identity;});
    BadCommit([](auto&, auto&, auto& p, auto&, auto&, auto&, auto&, auto&){p.vertex_module=p.certified_vertex_module=&other_owner;});
    BadCommit([](auto&, auto&, auto& p, auto&, auto&, auto&, auto&, auto&){p.pixel_module=p.certified_pixel_module=&other_owner;});
    BadCommit([](auto&, auto&, auto& p, auto&, auto&, auto&, auto&, auto&){p.active_query=true;});
    BadCommit([](auto&, auto&, auto& p, auto&, auto&, auto&, auto&, auto&){p.certified_pipeline=false;});
    BadCommit([](auto&, auto&, auto& p, auto&, auto&, auto&, auto&, auto&){p.raster.color[0].blend^=1;p.selected_specialization=NativeFsiStateSpecialization::From(p.raster);});
    BadCommit([](auto&, auto&, auto&, auto& pre, auto&, auto&, auto&, auto&){++pre;});
    BadCommit([](auto&, auto&, auto&, auto&, auto& post, auto&, auto&, auto&){--post;});
    BadCommit([](auto&, auto&, auto&, auto&, auto&, auto& pre, auto&, auto&){pre=0;});
    BadCommit([](auto&, auto&, auto&, auto&, auto&, auto& pre, auto&, auto&){pre=10;});
    BadCommit([](auto&, auto&, auto&, auto&, auto&, auto&, auto& post, auto&){++post;});
    BadCommit([](auto&, auto&, auto&, auto&, auto& post, auto&, auto& spost, auto&){++post;++spost;});
    BadCommit([](auto&, auto&, auto&, auto&, auto& post, auto& spre, auto& spost, auto&){spre=UINT64_MAX;post=spost=0;});
    for (uint64_t generation : {0u, 1u, 7u}) for (uint64_t source : {0u, 11u}) {
        auto p=Proof();NativeEarlyDepthLowerBoundVersion v(generation,kEarlyDepthMaterialDepthLayout,source);
        Check(bool(CaptureNativeEarlyDepthMaterialRetention(v,p,source))==bool(generation&&source),"Invalid version capture");
    }
    // An owner address match cannot replace the generation/anchor checks.
    { auto p=Proof();NativeEarlyDepthLowerBoundVersion a(7,kEarlyDepthMaterialDepthLayout,11), b(8,kEarlyDepthMaterialDepthLayout,11);
      auto t=CaptureNativeEarlyDepthMaterialRetention(a,p,11);t->owner=&b;
      Check(!CommitNativeEarlyDepthMaterialRetention(b,*t,p,11,12,11,12,true),"Different generation accepted"); }
    // A long material sequence advances only the current token, never the anchor.
    { NativeEarlyDepthLowerBoundVersion v(7,kEarlyDepthMaterialDepthLayout,11);
      for(uint64_t i=0;i<4096;++i){auto p=Proof(bool(i&1));p.draw_identity=i+1;
        const uint64_t pre=v.current(), post=pre+4;
        auto t=CaptureNativeEarlyDepthMaterialRetention(v,p,pre);Check(bool(t),"Sequence capture failed");
        Check(CommitNativeEarlyDepthMaterialRetention(v,*t,p,pre,post,post-1,post,true),"Sequence commit failed");
        Check(v.anchor()==11&&v.current()==post,"Sequence anchor changed");} }
    uint32_t random=0x35791357u;
    for(unsigned i=0;i<65536;++i){random=random*1664525u+1013904223u;const uint32_t old=random&0xffffffu;
      random=random*1664525u+1013904223u;const uint32_t incoming=random&0xffffffu;
      const uint32_t next=incoming>=old?incoming:old;
      Check(next>=old&&Fp24(next)>=Fp24(old),"Packed GE store broke monotonicity");}
    for(uint32_t e=1;e<16;++e){const uint32_t edge=e<<20;Check(Fp24(edge)>Fp24(edge-1),"FP24 exponent boundary decreased");}
    std::cout << "Early depth material retention CPU policy PASS " << checks
              << " checks; module certification and GPU parity are separate\n";
}
