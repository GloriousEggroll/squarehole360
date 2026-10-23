#pragma once
#include "renderer/native/scene_executor.h"
namespace gow3::renderer::validation {
void RetainedFp24DepthSceneFixture(const SceneExecutorConfig&,std::span<const LiveOperation* const>,
    const std::array<std::span<const uint32_t>,2>& vertex,std::span<const uint32_t> original_builtin);
}
