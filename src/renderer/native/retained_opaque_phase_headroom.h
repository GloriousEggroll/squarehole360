#pragma once
#include "retained_opaque_phase_catalog.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace gow3::renderer {
// Actual optional admissions add to the caller's existing composition credits.
// This helper owns no resource and never changes cache hard limits or pins an
// entry. Call admission only after resource creation succeeds; on eviction,
// retire the matching credit. Scene still checks prerequisites and hard space.
class NativeRetainedOpaquePhaseHeadroom {
public:
    struct Targets {
        uint64_t modules = 0, bytes = 0, pipelines = 0;
        constexpr bool operator==(const Targets&) const = default;
    };
    explicit constexpr NativeRetainedOpaquePhaseHeadroom(
            uint64_t module_byte_limit = kRetainedOpaquePhaseMaxModuleBytes) noexcept
        : byte_limit_(std::min(module_byte_limit, kRetainedOpaquePhaseMaxModuleBytes)) {}

    bool HasModule(size_t pixel_index) const noexcept {
        return pixel_index < module_bytes_.size() && module_bytes_[pixel_index] != 0;
    }
    bool HasPipeline(size_t recipe_index) const noexcept {
        return recipe_index < admitted_pipelines_.size() && admitted_pipelines_[recipe_index];
    }
    bool CanAdmitModule(size_t pixel_index, uint64_t bytes) const noexcept {
        return pixel_index < module_bytes_.size() && !HasModule(pixel_index) && bytes &&
            modules_ < kRetainedOpaquePhaseMaxPrograms && bytes <= byte_limit_ - bytes_;
    }
    bool AdmitModule(size_t pixel_index, uint64_t bytes) noexcept {
        if (!CanAdmitModule(pixel_index, bytes)) return false;
        module_bytes_[pixel_index] = bytes;
        ++modules_;
        bytes_ += bytes;
        return true;
    }
    bool CanAdmitPipeline(size_t recipe_index) const noexcept {
        if (recipe_index >= admitted_pipelines_.size() || HasPipeline(recipe_index) ||
            pipelines_ >= kRetainedOpaquePhaseMaxPipelines) return false;
        const auto pixel = NativeRetainedOpaquePhasePixelIndex(kRetainedOpaquePhaseRecipes[recipe_index].pixel);
        return pixel && HasModule(*pixel);
    }
    bool AdmitPipeline(size_t recipe_index) noexcept {
        if (!CanAdmitPipeline(recipe_index)) return false;
        admitted_pipelines_[recipe_index] = true;
        ++pipelines_;
        return true;
    }
    bool RetireModule(size_t pixel_index) noexcept {
        if (!HasModule(pixel_index)) return false;
        bytes_ -= module_bytes_[pixel_index];
        module_bytes_[pixel_index] = 0;
        --modules_;
        // Cached PSOs can outlive their modules; their actual credit remains
        // until their separate retirement. Residency proofs handle selection.
        return true;
    }
    bool RetirePipeline(size_t recipe_index) noexcept {
        if (!HasPipeline(recipe_index)) return false;
        admitted_pipelines_[recipe_index] = false;
        --pipelines_;
        return true;
    }
    Targets AddToSoftTargets(bool enabled, Targets existing, Targets hard) const noexcept {
        const auto add = [enabled](uint64_t soft, uint64_t ceiling, uint64_t credit) {
            const auto base = std::min(soft, ceiling);
            return base + (enabled ? std::min(credit, ceiling - base) : 0);
        };
        return {add(existing.modules, hard.modules, modules_),
                add(existing.bytes, hard.bytes, bytes_),
                add(existing.pipelines, hard.pipelines, pipelines_)};
    }
    uint64_t modules() const noexcept { return modules_; }
    uint64_t bytes() const noexcept { return bytes_; }
    uint64_t pipelines() const noexcept { return pipelines_; }
    uint64_t byte_limit() const noexcept { return byte_limit_; }

private:
    std::array<uint64_t, kRetainedOpaquePhaseMaxPrograms> module_bytes_{};
    std::array<bool, kRetainedOpaquePhaseMaxPipelines> admitted_pipelines_{};
    uint64_t modules_ = 0, bytes_ = 0, pipelines_ = 0, byte_limit_;
};
} // namespace gow3::renderer

