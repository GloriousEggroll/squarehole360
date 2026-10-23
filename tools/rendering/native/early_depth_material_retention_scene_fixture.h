#pragma once
#include "early_depth_catalog_scene_fixture.h"
namespace gow3::renderer::validation {
void RunNativeEarlyDepthMaterialRetentionSceneFixtures(const SceneExecutorConfig&,
    std::span<const LiveOperation* const>,std::span<const EarlyDepthCatalogSceneInput>);
}
