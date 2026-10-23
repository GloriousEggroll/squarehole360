#pragma once
#include "renderer/native/scene_executor.h"
#include "renderer/native/native_early_depth_shader.h"

namespace gow3::renderer::validation {
// Spans must refer to actual canonical compiler outputs retained by the caller.
// Every recipe is an observed original recipe, not a fabricated shader identity.
struct EarlyDepthCatalogSceneInput {
    NativeFsiPipelineRecipe recipe;
    std::span<const uint32_t> vertex,pixel;
    std::shared_ptr<const NativeEarlyDepthProgram> program;
};
// Shared only by the two explicit offline Scene fixtures. Preparation validates
// the original capture's owners, history and fixed material/query donors.
struct EarlyDepthFixtureContext {
    std::array<TypedLiveDraw,3> draws;
    TypedLiveDraw positive_query;
    std::vector<uint32_t> query_vertex_spirv,query_builtin_spirv;
    nlohmann::json resolve,metadata,history;
};
EarlyDepthFixtureContext PrepareNativeEarlyDepthFixtureContext(const SceneExecutorConfig&,
    std::span<const LiveOperation* const>);
void PrewarmNativeEarlyDepthFixture(SceneExecutor&,
    std::span<const EarlyDepthCatalogSceneInput>,bool catalog);
void RunNativeEarlyDepthCatalogSceneFixtures(const SceneExecutorConfig&,
    std::span<const LiveOperation* const>,
    std::span<const EarlyDepthCatalogSceneInput>);
}
