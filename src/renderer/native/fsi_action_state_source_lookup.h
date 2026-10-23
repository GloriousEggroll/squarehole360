#pragma once
#include "fsi_state_specialization_recipe.h"
#include "native_overflow_prewarm.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace gow3::renderer {
// One exact unqueried action tuple observed in the real turret/explosion route.
// This admits no other overflow source or state; it does not add a learned row.
inline bool NativeFsiActionStateOverflowAllowed(const NativeFsiStateRecipe& recipe) noexcept {
    const auto& generic=recipe.generic;
    return generic.vertex=="c2c62310fb6b4157fbb21fe9fdb8736bdb388f8ed45548565a84bf9b82f79ee3"&&
        generic.pixel&&*generic.pixel=="1a70c06695ecde49f80867ce0a93e9880143ed126dd8a45af46173ccfebc50d1"&&
        generic.raster==std::array<uint32_t,6>{3,2,1,4,0,0}&&
        generic.color_format==20&&generic.depth_format==-1&&
        recipe.state.words==std::array<uint32_t,15>{7341926,1,0,3,15,65537,0,0,65537,0,0,65537,0,0,65537};
}
// Startup callback only. Both backing vectors must remain alive and unmodified
// until the lookup is destroyed. The result supplies authenticated source bytes,
// not a residency promise: Scene must still verify generic PSO, resident VS,
// exact state/program/variant and every existing hard capacity before admission.
template<class Prepared>
class NativeFsiActionStateSourceLookup {
  public:
    struct Match {
        const Prepared* source = nullptr;
        bool overflow = false;
    };
    NativeFsiActionStateSourceLookup(std::span<const Prepared> persistent,
                                 std::span<const Prepared> overflow,
                                 bool enable_overflow = false)
        : persistent_(persistent),
          overflow_(enable_overflow ? overflow.first(std::min(
              overflow.size(), size_t(kNativeOverflowPrewarmMaximumPipelines)))
                                    : std::span<const Prepared>{}),
          ready_(overflow_.size(), 0) {}

    // Record completed results, never planned admission or an attempted call.
    // Unrecorded, refused, cancelled and throwing preparations remain unavailable.
    void RecordOverflowResult(size_t index, NativeOverflowPrewarmResult result) noexcept {
        if(index < ready_.size()) ready_[index] = result.ready();
    }
    size_t ReadyCount() const noexcept {
        return size_t(std::count(ready_.begin(), ready_.end(), uint8_t(1)));
    }
    Match Find(const NativeFsiStateRecipe& state) const noexcept {
        const auto& recipe=state.generic;
        // Preserve the original persistent source and its priority, even if the
        // caller recorded a duplicate ready overflow entry.
        for(const auto& item : persistent_)
            if(item.recipe == recipe) return {&item, false};
        if(!NativeFsiActionStateOverflowAllowed(state)) return {};
        for(size_t i = 0; i < overflow_.size(); ++i)
            if(ready_[i] && overflow_[i].recipe == recipe) return {&overflow_[i], true};
        return {};
    }
  private:
    std::span<const Prepared> persistent_, overflow_;
    std::vector<uint8_t> ready_;
};
} // namespace gow3::renderer
