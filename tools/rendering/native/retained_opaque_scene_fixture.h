#pragma once
#include "renderer/native/scene_executor.h"
#include "renderer/native/retained_opaque_catalog.h"
namespace gow3::renderer::validation {
void RetainedOpaquePartialOwnedFixture(const SceneExecutorConfig&,
    std::span<const LiveOperation* const>,std::span<const uint32_t> vertex,
    std::span<const uint32_t> original_pixel,bool seed_full_edram);
struct RetainedOpaqueCatalogFixtureSource {
    NativeRetainedOpaqueCatalogPair pair;
    std::span<const uint32_t> vertex,pixel;
};
void RetainedOpaqueCatalogPartialOwnedFixture(const SceneExecutorConfig&,
    std::span<const LiveOperation* const>,std::span<const RetainedOpaqueCatalogFixtureSource>,bool seed_full_edram,
    uint32_t through_override=UINT32_MAX,bool require_source_switch=false);
}
