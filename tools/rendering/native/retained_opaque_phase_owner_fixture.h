#pragma once
#include "renderer/native/scene_executor.h"
namespace gow3::renderer::validation {
// Reuse the already-authenticated canonical words prepared by replay. No compiler
// or shader-cache reader is introduced by this fixture.
struct RetainedOpaquePhaseOwnerFixtureSource {
    NativeFsiPipelineRecipe recipe;
    std::span<const uint32_t> vertex,pixel;
};
void RetainedOpaquePhaseOwnerFixture(const SceneExecutorConfig&,
    std::span<const LiveOperation* const>,std::span<const RetainedOpaquePhaseOwnerFixtureSource>,bool seed_full_edram);
// Separate before-clear oracle. Runs exact OFF/ON prefixes at289/487/1535/1736;
// does not call the ordinary replay executable's post-run coverage assertions.
void RetainedOpaquePhasePrefixFixture(const SceneExecutorConfig&,
    std::span<const LiveOperation* const>,std::span<const RetainedOpaquePhaseOwnerFixtureSource>,bool seed_full_edram);
// Separate queued typed-draw boundary: actual prefix309 then excluded draw310,
// paired queued/strong export, real empty query and strong CPU read closure.
void RetainedOpaquePhaseQueuedExportFixture(const SceneExecutorConfig&,
    std::span<const LiveOperation* const>,std::span<const RetainedOpaquePhaseOwnerFixtureSource>,bool seed_full_edram);
}
