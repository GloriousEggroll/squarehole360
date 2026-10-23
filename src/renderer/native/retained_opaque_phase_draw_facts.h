#pragma once
#include <array>
#include <cstdint>
#include <stdexcept>
#include <utility>

namespace gow3::renderer {
// One Draw stack frame only. Scene owns the exact const TypedLiveDraw and its
// immutable state for this entire lifetime. No GPU owner, query, generation,
// cache residency or overall admission result is remembered by this helper.
class NativeRetainedOpaquePhaseDrawFacts {
public:
    using Raster=std::array<uint32_t,6>;
    NativeRetainedOpaquePhaseDrawFacts(const void* draw,const void* state_owner)noexcept
        :draw_(draw),state_owner_(state_owner){}
    NativeRetainedOpaquePhaseDrawFacts(const NativeRetainedOpaquePhaseDrawFacts&)=delete;
    NativeRetainedOpaquePhaseDrawFacts& operator=(const NativeRetainedOpaquePhaseDrawFacts&)=delete;
    template<class Classify,class Resident>
    bool Allowed(const void* draw,const void* state_owner,Raster* out_raster,
            bool require_resident,Classify&& classify,Resident&& resident) {
        if(!draw_||!state_owner_||draw!=draw_||state_owner!=state_owner_)
            throw std::logic_error("Draw-local opaque phase facts lost their immutable input");
        if(!evaluated_) {
            Raster raster{};
            const bool eligible=std::forward<Classify>(classify)(raster);
            // Failed/throwing lazy reads never become a remembered result.
            raster_=raster;eligible_=eligible;evaluated_=true;
        }
        if(!eligible_)return false;
        if(out_raster)*out_raster=raster_;
        // Called afresh even after an earlier residency refusal or success.
        return !require_resident||std::forward<Resident>(resident)(raster_);
    }
private:
    const void* const draw_;
    const void* const state_owner_;
    bool evaluated_=false,eligible_=false;
    Raster raster_{};
};
} // namespace gow3::renderer
