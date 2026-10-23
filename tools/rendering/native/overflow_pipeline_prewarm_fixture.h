#pragma once
#include "renderer/native/scene_executor.h"
#include "renderer/native/native_overflow_prewarm.h"
#include "renderer/native/pipeline_prewarm.h"
#include "renderer/native/draw_state.h"
#include "renderer/native/live_plan.h"
#include "renderer/native/shader_compiler.h"
#include "renderer/native/content_hash.h"
#include "renderer/native/native_early_depth_shader.h"
#include "renderer/native/builtin_readonly_query.h"
#include <cstring>
#include <fstream>
#include <map>

namespace gow3::renderer::validation {
// Explicit offline fixture only. No runtime admission or ordinary replay changes.
class OverflowPipelinePrewarmFixture {
    using Json=nlohmann::json;
    struct Input {NativeFsiPipelineRecipe recipe;std::vector<uint32_t> vertex,pixel;uint64_t draws=0;};
    std::vector<Input> inputs_;
    Json before_,prepared_,records_=Json::array();
    uint64_t new_pipelines_=0,temporary_modules_=0,temporary_bytes_=0,ordinary_witness_draws_=0;
    static void Require(bool value,const char* message) {
        if(!value)throw std::runtime_error(message);
    }
    static uint64_t Count(const Json& report,const char* bank,const char* field) {
        return report.at(bank).at(field).get<uint64_t>();
    }
    static Json OptionalOwners(const Json& report) {
        Json out=Json::object();
        for(const char* name:{"native_early_depth","native_readonly_depth_attachment"})
            if(report.contains(name))out[name]=report.at(name);
        return out;
    }
    static Json Recipe(const NativeFsiPipelineRecipe& recipe) {
        return {{"vertex",recipe.vertex},{"pixel",recipe.pixel?Json(*recipe.pixel):Json(nullptr)},
            {"raster",recipe.raster},{"color",recipe.color_format},{"depth",recipe.depth_format}};
    }
    static std::vector<uint32_t> Words(const std::filesystem::path& path) {
        std::ifstream stream(path,std::ios::binary|std::ios::ate);const auto size=stream.tellg();
        Require(bool(stream)&&size>=20&&uint64_t(size)<=16u*1024*1024&&uint64_t(size)%4==0,
            "Overflow fixture canonical SPIR-V extent is invalid");
        std::vector<uint32_t> words(size_t(size)/4);stream.seekg(0);
        Require(bool(stream.read(reinterpret_cast<char*>(words.data()),size)),"Overflow fixture SPIR-V read failed");
        Require(words.front()==0x07230203,"Overflow fixture SPIR-V magic is invalid");return words;
    }
    static LiveResourceBytes Source(const std::vector<std::shared_ptr<LiveRenderFrame>>& frames,std::string_view digest) {
        for(const auto& frame:frames)if(const auto* bytes=frame->FindResource(digest,true))return *bytes;
        throw std::runtime_error("Overflow fixture lacks captured canonical source "+std::string(digest));
    }
    static NativeFsiPipelineRecipe DrawRecipe(const TypedLiveDraw& draw,
            const std::vector<std::shared_ptr<LiveRenderFrame>>& frames) {
        auto raw=draw.lazy_device_state?draw.lazy_device_state->Owner():draw.device_state.owner;
        if(!raw)for(const auto& frame:frames)if(const auto* found=frame->FindResource(draw.device_state.hash.View())) {raw=*found;break;}
        Require(raw&&raw->size()>=0x2a60,"Overflow fixture captured Draw state is missing");
        const auto be=[&](size_t offset) {
            return uint32_t((*raw)[offset])<<24|uint32_t((*raw)[offset+1])<<16|
                uint32_t((*raw)[offset+2])<<8|uint32_t((*raw)[offset+3]);
        };
        // Exactly the packed Scene's decoding inputs; original primitive/index
        // expansion uses triangles before DecodeDrawState, except rectangle8.
        const bool color=draw.pixel_shader&&draw.surfaces[0]&&(be(0x28dc)&15);
        const auto& extent=draw.surfaces[color?0:4];
        Require(bool(extent)&&bool(draw.vertex_shader),"Overflow fixture Draw attachment/VS is missing");
        std::array<uint32_t,kDrawStateWords> words{};
        for(size_t i=0;i<words.size();++i)words[i]=be(kDrawStateOffset+i*4);
        words[(0x2934-kDrawStateOffset)/4]=0;words[(0x2948-kDrawStateOffset)/4]&=~(7u<<11);
        const auto state=DecodeDrawState(words,extent->width,extent->height,draw.rectangle_list?8:4,true);
        const auto& p=state.pipeline;
        NativeFsiPipelineRecipe recipe{std::string(draw.vertex_shader->hash.View()),
            draw.pixel_shader?std::optional<std::string>(draw.pixel_shader->hash.View()):std::nullopt,
            {uint32_t(p.cullMode),uint32_t(p.frontFace),uint32_t(p.depthClipEnabled),uint32_t(p.primitiveTopology),
                state.specialization,uint32_t(draw.rectangle_list)},int32_t(plume::RenderFormat::R8G8B8A8_UNORM),-1};
        Require(ValidNativeFsiPipelineRecipe(recipe),"Overflow fixture captured Draw recipe is unsupported");return recipe;
    }
public:
    OverflowPipelinePrewarmFixture(const std::vector<std::shared_ptr<LiveRenderFrame>>& frames,
            const std::filesystem::path& directory,const std::filesystem::path& compiled) {
        std::map<NativeFsiPipelineRecipe,uint64_t> required;
        for(const auto& frame:frames)for(const auto& operation:frame->operations)
            if(const auto* draw=operation.Draw())++required[DrawRecipe(*draw,frames)];
        Require(!required.empty()&&required.size()<=kNativeOverflowPrewarmMaximumPipelines,
            "Overflow fixture captured recipe set is empty or exceeds the PSO limit");
        NativePipelineRecipeCache observed(directory,NativeShaderCompiler::CacheVersion());
        const auto loaded=observed.Load();
        std::map<NativeFsiPipelineRecipe,const ObservedNativePipeline*> authenticated;
        for(const auto& item:loaded)authenticated.emplace(item.recipe,&item);
        uint64_t retained_bytes=0;
        for(const auto& [recipe,draws]:required) {
            const auto found=authenticated.find(recipe);
            Require(found!=authenticated.end(),"Overflow fixture recipe seed does not cover every captured exact Draw key");
            const auto& item=*found->second;const auto vertex=Source(frames,recipe.vertex);
            Require(item.vertex&&*item.vertex==*vertex&&ContentHash(*vertex)==recipe.vertex,
                "Overflow fixture VS source differs from authenticated recipe/capture");
            if(recipe.pixel) {
                const auto pixel=Source(frames,*recipe.pixel);
                Require(item.pixel&&*item.pixel==*pixel&&ContentHash(*pixel)==*recipe.pixel,
                    "Overflow fixture PS source differs from authenticated recipe/capture");
            }else Require(!item.pixel,"Overflow fixture builtin recipe unexpectedly owns a guest PS");
            auto vs=Words(compiled/(recipe.vertex+".spv"));
            auto ps=Words(compiled/(recipe.pixel?*recipe.pixel+".fsi.spv":"depth-only.fsi.spv"));
            const uint64_t bytes=vs.size()*4+ps.size()*4;
            Require(bytes<=64u*1024*1024-retained_bytes,"Overflow fixture prepared span owners exceed64MiB");
            retained_bytes+=bytes;inputs_.push_back({recipe,std::move(vs),std::move(ps),draws});
        }
    }
    void Prepare(SceneExecutor& scene) {
        Require(!scene.HasPendingGpuWork()&&!scene.mutation_serial(),"Overflow fixture startup already accepted work");
        before_=scene.report();
        for(const auto& input:inputs_) {
            const auto result=scene.PrewarmFsiOverflowPipeline(input.recipe,input.vertex,input.pixel);
            Require(result.ready(),"Overflow fixture captured canonical pipeline was refused");
            const bool created=result.status==NativeOverflowPrewarmStatus::Prepared;
            new_pipelines_+=created;temporary_modules_+=result.temporary_modules;temporary_bytes_+=result.temporary_bytes;
            const bool optional_pair=(input.recipe.vertex==kNativeEarlyDepthVertexSource&&
                input.recipe.pixel&&*input.recipe.pixel==kNativeEarlyDepthPixelSource)||
                (input.recipe.vertex==kReadonlyQueryVertexSource&&!input.recipe.pixel);
            if(created&&result.temporary_modules&&!optional_pair)ordinary_witness_draws_+=input.draws;
            records_.push_back({{"recipe",Recipe(input.recipe)},{"captured_draws",input.draws},
                {"new_pipeline",created},{"temporary_modules",result.temporary_modules},{"temporary_bytes",result.temporary_bytes},
                {"vertex_spirv_sha256",ContentHash({reinterpret_cast<const uint8_t*>(input.vertex.data()),input.vertex.size()*4})},
                {"pixel_spirv_sha256",ContentHash({reinterpret_cast<const uint8_t*>(input.pixel.data()),input.pixel.size()*4})}});
        }
        prepared_=scene.report();
        Require(new_pipelines_>0&&temporary_modules_>0&&ordinary_witness_draws_>0,
            "Overflow fixture temporary-module/new-generic-pipeline witness is vacuous");
        Require(before_.at("shader_cache")==prepared_.at("shader_cache"),"Overflow fixture mutated persistent module owners/accounting");
        Require(OptionalOwners(before_)==OptionalOwners(prepared_),"Overflow fixture mutated optional startup owners/counters");
        Require(Count(prepared_,"pipeline_cache","creations")==Count(before_,"pipeline_cache","creations")+new_pipelines_&&
            Count(prepared_,"pipeline_cache","resident")==Count(before_,"pipeline_cache","resident")+new_pipelines_&&
            Count(prepared_,"pipeline_cache","evictions")==Count(before_,"pipeline_cache","evictions")&&
            Count(prepared_,"pipeline_cache","pressure_drains")==Count(before_,"pipeline_cache","pressure_drains"),
            "Overflow fixture PSO ownership/capacity accounting changed unexpectedly");
        // Each API return has destroyed the local VkShaderModule owners. Reprobe
        // before Draw proves the cached pipeline key is independently resident.
        for(const auto& input:inputs_) {
            const auto result=scene.PrewarmFsiOverflowPipeline(input.recipe,input.vertex,input.pixel);
            Require(result.status==NativeOverflowPrewarmStatus::AlreadyResident&&!result.temporary_modules&&!result.temporary_bytes,
                "Overflow fixture PSO failed its post-local-owner resident reprobe");
        }
        Require(scene.report().at("shader_cache")==prepared_.at("shader_cache")&&
            scene.report().at("pipeline_cache")==prepared_.at("pipeline_cache")&&
            OptionalOwners(scene.report())==OptionalOwners(prepared_)&&!scene.HasPendingGpuWork()&&!scene.mutation_serial(),
            "Overflow fixture resident reprobe mutated startup or accepted GPU work");
    }
    Json Finish(SceneExecutor& scene,bool full_edram,bool wrapped) const {
        scene.FlushCommittedState();const Json after=scene.report();
        Require(Count(after,"pipeline_cache","creations")==Count(prepared_,"pipeline_cache","creations")&&
            Count(after,"pipeline_cache","misses")==Count(prepared_,"pipeline_cache","misses")&&
            Count(after,"pipeline_cache","hits")>Count(prepared_,"pipeline_cache","hits")&&
            Count(after,"pipeline_cache","evictions")==Count(prepared_,"pipeline_cache","evictions")&&
            Count(after,"pipeline_cache","pressure_drains")==Count(prepared_,"pipeline_cache","pressure_drains"),
            "Overflow fixture actual complete capture recreated/missed/evicted a prepared PSO");
        Require(!scene.HasPendingGpuWork(),"Overflow fixture Native work did not retire");
        return {{"schema",1},{"passed",true},{"whole_capture_replay_executed",true},
            {"capture_recipe_completeness_checked",true},{"canonical_shader_version",NativeShaderCompiler::CacheVersion()},
            {"recipes",records_},{"prepared_pipelines",new_pipelines_},{"temporary_modules",temporary_modules_},
            {"cumulative_temporary_module_bytes",temporary_bytes_},{"ordinary_temporary_pipeline_capture_draws",ordinary_witness_draws_},
            {"before",before_},{"prepared",prepared_},{"after",after},{"full_edram_fixture",full_edram},{"wrapped_frame_queries",wrapped},
            {"scope","Actual complete capture reuses prepared canonical PSOs after API-local modules are released; persistent/optional startup owners stay exact. Source spans are the supplied canonical replay SPIR-V oracle, not newly compiled certificates. Later ordinary shader-module reload is allowed. Paired images/query values/full final10MiB equivalence is an external requirement, not inferred from PSO counters."}};
    }
};
} // namespace gow3::renderer::validation
