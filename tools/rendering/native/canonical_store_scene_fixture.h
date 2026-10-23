#pragma once
#include "renderer/native/scene_executor.h"

namespace gow3::renderer::validation {
void RunNativeCanonicalStoreSceneFixture(const SceneExecutorConfig&,
    const nlohmann::json& original);
}
