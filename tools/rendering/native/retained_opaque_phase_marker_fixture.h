#pragma once
#include "retained_opaque_phase_owner_fixture.h"
namespace gow3::renderer::validation {
// Real Scene progress/lifetime proof. SDK packet/target guards are tested
// separately by coherence_wait_test; this fixture does not publish guest markers.
void RetainedOpaquePhaseMarkerFixture(const SceneExecutorConfig&,
    std::span<const LiveOperation* const>,
    std::span<const RetainedOpaquePhaseOwnerFixtureSource>,bool seed_full_edram);
}
