#pragma once

#include "early_depth_lower_bound_version.h"
#include "fsi_shader_variant.h"
#include "fsi_state_specialization.h"
#include <cstdint>
#include <limits>
#include <optional>

namespace gow3::renderer {

inline constexpr bool kNativeEarlyDepthMaterialRetentionDefaultEnabled = false;
inline constexpr NativeReadonlyDepthLayoutKey kEarlyDepthMaterialDepthLayout{
    1280, 720, 1280, 0, 1, 1};
inline constexpr NativeReadonlyDepthLayoutKey kEarlyDepthMaterialColorLayout{
    1280, 720, 1280, 720, 3, 0};

// This describes the ACTUALLY selected draw after all variant/PSO decisions.
// Module certification is a separate, byte-authenticated structural/pin check.
// certified_* must be null unless that exact live RenderShader passed it; never
// populate them merely from a source digest, compiler version or intended PSO.
// Caller pins the modules and lower-bound resource through accepted submission.
struct NativeEarlyDepthMaterialProof {
    bool enabled = false;
    bool native_fsi = false;
    bool private_framebuffer = false;
    bool active_query = false;
    bool discarded = false;
    bool rectangle = false;
    bool indexed_triangles = false;
    bool gpu_geometry = false;
    bool bounded_scissor = false;
    bool single_sample = false;
    // The selected cached PSO must also have been created from certified module
    // owners. Certifying replacement shader-cache entries does not certify an
    // older PSO left resident under the same source/variant key.
    bool certified_pipeline = false;
    uint64_t draw_identity = 0;
    const void* vertex_module = nullptr;
    const void* pixel_module = nullptr;
    const void* certified_vertex_module = nullptr;
    const void* certified_pixel_module = nullptr;
    NativeShaderVariant vertex_variant = NativeShaderVariant::Unmodified;
    NativeShaderVariant pixel_variant = NativeShaderVariant::Unmodified;
    NativeReadonlyDepthLayoutKey depth_layout{};
    NativeReadonlyDepthLayoutKey color_layout{};
    NativeFsiConstants raster{};
    std::optional<NativeFsiStateSpecialization> selected_specialization;
};

enum class NativeEarlyDepthMaterialAdmission {
    Disabled, Query, Draw, Module, Layout, State, Admitted
};

inline NativeEarlyDepthMaterialAdmission ClassifyNativeEarlyDepthMaterialRetention(
    const NativeEarlyDepthMaterialProof& p) noexcept {
    if (!p.enabled) return NativeEarlyDepthMaterialAdmission::Disabled;
    if (p.active_query) return NativeEarlyDepthMaterialAdmission::Query;
    if (!p.native_fsi || !p.private_framebuffer || p.discarded || p.rectangle ||
        !p.indexed_triangles || p.gpu_geometry || !p.bounded_scissor ||
        !p.single_sample || !p.draw_identity)
        return NativeEarlyDepthMaterialAdmission::Draw;
    if (!p.certified_pipeline || !p.vertex_module || !p.pixel_module ||
        p.vertex_module != p.certified_vertex_module ||
        p.pixel_module != p.certified_pixel_module ||
        p.vertex_variant != NativeShaderVariant::Unmodified ||
        (p.pixel_variant != NativeShaderVariant::Unmodified &&
         p.pixel_variant != NativeShaderVariant::PipelineStateEpilogueV1))
        return NativeEarlyDepthMaterialAdmission::Module;
    // The exact bounded 32-bit primary surfaces occupy disjoint, nonwrapping
    // ranges: depth tiles0..719 and color tiles720..1439. Do not generalize this
    // to another pitch, sample count, format, viewport/scissor, MRT or alias.
    if (p.depth_layout != kEarlyDepthMaterialDepthLayout ||
        p.color_layout != kEarlyDepthMaterialColorLayout)
        return NativeEarlyDepthMaterialAdmission::Layout;
    const auto& r = p.raster;
    if (r.pitch != 1280 || r.depth_base || r.depth_format != 1 ||
        r.depth_control != 0x00700766u || r.color[0].base != 720 ||
        r.color[0].format != 3 || r.color[0].mask != 15 ||
        r.color[1].mask || r.color[2].mask || r.color[3].mask ||
        r.front_slope_bias != 0.0f || r.front_constant_bias != 0.0f ||
        r.back_slope_bias != 0.0f || r.back_constant_bias != 0.0f)
        return NativeEarlyDepthMaterialAdmission::State;
    // The certified state module still requires this draw's complete actual
    // specialization tuple. A certified module alone is not a certified PSO.
    if (p.pixel_variant == NativeShaderVariant::PipelineStateEpilogueV1) {
        if (!p.selected_specialization ||
            *p.selected_specialization != NativeFsiStateSpecialization::From(r))
            return NativeEarlyDepthMaterialAdmission::State;
    } else if (p.selected_specialization) {
        return NativeEarlyDepthMaterialAdmission::State;
    }
    return NativeEarlyDepthMaterialAdmission::Admitted;
}

// Borrowed identity only, never a resource owner. Scene must keep the exact
// generation alive and call capture after draw preparation; its shader modules
// must remain pinned. All strong invalidations still invalidate this version.
struct NativeEarlyDepthMaterialTicket {
    const NativeEarlyDepthLowerBoundVersion* owner = nullptr;
    NativeEarlyDepthLowerBoundVersion::Ticket version{};
    uint64_t draw_identity = 0;
    const void* vertex_module = nullptr;
    const void* pixel_module = nullptr;
    NativeShaderVariant pixel_variant = NativeShaderVariant::Unmodified;
    NativeFsiStateSpecialization state{};
};

inline std::optional<NativeEarlyDepthMaterialTicket>
CaptureNativeEarlyDepthMaterialRetention(NativeEarlyDepthLowerBoundVersion& version,
    const NativeEarlyDepthMaterialProof& proof, uint64_t observed_surface_version) noexcept {
    if (ClassifyNativeEarlyDepthMaterialRetention(proof) !=
        NativeEarlyDepthMaterialAdmission::Admitted) {
        version.Invalidate();
        return {};
    }
    auto ticket = version.Capture(kEarlyDepthMaterialDepthLayout, observed_surface_version);
    if (!ticket) {
        version.Invalidate();
        return {};
    }
    return NativeEarlyDepthMaterialTicket{&version, *ticket, proof.draw_identity,
        proof.vertex_module, proof.pixel_module, proof.pixel_variant,
        NativeFsiStateSpecialization::From(proof.raster)};
}

// Invoke only at the original accepted draw's metadata depth-write transition.
// Observe surface_pre and serial_pre immediately before RecordSurfaceWrite;
// observe surface_post/serial_post immediately after it. That operation advances
// the physical serial exactly once and stamps these exact depth pages. This
// excludes an unrelated write hidden by simply accepting any increasing token.
// Disjoint color metadata may advance the global serial BEFORE this checkpoint.
// No GPU data/image/barrier/query change or eager speculative token publication.
inline bool CommitNativeEarlyDepthMaterialRetention(
    NativeEarlyDepthLowerBoundVersion& version,
    const NativeEarlyDepthMaterialTicket& ticket,
    const NativeEarlyDepthMaterialProof& final_proof,
    uint64_t surface_pre, uint64_t surface_post,
    uint64_t serial_pre, uint64_t serial_post, bool accepted) noexcept {
    const bool valid = accepted && ticket.owner == &version &&
        ClassifyNativeEarlyDepthMaterialRetention(final_proof) ==
            NativeEarlyDepthMaterialAdmission::Admitted &&
        ticket.draw_identity == final_proof.draw_identity &&
        ticket.vertex_module == final_proof.vertex_module &&
        ticket.pixel_module == final_proof.pixel_module &&
        ticket.pixel_variant == final_proof.pixel_variant &&
        ticket.state == NativeFsiStateSpecialization::From(final_proof.raster) &&
        serial_pre && serial_pre != std::numeric_limits<uint64_t>::max() &&
        serial_post == serial_pre + 1 && surface_pre <= serial_pre &&
        surface_post == serial_post;
    if (!valid || !version.PromoteAcceptedMonotonic(kEarlyDepthMaterialDepthLayout,
            ticket.version, surface_pre, surface_post, true, true, false)) {
        version.Invalidate();
        return false;
    }
    return true;
}

} // namespace gow3::renderer
