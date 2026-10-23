#pragma once
#include "retained_opaque_phase_owner_fixture.h"
namespace gow3::renderer::validation {
// Isolated diagnostic process only. Verified poisoned follower reaches the real
// fatal Scene destructor. Persisted proof + termination receipt + exit85 are
// jointly required; unexpected termination exits86.
void RetainedOpaquePhasePassFailureFixture(const SceneExecutorConfig&,
    std::span<const LiveOperation* const>,std::span<const RetainedOpaquePhaseOwnerFixtureSource>,bool seed_full_edram);
}
