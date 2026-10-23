#pragma once
#include "renderer/native/scene_executor.h"
void CheckMultiDrawCommandBuffers(plume::RenderDevice&,plume::RenderCommandQueue&,
    gow3::renderer::SceneExecutorConfig,nlohmann::json,const std::vector<uint8_t>&,
    const std::string& alias_pixel);
// Run only in an isolated child. A verified poisoned follower intentionally
// reaches Scene's actual fatal destructor; returning from this scope aborts.
void CheckMultiDrawFollowerFailure(plume::RenderDevice&,plume::RenderCommandQueue&,
    gow3::renderer::SceneExecutorConfig,nlohmann::json);
