#pragma once
#include "retained_opaque_scene_fixture.h"
namespace gow3::renderer::validation {
void RetainedDisjointQuerySceneFixture(const SceneExecutorConfig&,
    std::span<const LiveOperation* const>,std::span<const RetainedOpaqueCatalogFixtureSource>,
    const std::array<std::span<const uint32_t>,2>& depth_vertex,
    std::span<const uint32_t> query_vertex,std::span<const uint32_t> original_builtin);
}
