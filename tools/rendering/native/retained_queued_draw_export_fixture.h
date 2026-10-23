#pragma once
#include "retained_opaque_scene_fixture.h"
namespace gow3::renderer::validation {
void RetainedQueuedDrawExportFixture(const SceneExecutorConfig&,
    std::span<const LiveOperation* const>,
    std::span<const RetainedOpaqueCatalogFixtureSource>);
}
