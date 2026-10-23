#pragma once
#include "retained_opaque_scene_fixture.h"
namespace gow3::renderer::validation {
void RetainedSharedDepthSceneFixture(const SceneExecutorConfig&,
    std::span<const LiveOperation* const>,std::span<const RetainedOpaqueCatalogFixtureSource>,
    std::span<const uint32_t> query_vertex,std::span<const uint32_t> original_builtin);
void RetainedPooledSharedDepthSceneFixture(const SceneExecutorConfig&,
    std::span<const LiveOperation* const>,std::span<const RetainedOpaqueCatalogFixtureSource>,
    std::span<const uint32_t> query_vertex,std::span<const uint32_t> original_builtin);
}
