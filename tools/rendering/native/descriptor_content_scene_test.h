#pragma once
#include "renderer/native/scene_executor.h"
void CheckDescriptorContentCache(plume::RenderDevice&,plume::RenderCommandQueue&,
    gow3::renderer::SceneExecutorConfig,const std::vector<uint8_t>&);
