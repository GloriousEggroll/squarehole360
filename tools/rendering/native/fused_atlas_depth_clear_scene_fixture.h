#pragma once
#include "renderer/native/scene_executor.h"

namespace gow3::renderer::validation {
void RunNativeFusedAtlasDepthClearSceneFixture(const SceneExecutorConfig&,
    const nlohmann::json& original);
void RunNativeDirectAtlasDepthResolveSceneFixture(const SceneExecutorConfig&,
    const nlohmann::json& original);
void RunNativePackedPrimaryStencilClearSceneFixture(const SceneExecutorConfig&,
    const nlohmann::json& original);
void RunNativeDirectPackedColorResolveSceneFixture(const SceneExecutorConfig&,
    const nlohmann::json& original);
void RunNativePackedSecondaryStencilClearSceneFixture(const SceneExecutorConfig&,
    const nlohmann::json& original);
void RunNativeFusedPackedAtlasResolveSceneFixture(const SceneExecutorConfig&,
    const nlohmann::json& original);
}
