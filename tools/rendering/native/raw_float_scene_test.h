#pragma once
#include "renderer/native/scene_executor.h"
namespace gow3::renderer {
void CheckRawFloatCpuTextures(plume::RenderDevice&,plume::RenderCommandQueue&,
    SceneExecutorConfig,nlohmann::json,const std::vector<uint8_t>&);
}
