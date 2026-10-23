#include <unordered_map>
#include <condition_variable>
#include <thread>
#include <deque>
// Ordered native execution of user-owned D3D captures and live draws. Pending
// draws retain immutable resources until a shared submission fence completes.
// Unsupported operations stop the replay; they are never silently skipped.
#include "scene_executor.h"
#include "native_frame_capture_reader.h"
#include "fsi_material_shader_cache_headroom.h"
#include "stencil_material_cutoff_compiler.h"
#ifdef GOW3_NATIVE_STENCIL_MATERIAL_REPLAY
#include "stencil_material_census.h"
#endif
#include "fsi_exact_stores_policy.h"
#include "native_fsi_exact_stores_version.h"
#include "native_fsi_hoist_version.h"
#include "retired_buffer_pool.h"
#include "cpu_write_policy.h"
#include "packed_zero_depth_clear.h"
#include "packed_primary_stencil_clear.h"
#include "packed_primary_stencil_clear_spv.h"
#include "packed_secondary_stencil_clear.h"
#include "packed_secondary_stencil_clear_spv.h"
#include "draw_barriers.h"
#include "dummy_fsi_render_pass.h"
#include "attachmentless_fsi.h"
#include "builtin_atomic_depth.h"
#include "packed_depth_pass_groups.h"
#include "packed_color_pass_groups.h"
#include "fsi_draw_pass_groups.h"
#include "packed_color_variant8_catalog.h"
#include "packed_color_blend_variant8_catalog.h"
#include "packed_color_blend_variant8_module.h"
#include "packed_color_action_variant8.h"
#include "builtin_readonly_query.h"
#include "readonly_depth_attachment.h"
#include "readonly_depth_less.h"
#include "retained_disjoint_query.h"
#include "readonly_depth_conversion.h"
#include "early_depth_lower_bound_version.h"
#include "early_depth_material_retention.h"
#include "native_monotonic_depth_module.h"
#include "native_early_depth_shader.h"
#include "native_early_depth_catalog.h"
#include "early_depth_lower_bound_comp_spv.h"
#include "retained_opaque_writer.h"
#include "retained_fp24_depth_writer.h"
#include "retained_fp24_depth.h"
#include "retained_fp24_depth_restore_spv.h"
#include "retained_fp24_depth_store_spv.h"
#include "retained_opaque_compiler.h"
#include "retained_opaque_catalog.h"
#include "retained_opaque_phase_catalog.h"
#include "retained_opaque_phase_compiler.h"
#include "retained_opaque_phase_module.h"
#include "retained_opaque_phase_headroom.h"
#include "retained_opaque_phase_pass_groups.h"
#include "retained_opaque_phase_draw_facts.h"
#include "retained_opaque_pressure_policy.h"
#include "retained_opaque_catalog_headroom.h"
#include "retained_opaque_catalog_compiler.h"
#include "retained_opaque_restore_spv.h"
#include "retained_opaque_store_spv.h"
#include "retained_opaque_phase_restore_spv.h"
#include "retained_opaque_phase_store_spv.h"
#include "readonly_depth_attachment_comp_spv.h"
#include "readonly_depth_attachment_frag_spv.h"
#include "opaque_color_overwrite_compiler.h"
#include "forward_control_flow_compiler.h"
#include "forward_vertex_compiler.h"
#include "forward_pixel_catalog_compiler.h"
#include "fsi_state_specialization_compiler.h"
#include "fsi_state_material_composition.h"
#include "fsi_state_forward_cfg_compiler.h"
#include "fsi_state_forward_cfg.h"
#include "catalog_shader_cache_headroom.h"
#include "content_hash.h"
#include "native_opaque_shader_version.h"
#include "native_forward_shader_version.h"
#include "native_forward_vertex_shader_version.h"
#include "native_forward_pixel_catalog_shader_version.h"
#include "native_fsi_state_specialization_shader_version.h"
#include "native_fsi_state_forward_cfg_shader_version.h"
#include "draw_descriptor_batch.h"
#include "shader_descriptor_sets.h"
#include "descriptor_content_cache.h"
#include "draw_command_pool.h"
#include "live_draw.h"
#include "live_draw_json.h"
#include "live_operations.h"
#include "cache_bookkeeping.h"
#include "cpu_image_slot_lookup.h"
#include "cpu_texture_storage_sampling_key.h"
#include "shader_lookup_cache.h"
#include "lookaside_map.h"
#include <xxhash.h>
#include "dormant_cache.h"
#include "texture_eviction_reload_census.h"
#include "sampled_alias_index.h"
#include "transfer_batch.h"
#include "gpu_work_queue.h"
#include "gpu_batch_attribution_json.h"
#include "draw_timestamps.h"
#ifdef GOW3_NATIVE_FRAGMENT_CENSUS_REPLAY
#include "fragment_invocation_census.h"
#endif
#include "texture_layout_cache.h"
#include "synchronous_gpu_timing.h"
#include "gpu_helpers.h"
#include "renderer/native/draw_state.h"
#include "renderer/native/constant_data.h"
#include "renderer/native/constant_delta_data.h"
#include "renderer/native/pinned_null_constant_data.h"
#include "atlas_depth_region.h"
#include "fused_atlas_depth_clear.h"
#include "atlas_depth_clear.h"
#include "atlas_depth_clear_spv.h"
#include "atlas_depth_clear_host_spv.h"
#include "direct_atlas_depth_resolve.h"
#include "fused_packed_atlas_resolve.h"
#include "fused_packed_atlas_resolve_spv.h"
#include "direct_packed_color_resolve.h"
#include "memory_store_rgba_stencil_spv.h"
#include "renderer/native/index_data.h"
#include "renderer/native/index_geometry_cache.h"
#include "renderer/native/pass_transfer.h"
#include "renderer/native/resolve_state.h"
#include "renderer/native/texture_upload.h"
#include "renderer/native/texture_probe.h"
#include "renderer/native/resolve_memory.h"
#include "renderer/native/query_tracker.h"
#include "renderer/native/fsi_device.h"
#include "renderer/native/fsi_state.h"
#include "fullscreen_spv.h"
#include "resolve_spv.h"
#include "memory_store_spv.h"
#include "memory_store_canonical_spv.h"
#include "memory_load_spv.h"
#include "memory_restore_frag_spv.h"
#include "memory_load_r32_spv.h"
#include "memory_load_rg32_spv.h"
#include "raw_float_texture_spv.h"
#include "raw_float_texture.h"
#include "restore_depth_spv.h"
#include "restore_depth_stencil_spv.h"
#include <bitset>
#include <nlohmann/json.hpp>
#include <picosha2.h>
#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <string_view>
#include <type_traits>
#include <tuple>
#include <variant>
using namespace plume;
using namespace gow3::renderer;
using Json = nlohmann::json;
namespace fs = std::filesystem;
namespace gow3::renderer {
namespace {
// Host lifetime identity only; exhausted identities become unavailable rather
// than wrap or reuse a guest address/serial. One relaxed allocation per Scene.
uint64_t AllocateDiagnosticSceneGeneration() noexcept {
    static std::atomic<uint64_t> next{1};
    auto value=next.load(std::memory_order_relaxed);
    while(value!=UINT64_MAX) {
        if(next.compare_exchange_weak(value,value+1,std::memory_order_relaxed))return value;
    }
    return 0;
}
std::vector<uint8_t> Read(const fs::path& path, size_t limit=16*1024*1024) {
    std::ifstream file(path,std::ios::binary|std::ios::ate);
    const auto size=file.tellg();
    if(!file || size<0 || uint64_t(size)>limit) throw std::runtime_error("Invalid input: "+path.string());
    std::vector<uint8_t> out(static_cast<size_t>(size)); file.seekg(0);
    file.read(reinterpret_cast<char*>(out.data()),size);
    if(!file) throw std::runtime_error("Truncated input: "+path.string());
    return out;
}
std::string_view DigestView(const Json& j) {
    const auto& s=j.get_ref<const std::string&>();
    if(s.size()!=64 || s.find_first_not_of("0123456789abcdef")!=std::string::npos)
        throw std::runtime_error("Invalid capture digest");
    return s;
}
std::string Digest(const Json& j) {return std::string(DigestView(j));}
std::optional<std::string_view> OptionalDigestView(const Json& j) {
    if(j.is_null())return std::nullopt;
    return DigestView(j);
}
// These views preserve the offline adapter's lazy field/provider accesses at
// their original execution points. Live input reads immutable typed values;
// neither path constructs a second full draw representation.
struct DrawResourceInput {
    const CanonicalResourceRef* typed=nullptr;
    const Json* json=nullptr;
    const LazyDeviceState* lazy_state=nullptr;
    bool IsNull() const {return json?json->is_null():!typed&&!lazy_state;}
    std::string_view Digest() const {
        if(json)return DigestView(*json);
        if(lazy_state)return lazy_state->Digest();
        if(!typed)throw std::runtime_error("Invalid capture digest");
        return typed->hash.View();
    }
    bool Matches(std::string_view expected)const {
        return json?*json==std::string(expected):typed&&typed->hash.View()==expected;
    }
    std::optional<std::string_view> OptionalDigest() const {
        return IsNull()?std::nullopt:std::optional<std::string_view>(Digest());
    }
};
struct DrawSurfaceInput {
    const TypedSurface* typed=nullptr;
    const Json* json=nullptr;
    explicit operator bool() const {return typed||json;}
    uint32_t Width()const{return typed?typed->width:json->at("width").get<uint32_t>();}
    uint32_t Height()const{return typed?typed->height:json->at("height").get<uint32_t>();}
    uint32_t Info()const{return typed?typed->surface_info:json->at("surface_info").get<uint32_t>();}
    uint32_t FormatInfo()const{return typed?typed->format_info:json->at("format_info").get<uint32_t>();}
    uint32_t Base()const{return typed?typed->edram_base:json->at("edram_base").get<uint32_t>();}
    uint32_t Format()const{return typed?typed->format:json->at("format").get<uint32_t>();}
    bool Depth()const{return typed?typed->depth:json->at("depth").get<bool>();}
    Json ToJson()const{return typed?gow3::renderer::ToJson(*typed):*json;}
};
struct DrawGeometryInput {
    const TypedGeometry* typed=nullptr;
    const Json* json=nullptr;
    explicit operator bool()const{return typed||json;}
    uint32_t Kind()const{return typed?typed->kind:json->at("kind").get<uint32_t>();}
    std::optional<uint32_t> FetchSlot()const {
        if(typed)return typed->fetch_slot;
        if(json->contains("fetch_slot"))return json->at("fetch_slot").get<uint32_t>();
        return {};
    }
    bool GpuMemory()const{return typed?typed->gpu_memory:json->value("source","")=="gpu_memory";}
    uint32_t Address()const{return typed?typed->address:uint32_t(std::stoul(json->at("address").get<std::string>(),nullptr,16));}
    uint32_t Bytes()const{return typed?typed->bytes:json->at("bytes").get<uint32_t>();}
    uint32_t Endian()const {
        if(typed) {
            if(!typed->endian)throw std::out_of_range("Missing native vertex endian");
            return *typed->endian;
        }
        return json->at("endian").get<uint32_t>();
    }
    DrawResourceInput Payload()const{return typed?DrawResourceInput{&typed->payload}:DrawResourceInput{nullptr,&json->at("sha256")};}
};
struct DrawTextureIdentityInput {
    const TypedTextureIdentity* typed=nullptr;
    const Json* json=nullptr;
    uint32_t Address()const{return typed?typed->base:uint32_t(std::stoul(json->at("base").get<std::string>(),nullptr,16));}
    std::string BaseText()const {
        if(json)return json->at("base").get<std::string>();
        constexpr char hex[]="0123456789abcdef";std::string text(8,'0');
        for(unsigned i=0;i<8;++i)text[7-i]=hex[(typed->base>>(i*4))&15];
        return text;
    }
    Json ToJson()const{return typed?gow3::renderer::ToJson(*typed):*json;}
    template<class Index>auto FindOrCreate(Index& index,uint32_t signs)const {
        if(json)return index.FindOrCreate(*json,signs);
        const CanonicalSampledAlias canonical{typed->base,typed->dimension,typed->endian,typed->format,
            typed->height,typed->layers,uint32_t(typed->packed),typed->pitch,uint32_t(typed->tiled),typed->width,signs};
        return index.FindOrCreate(canonical,[&]{return ToJson();});
    }
};
struct DrawTextureInput {
    const TypedTexture* typed=nullptr;
    const Json* json=nullptr;
    uint32_t Slot()const{return typed?typed->slot:json->at("slot").get<uint32_t>();}
    std::array<uint32_t,6> Fetch()const{return typed?typed->fetch:json->at("fetch").get<std::array<uint32_t,6>>();}
    bool SourceIs(std::string_view expected)const {
        if(json)return json->at("source")==std::string(expected);
        switch(typed->source) {
            case TextureSource::CpuSnapshot:return expected=="cpu_snapshot";
            case TextureSource::GpuPreviousOrAlias:return expected=="gpu_alias_or_previous_frame";
            case TextureSource::GpuResolve:return expected=="gpu_resolve";
        }
        return false;
    }
    std::string SourceText()const {
        if(json)return json->at("source").get<std::string>();
        if(SourceIs("cpu_snapshot"))return "cpu_snapshot";
        if(SourceIs("gpu_resolve"))return "gpu_resolve";
        if(SourceIs("gpu_alias_or_previous_frame"))return "gpu_alias_or_previous_frame";
        return "unknown";
    }
    DrawResourceInput BasePayload()const {
        if(json)return {nullptr,&json->at("base_payload")};
        return {typed->base_payload?&*typed->base_payload:nullptr};
    }
    DrawResourceInput MipPayload()const {
        if(json)return {nullptr,&json->at("mip_payload")};
        return {typed->mip_payload?&*typed->mip_payload:nullptr};
    }
    DrawTextureIdentityInput Identity()const {
        if(json)return {nullptr,&json->at("identity")};
        if(!typed->identity)throw std::out_of_range("Missing native texture identity");
        return {&*typed->identity};
    }
};
struct DrawInput {
    const TypedLiveDraw* typed=nullptr;
    const Json* json=nullptr;
    explicit DrawInput(const TypedLiveDraw& value):typed(&value){}
    explicit DrawInput(const Json& value):json(&value){}
    uint64_t Event()const{return typed?typed->event:json->at("event").get<uint64_t>();}
    uint32_t Frame()const{return typed?typed->frame:json->at("frame").get<uint32_t>();}
    DrawResourceInput State()const{return typed?(typed->lazy_device_state?DrawResourceInput{nullptr,nullptr,typed->lazy_device_state.get()}:DrawResourceInput{&typed->device_state}):DrawResourceInput{nullptr,&json->at("device_state")};}
    std::array<uint32_t,8> Args()const{return typed?typed->args:json->at("args").get<std::array<uint32_t,8>>();}
    std::string_view Function()const{return typed?DrawFunctionName(typed->function):std::string_view(json->at("function").get_ref<const std::string&>());}
    bool Rectangle()const{return typed?typed->rectangle_list:json->value("vertex_input","")=="affine_float4_rectangle";}
    DrawResourceInput Shader(bool pixel)const {
        if(json)return {nullptr,&json->at(pixel?"pixel_shader":"vertex_shader")};
        const auto& resource=pixel?typed->pixel_shader:typed->vertex_shader;
        return {resource?&*resource:nullptr};
    }
    std::array<DrawSurfaceInput,5> Surfaces()const {
        std::array<DrawSurfaceInput,5> result{};
        if(typed) {
            for(size_t i=0;i<result.size();++i)if(typed->surfaces[i])result[i].typed=&*typed->surfaces[i];
        } else {
            const auto& surfaces=json->at("surfaces");
            constexpr std::array<const char*,5> keys{"6","7","8","9","10"};
            for(size_t i=0;i<result.size();++i) {
                const auto found=surfaces.find(keys[i]);if(found!=surfaces.end())result[i].json=&*found;
            }
        }
        return result;
    }
    template<class Visit>void Geometry(bool up,Visit&& visit)const {
        if(typed)for(const auto& g:up?typed->up_geometry:typed->geometry)visit(DrawGeometryInput{&g});
        else for(const auto& g:json->at(up?"up_geometry":"geometry"))visit(DrawGeometryInput{nullptr,&g});
    }
    template<class Visit>void VertexSlots(Visit&& visit)const {
        if(typed)for(auto slot:typed->vertex_fetch_slots)visit(slot);
        else for(unsigned slot:json->at("vertex_slots"))visit(slot);
    }
    template<class Visit>void Textures(Visit&& visit)const {
        if(typed)for(const auto& t:typed->textures)visit(DrawTextureInput{&t});
        else for(const auto& t:json->at("textures"))visit(DrawTextureInput{nullptr,&t});
    }
};
template<class String>std::optional<std::string_view> DigestKeyView(const std::optional<String>& digest) {
    if(!digest)return std::nullopt;
    return std::string_view(*digest);
}
std::optional<std::string> OwnDigest(const std::optional<std::string_view>& digest) {
    if(!digest)return std::nullopt;
    return std::string(*digest);
}
using PipelineRasterKey=std::variant<std::array<uint32_t,6>,std::array<uint32_t,kDrawStateWords>>;
template<class String>struct PipelineKey {
    std::optional<String> vertex,pixel;
    PipelineRasterKey raster;
    int32_t color_format,depth_format;
    NativeShaderVariant pixel_variant=NativeShaderVariant::Unmodified;
    NativeShaderVariant vertex_variant=NativeShaderVariant::Unmodified;
    std::optional<NativeFsiStateSpecialization> fsi_state;
};
// Same order as the variant's <=> (index, then element-wise words), but skips
// equal 32-byte runs with memcmp: keys sharing shaders often differ only late.
template<size_t N>std::strong_ordering CompareWords(const std::array<uint32_t,N>& a,const std::array<uint32_t,N>& b) {
    size_t i=0;
    for(;i+8<=N;i+=8)if(std::memcmp(a.data()+i,b.data()+i,32))break;
    for(;i<N;++i)if(a[i]!=b[i])return a[i]<=>b[i];
    return std::strong_ordering::equal;
}
std::strong_ordering CompareRaster(const PipelineRasterKey& a,const PipelineRasterKey& b) {
    if(a.index()!=b.index())return a.index()<=>b.index();
    return std::visit([&](const auto& words){
        return CompareWords(words,std::get<std::decay_t<decltype(words)>>(b));},a);
}
struct PipelineLess {
    using is_transparent=void;
    template<class A,class B>bool operator()(const PipelineKey<A>& a,const PipelineKey<B>& b)const {
        if(const auto c=DigestKeyView(a.vertex)<=>DigestKeyView(b.vertex);c!=0)return c<0;
        if(const auto c=DigestKeyView(a.pixel)<=>DigestKeyView(b.pixel);c!=0)return c<0;
        if(const auto c=CompareRaster(a.raster,b.raster);c!=0)return c<0;
        if(a.color_format!=b.color_format)return a.color_format<b.color_format;
        if(a.depth_format!=b.depth_format)return a.depth_format<b.depth_format;
        if(a.pixel_variant!=b.pixel_variant)return a.pixel_variant<b.pixel_variant;
        if(a.vertex_variant!=b.vertex_variant)return a.vertex_variant<b.vertex_variant;
        return a.fsi_state<b.fsi_state;
    }
};

// Lookaside slot hint only: PipelineEqual (the exact ordered-key relation)
// authorizes every hit.
struct PipelineIndex {
    static uint64_t Digest(const std::optional<std::string_view>& digest) {
        if(!digest||digest->size()<16)return digest?std::hash<std::string_view>{}(*digest):0;
        uint64_t first,second;
        std::memcpy(&first,digest->data(),8);std::memcpy(&second,digest->data()+8,8);
        return first^(second<<1|second>>63);
    }
    template<class String>size_t operator()(const PipelineKey<String>& key)const {
        const uint64_t raster=std::visit([](const auto& words){
            return XXH3_64bits(words.data(),sizeof(words));},key.raster);
        uint64_t hash=Digest(DigestKeyView(key.vertex))*0x9E3779B97F4A7C15ull^Digest(DigestKeyView(key.pixel))^raster;
        hash^=(uint64_t(uint32_t(key.color_format))<<32|uint32_t(key.depth_format))*0xC2B2AE3D27D4EB4Full;
        hash^=(uint64_t(key.pixel_variant)<<8|uint64_t(key.vertex_variant)|uint64_t(key.raster.index())<<16)*0x165667B19E3779F9ull;
        if(key.fsi_state)hash^=XXH3_64bits(key.fsi_state->words.data(),sizeof(key.fsi_state->words));
        return size_t(hash^(hash>>31));
    }
};
struct PipelineEqual {
    template<class A,class B>bool operator()(const PipelineKey<A>& a,const PipelineKey<B>& b)const {
        return a.color_format==b.color_format&&a.depth_format==b.depth_format&&
            a.pixel_variant==b.pixel_variant&&a.vertex_variant==b.vertex_variant&&
            DigestKeyView(a.vertex)==DigestKeyView(b.vertex)&&DigestKeyView(a.pixel)==DigestKeyView(b.pixel)&&
            CompareRaster(a.raster,b.raster)==0&&a.fsi_state==b.fsi_state;
    }
};

// Lookaside slot hint only; CpuImageEqual authorizes every hit.
struct CpuImageIndex {
    template<class String>size_t operator()(const CpuImageKey<String>& key)const {
        uint64_t hash=key.version*0x9E3779B97F4A7C15ull;
        auto mix=[&](uint64_t value){hash=(hash^value)*0x100000001B3ull;};
        for(const auto word:key.fetch)mix(word);
        mix(PipelineIndex::Digest(CpuImageDigestView(key.base)));
        mix(PipelineIndex::Digest(CpuImageDigestView(key.mips)));
        return size_t(hash^(hash>>31));
    }
};

// Vertex buffers are keyed by 64-character content digests. Slot hint only;
// exact string equality authorizes every hit.
struct VertexDigestIndex {
    size_t operator()(std::string_view digest)const {
        if(digest.size()<16)return std::hash<std::string_view>{}(digest);
        uint64_t first,second;
        std::memcpy(&first,digest.data(),8);std::memcpy(&second,digest.data()+8,8);
        return size_t(((first^(second<<1|second>>63))*0x9E3779B97F4A7C15ull)>>40);
    }
};
struct VertexDigestEqual {
    bool operator()(std::string_view a,std::string_view b)const {return a==b;}
};

// Hardware render targets: identity of every decoded pipeline field that
// Plume bakes into a VkPipeline. Viewport, scissor and depth-bias values are
// dynamic state; alpha reference, half-pixel and index offsets are not
// pipeline state. Keying raw register words built duplicate pipelines for
// each of those and omitted the draw's primitive topology.
std::array<uint32_t,kDrawStateWords> HardwarePipelineRasterKey(const DrawState& state,bool rectangle) {
    const auto& p=state.pipeline;
    std::array<uint32_t,kDrawStateWords> key{};size_t n=0;
    auto put=[&](auto value){key.at(n++)=uint32_t(value);};
    put(p.primitiveTopology);put(p.cullMode);put(p.frontFace);put(p.depthClipEnabled);put(p.dynamicDepthBiasEnabled);
    put(p.depthBias);put(std::bit_cast<uint32_t>(p.depthBiasClamp));put(std::bit_cast<uint32_t>(p.slopeScaledDepthBias));
    put(p.depthEnabled);put(p.depthWriteEnabled);put(p.depthFunction);
    put(p.stencilEnabled);put(p.stencilReadMask);put(p.stencilWriteMask);put(p.stencilReference);
    for(const auto* face:{&p.stencilFrontFace,&p.stencilBackFace}) {
        put(face->failOp);put(face->depthFailOp);put(face->passOp);put(face->compareFunction);
    }
    put(p.alphaToCoverageEnabled);put(p.logicOpEnabled);put(p.logicOp);
    for(const auto& blend:p.renderTargetBlend) {
        put(blend.blendEnabled);put(blend.srcBlend);put(blend.dstBlend);put(blend.blendOp);
        put(blend.srcBlendAlpha);put(blend.dstBlendAlpha);put(blend.blendOpAlpha);put(blend.renderTargetWriteMask);
    }
    put(state.specialization);put(rectangle);
    return key;
}
using PipelineIdentity=PipelineKey<std::string>;
using PipelineLookup=PipelineKey<std::string_view>;
inline uint32_t LoadUnalignedU32(const uint8_t* data) noexcept {
    uint32_t value;std::memcpy(&value,data,sizeof(value));return value;
}
uint32_t BE(std::span<const uint8_t> data,size_t offset) {
    if(offset+4>data.size()) throw std::runtime_error("Truncated captured word");
    return uint32_t(data[offset])<<24|uint32_t(data[offset+1])<<16|uint32_t(data[offset+2])<<8|data[offset+3];
}
RenderRect Rectangle(const Json& v) {return {v.at(0),v.at(1),v.at(2),v.at(3)};}
std::string SurfaceKey(const Json& s) {
    return Json::array({s.at("depth"),s.at("edram_base"),s.at("format"),s.at("surface_info"),s.at("width"),s.at("height")}).dump();
}
RenderFormat SurfaceFormat(const Json& s,bool half_float_color=false) {
    // Plume exposes float depth. Resolve conversion restores the sampled 24-bit
    // grid; per-fragment depth quantization remains an equivalence limitation.
    if(s.at("depth").get<bool>()) return RenderFormat::D32_FLOAT_S8_UINT;
    switch(s.at("format").get<unsigned>()) {
        case 0: return RenderFormat::R8G8B8A8_UNORM;
        // Hardware render targets store 10:10:10:2 and 7e3 in fp16, which holds
        // every 7e3 value; k_16_16_16_16 fixed point keeps 32-bit floats.
        case 2: case 10: case 3: case 12:
            return half_float_color?RenderFormat::R16G16B16A16_FLOAT:RenderFormat::R32G32B32A32_FLOAT;
        case 5: return RenderFormat::R32G32B32A32_FLOAT;
        case 4: return RenderFormat::R32G32_FLOAT;
        case 7: return RenderFormat::R16G16B16A16_FLOAT;
        case 6: return RenderFormat::R16G16_FLOAT;
        case 14: return RenderFormat::R32_FLOAT;
        case 15: return RenderFormat::R32G32_FLOAT;
        default: throw std::runtime_error("Unsupported native surface format");
    }
}
// Same physical extent and storage layout; filter/swizzle state is separate.
std::string TextureKey(const Json& t) {return t.dump();}
using TextureBindingKey=std::array<uint32_t,8>;
using TextureBindingCache=std::map<TextureBindingKey,TextureBinding>;
struct Image {
    std::unique_ptr<RenderTexture> texture; Json identity;uint64_t version=0;bool dirty=false;std::bitset<2048> tiles;
    // Pre-encode host floats after a fused clear. This is clean read state,
    // never a dirty working image or a source for a packed commit.
    ResolveMemory::AtlasDepthClearReceipt atlas_clear_receipt;
    // Immutable layout metadata, populated only for the optional no-write path.
    std::optional<ResolveMemory::SurfacePageMask> physical_pages;
    // Views must be destroyed before their image, including on cache eviction.
    TextureBindingCache bindings;
};
struct AliasRetention {
    static constexpr uint64_t kMaxBytes=128u*1024*1024,kMaxGenerations=256,kMaxBindings=4096;
    uint64_t bytes=0,generations=0,bindings=0,peak_bytes=0,peak_generations=0,peak_bindings=0;
    uint64_t created=0,reused=0,retired=0,pressure_drains=0;
    bool Fits(uint64_t extra_bytes,size_t extra_bindings,uint64_t limit) const {
        return extra_bytes<=limit&&bytes<=limit-extra_bytes&&generations<kMaxGenerations&&
            extra_bindings<=kMaxBindings&&bindings<=kMaxBindings-extra_bindings;
    }
};
struct AliasGeneration {
    // Members destroy bindings before the image. A draw owns this whole bundle,
    // including sampler variants, until its own (possibly partial) FIFO fence.
    std::shared_ptr<RenderTexture> texture;
    TextureBindingCache bindings;
    uint64_t version=0,bytes=0;
    std::shared_ptr<AliasRetention> stale;
    ~AliasGeneration() {
        if(stale) {
            stale->bytes-=bytes;--stale->generations;stale->bindings-=bindings.size();++stale->retired;
        }
    }
    void RetainStale(const std::shared_ptr<AliasRetention>& budget) {
        stale=budget;budget->bytes+=bytes;++budget->generations;budget->bindings+=bindings.size();
        budget->peak_bytes=std::max(budget->peak_bytes,budget->bytes);
        budget->peak_generations=std::max(budget->peak_generations,budget->generations);
        budget->peak_bindings=std::max(budget->peak_bindings,budget->bindings);
    }
};
struct SampledImage {std::shared_ptr<AliasGeneration> current;};
struct UploadedImage {NativeTexture texture;TextureBindingCache bindings;DormantCacheEntry residency;
    std::array<uint32_t,6> initial_fetch{}; // Diagnostic only, never storage authority.
};
struct CachedVertex {std::unique_ptr<RenderBuffer> buffer;DormantCacheEntry residency;};
uint64_t TextureAllocationBytes(const NativeTexture& texture) {
    const uint64_t image=static_cast<const VulkanTexture&>(*texture.GetImage()).allocationInfo.size;
    const uint64_t staging=texture.staging?static_cast<const VulkanBuffer&>(*texture.staging).allocationInfo.size:0;
    if(staging>UINT64_MAX-image)throw std::runtime_error("Native texture allocation size overflow");
    return image+staging;
}
bool AuditedRetainedFp24Vertex(std::string_view source,std::span<const uint8_t> bytes) {
    const auto index=NativeRetainedFp24DepthSourceIndex(source);
    return index&&bytes.size()==10096&&ContentHash(bytes)==kNativeRetainedFp24DepthSources[*index].spirv;
}
struct CachedShader {std::unique_ptr<RenderShader> shader;uint64_t last_used_epoch=0,spirv_bytes=0;bool audited_atomic_vertex=false,audited_readonly_vertex=false,audited_retained_fp24_vertex=false,audited_early_depth_vertex=false,audited_early_depth_catalog_vertex=false,audited_null_constant_footprint=false,audited_packed_depth_pixel=false,audited_packed_color_module=false;
    bool audited_stencil_material=false;
    bool audited_monotonic_depth=false;
    bool audited_opaque_phase=false;
    bool audited_opaque_phase_canonical=false;
    bool audited_packed_color_blend=false;
    bool audited_packed_color_action=false;
};
struct NativeEarlyDepthGeneration {
    std::shared_ptr<NativeReadonlyDepthGeneration> resources;
    NativeEarlyDepthLowerBoundVersion version;
    explicit NativeEarlyDepthGeneration(std::shared_ptr<NativeReadonlyDepthGeneration> value)
        :resources(std::move(value)),version(resources->version.generation(),resources->version.layout(),resources->version.source()) {}
};
struct FsiStateCoverage {std::atomic<uint64_t>* observed=nullptr;uint64_t candidates=0,selected=0,queried=0,queried_candidates=0;};
struct CachedPipeline {std::unique_ptr<RenderPipeline> pipeline;uint64_t last_used_epoch=0;uint32_t diagnostic_pair=0;bool diagnostic_pair_assigned=false;uint32_t timing_pair=0;bool timing_pair_assigned=false;
    // Non-owning Scene-local coverage node. The bounded std::map never erases
    // these nodes during Scene life; PSO eviction does not erase their history.
    FsiStateCoverage* fsi_coverage=nullptr;
    bool audited_monotonic_depth=false;
    bool audited_opaque_phase=false;
    const RenderShader* opaque_phase_vertex=nullptr;
    const RenderShader* opaque_phase_pixel=nullptr;
    bool audited_packed_color_blend=false;
    bool audited_packed_color_action=false;
};
struct ResidentFsiStateSelection {
    RenderShader* shader=nullptr;CachedPipeline* pipeline=nullptr;
    const PipelineIdentity* identity=nullptr;
    explicit operator bool()const noexcept{return shader&&pipeline&&identity;}
};
struct DrawUpload {
    std::unique_ptr<RenderBuffer> buffer;size_t capacity=0;std::unique_ptr<ConstantDeltaPacker> constant_delta;
    // Constant uploads stay mapped: a map/unmap per draw takes VMA's block lock
    // and may reach the driver. Unmapped before the buffer is replaced or freed.
    uint8_t* mapped=nullptr;
    DrawUpload()=default;
    DrawUpload(DrawUpload&& o)noexcept:buffer(std::move(o.buffer)),capacity(o.capacity),
        constant_delta(std::move(o.constant_delta)),mapped(std::exchange(o.mapped,nullptr)) {}
    DrawUpload& operator=(DrawUpload&& o)noexcept {
        if(this!=&o) {
            Unmap();buffer=std::move(o.buffer);capacity=o.capacity;
            constant_delta=std::move(o.constant_delta);mapped=std::exchange(o.mapped,nullptr);
        }
        return *this;
    }
    ~DrawUpload(){Unmap();}
    void Unmap()noexcept{if(mapped&&buffer)buffer->unmap();mapped=nullptr;}
};
struct DrawSlot {
    uint64_t token=0;size_t accepted_upload_bytes=0;bool queried=false;
    // Occurrences of this slot in pending_draws_ (maintained at push/erase).
    uint32_t pending_occurrences=0;
    std::unique_ptr<RenderCommandList> commands;
    std::array<std::unique_ptr<RenderDescriptorSet>,5> descriptors;
    std::array<uint64_t,4> descriptor_set_generations{};
    std::unique_ptr<NativeDescriptorContentCache> descriptor_contents;
    bool descriptor_contents_allocation_refused=false;
    DrawUpload indices,constants,rectangle;
    ConstantDeltaPacker::Writes constant_delta_writes{};
    bool account_constant_delta=false;
#ifdef GOW3_NATIVE_STENCIL_MATERIAL_REPLAY
    std::unique_ptr<NativeStencilMaterialCounterSlot> stencil_material_counter;
    bool stencil_material_selected=false;
#endif
    bool account_pinned_null_constants=false,pinned_null_constants_selected=false,pinned_null_constants_candidate=false;
    std::unique_ptr<RenderFramebuffer> framebuffer;
    std::array<std::shared_ptr<AliasGeneration>,32> aliases;
    uint32_t alias_count=0;
    std::vector<TextureBinding> transient_bindings;
    std::shared_ptr<NativeReadonlyDepthGeneration> readonly_depth;
    std::shared_ptr<NativeEarlyDepthGeneration> early_depth;
    std::shared_ptr<NativeRetainedOpaqueGeneration> retained_opaque;
    std::shared_ptr<NativeRetainedFp24DepthGeneration> retained_fp24_depth;
    void KeepAlias(const std::shared_ptr<AliasGeneration>& generation) {
        for(uint32_t i=0;i<alias_count;++i)if(aliases[i]==generation)return;
        if(alias_count==aliases.size())throw std::runtime_error("Native draw exceeded texture generation slots");
        aliases[alias_count++]=generation;
    }
    void ReleaseAliases() {while(alias_count)aliases[--alias_count].reset();}
};
using DrawClock=std::chrono::steady_clock;
double DrawMilliseconds(DrawClock::time_point begin,DrawClock::time_point end) {
    return std::chrono::duration<double,std::milli>(end-begin).count();
}
struct CreationTiming {
    double total_ms=0,maximum_ms=0,last_slow_ms=0;
    uint64_t maximum_event=0,last_slow_event=0,slow_creations=0;
    uint32_t maximum_frame=0,last_slow_frame=0;
    void Record(double elapsed,const DrawInput& operation) {
        total_ms+=elapsed;
        if(elapsed>maximum_ms) {
            maximum_ms=elapsed;maximum_frame=operation.Frame();maximum_event=operation.Event();
        }
        if(elapsed>=10) {
            ++slow_creations;last_slow_ms=elapsed;
            last_slow_frame=operation.Frame();last_slow_event=operation.Event();
        }
    }
    Json Report() const {
        return {{"total_ms",total_ms},{"maximum_ms",maximum_ms},{"maximum_frame",maximum_frame},
            {"maximum_event",maximum_event},{"over10ms",slow_creations},{"last_slow_ms",last_slow_ms},
            {"last_slow_frame",last_slow_frame},{"last_slow_event",last_slow_event}};
    }
};
}
struct SceneExecutor::State {
    SceneExecutorConfig config_;
    fs::path capture_,compiled_,output_;
    bool diagnostic_zero_history_=false;
    bool detailed_reports_=false;
    const bool time_cpu_stages_;
    bool transfer_report_dirty_=true;
    uint64_t diagnostic_serializations_=0;
    uint64_t mutation_serial_=0;
    NativeDrawBarrierStatistics draw_barriers_;
    struct ReadonlyQueryStats {uint64_t candidates=0,selected=0,queried_selected=0,prepared=0,prewarm_attempts=0,prewarm_refusals=0,vertex_refusals=0,module_refusals=0,residency_refusals=0;} readonly_query_stats_;
    NativeCatalogShaderHeadroom readonly_query_headroom_;

    struct AtomicDepthStats {uint64_t candidates=0,selected=0,vertex_refusals=0,module_refusals=0;} atomic_depth_stats_;
    bool atomic_builtin_refused_=false;
    bool opaque_program_ready_=false;
    struct OpaqueOverwriteStats {
        uint64_t candidates=0,selected=0,prepared=0,residency_refusals=0,prewarm_refusals=0;
    } opaque_overwrite_stats_;
    bool forward_program_ready_=false;
    struct ForwardControlFlowStats {
        uint64_t candidates=0,selected=0,queried_selected=0,prepared=0,residency_refusals=0,prewarm_refusals=0;
    } forward_control_flow_stats_;
    bool forward_vertex_program_ready_=false;
    struct ForwardVertexStats {
        uint64_t candidates=0,selected=0,queried_selected=0,prepared=0,residency_refusals=0,prewarm_refusals=0;
    } forward_vertex_stats_;
    bool forward_pixel_catalog_ready_=false;
    struct ForwardPixelCatalogStats {uint64_t candidates=0,selected=0,queried_selected=0,prepared=0,residency_refusals=0,prewarm_refusals=0;};
    ForwardPixelCatalogStats forward_pixel_catalog_stats_;
    NativeCatalogShaderHeadroom forward_pixel_catalog_headroom_;
    std::map<std::string,ForwardPixelCatalogStats,std::less<>> forward_pixel_catalog_source_stats_;
    NativeDrawDescriptorStatistics draw_descriptors_;
    struct UnusedDescriptorStatistics {uint64_t draws=0,selected_draws=0,unknown_draws=0,omitted_set_bindings=0,required_set_bindings=0;} unused_descriptors_;
    NativeDescriptorGenerations descriptor_generations_;
    struct DescriptorContentStatistics {
        uint64_t draws=0,compared=0,skipped_images=0,skipped_samplers=0;
        uint64_t image_write_requests=0,sampler_write_requests=0,committed_writes=0,update_calls=0;
        std::array<uint64_t,3> skipped_images_by_set{};
        uint64_t caches=0,allocation_refusals=0,set_identity_resets=0,aborts=0;
        uint64_t generation_refusals=0,invalid_handle_refusals=0;
    } descriptor_contents_;
    struct ConstantDeltaStatistics {uint64_t submitted_draws=0,float_written_bytes=0,shared_written_bytes=0,unchanged_blocks=0;} constant_delta_stats_;
    struct PinnedNullConstantStatistics {uint64_t retired_draws=0,candidates=0,selected=0,refusals=0;} pinned_null_constant_stats_;
    NativeDrawRecordingStatistics draw_recording_;
    struct PackedDepthPassStats {uint64_t candidates=0,eligible=0,selected=0,refusals=0,
        query_refusals=0,timestamp_refusals=0,preparation_refusals=0,groups=0,continued=0,
        maximum_group_draws=0,atomic_selected=0,atomic_continued=0,access_model_boundaries=0;} packed_depth_pass_stats_;
    struct PackedColorPassStats {uint64_t candidates=0,eligible=0,selected=0,refusals=0,
        query_refusals=0,timestamp_refusals=0,preparation_refusals=0,groups=0,continued=0,
        maximum_group_draws=0,sampled_barriers_avoided=0,
        variant8_candidates=0,variant8_eligible=0,variant8_selected=0,variant8_continued=0,
        variant8_module_refusals=0,variant8_pipeline_refusals=0;} packed_color_pass_stats_;
    struct PackedColorBlendPassStats {uint64_t candidates=0,eligible=0,selected=0,refusals=0,
        query_refusals=0,timestamp_refusals=0,preparation_refusals=0,continued=0,
        module_refusals=0,pipeline_refusals=0,alpha_selected=0,additive_selected=0,
        old_to_blend_continued=0,blend_to_old_continued=0;} packed_color_blend_pass_stats_;
    struct PackedColorActionPassStats {uint64_t candidates=0,eligible=0,selected=0,refusals=0,
        query_refusals=0,timestamp_refusals=0,preparation_refusals=0,continued=0,
        module_refusals=0,pipeline_refusals=0;} packed_color_action_pass_stats_;
    bool last_packed_color_child_blend_=false,last_packed_color_child_variant8_=false;
    struct RetainedPhasePassStats {uint64_t eligible=0,selected=0,continued=0,groups=0,
        preparation_refusals=0,timestamp_refusals=0,generation_boundaries=0,attachment_state_boundaries=0,
        maximum_group_draws=0,sampled_barriers_avoided=0;} retained_phase_pass_stats_;
    std::weak_ptr<NativeRetainedOpaqueGeneration> open_retained_phase_owner_;
    NativeRetainedOpaquePhasePassIdentity open_retained_phase_identity_{};
    bool open_retained_phase_pass_=false;
    uint32_t retained_phase_pass_draws_=0;
    bool open_packed_color_pass_=false;
    uint32_t packed_color_pass_draws_=0;
    // General FSI draw pass groups (fsi_draw_pass_groups.h).
    struct FsiDrawPassStats {uint64_t candidates=0,shape_refusals=0,plane_refusals=0,eligible=0,selected=0,
        continued=0,boundaries=0,groups=0,maximum_group_draws=0,sampled_barriers_avoided=0,queried_selected=0,query_controls_preserved=0;} fsi_draw_pass_stats_;
    bool open_fsi_draw_pass_=false;
    uint64_t async_shader_skipped_draws_=0;
    uint32_t fsi_draw_pass_draws_=0;
    NativeFsiPassPlanes open_fsi_draw_planes_{};
    bool open_packed_depth_pass_=false,open_packed_depth_atomic_=false;
    uint32_t packed_depth_pass_draws_=0;
    struct ReadonlyQueryPassStats {uint64_t eligible=0,selected=0,continued=0,groups=0,
        preparation_refusals=0,query_controls_preserved=0,maximum_group_draws=0;} readonly_query_pass_stats_;
    std::weak_ptr<NativeReadonlyDepthGeneration> open_readonly_query_owner_;
    bool open_readonly_query_pass_=false;
    uint32_t readonly_query_pass_draws_=0;
    bool DrawGroupsEnabled()const noexcept {
        return config_.multi_draw_command_buffers||config_.packed_depth_render_pass_groups||
            config_.packed_color_render_pass_groups||config_.readonly_query_render_pass_groups||
            config_.retained_opaque_phase_render_pass_groups||config_.fsi_draw_pass_groups;
    }
    std::unique_ptr<RenderInterface> interface_;
    std::unique_ptr<RenderDevice> owned_device_;
    std::unique_ptr<FsiDevice> owned_fsi_device_;
    std::unique_ptr<RenderCommandQueue> owned_queue_;
    RenderDevice* device_=nullptr;
    RenderCommandQueue* queue_=nullptr;
    // GPU resources must die before the owned queue/device (reverse member order).
    std::unique_ptr<NativeReadonlyDepthConversion> readonly_depth_conversion_;
    uint64_t readonly_depth_read_barriers_avoided_=0;
    std::shared_ptr<NativeReadonlyDepthGeneration> readonly_depth_current_;
    std::weak_ptr<NativeReadonlyDepthGeneration> readonly_query_read_owner_;
    uint64_t readonly_query_read_mutation_=0,readonly_query_read_physical_=0;
    uint64_t readonly_query_read_barriers_=0,readonly_query_read_reuses_=0;
    uint64_t readonly_depth_generation_=0;
    struct ReadonlyNoWriteStats {uint64_t prepared=0,selected=0,refusals=0,host_invalidations=0,receipt_invalidations=0;} readonly_no_write_stats_;
    std::optional<NativeReadonlyDepthLayoutKey> readonly_no_write_layout_;
    ResolveMemory::SurfacePageMask readonly_no_write_pages_;
    struct ReadonlyDepthConservativeStats {uint64_t prepared=0,selected=0,module_refusals=0,residency_refusals=0;} readonly_depth_conservative_stats_;
    std::unique_ptr<NativeReadonlyDepthConversion> early_depth_conversion_;
    std::shared_ptr<NativeEarlyDepthGeneration> early_depth_current_;
    std::shared_ptr<const NativeEarlyDepthProgram> early_depth_program_;
    uint64_t early_depth_generation_=0,early_depth_run_=0;
    struct EarlyDepthStats {uint64_t prepared=0,modules=0,module_bytes=0,candidates=0,selected=0,queried_selected=0,
        source_refusals=0,residency_refusals=0,prewarm_refusals=0,resource_refusals=0,cache_hits=0,cache_misses=0,
        promotions=0,invalidations=0,maximum_run=0;} early_depth_stats_,early_depth_catalog_stats_;
    struct EarlyDepthBoundaryStatistics {uint64_t checks=0,retained=0,token_refusals=0,
        check_errors=0,failed_boundaries=0;} early_depth_boundary_stats_;
    struct EarlyDepthMaterialStatistics {uint64_t candidates=0,admitted=0,selected=0,
        query_refusals=0,module_refusals=0,state_refusals=0,layout_refusals=0,
        draw_refusals=0,token_refusals=0,late_refusals=0;} early_depth_material_stats_;
    struct EarlyDepthMaterialRefusal {
        std::string vertex,pixel;
        NativeShaderVariant vertex_variant{},pixel_variant{};
        std::array<bool,9> certificates{};
        std::array<uint32_t,15> state{};
        uint64_t first_event=0,last_event=0,count=0;
        uint32_t first_frame=0,last_frame=0;
    };
    // Optional bounded provenance only. Never consulted for draw admission,
    // cache ownership, invalidation or command recording.
    std::vector<EarlyDepthMaterialRefusal> early_depth_material_refusals_;
    uint64_t early_depth_material_refusal_overflow_=0,early_depth_material_refusal_errors_=0;
    std::map<std::string,std::shared_ptr<const NativeEarlyDepthProgram>,std::less<>> early_depth_catalog_programs_;
    std::array<uint64_t,2> early_depth_catalog_source_selected_{};
    uint64_t early_depth_catalog_shader_switches_=0;
    uint32_t early_depth_last_source_=0;
    static constexpr uint64_t kEarlyDepthCatalogMaximumModuleBytes=256u*1024;

    std::unique_ptr<NativeRetainedFp24DepthWriter> retained_fp24_depth_writer_;
    std::shared_ptr<NativeRetainedFp24DepthGeneration> retained_fp24_depth_current_;
    uint64_t retained_fp24_depth_generation_=0,retained_fp24_depth_run_=0;
    uint32_t retained_fp24_depth_last_source_=UINT32_MAX;
    NativeCatalogShaderHeadroom retained_fp24_depth_headroom_;
    struct RetainedFp24DepthStats {uint64_t candidates=0,selected=0,queried_selected=0,reuses=0,restores=0,materializations=0,maximum_run=0,shader_switches=0,prepared=0,prewarm_attempts=0,prewarm_refusals=0,source_refusals=0,resource_refusals=0,residency_refusals=0;} retained_fp24_depth_stats_;
    std::array<uint64_t,2> retained_fp24_depth_source_selected_{};
    std::unique_ptr<NativeRetainedOpaqueWriter> retained_opaque_writer_;
    std::shared_ptr<NativeRetainedOpaqueGeneration> retained_opaque_current_;
    uint64_t retained_opaque_generation_=0,retained_opaque_run_=0;
    NativeCatalogShaderHeadroom retained_opaque_headroom_; // At most one admitted V1 module.
    struct RetainedOpaqueStats {uint64_t prepared=0,candidates=0,selected=0,queried_selected=0,reuses=0,restores=0,materializations=0,maximum_run=0,source_refusals=0,resource_refusals=0,residency_refusals=0,prewarm_refusals=0,modules_created=0,module_bytes=0,shader_switches=0,queued_draw_materializations=0,queued_draw_pending=0;} retained_opaque_stats_;
    struct RetainedOpaquePairStats {uint64_t candidates=0,selected=0,queried_selected=0;};
    std::array<RetainedOpaquePairStats,kRetainedOpaqueCatalogMaxPrograms> retained_opaque_pair_stats_{};
    uint32_t retained_opaque_last_pair_=UINT32_MAX;
    NativeRetainedOpaqueCatalogHeadroom retained_opaque_catalog_headroom_;
    NativeRetainedOpaquePhaseHeadroom retained_opaque_phase_headroom_;
    std::array<uint64_t,65> retained_opaque_phase_selected_{};
    uint64_t phase_primary_write_receipts_=0,phase_primary_write_receipt_fallbacks_=0;
    uint64_t retained_opaque_last_pair_bytes_=0;
    struct RetainedOpaquePressureStats {
        uint64_t allocation_checks=0,polls=0,fence_retirements=0,retired_image_bytes=0;
        uint64_t unchanged_byte_polls=0,admitted_after_poll=0,refused_after_poll=0;
    } retained_opaque_pressure_stats_;
    struct SharedDepthQueryStats {
        uint64_t prepared=0,candidates=0,selected=0,queried_selected=0,promotions=0,
            residency_refusals=0,framebuffer_refusals=0,prewarm_refusals=0,module_bytes=0,
            preparation_calls=0,retained_preparations=0,strong_fallbacks=0,
            accepted_begins=0,accepted_ends=0,last_end_required_work=0,last_end_completed_work=0;
    } shared_depth_query_stats_;
    struct DisjointQueryStats {
        uint64_t preparation_calls=0,retained_preparations=0,accepted_draws=0,
            opaque_preserved_draws=0,depth_preserved_draws=0,late_fallbacks=0,
            query_preparation_calls=0,retained_query_preparations=0,query_strong_fallbacks=0,
            accepted_begins=0,accepted_ends=0,last_end_required_work=0,last_end_completed_work=0,
            full_query_completions=0,dirty_query_completions=0;
    } disjoint_query_stats_;
    struct ReadonlyDepthStats {uint64_t prepared=0,candidates=0,selected=0,queried_selected=0,cache_hits=0,cache_misses=0,
        source_refusals=0,residency_refusals=0,prewarm_refusals=0,resource_refusals=0,promotions=0,promotion_refusals=0,
        skipped_edram_barriers=0,retained_edram_barriers=0;} readonly_depth_stats_;
    std::unique_ptr<RenderCommandList> commands_;
    std::unique_ptr<RenderCommandFence> fence_;
    std::unique_ptr<NativeQueryTracker> queries_;
    TextureLayoutCache texture_layout_cache_{config_.associative_texture_layouts};
    struct DeferredQuery {
        CompletedNativeQuery query;
        uint64_t required_work=0;
        // Diagnostic all-work upper bound only; actual publication still uses
        // the original required_work and original prefix-mode policy.
        uint64_t diagnostic_required_work=0;
        bool diagnostic_frontier_known=false;
    };
    std::vector<DeferredQuery> deferred_queries_;
    uint64_t query_prefix_completions_=0,query_prefix_while_pending_=0,query_prefix_pressure_=0;
    size_t ready_query_count_=0;
    // Actual completion/publication uses typed results. Keep the same bounded
    // diagnostic history without expanding ten JSON fields for every query.
    std::vector<CompletedNativeQuery> query_history_;
    bool query_report_dirty_=false;
    uint64_t query_report_serializations_=0;
    std::unique_ptr<NativeGpuWorkQueue> gpu_work_queue_;
    // Destroyed after the public Scene destructor drains all accepted work.
    std::unique_ptr<NativeDrawTimestamps> draw_timestamps_;
#ifdef GOW3_NATIVE_FRAGMENT_CENSUS_REPLAY
    std::unique_ptr<NativeFragmentInvocationCensus> fragment_census_;
#endif
    std::unique_ptr<NativeTransferBatch> transfer_batch_;
    std::unique_ptr<PassTransfer> transfers_;
    std::unique_ptr<NativeFusedPackedAtlasResolve> fused_packed_atlas_resolve_;
    std::unique_ptr<NativeSynchronousGpuTimings> presentation_gpu_timings_;
    std::map<std::string,Image> surfaces_,resolved_;
    // False only when every surfaces_ image is clean; lets per-draw commit scans skip the map.
    bool surfaces_maybe_dirty_=false;
    // Working-image <-> packed EDRAM conversions (hardware render targets).
    struct SurfaceConversionStats {uint64_t commits=0,commit_pixels=0,restores=0,restore_pixels=0,first_restores=0;
        std::map<std::string,uint64_t> restore_keys,commit_keys;} surface_conversions_;
    struct HardwarePassStats {uint64_t started=0,continued=0;} hardware_pass_stats_;
    // Hardware render targets: framebuffers by (color, depth) working image.
    // Declared after surfaces_ so they are destroyed before their images.
    std::map<std::pair<const RenderTexture*,const RenderTexture*>,std::unique_ptr<RenderFramebuffer>> hardware_framebuffers_;
    static constexpr size_t kMaxRetainedHardwareSurfaces=64;
    struct CanonicalResolveReceipt {
        ResolveMemory::CanonicalStoreReceipt receipt;
        uint64_t bytes=0;
    };
    std::map<uint32_t,CanonicalResolveReceipt> canonical_resolve_receipts_;
    static constexpr uint64_t kCanonicalResolveReceiptBytes=64u*1024*1024;
    static constexpr size_t kCanonicalResolveReceiptCount=128;
    struct CanonicalResolveStatistics {
        bool configured=false;
        uint64_t stores=0,adopted=0,discarded=0,budget_refusals=0,
                 receipt_bytes=0,peak_receipt_bytes=0;
    } canonical_resolve_stats_;
    std::map<std::string,SampledImage> sampled_resolves_;
    SampledAliasIndex<decltype(sampled_resolves_)> sampled_alias_index_{sampled_resolves_};
    std::shared_ptr<AliasRetention> alias_retention_=std::make_shared<AliasRetention>();
    std::unique_ptr<ResolveMemory> resolve_memory_,edram_memory_;
    std::unique_ptr<NativeTextureProbe> texture_probe_;
    LookasideMap<CpuImageIdentity,UploadedImage,CpuImageLess,CpuImageIndex,CpuImageEqual> textures_{config_.cpu_image_lookup_lookaside};
    using CpuImageSlots=BorrowedSlotLookup<decltype(textures_),CpuImageEqual>;
    CpuImageSlots cpu_image_slots_{textures_,config_.cpu_image_slot_lookup};
    struct CpuTextureDrainStats {uint64_t candidates=0,pure_cpu=0,pending_pure_cpu=0,overlays=0;} cpu_texture_drains_;
    struct CpuTextureSamplingStats {uint64_t lookups=0,image_hits=0,image_creations=0,variant_image_hits=0,
        reserved_binding_refusals=0,zero_bias_cached=0;} cpu_texture_sampling_;
    size_t cached_texture_bindings_=0;
    uint64_t texture_binding_hits_=0,texture_binding_misses_=0,texture_binding_transient_=0;
    LookasideMap<std::string,CachedVertex,std::less<>,VertexDigestIndex,VertexDigestEqual> vertex_buffers_{config_.vertex_lookup_lookaside};
    std::unique_ptr<RetiredBufferPool<RenderBuffer>> vertex_buffer_pool_;
    uint64_t vertex_buffer_allocations_=0,vertex_buffer_reuses_=0;
    DormantCache texture_cache_{{config_.max_dormant_texture_bytes,config_.max_dormant_texture_entries,config_.max_dormant_texture_epochs}};
    std::unique_ptr<NativeTextureEvictionReloadCensus> texture_eviction_reload_census_;
    uint32_t texture_census_frame_=0;bool texture_census_frame_valid_=false;
    DormantCache vertex_cache_{{config_.max_dormant_vertex_bytes,config_.max_dormant_vertex_entries,config_.max_dormant_cache_epochs}};
    IndexGeometryCache<std::unique_ptr<RenderBuffer>> index_cache_;
    // One bounded standalone-fixture mutation. Declared before cache owners so
    // displaced modules/PSOs outlive every cached certificate but not the device.
    std::unique_ptr<RenderShader> test_retained_phase_old_shader_;
    std::unique_ptr<RenderShader> test_packed_color_old_shader_;
    std::unique_ptr<RenderPipeline> test_retained_phase_old_pipeline_;
    ShaderLookupCache<CachedShader> shaders_{config_.shader_lookup_lookaside};
    LookasideMap<PipelineIdentity,CachedPipeline,PipelineLess,PipelineIndex,PipelineEqual> pipelines_{config_.pipeline_lookup_lookaside};
    PersistentCacheStatistics shader_cache_stats_,pipeline_cache_stats_;
    CreationTiming shader_input_timing_,shader_module_timing_,pipeline_creation_timing_;
    std::map<std::string,std::vector<uint8_t>> debug_pixel_values_;
    std::set<std::string> frame_surfaces_,frame_resolved_,frame_sampled_;
    CacheFrameEpoch cache_epoch_;
    std::unique_ptr<RenderPipelineLayout> layout_;
    std::unique_ptr<RenderTexture> fsi_attachment_;
    std::unique_ptr<RenderFramebuffer> fsi_framebuffer_;
    uint64_t fsi_attachment_creations_=0,fsi_attachment_extent_reuses_=0;
    uint32_t fsi_raster_width_=0,fsi_raster_height_=0;
    bool attachmentless_fsi_supported_=false,attachmentless_fsi_active_=false,attachmentless_fsi_refused_=false;
    uint64_t attachmentless_fsi_attempts_=0,attachmentless_fsi_installs_=0,attachmentless_fsi_refusals_=0;
    uint64_t attachmentless_fsi_draws_=0,attachmentless_fsi_queried_=0,attachmentless_fsi_no_pixel_=0,attachmentless_fsi_discarded_=0;
    uint64_t attachmentless_fsi_barriers_=0,attachmentless_fsi_prewarm_attempts_=0;
    uint64_t attachmentless_fsi_prewarm_prepared_=0,attachmentless_fsi_prewarm_refusals_=0;
    struct FsiHoistStats {uint64_t selected=0,queried=0,incoming_depth_state=0;} fsi_hoist_stats_;
    struct ExactFsiStoresStats {
        uint64_t selected=0,queried=0,depth_identity_candidates=0,depth_identity_certified=0,
            depth_identity_uncertified=0,color_overwrite_draws=0,color_overwrite_targets=0;
    } exact_fsi_stores_stats_;
    bool discard_dummy_fsi_installed_=false;
    uint64_t discard_dummy_fsi_attempts_=0,discard_dummy_fsi_installs_=0,discard_dummy_fsi_refusals_=0;
    uint64_t discard_dummy_fsi_draws_=0,discard_dummy_fsi_queried_=0,discard_dummy_fsi_no_pixel_=0,discard_dummy_fsi_discarded_=0;
    VkResult discard_dummy_fsi_last_result_=VK_SUCCESS;
    RenderDescriptorRange images_{RenderDescriptorRangeType::TEXTURE,0,32},samplers_{RenderDescriptorRangeType::SAMPLER,0,32};
    RenderDescriptorRange edram_{RenderDescriptorRangeType::READ_WRITE_BYTE_ADDRESS_BUFFER,0,1};
    std::array<RenderDescriptorSetDesc,5> descriptor_descs_{{{&images_,1,true,32},{&images_,1,true,32},{&images_,1,true,32},{&samplers_,1,true,32},{&edram_,1}}};
#ifdef GOW3_NATIVE_STENCIL_MATERIAL_REPLAY
    std::array<RenderDescriptorRange,2> stencil_material_ranges_{{
        {RenderDescriptorRangeType::READ_WRITE_BYTE_ADDRESS_BUFFER,0,1},
        {RenderDescriptorRangeType::READ_WRITE_BYTE_ADDRESS_BUFFER,1,1}}};
#endif
    struct StencilMaterialStats {
        uint64_t prepared=0,candidates=0,selected=0,refusals=0,query_refusals=0,budget_refusals=0,accepted=0,retired=0;
#ifdef GOW3_NATIVE_STENCIL_MATERIAL_REPLAY
        Json samples;
#endif
        std::map<std::string,uint64_t> refusal_reasons;
    } stencil_material_stats_;
    std::array<DrawSlot,NativeSubmissionCapacity::kMaximum> draw_slots_;
    NativeSubmissionCapacity submission_capacity_;
    uint32_t next_draw_slot_=0;
    uint64_t next_draw_token_=0,last_retired_gpu_submission_=0;
    std::vector<DrawSlot*> pending_draws_;
    RenderCommandList* open_draw_command_group_=nullptr;
    bool draw_group_recording_=false,draw_group_recording_failed_=false;
    uint64_t group_preparation_standalone_=0,group_readonly_deferred_=0,group_readonly_changed_=0;
    size_t pending_upload_bytes_=0;
    uint64_t pending_last_event_=0;
    uint64_t batch_submissions_=0,batch_draws_=0,batch_peak_=0;
    bool batch_failed_=false;
    uint64_t draw_upload_allocations_=0,draw_upload_reuses_=0,draw_descriptor_allocations_=0;
    struct TextureTiming {
        uint64_t uploads=0,upload_bytes=0,overlay_bytes=0,alias_loads=0;
        uint64_t overlay_regions=0,overlay_readback_bytes=0;
        double version=0,payload=0,overlay=0,decode=0,upload=0,alias_load=0,binding=0;
    } texture_timing_;
    struct FullCpuOverlayDiagnostic {
        static constexpr size_t kMaximumLayouts=64;
        uint64_t misses=0,empty_footprints=0,partial_overlays=0,full_overlays=0,
            storage_layout_supported=0,format_decoder_supported=0,
            load_shared_supported=0,layout_record_refusals=0,record_errors=0;
        size_t layouts=0;
        std::array<std::array<uint32_t,6>,kMaximumLayouts> fetches{};
        Json records=Json::array();
    } full_cpu_overlay_diagnostic_;
    struct DrawTiming {uint64_t count=0;double setup=0,textures=0,submit_wait=0,finish=0;} draw_timing_;
    struct RetainedPhasePredicateTiming {uint64_t calls=0;double elapsed_ms=0;} retained_phase_predicate_timing_;
    struct RetainedPhaseBookkeepingTiming {
        RetainedPhasePredicateTiming inclusive;
        std::array<RetainedPhasePredicateTiming,4> surface_versions{};
    } retained_phase_bookkeeping_timing_;
    uint64_t retained_phase_timing_event_=0;
    uint32_t retained_phase_timing_frame_=UINT32_MAX;
    // Diagnostic operation identity only. Nested contexts restore their parent;
    // an unidentifiable operation clears it rather than inheriting stale labels.
    // External strong reads outside an operation retain the unknown frame.
    struct RetainedPhaseTimingContext {
        State& state;uint64_t previous_event;uint32_t previous_frame;
        explicit RetainedPhaseTimingContext(State& value):state(value),
            previous_event(value.retained_phase_timing_event_),previous_frame(value.retained_phase_timing_frame_) {
            state.retained_phase_timing_event_=0;state.retained_phase_timing_frame_=UINT32_MAX;
        }
        RetainedPhaseTimingContext(State& value,const DrawInput& o):RetainedPhaseTimingContext(value) {
            try {const auto event=o.Event();const auto frame=o.Frame();
                state.retained_phase_timing_event_=event;state.retained_phase_timing_frame_=frame;}catch(...) {}
        }
        RetainedPhaseTimingContext(State& value,uint64_t event,uint32_t frame):RetainedPhaseTimingContext(value) {
            state.retained_phase_timing_event_=event;state.retained_phase_timing_frame_=frame;
        }
        RetainedPhaseTimingContext(State& value,const Json& o):RetainedPhaseTimingContext(value) {
            try {const auto event=o.at("event").get<uint64_t>();const auto frame=o.at("frame").get<uint32_t>();
                state.retained_phase_timing_event_=event;state.retained_phase_timing_frame_=frame;}catch(...) {}
        }
        ~RetainedPhaseTimingContext() {
            state.retained_phase_timing_event_=previous_event;state.retained_phase_timing_frame_=previous_frame;
        }
    };
    bool RetainedPhaseGpuTimingEnabled()const {
        return config_.retained_opaque_phase&&(config_.gpu_batch_attribution||config_.gpu_physical_timestamps);
    }
    std::optional<NativeGpuWorkTag> RetainedPhaseTransferTag(NativeGpuWorkKind kind,uint64_t event=0)const {
        if(!RetainedPhaseGpuTimingEnabled())return {};
        NativeGpuWorkTag tag;tag.kind=kind;tag.frame=retained_phase_timing_frame_;
        tag.event=tag.frame==UINT32_MAX?event:retained_phase_timing_event_;
        tag.width=1280;tag.height=720;tag.queried=queries_->active();return tag;
    }
    // Routine diagnostics accumulate without map lookups or JSON tree rebuilds.
    // Scalar JSON event/frame values preserve offline input spelling and types;
    // ordinary unsigned live values do not allocate. Observation owns the only
    // serialization of these counters and completed-frame snapshots.
    struct PackedZeroClearStats {uint64_t considered=0,selected=0,fallback=0;} packed_zero_clear_stats_;
    struct PackedPrimaryStencilClearStats {
        bool configured=false;
        uint64_t candidates=0,selected=0,query_refusals=0,current_host_refusals=0,
            provenance_refusals=0,resource_refusals=0,source_pixels=0,clear_pixels=0;
    } packed_primary_stencil_clear_stats_;
    struct PackedSecondaryStencilClearStats {
        bool configured=false;
        uint64_t candidates=0,selected=0,query_refusals=0,current_host_refusals=0,
            provenance_refusals=0,resource_refusals=0,source_pixels=0,clear_pixels=0;
    } packed_secondary_stencil_clear_stats_;
    struct AtlasDepthRegionStats {
        uint64_t requests=0,selected=0,refusals=0,partial_restores=0,
            restored_pixels=0,full_extent_pixels=0,current_source_reads=0;
    } atlas_depth_region_stats_;
    struct FusedAtlasDepthClearStats {
        bool configured=false;
        uint64_t candidates=0,selected=0,provenance_refusals=0,resource_refusals=0,
            packed_sources=0,host_sources=0,source_pixels=0,clear_pixels=0,
            deferred_host_materializations=0,deferred_host_discards=0,peak_receipt_bytes=0;
    } fused_atlas_depth_clear_stats_;
    struct DirectAtlasDepthResolveStats {
        bool configured=false;
        uint64_t candidates=0,selected=0,packed_sources=0,receipt_sources=0,
            current_host_refusals=0,provenance_refusals=0,resource_refusals=0,
            layout_refusals=0,query_refusals=0,source_pixels=0,packed_decode_pixels=0,resolve_pixels=0;
    } direct_atlas_depth_resolve_stats_;
    struct FusedPackedAtlasResolveStats {
        bool configured=false;
        uint64_t candidates=0,selected=0,receipt_refusals=0,current_host_refusals=0,query_refusals=0,
            layout_refusals=0,provenance_refusals=0,resource_refusals=0,source_pixels=0,resolve_pixels=0;
    } fused_packed_atlas_resolve_stats_;
    struct DirectPackedColorResolveStats {
        uint64_t candidates=0,selected=0,packed_sources=0,query_refusals=0,
            current_host_refusals=0,provenance_refusals=0,source_pixels=0,resolve_pixels=0;
    } direct_packed_color_resolve_stats_;
    struct FullColorClearRestoreStats {
        uint64_t requests=0,full_overwrites=0,skipped_restores=0,skipped_restore_pixels=0;
    } full_color_clear_restore_stats_;
    struct DiagnosticCounters {
        unsigned draws=0,clears=0,resolves=0,gpu_texture_reads=0;
        std::optional<uint64_t> pipelines_created,direct_fixed_source_resolves;
        std::optional<int32_t> anisotropy;
        std::optional<Json> last_accepted_event,last_completed_event;
        std::vector<Json> completed_frames;
    } diagnostic_counters_;
    struct IndexSnapshot {
        IndexGeometryCache<std::unique_ptr<RenderBuffer>>::Statistics statistics;
        size_t resident=0,retained_bytes=0,index_bytes=0;
    };
    struct BindingSnapshot {uint64_t hits=0,created=0,transient=0;size_t resident=0;};
    struct BatchSnapshot {uint64_t submissions=0,draws=0,largest=0;};
    struct TextureSnapshot {Json frame;TextureTiming timing;double other=0;};
    struct ResourceSnapshot {
        uint64_t descriptors=0,allocations=0,reuses=0;
        size_t index_capacity=0,constant_capacity=0,rectangle_capacity=0;
    };
    struct DrawSnapshot {Json frame;DrawTiming timing;};
    // Capture each field at its original EndFrame assignment point. A later
    // retirement error must not erase an already-completed diagnostic stage.
    std::optional<IndexSnapshot> index_snapshot_;
    std::optional<BindingSnapshot> binding_snapshot_;
    std::optional<BatchSnapshot> batch_snapshot_;
    std::optional<TextureSnapshot> texture_snapshot_;
    std::optional<ResourceSnapshot> resource_snapshot_;
    std::optional<DrawSnapshot> draw_snapshot_;
    OwnedPayload ReadPayload(const Json& digest) {
        (void)DigestView(digest);
        // The immutable operation owns this string throughout the callback;
        // content validation stays exact without allocating a duplicate key.
        const auto& key=digest.get_ref<const std::string&>();
        if(config_.payload_snapshot)return OwnedPayload(config_.payload_snapshot(key));
        return OwnedPayload(config_.payload?config_.payload(key):Read(capture_/(key+".payload.bin")));
    }
    std::vector<uint8_t> Payload(const Json& digest) {return ReadPayload(digest).TakeMutable();}
    OwnedPayload ReadPayload(const DrawResourceInput& resource) {
        if(resource.json)return ReadPayload(*resource.json);
        if(resource.lazy_state)return OwnedPayload(resource.lazy_state->Owner());
        const auto digest=resource.Digest();
        if(resource.typed->owner)return OwnedPayload(resource.typed->owner);
        const auto key=std::string(digest);
        if(config_.payload_snapshot)return OwnedPayload(config_.payload_snapshot(key));
        return OwnedPayload(config_.payload?config_.payload(key):Read(capture_/(key+".payload.bin")));
    }
    std::vector<uint8_t> Payload(const DrawResourceInput& resource) {return ReadPayload(resource).TakeMutable();}
    // Draw-local read-only view. Live operation-owned immutable bytes are
    // borrowed: the executing DrawInput keeps its operation (and so these
    // owners) alive for the whole Draw, so copying the shared_ptr only adds
    // an atomic increment/decrement pair per payload. Every other source keeps
    // the exact ReadPayload path, ordering and exceptions.
    struct DrawPayloadView {
        const OwnedPayload::Snapshot* borrowed=nullptr;
        std::optional<OwnedPayload> owned;
        std::span<const uint8_t> bytes() const & {
            return borrowed?std::span<const uint8_t>(**borrowed):owned->bytes();
        }
        std::span<const uint8_t> bytes() const &&=delete;
    };
    static const OwnedPayload::Snapshot* BorrowedPayloadOwner(const DrawResourceInput& resource) {
        // Same precedence and validation as ReadPayload(DrawResourceInput):
        // json, lazy, then typed, whose digest must be assigned even when an
        // owner is present (Digest() throws exactly as ReadPayload does).
        if(resource.json)return nullptr;
        if(resource.lazy_state)return resource.lazy_state->Owner()?&resource.lazy_state->Owner():nullptr;
        if(resource.typed)(void)resource.Digest();
        if(resource.typed&&resource.typed->owner)return &resource.typed->owner;
        return nullptr;
    }
    DrawPayloadView ViewPayload(const DrawResourceInput& resource) {
        DrawPayloadView view;
        view.borrowed=BorrowedPayloadOwner(resource);
        if(!view.borrowed)view.owned.emplace(ReadPayload(resource));
        return view;
    }

    std::unique_ptr<RenderShader> CreateShader(const void* data,uint64_t size,
                                              const char* entry,RenderShaderFormat format) {
        auto shader=device_->createShader(data,size,entry,format);
        if(shader&&config_.skip_unused_descriptor_sets&&format==RenderShaderFormat::SPIRV)
            static_cast<VulkanShader&>(*shader).declaredDescriptorSets=
                NativeDeclaredDescriptorSets({static_cast<const uint8_t*>(data),size_t(size)});
        return shader;
    }

    void Submit(NativeSynchronousGpuTimingScope* presentation_scope=nullptr) {
        if(presentation_scope)presentation_scope->End(*commands_);
        commands_->end();if(presentation_scope)presentation_scope->BeforeSubmit();
        queue_->executeCommandLists(commands_.get(),fence_.get());
        if(presentation_scope)presentation_scope->BeforeFenceWait();
        queue_->waitForCommandFence(fence_.get());
        if(presentation_scope)presentation_scope->FenceCompleted();
    }
    void ReportQuery(const CompletedNativeQuery& query) {
        query_history_.push_back(query);query_report_dirty_=true;
    }
    void ReportQueries() {
        if(!query_report_dirty_)return;
        auto results=Json::array();
        results.get_ref<Json::array_t&>().reserve(query_history_.size());
        for(const auto& query:query_history_) {
            const auto& result=query.result;
            results.push_back({{"frame",query.frame},{"handle",result.handle},
                {"begin_address",result.begin_address},{"end_address",result.end_address},
                {"generation",query.generation},{"end_event",query.end_event},
                {"samples",result.samples},{"draws",result.draws},{"published_to_guest",false}});
        }
        // Publish only a complete snapshot. Allocation failure preserves every
        // typed completion and leaves the next observation able to retry.
        report["native_occlusion_results"]=std::move(results);
        report["query_report_serializations"]=++query_report_serializations_;
        query_report_dirty_=false;
    }
    bool HasPendingGpuWork() const {
        return !pending_draws_.empty()||(gpu_work_queue_&&gpu_work_queue_->pending())||
            (transfer_batch_&&transfer_batch_->has_recording());
    }
    std::optional<uint64_t> ObserveDiagnosticQueryFrontier() const noexcept {
        if(!gpu_work_queue_||batch_failed_||draw_group_recording_||draw_group_recording_failed_||
           (transfer_batch_&&(transfer_batch_->in_scope()||transfer_batch_->has_recording())))return {};
        const auto work=gpu_work_queue_->ObserveAcceptedWork();
        const auto completed_draws=gpu_work_queue_->statistics().draw_lists;
        // Every Scene pending draw must already be a logical queue entry,
        // including accepted unsubmitted children of an open command group.
        if(!work.available||work.draws<completed_draws||
           work.draws-completed_draws!=pending_draws_.size())return {};
        return work.accepted;
    }
    void CollectCompletedQueries() {
        if(!config_.defer_native_queries)return;
        const bool prefix=config_.retire_completed_query_prefixes;
        if(!prefix&&HasPendingGpuWork())return;
        auto count=queries_->deferred_queries();
        if(!count)return;
        if(ready_query_count_+count!=deferred_queries_.size())
            throw std::runtime_error("Native deferred query metadata count mismatch");
        if(prefix) {
            count=0;
            while(ready_query_count_+count<deferred_queries_.size()&&
                  gpu_work_queue_->WorkCompleted(deferred_queries_[ready_query_count_+count].required_work))++count;
            if(!count)return;
        }
        const auto results=prefix?queries_->TakeDeferredPrefix(count):queries_->TakeDeferredResults();
        for(size_t i=0;i<results.size();++i) {
            auto& query=deferred_queries_[ready_query_count_+i].query;const auto& result=results[i];
            if(query.result.handle!=result.handle||query.result.begin_address!=result.begin_address||
               query.result.end_address!=result.end_address)
                throw std::runtime_error("Native deferred query identity mismatch");
            query.result=result;
        }
        for(size_t i=0;i<results.size();++i)ReportQuery(deferred_queries_[ready_query_count_+i].query);
        ready_query_count_+=results.size();
        if(prefix) {
            query_prefix_completions_+=results.size();
            if(HasPendingGpuWork())query_prefix_while_pending_+=results.size();
        }
    }
    std::vector<CompletedNativeQuery> TakeCompletedQueries() {
        if(batch_failed_||(gpu_work_queue_&&gpu_work_queue_->failed()))
            throw NativeBatchCompletionError("Native query consumption after failed GPU work");
        if(!config_.retire_completed_query_prefixes&&HasPendingGpuWork())
            throw std::runtime_error("Native query consumption before all GPU work completed");
        CollectCompletedQueries();
        if(!config_.retire_completed_query_prefixes&&ready_query_count_!=deferred_queries_.size())
            throw std::runtime_error("Native query consumption before query retirement");
        // Copy before erasing so an allocation failure retains every identity.
        std::vector<CompletedNativeQuery> completed;completed.reserve(ready_query_count_);
        for(size_t i=0;i<ready_query_count_;++i)completed.push_back(deferred_queries_[i].query);
        deferred_queries_.erase(deferred_queries_.begin(),deferred_queries_.begin()+ready_query_count_);
        ready_query_count_=0;
        return completed;
    }
    void PollCompletedQueries() {
        if(!config_.retire_completed_query_prefixes)return;
        if(batch_failed_||draw_group_recording_||draw_group_recording_failed_)
            throw NativeBatchCompletionError("Native query polling after failed/preparing GPU work");
        // Poll only submitted fences. An open newer group has accepted logical
        // ownership but cannot have a completed frontier; keep recording it.
        try {gpu_work_queue_->PollCompleted();CollectCompletedQueries();transfer_report_dirty_=true;}
        catch(const std::exception& error) {batch_failed_=true;throw NativeBatchCompletionError(error.what());}
    }
    void FlushOldestQuery() {
        SealDrawCommandGroup();
        if(!config_.retire_completed_query_prefixes) {FlushPendingDraws();return;}
        if(batch_failed_)throw NativeBatchCompletionError("Native query pressure after failed GPU work");
        try {
            PollCompletedQueries();
            if(ready_query_count_||deferred_queries_.empty())return;
            const auto required=deferred_queries_.front().required_work;
            // Earlier queued lists already include every transfer that preceded
            // this end. Do not seal newer transfer recording merely to publish it.
            gpu_work_queue_->WaitThrough(required);
            CollectCompletedQueries();++query_prefix_pressure_;transfer_report_dirty_=true;
        } catch(const std::exception& error) {batch_failed_=true;throw NativeBatchCompletionError(error.what());}
    }
    void RetireGpuWork(const NativeGpuWorkQueue::Completion& completion) {
        transfer_report_dirty_=true;
        if(gpu_work_queue_&&completion.submission!=last_retired_gpu_submission_+1)
            throw NativeBatchCompletionError("Native GPU fence generation retired out of order");
        if(completion.draws>pending_draws_.size())
            throw NativeBatchCompletionError("Shared GPU fence exceeds accepted draw ownership");
        uint32_t queried=0;
        for(uint32_t i=0;i<completion.draws;++i) {
            const auto* slot=pending_draws_[i];
            if(gpu_work_queue_&&completion.draw_tokens[i]!=slot->token)
                throw NativeBatchCompletionError("Shared GPU fence has stale/out-of-order draw ownership");
            queried+=slot->queried;
        }
        if(draw_timestamps_) {
            std::array<NativeDrawTimestamps::Token,NativeSubmissionCapacity::kMaximum> tokens;
            for(uint32_t i=0;i<completion.draws;++i)tokens[i]={
                uint32_t(pending_draws_[i]-draw_slots_.data()),pending_draws_[i]->token};
            draw_timestamps_->Retire({tokens.data(),completion.draws},completion.submission);
        }
#ifdef GOW3_NATIVE_FRAGMENT_CENSUS_REPLAY
        if(fragment_census_) {
            std::array<NativeFragmentInvocationCensus::Token,NativeSubmissionCapacity::kMaximum> tokens;
            for(uint32_t i=0;i<completion.draws;++i)tokens[i]={
                uint32_t(pending_draws_[i]-draw_slots_.data()),pending_draws_[i]->token};
            fragment_census_->Retire({tokens.data(),completion.draws},completion.submission);
        }
#endif
        // Only this fence's physical query slots are available. Later closed
        // and active logical records remain owned until their own fences.
        queries_->CompletePrefix(queried);
        if(gpu_work_queue_)last_retired_gpu_submission_=completion.submission;
        if(completion.draws) {
            ++batch_submissions_;batch_draws_+=completion.draws;
            batch_peak_=std::max(batch_peak_,uint64_t(completion.draws));
        }
        if(completion.last_event)diagnostic_counters_.last_completed_event=completion.last_event;
        size_t resident=0;
        for(const auto& slot:std::span(draw_slots_).first(submission_capacity_.lists))resident+=slot.indices.capacity+slot.constants.capacity+slot.rectangle.capacity;
        for(uint32_t i=0;i<completion.draws;++i) {
            auto* slot=pending_draws_[i];
#ifdef GOW3_NATIVE_STENCIL_MATERIAL_REPLAY
            if(slot->stencil_material_selected) {
                if(!slot->stencil_material_counter||!slot->stencil_material_counter->pending())
                    throw NativeBatchCompletionError("Stencil material counter lost accepted slot");
                stencil_material_stats_.samples.push_back(slot->stencil_material_counter->Retire(slot->token,completion.submission));
                slot->stencil_material_selected=false;++stencil_material_stats_.retired;
            }
#endif
            if(slot->account_constant_delta) {
                // This original fence has completed the submitted slot. Do not
                // count an upload whose later command recording was refused.
                ++constant_delta_stats_.submitted_draws;
                constant_delta_stats_.float_written_bytes+=slot->constant_delta_writes.float_bytes;
                constant_delta_stats_.shared_written_bytes+=slot->constant_delta_writes.shared_bytes;
                constant_delta_stats_.unchanged_blocks+=slot->constant_delta_writes.unchanged_blocks;
                slot->account_constant_delta=false;
            }
            if(slot->account_pinned_null_constants) {
                ++pinned_null_constant_stats_.retired_draws;
                if(slot->pinned_null_constants_candidate) {
                    ++pinned_null_constant_stats_.candidates;
                    if(slot->pinned_null_constants_selected)++pinned_null_constant_stats_.selected;
                    else ++pinned_null_constant_stats_.refusals;
                }
                slot->account_pinned_null_constants=false;
            }
            if(slot->accepted_upload_bytes>pending_upload_bytes_)
                throw NativeBatchCompletionError("Native upload retirement exceeds its accepted prefix");
            pending_upload_bytes_-=slot->accepted_upload_bytes;slot->accepted_upload_bytes=0;
            slot->queried=false;slot->transient_bindings.clear();slot->ReleaseAliases();slot->framebuffer.reset();slot->readonly_depth.reset();slot->early_depth.reset();slot->retained_opaque.reset();slot->retained_fp24_depth.reset();
            for(auto* upload:{&slot->indices,&slot->constants,&slot->rectangle})
                if(resident>32u*1024*1024) {resident-=upload->capacity;*upload={};}
        }
        for(uint32_t i=0;i<completion.draws;++i)--pending_draws_[i]->pending_occurrences;
        pending_draws_.erase(pending_draws_.begin(),pending_draws_.begin()+completion.draws);
        // Only ends whose all-work frontier is covered may be collected. A
        // newer staging tail, batch or transfer retains its own ownership.
        CollectCompletedQueries();
        draw_timing_.submit_wait+=completion.milliseconds;
    }
    void RelieveDrawPressure() {
        SealDrawCommandGroup();
        if(!config_.async_gpu_submissions) {FlushPendingDraws();return;}
        gpu_work_queue_->RecordDrawPressure(pending_draws_.size()>=submission_capacity_.lists,
            pending_upload_bytes_>=16u*1024*1024);
        gpu_work_queue_->SubmitPending(NativeGpuWorkQueue::SubmissionReason::DrawOwnershipPressure);
        while(pending_draws_.size()>=submission_capacity_.lists||pending_upload_bytes_>=16u*1024*1024) {
            if(!gpu_work_queue_->inflight_batches())
                throw NativeGpuWorkError("Native draw pressure has no submitted fence");
            gpu_work_queue_->WaitOldest(NativeGpuWorkQueue::WaitReason::DrawOwnershipPressure);
        }
    }
    void SubmitGpuWork() {
        if(!config_.async_gpu_submissions||!gpu_work_queue_||!transfer_batch_)
            throw std::invalid_argument("Native progress submission requires an asynchronous unified queue");
        if(batch_failed_)throw NativeBatchCompletionError("Native submission after failed accepted work");
        SealDrawCommandGroup();
        transfer_report_dirty_=true;
        try {
            transfer_batch_->Seal();gpu_work_queue_->SubmitPending(NativeGpuWorkQueue::SubmissionReason::CoherenceProgress);
            gpu_work_queue_->PollCompleted();CollectCompletedQueries();
        } catch(const std::exception& error) {
            batch_failed_=true;throw NativeBatchCompletionError(error.what());
        } catch(...) {
            batch_failed_=true;throw NativeBatchCompletionError("Native coherence progress submission failed");
        }
    }
    void SealDrawCommandGroup() {
        if(!DrawGroupsEnabled())return;
        if(draw_group_recording_||draw_group_recording_failed_||batch_failed_)
            throw NativeBatchCompletionError("Native draw command group contains a poisoned preparing tail");
        if(!open_draw_command_group_)return;
        if(!gpu_work_queue_||!gpu_work_queue_->open_group_draws())
            throw NativeBatchCompletionError("Native draw command group lost its accepted logical owner");
        try {
            const auto start=config_.time_draw_recording?DrawClock::now():DrawClock::time_point{};
            open_draw_command_group_->end();
            if(config_.time_draw_recording)draw_recording_.end_ms+=DrawMilliseconds(start,DrawClock::now());
            if(config_.time_draw_recording||config_.reset_draw_command_pool)++draw_recording_.ends;
            gpu_work_queue_->ClosePinnedDrawGroup(*open_draw_command_group_);
            open_draw_command_group_=nullptr;
            if(open_packed_color_pass_) {
                ++packed_color_pass_stats_.groups;
                packed_color_pass_stats_.maximum_group_draws=std::max<uint64_t>(
                    packed_color_pass_stats_.maximum_group_draws,packed_color_pass_draws_);
            }
            open_packed_color_pass_=false;packed_color_pass_draws_=0;
            if(open_fsi_draw_pass_) {
                ++fsi_draw_pass_stats_.groups;
                fsi_draw_pass_stats_.maximum_group_draws=std::max<uint64_t>(
                    fsi_draw_pass_stats_.maximum_group_draws,fsi_draw_pass_draws_);
            }
            open_fsi_draw_pass_=false;fsi_draw_pass_draws_=0;
            if(open_packed_depth_pass_) {
                ++packed_depth_pass_stats_.groups;
                packed_depth_pass_stats_.maximum_group_draws=std::max<uint64_t>(
                    packed_depth_pass_stats_.maximum_group_draws,packed_depth_pass_draws_);
            }
            open_packed_depth_pass_=false;open_packed_depth_atomic_=false;packed_depth_pass_draws_=0;transfer_report_dirty_=true;
            if(open_readonly_query_pass_) {
                ++readonly_query_pass_stats_.groups;
                readonly_query_pass_stats_.maximum_group_draws=std::max<uint64_t>(
                    readonly_query_pass_stats_.maximum_group_draws,readonly_query_pass_draws_);
            }
            open_readonly_query_pass_=false;readonly_query_pass_draws_=0;open_readonly_query_owner_.reset();
            if(open_retained_phase_pass_) {
                ++retained_phase_pass_stats_.groups;
                retained_phase_pass_stats_.maximum_group_draws=std::max<uint64_t>(
                    retained_phase_pass_stats_.maximum_group_draws,retained_phase_pass_draws_);
            }
            open_retained_phase_pass_=false;retained_phase_pass_draws_=0;
            open_retained_phase_owner_.reset();open_retained_phase_identity_={};
        } catch(const std::exception& error) {
            batch_failed_=draw_group_recording_failed_=true;
            gpu_work_queue_->FailDrawCommandGroupRecording();
            throw NativeBatchCompletionError(error.what());
        } catch(...) {
            batch_failed_=draw_group_recording_failed_=true;
            gpu_work_queue_->FailDrawCommandGroupRecording();
            throw NativeBatchCompletionError("Native draw command group close failed");
        }
    }
    void FlushPendingDraws() {
        transfer_report_dirty_=true;
        if(batch_failed_)throw NativeBatchCompletionError("Native GPU submission previously failed; replay is forbidden");
        try {
            SealDrawCommandGroup();
            if(gpu_work_queue_) {
                transfer_batch_->Seal();gpu_work_queue_->Drain();CollectCompletedQueries();return;
            }
            if(pending_draws_.empty()){CollectCompletedQueries();return;}
            const auto start=DrawClock::now();
            std::array<const RenderCommandList*,64> lists{};
            for(size_t i=0;i<pending_draws_.size();++i)lists[i]=pending_draws_[i]->commands.get();
            queue_->executeCommandLists(lists.data(),uint32_t(pending_draws_.size()),nullptr,0,nullptr,0,fence_.get());
            queue_->waitForCommandFence(fence_.get());
            RetireGpuWork({uint32_t(pending_draws_.size()),0,pending_last_event_,DrawMilliseconds(start,DrawClock::now())});
        } catch(const std::exception& error) {
            batch_failed_=true;throw NativeBatchCompletionError(error.what());
        } catch(...) {
            batch_failed_=true;throw NativeBatchCompletionError("Native GPU completion failed");
        }
    }
    void SetTextureCensusFrame(uint32_t frame) noexcept {
        texture_census_frame_=frame;texture_census_frame_valid_=true;
    }
    NativeTextureEvictionReloadCensus::Observer TextureCensusObserver() noexcept {
        return {texture_eviction_reload_census_.get(),texture_census_frame_,texture_census_frame_valid_};
    }
    bool EvictTextureForBindings() {
        if(config_.diagnose_texture_eviction_reloads)
            return texture_cache_.EvictUnusedForBindings(textures_,[&](const auto& entry){RetireCpuImageBindings(entry);},
                [](const auto& entry){return !entry.bindings.empty();},TextureCensusObserver());
        return texture_cache_.EvictUnusedForBindings(textures_,[&](const auto& entry){RetireCpuImageBindings(entry);},
            [](const auto& entry){return !entry.bindings.empty();});
    }
    void RetireCpuImageBindings(const UploadedImage& image) {
        // Both frame retirement and binding-pressure eviction call this before
        // erasing a map node. Invalidate even when no bindings are attached.
        cpu_image_slots_.Reset(CpuImageSlots::ResetReason::BeforeErase);
        if(image.bindings.size()>cached_texture_bindings_)
            throw std::logic_error("Native CPU image binding accounting underflow");
        cached_texture_bindings_-=image.bindings.size();
    }
    TextureBinding& BindCachedTexture(TextureBindingCache& cache,RenderTexture& image,const TextureLayout& texture,
                                      bool decoded_gpu,int32_t anisotropy,std::vector<TextureBinding>& transient,
                                      TextureBinding* uploaded_binding=nullptr,bool shared_cpu_storage=false) {
        TextureBindingKey key;
        std::copy(texture.fetch.begin(),texture.fetch.end(),key.begin());
        key[6]=uint32_t(anisotropy);key[7]=config_.max_sampler_anisotropy;
        if(auto found=cache.find(key);found!=cache.end()){
            ++texture_binding_hits_;
            if(config_.cache_draw_descriptors&&config_.batch_draw_descriptors&&!found->second.descriptor_generation) {
                found->second.descriptor_generation=descriptor_generations_.Issue();
                if(!found->second.descriptor_generation)++descriptor_contents_.generation_refusals;
            }
            return found->second;
        }
        // Inactive CPU images may keep old sampler variants across frames.
        // Release those already-fenced owners before they force every new
        // binding into transient allocation. All current/preparing owners are
        // marked before reaching this point and cannot be selected.
        const auto binding_limit=CpuTextureSamplingBindingLimit(texture.fetch,shared_cpu_storage);
        if(shared_cpu_storage&&cache.size()>=binding_limit&&cache.size()<16)
            ++cpu_texture_sampling_.reserved_binding_refusals;
        if(cache.size()<binding_limit)while(cached_texture_bindings_>=4096&&EvictTextureForBindings()) {}
        // UploadTexture already creates a binding for its initial fetch state.
        // Adopt it on first use instead of allocating a duplicate view/sampler.
        auto binding=uploaded_binding&&uploaded_binding->view?std::move(*uploaded_binding):
            BindTexture(*device_,image,texture,decoded_gpu,anisotropy,config_.max_sampler_anisotropy);
        if(config_.cache_draw_descriptors&&config_.batch_draw_descriptors) {
            binding.descriptor_generation=descriptor_generations_.Issue();
            if(!binding.descriptor_generation)++descriptor_contents_.generation_refusals;
        }
        ++texture_binding_misses_;
        // Avoid unbounded sampler variants when settings or guest fetch state
        // animate. At the bound, keep this draw's binding alive until its fence
        // completes rather than evicting a view used by an earlier draw slot.
        if(cache.size()<binding_limit&&cached_texture_bindings_<4096) {
            ++cached_texture_bindings_;
            if(shared_cpu_storage&&!(texture.fetch[4]&0x003ff000u))++cpu_texture_sampling_.zero_bias_cached;
            return cache.emplace(key,std::move(binding)).first->second;
        }
        ++texture_binding_transient_;transient.push_back(std::move(binding));return transient.back();
    }
    // Multisampled EDRAM is not emulated. With msaa_single_sample_fallback a
    // 2x/4x surface is stored as a single-sample surface of its pixel size:
    // draws render without anti-aliasing and a resolve copies that one value.
    // Its EDRAM footprint is the 1x one, so a later reinterpretation of the
    // extra sample tiles is not modeled. Every distinct layout is recorded.
    struct MsaaFallbackStatistics {
        uint64_t surfaces=0;
        std::vector<std::array<uint32_t,6>> layouts;
    } msaa_fallback_;
    void AdmitMultisampled(uint32_t width,uint32_t height,uint32_t info,uint32_t base,uint32_t format,bool depth) {
        if(!(info&0x30000u))return;
        if(!config_.msaa_single_sample_fallback)throw std::runtime_error("Multisampled EDRAM is not implemented");
        ++msaa_fallback_.surfaces;
        const std::array<uint32_t,6> layout{width,height,info,base,format,uint32_t(depth)};
        if(msaa_fallback_.layouts.size()<32&&
           std::find(msaa_fallback_.layouts.begin(),msaa_fallback_.layouts.end(),layout)==msaa_fallback_.layouts.end()) {
            msaa_fallback_.layouts.push_back(layout);
            std::fprintf(stderr,"Native renderer: %ux MSAA %s surface %ux%u pitch %u base %u format %u stored single-sample (no anti-aliasing)\n",
                (info&0x30000u)==0x10000u?2u:4u,depth?"depth":"color",width,height,info&16383u,base,format);
        }
    }
    SurfaceLayout SurfaceStorage(const Json& s) {
        const unsigned info=s.at("surface_info").get<unsigned>();
        AdmitMultisampled(s.at("width"),s.at("height"),info,s.at("edram_base"),s.at("format"),s.at("depth"));
        return {s.at("width"),s.at("height"),info&16383u,
            s.at("edram_base"),s.at("format"),s.at("depth")};
    }
    SurfaceLayout SurfaceStorage(const DrawSurfaceInput& s) {
        AdmitMultisampled(s.Width(),s.Height(),s.Info(),s.Base(),s.Format(),s.Depth());
        return {s.Width(),s.Height(),s.Info()&16383u,s.Base(),s.Format(),s.Depth()};
    }
    // Typed surfaces reuse the exact SurfaceKey string the JSON path builds.
    std::map<std::array<uint32_t,6>,std::string> typed_surface_keys_;
    RenderTexture& Surface(const DrawSurfaceInput& s,bool write=false) {
        MaterializeRetainedOpaque();
        if(s.json)return Surface(*s.json,write);
        const std::array<uint32_t,6> typed{uint32_t(s.Depth()),s.Base(),s.Format(),s.Info(),s.Width(),s.Height()};
        auto found=typed_surface_keys_.find(typed);
        if(found==typed_surface_keys_.end()) {
            if(typed_surface_keys_.size()>=4096)return Surface(s.ToJson(),write);
            found=typed_surface_keys_.emplace(typed,SurfaceKey(s.ToJson())).first;
        }
        return SurfaceAt(found->second,SurfaceStorage(s),[&]{return s.ToJson();},write,nullptr,nullptr);
    }
    // Pure function of the layout; a draw asks for it per bound surface.
    std::map<std::array<uint32_t,6>,std::bitset<2048>> surface_tiles_memo_;
    const std::bitset<2048>& SurfaceTiles(const SurfaceLayout& s) {
        const std::array<uint32_t,6> key{s.width,s.height,s.pitch,s.base,s.format,uint32_t(s.depth)};
        if(const auto found=surface_tiles_memo_.find(key);found!=surface_tiles_memo_.end())return found->second;
        std::bitset<2048> tiles;
        // Both 32-bit half-tiles and 64-bit tiles fit within forty pixels.
        for(unsigned y=0;y<s.height;y+=16)for(unsigned x=0;x<s.width;x+=40)
            tiles.set(SurfaceByteOffset(s,x,y)/5120);
        if(surface_tiles_memo_.size()>=1024)surface_tiles_memo_.clear();
        return surface_tiles_memo_.emplace(key,tiles).first->second;
    }
    bool PrepareReadonlyNoWriteMetadata(const SurfaceLayout& storage) {
        const NativeReadonlyDepthLayoutKey key{storage.width,storage.height,storage.pitch,storage.base,storage.format,uint32_t(storage.depth)};
        if(!readonly_no_write_layout_||*readonly_no_write_layout_!=key) {
            auto pages=edram_memory_->SurfacePages(storage);
            readonly_no_write_pages_=pages;readonly_no_write_layout_=key;
        }
        for(auto& [key,image]:surfaces_) {
            if(image.dirty)return false;
            if(!image.physical_pages)image.physical_pages=edram_memory_->SurfacePages(SurfaceStorage(image.identity));
        }
        ++readonly_no_write_stats_.prepared;return true;
    }
    bool ReadonlyNoWriteHostsPrepared() const {
        for(const auto& [key,image]:surfaces_)if(image.dirty||!image.physical_pages)return false;
        return true;
    }
    void InvalidateReadonlyNoWriteHosts() {
        // RecordSurfaceWrite used to change all overlapping 4 KiB page tokens.
        // Keep its precision-bearing host/receipt invalidation, even for
        // different layouts or neighboring tiles sharing a physical page.
        // Packed-derived aliases remain valid because no packed byte changed.
        for(auto& [key,image]:surfaces_)if((*image.physical_pages&readonly_no_write_pages_).any()) {
            if(image.version){image.version=0;++readonly_no_write_stats_.host_invalidations;}
            if(image.atlas_clear_receipt.source) {
                image.atlas_clear_receipt={};++readonly_no_write_stats_.receipt_invalidations;
                ++fused_atlas_depth_clear_stats_.deferred_host_discards;
            }
        }
    }
    RenderTexture& Surface(const Json& s,bool write=false,const RenderRect* overwrite=nullptr,
                           const RenderRect* depth_read_region=nullptr) {
        MaterializeRetainedOpaque();
        return SurfaceAt(SurfaceKey(s),SurfaceStorage(s),[&]()->Json{return s;},write,overwrite,depth_read_region);
    }
    // identity() builds the JSON identity only when an image is created or reported.
    // True when SurfaceAt will commit an overlapping dirty alias or restore
    // this image from packed EDRAM. Conservative: may report pending work
    // that SurfaceAt then skips (for example a validated full overwrite).
    bool SurfaceConversionPending(const std::string& key,const SurfaceLayout& storage) {
        const auto& tiles=SurfaceTiles(storage);
        if(surfaces_maybe_dirty_)for(const auto& [other,image]:surfaces_)
            if(other!=key&&image.dirty&&(image.tiles&tiles).any())return true;
        const auto found=surfaces_.find(key);
        if(found==surfaces_.end()||!found->second.texture)return edram_memory_->SurfaceVersion(storage)!=0;
        const auto& entry=found->second;
        return bool(entry.atlas_clear_receipt.source)||(!entry.dirty&&edram_memory_->SurfaceVersion(storage)!=entry.version);
    }
    template<class Identity>
    RenderTexture& SurfaceAt(const std::string& key,const SurfaceLayout& storage,Identity&& identity,bool write,
                             const RenderRect* overwrite,const RenderRect* depth_read_region) {
        // Hardware render targets: queue alias commits/restores in order with
        // the surrounding draws. Outside a transfer scope each conversion
        // would submit synchronously and drain the whole GPU queue.
        if(!config_.native_fsi&&gpu_work_queue_&&transfer_batch_&&!transfer_batch_->in_scope()&&
           SurfaceConversionPending(key,storage)) {
            RenderTexture* result=nullptr;NativeGpuWorkTag tag;tag.kind=NativeGpuWorkKind::SurfaceConversion;
            RunTransferScope([&]{result=&SurfaceAt(key,storage,identity,write,overwrite,depth_read_region);},0,
                (config_.gpu_batch_attribution||config_.gpu_physical_timestamps)?&tag:nullptr);
            return *result;
        }
        if(depth_read_region&&(write||overwrite||!storage.depth||depth_read_region->isEmpty()||
           depth_read_region->left<0||depth_read_region->top<0||
           depth_read_region->right>int32_t(storage.width)||depth_read_region->bottom>int32_t(storage.height)))
            throw std::invalid_argument("Invalid transfer-only depth source region");
        frame_surfaces_.insert(key);
        const auto tiles=SurfaceTiles(storage);
        // Commit dirty images before a different interpretation can read or
        // overwrite their tiles. Base addresses need not be equal to alias.
        for(auto& [other,image]:surfaces_)if(other!=key&&image.dirty&&(image.tiles&tiles).any()) {
            const auto previous=SurfaceStorage(image.identity);
            ++surface_conversions_.commits;surface_conversions_.commit_pixels+=uint64_t(previous.width)*previous.height;
            if(surface_conversions_.commit_keys.size()<64||surface_conversions_.commit_keys.contains(other))
                ++surface_conversions_.commit_keys[other+"->"+key];
            edram_memory_->StoreSurface(*image.texture,previous);
            image.version=edram_memory_->SurfaceVersion(previous);image.dirty=false;
        }
        auto& entry=surfaces_[key];
        if(!entry.texture) {
            entry.identity=identity();
            const auto format=SurfaceFormat(entry.identity,!config_.native_fsi);
            entry.texture=device_->createTexture(storage.depth?RenderTextureDesc::DepthTarget(storage.width,storage.height,format):
                RenderTextureDesc::ColorTarget(storage.width,storage.height,format));
            entry.tiles=tiles;
        }
        const auto version=edram_memory_->SurfaceVersion(storage);
        if(entry.atlas_clear_receipt.source) {
            if(!entry.dirty&&NativeFusedAtlasDepthClearReceiptCurrent(true,
                    entry.atlas_clear_receipt.packed_version,version)) {
                // Re-own this image for the NEW restore packet: its original
                // fused-compute packet may already have retired. Keep the full
                // pre-encode image, including outside-clear floats and stencil.
                auto restored=entry.atlas_clear_receipt.source;
                if(transfer_batch_&&transfer_batch_->active())
                    transfer_batch_->KeepImage(restored,
                        static_cast<const VulkanTexture&>(*restored).allocationInfo.size);
                transfers_->RestoreDepth(*restored,*entry.texture);
                entry.version=version;
                ++fused_atlas_depth_clear_stats_.deferred_host_materializations;
            } else {
                ++fused_atlas_depth_clear_stats_.deferred_host_discards;
            }
            entry.atlas_clear_receipt={};
        }
        // Only a validated full color clear may discard the working image's
        // prior contents. Keep alias commits and packed-layout validation above.
        const bool full_overwrite=overwrite&&write&&!storage.depth&&
            overwrite->left==0&&overwrite->top==0&&
            overwrite->right==int32_t(storage.width)&&overwrite->bottom==int32_t(storage.height);
        if(overwrite)++full_color_clear_restore_stats_.requests;
        if(full_overwrite)++full_color_clear_restore_stats_.full_overwrites;
        if(!entry.dirty&&version!=entry.version&&full_overwrite) {
            ++full_color_clear_restore_stats_.skipped_restores;
            full_color_clear_restore_stats_.skipped_restore_pixels+=uint64_t(storage.width)*storage.height;
            // Do not certify the stale image as restored. The unchanged clear
            // and StoreSurface path establishes its new contents and version.
        } else if(!entry.dirty&&version!=entry.version) {
            ++surface_conversions_.restores;surface_conversions_.restore_pixels+=uint64_t(storage.width)*storage.height;
            if(!entry.version)++surface_conversions_.first_restores;
            if(surface_conversions_.restore_keys.size()<64||surface_conversions_.restore_keys.contains(key))
                ++surface_conversions_.restore_keys[key];
            if(!storage.depth&&edram_memory_->RestoreColorSurface(*entry.texture,storage)) {
                // One fullscreen decode draw; same values as load + identity Resolve.
            } else {
                auto restored=depth_read_region?edram_memory_->LoadSurfaceSharedRegion(storage,*depth_read_region):
                    edram_memory_->LoadSurfaceShared(storage);
                if(storage.depth)transfers_->RestoreDepth(*restored,*entry.texture,depth_read_region);
                else {
                    const RenderRect r(0,0,storage.width,storage.height);
                    transfers_->Resolve(*restored,*entry.texture,r,r);
                }
            }
            if(depth_read_region) {
                // Only the transfer's rectangle is current. Keeping the old
                // token forces every future generic Surface read/write to take
                // its original FULL restoration before accessing other pixels.
                entry.version=NativeAtlasPartialRestoreVersion(entry.version,version);
                ++atlas_depth_region_stats_.partial_restores;
                atlas_depth_region_stats_.restored_pixels+=uint64_t(depth_read_region->right-depth_read_region->left)*
                    uint64_t(depth_read_region->bottom-depth_read_region->top);
                atlas_depth_region_stats_.full_extent_pixels+=uint64_t(storage.width)*storage.height;
                if(detailed_reports_)report["surface_region_restores"].push_back({{"identity",identity()},
                    {"packed_version",version},{"retained_image_version",entry.version},
                    {"source_rectangle",std::array<int32_t,4>{depth_read_region->left,depth_read_region->top,
                        depth_read_region->right,depth_read_region->bottom}}});
            } else {
                entry.version=version;
                if(detailed_reports_)report["surface_alias_restores"].push_back({{"identity",identity()},{"version",version}});
            }
        } else if(depth_read_region) {
            ++atlas_depth_region_stats_.current_source_reads;
        }
        if(write){entry.dirty=true;surfaces_maybe_dirty_=true;}return *entry.texture;
    }
    RenderShader* ResidentOpaqueColorOverwrite(const PipelineLookup& key) {
        const auto shader=shaders_.find(ShaderCacheLookup{kOpaqueColorOverwritePixel,true,
            NativeShaderVariant::OpaqueFormat3FullWordOverwriteV1});
        const auto pipeline=pipelines_.find(key);
        if(shader==shaders_.end()||pipeline==pipelines_.end()||!pipeline->second.pipeline) {
            ++opaque_overwrite_stats_.residency_refusals;
            return nullptr;
        }
        cache_epoch_.Mark(shader->second);
        cache_epoch_.Mark(pipeline->second);
        return shader->second.shader.get();
    }
    RenderShader* ResidentForwardControlFlow(const PipelineLookup& key) {
        const auto shader=shaders_.find(ShaderCacheLookup{kForwardControlFlowPixel,true,
            NativeShaderVariant::ForwardOnlyGuestControlFlowV1});
        const auto pipeline=pipelines_.find(key);
        if(shader==shaders_.end()||pipeline==pipelines_.end()||!pipeline->second.pipeline) {
            ++forward_control_flow_stats_.residency_refusals;
            return nullptr;
        }
        cache_epoch_.Mark(shader->second);
        cache_epoch_.Mark(pipeline->second);
        return shader->second.shader.get();
    }
    RenderShader* ResidentForwardVertex(const PipelineLookup& key) {
        const auto shader=shaders_.find(ShaderCacheLookup{kForwardVertexSource,false,
            NativeShaderVariant::ForwardStraightVertexControlFlowV1});
        const auto pipeline=pipelines_.find(key);
        if(shader==shaders_.end()||pipeline==pipelines_.end()||!pipeline->second.pipeline) {
            ++forward_vertex_stats_.residency_refusals;
            return nullptr;
        }
        cache_epoch_.Mark(shader->second);
        cache_epoch_.Mark(pipeline->second);
        return shader->second.shader.get();
    }
    std::map<PipelineIdentity,FsiStateCoverage,PipelineLess> fsi_state_coverage_;
    struct FsiStateStats {uint64_t candidates=0,selected=0,queried_selected=0,residency_refusals=0,
        prepared=0,prewarm_refusals=0,modules_created=0,module_bytes=0,observation_refusals=0,
        resident_handoffs=0,coverage_pointer_hits=0;} fsi_state_stats_;
    struct FsiStateMaterialStats {uint64_t examined=0,source_refusals=0,query_refusals=0,
        state_refusals=0,admitted=0,inactive_catalog_admitted=0,inactive_catalog_selected=0;} fsi_state_material_stats_;
    void ObserveFsiState(const PipelineLookup& key,bool selected,bool queried,CachedPipeline* resident=nullptr) noexcept {
        try {
            auto* coverage=resident?resident->fsi_coverage:nullptr;
            if(coverage)++fsi_state_stats_.coverage_pointer_hits;
            else {
                auto found=fsi_state_coverage_.find(key);
                if(found==fsi_state_coverage_.end()) {
                    if(fsi_state_coverage_.size()>=NativeFsiStateSidecar::kMaxRecipes) {
                        ++fsi_state_stats_.observation_refusals;return;
                    }
                    FsiStateCoverage value;
                    if(config_.observe_fsi_state&&key.vertex&&key.fsi_state) {
                        NativeFsiStateRecipe recipe{{std::string(*key.vertex),OwnDigest(key.pixel),
                            std::get<std::array<uint32_t,6>>(key.raster),int32_t(RenderFormat::R8G8B8A8_UNORM),-1},*key.fsi_state};
                        value.observed=config_.observe_fsi_state(recipe);
                        if(!value.observed)++fsi_state_stats_.observation_refusals;
                    }
                    found=fsi_state_coverage_.emplace(PipelineIdentity{OwnDigest(key.vertex),OwnDigest(key.pixel),key.raster,
                        key.color_format,key.depth_format,key.pixel_variant,key.vertex_variant,key.fsi_state},value).first;
                }
                coverage=&found->second;
                // Only a selected exact-specialization PSO can memoize this
                // tuple. Generic PSOs may render multiple different FSI states.
                if(resident)resident->fsi_coverage=coverage;
            }
            ++coverage->candidates;if(queried)++coverage->queried_candidates;
            if(selected){++coverage->selected;if(queried)++coverage->queried;}
            if(coverage->observed)coverage->observed->fetch_add(1,std::memory_order_relaxed);
        }catch(...){++fsi_state_stats_.observation_refusals;}
    }
    NativeShaderVariant FsiStatePixelVariant() const noexcept {
        return config_.fsi_state_forward_cfg?NativeShaderVariant::PipelineStateForwardCfgV1:
            NativeShaderVariant::PipelineStateEpilogueV1;
    }
    ResidentFsiStateSelection ResidentFsiState(const PipelineLookup& key) {
        const auto shader=shaders_.find(ShaderCacheLookup{key.pixel.value_or("depth-only"),true,
            key.pixel_variant});
        const auto pipeline=pipelines_.find(key);
        if(shader==shaders_.end()||pipeline==pipelines_.end()||!pipeline->second.pipeline) {
            ++fsi_state_stats_.residency_refusals;return {};
        }
        cache_epoch_.Mark(shader->second);cache_epoch_.Mark(pipeline->second);
        return {shader->second.shader.get(),&pipeline->second,&pipeline->first};
    }
    RenderPipeline* UseResidentFsiPipeline(const PipelineLookup& key,const ResidentFsiStateSelection& resident,
                                         bool* created,uint32_t* diagnostic_pair,uint32_t* timing_pair) {
        // The handoff is consumed in this same Draw after its exact resident
        // selection. All texture/resize/cache-pressure work preceded selection;
        // the selected path performs no Shader()/Trim()/Reserve before this
        // point. Reject an inconsistent tuple without compiling a candidate.
        if(!config_.specialize_fsi_pipeline_state||!resident||!resident.pipeline->pipeline||
           key.pixel_variant!=FsiStatePixelVariant()||!key.fsi_state||
           PipelineLess{}(*resident.identity,key)||PipelineLess{}(key,*resident.identity))
            throw std::logic_error("Invalid Native FSI resident pipeline handoff");
        if(created)*created=false;
        ++pipeline_cache_stats_.hits;cache_epoch_.Mark(*resident.pipeline);
        // Preserve Pipeline's lazy diagnostic IDs and hit/epoch accounting at
        // the original observation point; only the duplicate tree walk is gone.
        if(diagnostic_pair) {
            if(!resident.pipeline->diagnostic_pair_assigned) {
                resident.pipeline->diagnostic_pair=gpu_work_queue_->attribution()->InternPair(
                    key.vertex.value_or(std::string_view{}),key.pixel);
                resident.pipeline->diagnostic_pair_assigned=true;
            }
            *diagnostic_pair=resident.pipeline->diagnostic_pair;
        }
        if(timing_pair) {
            if(!resident.pipeline->timing_pair_assigned) {
                resident.pipeline->timing_pair=draw_timestamps_->InternPair(
                    key.vertex.value_or(std::string_view{}),key.pixel);
                resident.pipeline->timing_pair_assigned=true;
            }
            *timing_pair=resident.pipeline->timing_pair;
        }
        ++fsi_state_stats_.resident_handoffs;
        return resident.pipeline->pipeline.get();
    }
    RenderShader* ResidentForwardPixelCatalog(const PipelineLookup& key,std::string_view pixel) {
        const auto shader=shaders_.find(ShaderCacheLookup{pixel,true,NativeShaderVariant::ForwardColorPixelCatalogV1});
        const auto pipeline=pipelines_.find(key);
        if(shader==shaders_.end()||pipeline==pipelines_.end()||!pipeline->second.pipeline) {
            ++forward_pixel_catalog_stats_.residency_refusals;
            ++forward_pixel_catalog_source_stats_.find(pixel)->second.residency_refusals;return nullptr;
        }
        cache_epoch_.Mark(shader->second);cache_epoch_.Mark(pipeline->second);return shader->second.shader.get();
    }
    NativeShaderVariant ShaderVariant(bool fsi,bool null_builtin=false) const {
        if(fsi&&!null_builtin&&config_.fsi_hoist_mode==NativeFsiHoistMode::ImmutableState)
            return NativeShaderVariant::ImmutableFsiStateHoistV1;
        if(fsi&&!null_builtin&&config_.fsi_hoist_mode==NativeFsiHoistMode::ImmutableStateAndIncomingDepth)
            return NativeShaderVariant::ImmutableFsiStateAndIncomingDepthHoistV2;
        if(fsi&&config_.exact_fsi_stores&&!null_builtin)return NativeShaderVariant::ExactIdentityFsiStoresV1;
        return fsi && config_.implicit_fsi_barriers
            ? NativeShaderVariant::ImplicitCoherentInterlockV1
            : NativeShaderVariant::Unmodified;
    }
    RenderShader* Shader(const DrawResourceInput& hash,const DrawInput& operation,bool fsi=false,
                         const RenderShader* preparing_vertex=nullptr,bool atomic_builtin=false,
                         NativeShaderVariant* selected_variant=nullptr,bool* audited_vertex=nullptr,
                         bool* null_constant_footprint=nullptr) {
        if(null_constant_footprint)*null_constant_footprint=false;
        if(hash.IsNull()&&!fsi) return nullptr;
        const auto digest=hash.IsNull()?std::string_view("depth-only"):hash.Digest();
        auto variant=atomic_builtin&&!atomic_builtin_refused_&&fsi&&hash.IsNull()
            ? NativeShaderVariant::BuiltinAtomicDepthMaxV1 : ShaderVariant(fsi,hash.IsNull());
        auto found=shaders_.find(ShaderCacheLookup{digest,fsi,variant});
        if(found==shaders_.end()) {
            ++shader_cache_stats_.misses;
            const auto owned_digest=std::string(digest);
            const auto& provider=fsi?config_.fsi_shader:config_.shader;
            if((variant==NativeShaderVariant::ExactIdentityFsiStoresV1||
            variant==NativeShaderVariant::ImmutableFsiStateHoistV1||
            variant==NativeShaderVariant::ImmutableFsiStateAndIncomingDepthHoistV2)&&
               !(hash.typed&&hash.typed->owner&&config_.shader_snapshot)&&!config_.fsi_shader)
                throw std::runtime_error("Derived FSI module requires its source provider; canonical file fallback is forbidden");
            const auto input_start=DrawClock::now();
            auto bytes=hash.typed&&hash.typed->owner&&config_.shader_snapshot?config_.shader_snapshot(*hash.typed,fsi):
                hash.IsNull()&&config_.fsi_depth_shader?config_.fsi_depth_shader():
                !hash.IsNull()&&provider?provider(owned_digest):Read(compiled_/(owned_digest+(fsi?".fsi.spv":".spv")));
            shader_input_timing_.Record(DrawMilliseconds(input_start,DrawClock::now()),operation);
            if(bytes.size()<20||bytes.size()%4) throw std::runtime_error("Invalid compiled shader");
            const bool color_module_approved=config_.packed_color_render_pass_groups&&
                variant==NativeShaderVariant::Unmodified&&NativePackedColorModuleCertified(digest,fsi,bytes);
            const bool vertex_approved=(config_.builtin_atomic_depth||config_.packed_depth_render_pass_groups)&&!fsi&&
                AuditedAtomicDepthVertex(digest,bytes);
            bool packed_pixel_approved=config_.packed_depth_render_pass_groups&&fsi&&hash.IsNull()&&
                variant==NativeShaderVariant::Unmodified&&NativePackedDepthPassPixelCertified(bytes);
            const bool readonly_vertex_approved=(config_.builtin_readonly_depth_queries||config_.native_readonly_depth_attachment||config_.retained_shared_depth_queries)&&!fsi&&
                AuditedReadonlyQueryVertex(digest,bytes);
            const bool fp24_vertex_approved=config_.retained_fp24_depth_writer&&!fsi&&AuditedRetainedFp24Vertex(digest,bytes);
            bool early_depth_vertex_approved=false;
            if(config_.native_early_depth&&!fsi&&digest==kNativeEarlyDepthVertexSource)try {
                std::vector<uint32_t> words(bytes.size()/sizeof(uint32_t));
                std::memcpy(words.data(),bytes.data(),bytes.size());
                early_depth_vertex_approved=NativeEarlyDepthVertexCertified(words,digest,kNativeEarlyDepthCanonicalVersion);
            }catch(...) {/* Optional certificate refusal never changes the canonical module. */}
            bool early_depth_catalog_vertex_approved=false;
            if(config_.native_early_depth_catalog&&!fsi&&digest==kNativeEarlyDepthCatalogVertexSource)try {
                std::vector<uint32_t> words(bytes.size()/sizeof(uint32_t));
                std::memcpy(words.data(),bytes.data(),bytes.size());
                early_depth_catalog_vertex_approved=NativeEarlyDepthCatalogVertexCertified(words,digest,kNativeEarlyDepthCanonicalVersion);
            }catch(...) {/* Optional refusal never changes the canonical vertex module. */}
            bool null_constant_approved=false;
            if(config_.pinned_null_constant_uploads&&variant==NativeShaderVariant::Unmodified&&
               ((!fsi&&(digest==kAtomicDepthVertexSources[0]||digest==kAtomicDepthVertexSources[1]))||(fsi&&hash.IsNull())))try {
                std::vector<uint32_t> code(bytes.size()/4);std::memcpy(code.data(),bytes.data(),bytes.size());
                null_constant_approved=fsi?NativeNullDepthConstantPixelCertified(code,NativeShaderCompiler::CacheVersion()):
                    NativeNullDepthConstantVertexCertified(code,digest,NativeShaderCompiler::CacheVersion());
            }catch(...) {/* Optional footprint refusal keeps the original full upload. */}
            if(variant==NativeShaderVariant::BuiltinAtomicDepthMaxV1) {
                std::vector<uint32_t> words(bytes.size()/sizeof(uint32_t));
                std::memcpy(words.data(),bytes.data(),bytes.size());
                auto transformed=TryBuiltinAtomicDepth(words);
                if(transformed) {
                    // Transform accepted the exact pinned canonical input and
                    // emitted only the separately keyed audited atomic variant.
                    packed_pixel_approved=config_.packed_depth_render_pass_groups;
                    bytes.resize(transformed->size()*sizeof(uint32_t));
                    std::memcpy(bytes.data(),transformed->data(),bytes.size());
                } else {
                    atomic_builtin_refused_=true;++atomic_depth_stats_.module_refusals;
                    variant=ShaderVariant(fsi,hash.IsNull());
                    // Unsupported compiler output stays on the generic Native
                    // pipeline. Never use its key for the specialized module.
                    found=shaders_.find(ShaderCacheLookup{digest,fsi,variant});
                    if(found!=shaders_.end()) {
                        cache_epoch_.Mark(found->second);
                        if(selected_variant)*selected_variant=variant;
                        if(audited_vertex)*audited_vertex=found->second.audited_atomic_vertex;
                        if(null_constant_footprint)*null_constant_footprint=found->second.audited_null_constant_footprint;
                        return found->second.shader.get();
                    }
                }
            }
            if(variant==NativeShaderVariant::ImplicitCoherentInterlockV1) {
                // Provider bytes need not be uint32_t-aligned. Validate and
                // transform before cache admission or creating a Vulkan handle.
                std::vector<uint32_t> words(bytes.size()/sizeof(uint32_t));
                std::memcpy(words.data(),bytes.data(),bytes.size());
                words=TransformNativeFsiImplicitBarriers(words);
                bytes.resize(words.size()*sizeof(uint32_t));
                std::memcpy(bytes.data(),words.data(),bytes.size());
            }
            ReservePersistentCache(shaders_,bytes.size(),config_.max_shader_cache_entries,config_.max_shader_cache_bytes,
                shader_cache_stats_,[](const auto& entry){return entry.spirv_bytes;},
                [&](const auto& entry){return AsyncPinned(entry.shader.get())||entry.shader.get()==preparing_vertex;},[&]{FlushPendingDraws();});
            const auto module_start=DrawClock::now();
            auto shader=CreateShader(bytes.data(),bytes.size(),"main",RenderShaderFormat::SPIRV);
            shader_module_timing_.Record(DrawMilliseconds(module_start,DrawClock::now()),operation);
            if(!shader)throw std::runtime_error("Native shader module creation failed");
            found=shaders_.emplace(ShaderCacheIdentity{owned_digest,fsi,variant},CachedShader{std::move(shader),0,bytes.size(),vertex_approved,readonly_vertex_approved,fp24_vertex_approved,early_depth_vertex_approved,early_depth_catalog_vertex_approved,null_constant_approved,packed_pixel_approved,color_module_approved}).first;
            if(config_.native_early_depth_material_retention||config_.packed_color_render_pass_groups)
                found->second.audited_monotonic_depth=NativeMonotonicDepthModuleCertified(digest,fsi,variant,bytes);
            if(config_.packed_color_blend_pass_groups)
                found->second.audited_packed_color_blend=NativePackedColorBlendVariant8ModuleCertified(digest,fsi,variant,bytes);
            if(config_.packed_color_action_pass_groups)
                found->second.audited_packed_color_action=NativePackedColorActionVariant8ModuleCertified(digest,fsi,variant,bytes);
            if(config_.retained_opaque_phase) {
                found->second.audited_opaque_phase=NativeRetainedOpaquePhaseModuleCertified(digest,fsi,variant,bytes);
                found->second.audited_opaque_phase_canonical=fsi&&variant==NativeShaderVariant::Unmodified&&NativeRetainedOpaquePhaseCanonicalPixelCertified(digest,bytes);
            }
            shader_cache_stats_.bytes+=bytes.size();++shader_cache_stats_.creations;
        } else ++shader_cache_stats_.hits;
        cache_epoch_.Mark(found->second);
        if(selected_variant)*selected_variant=variant;
        if(audited_vertex)*audited_vertex=found->second.audited_atomic_vertex;
        if(null_constant_footprint)*null_constant_footprint=found->second.audited_null_constant_footprint;
        return found->second.shader.get();
    }
    RenderPipeline *Pipeline(const PipelineLookup &key, const RenderGraphicsPipelineDesc &description,
                             const DrawInput *operation = nullptr, bool *created = nullptr, uint32_t* diagnostic_pair = nullptr,
                             uint32_t* timing_pair = nullptr) {
        if (created)
            *created = false;
        const bool empty_prewarm=!operation&&config_.attachmentless_fsi&&key.color_format==-1&&key.depth_format==-1;
        if(empty_prewarm)++attachmentless_fsi_prewarm_attempts_;
        auto found = pipelines_.find(key);
        if(operation&&((key.pixel_variant==NativeShaderVariant::ConservativeEarlyDepthV1||key.pixel_variant==NativeShaderVariant::ConservativeEarlyDepthCatalogV2)||key.pixel_variant==NativeShaderVariant::RetainedFp24DepthWriterV1||IsNativeReadonlyDepthVariant(key.pixel_variant)||(key.pixel_variant==NativeShaderVariant::RetainedOpaquePackedAttachmentsV1||key.pixel_variant==NativeShaderVariant::RetainedOpaquePackedCatalogV2||key.pixel_variant==NativeShaderVariant::RetainedOpaquePhaseConservativeV4))&&
           (found==pipelines_.end()||!found->second.pipeline))
            throw std::logic_error("Native readonly D32 PSO must be resident before Draw");
        if(operation&&(key.pixel_variant==NativeShaderVariant::ReplayStencilMaterialObserveV1||
           key.pixel_variant==NativeShaderVariant::ReplayStencilMaterialCutoffV1||key.pixel_variant==NativeShaderVariant::StencilMaterialCutoffV1)&&
           (found==pipelines_.end()||!found->second.pipeline))
            throw std::logic_error("Stencil material diagnostic PSO must already be resident");
        if (found == pipelines_.end()) {
            ++pipeline_cache_stats_.misses;
            ReservePersistentCache(
                pipelines_, 0, config_.max_pipeline_cache_entries, UINT64_MAX, pipeline_cache_stats_,
                [](const auto &) { return uint64_t(0); }, [](const auto &) { return false; },
                [&] { FlushPendingDraws(); });
            found = pipelines_
                        .emplace(PipelineIdentity{OwnDigest(key.vertex), OwnDigest(key.pixel), key.raster,
                                                  key.color_format, key.depth_format,key.pixel_variant,key.vertex_variant,key.fsi_state},
                                 CachedPipeline{})
                        .first;
        } else
            ++pipeline_cache_stats_.hits;
        cache_epoch_.Mark(found->second);
        auto &pipeline = found->second.pipeline;
        if (!pipeline) {
            if(key.fsi_state&&operation)throw std::logic_error("FSI specialized PSO must be resident before Draw");
            const auto began = DrawClock::now();
            // Build canonical FSI state only on a cache miss. Ordinary hits
            // don't construct another large descriptor or rectangle layout.
            std::unique_ptr<RenderPipeline> candidate=config_.async_pipelines?TakeAsyncPipeline(key):nullptr;
            if (candidate) {
                // Built in the background from the same key, layout and modules.
            } else if (config_.native_fsi&&key.pixel_variant!=NativeShaderVariant::ConservativeEarlyDepthV1&&key.pixel_variant!=NativeShaderVariant::ConservativeEarlyDepthCatalogV2&&key.pixel_variant!=NativeShaderVariant::RetainedFp24DepthWriterV1&&!IsNativeReadonlyDepthVariant(key.pixel_variant)&&key.pixel_variant!=NativeShaderVariant::RetainedOpaquePackedAttachmentsV1&&key.pixel_variant!=NativeShaderVariant::RetainedOpaquePackedCatalogV2&&key.pixel_variant!=NativeShaderVariant::RetainedOpaquePhaseConservativeV4) {
                NativeFsiPipelineDescriptor owned(
                    std::get<std::array<uint32_t, 6>>(key.raster), key.color_format, key.depth_format,
                    description.pipelineLayout, description.vertexShader, description.pixelShader,
                    key.fsi_state?&*key.fsi_state:nullptr);
                candidate = device_->createGraphicsPipeline(owned.descriptor);
            } else
                candidate = device_->createGraphicsPipeline(description);
            if (operation)
                pipeline_creation_timing_.Record(DrawMilliseconds(began, DrawClock::now()), *operation);
            // Cache only successfully created handles. A skipped startup recipe
            // must not poison the later ordinary draw with a vk==0 cache hit.
            if (!candidate || !static_cast<VulkanGraphicsPipeline *>(candidate.get())->vk) {
                if(empty_prewarm)++attachmentless_fsi_prewarm_refusals_;
                throw std::runtime_error("Native draw pipeline creation failed");
            }
            pipeline = std::move(candidate);
            // A cached PSO may outlive its shader modules. Certify its actual
            // creation inputs once; replacement modules under the same key
            // cannot retroactively certify an older pipeline.
            if((config_.native_early_depth_material_retention||config_.packed_color_render_pass_groups)&&key.vertex&&key.pixel&&
               key.vertex_variant==NativeShaderVariant::Unmodified&&
               (key.pixel_variant==NativeShaderVariant::Unmodified||
                key.pixel_variant==NativeShaderVariant::PipelineStateEpilogueV1)) {
                const auto vs=shaders_.find(ShaderCacheLookup{*key.vertex,false,key.vertex_variant});
                const auto ps=shaders_.find(ShaderCacheLookup{*key.pixel,true,key.pixel_variant});
                found->second.audited_monotonic_depth=vs!=shaders_.end()&&ps!=shaders_.end()&&
                    vs->second.audited_monotonic_depth&&ps->second.audited_monotonic_depth&&
                    vs->second.shader.get()==description.vertexShader&&ps->second.shader.get()==description.pixelShader;
            }
            if(config_.packed_color_blend_pass_groups&&key.vertex&&key.pixel&&key.fsi_state&&
               key.vertex_variant==NativeShaderVariant::Unmodified&&key.pixel_variant==NativeShaderVariant::PipelineStateEpilogueV1&&
               std::holds_alternative<std::array<uint32_t,6>>(key.raster)&&
               NativePackedColorBlendVariant8Tuple(*key.vertex,*key.pixel,std::get<std::array<uint32_t,6>>(key.raster),
                   *key.fsi_state,key.color_format,key.depth_format)) {
                const auto vs=shaders_.find(ShaderCacheLookup{*key.vertex,false,key.vertex_variant});
                const auto ps=shaders_.find(ShaderCacheLookup{*key.pixel,true,key.pixel_variant});
                found->second.audited_packed_color_blend=vs!=shaders_.end()&&ps!=shaders_.end()&&
                    vs->second.audited_packed_color_blend&&ps->second.audited_packed_color_blend&&
                    vs->second.shader.get()==description.vertexShader&&ps->second.shader.get()==description.pixelShader;
            }
            if(config_.packed_color_action_pass_groups&&key.vertex&&key.pixel&&key.fsi_state&&
               key.vertex_variant==NativeShaderVariant::Unmodified&&key.pixel_variant==NativeShaderVariant::PipelineStateEpilogueV1&&
               std::holds_alternative<std::array<uint32_t,6>>(key.raster)&&
               NativePackedColorActionVariant8Tuple(*key.vertex,*key.pixel,std::get<std::array<uint32_t,6>>(key.raster),
                   *key.fsi_state,key.color_format,key.depth_format)) {
                const auto vs=shaders_.find(ShaderCacheLookup{*key.vertex,false,key.vertex_variant});
                const auto ps=shaders_.find(ShaderCacheLookup{*key.pixel,true,key.pixel_variant});
                found->second.audited_packed_color_action=vs!=shaders_.end()&&ps!=shaders_.end()&&
                    vs->second.audited_packed_color_action&&ps->second.audited_packed_color_action&&
                    vs->second.shader.get()==description.vertexShader&&ps->second.shader.get()==description.pixelShader;
            }
            if(config_.retained_opaque_phase&&key.vertex&&key.pixel&&
               key.vertex_variant==NativeShaderVariant::Unmodified&&key.pixel_variant==NativeShaderVariant::RetainedOpaquePhaseConservativeV4&&
               std::holds_alternative<std::array<uint32_t,6>>(key.raster)&&!key.fsi_state) {
                const auto& raster=std::get<std::array<uint32_t,6>>(key.raster);
                const auto vs=shaders_.find(ShaderCacheLookup{*key.vertex,false,key.vertex_variant});
                const auto ps=shaders_.find(ShaderCacheLookup{*key.pixel,true,key.pixel_variant});
                const auto& d=description;
                const bool exact=NativeRetainedOpaquePhaseRecipe(*key.vertex,*key.pixel,raster)&&
                    key.color_format==int32_t(RenderFormat::R32_UINT)&&key.depth_format==int32_t(RenderFormat::D32_FLOAT)&&
                    d.pipelineLayout==layout_.get()&&d.renderTargetCount==1&&d.renderTargetFormat[0]==RenderFormat::R32_UINT&&
                    !d.renderTargetBlend[0].blendEnabled&&d.renderTargetBlend[0].renderTargetWriteMask==1&&
                    d.depthTargetFormat==RenderFormat::D32_FLOAT&&d.depthEnabled&&d.depthWriteEnabled&&
                    d.depthFunction==RenderComparisonFunction::GREATER_EQUAL&&!d.stencilEnabled&&!d.dynamicDepthBiasEnabled&&
                    d.multisampling.sampleCount==1&&!d.multisampling.sampleLocationsEnabled&&!d.alphaToCoverageEnabled&&
                    uint32_t(d.cullMode)==raster[0]&&uint32_t(d.frontFace)==raster[1]&&uint32_t(d.depthClipEnabled)==raster[2]&&
                    uint32_t(d.primitiveTopology)==raster[3]&&d.specConstantsCount==1&&d.specConstants&&
                    d.specConstants[0].index==0&&d.specConstants[0].value==0&&!d.geometryShader&&!d.logicOpEnabled&&
                    !d.inputSlotsCount&&!d.inputElementsCount&&d.depthBias==0&&d.depthBiasClamp==0.f&&d.slopeScaledDepthBias==0.f&&
                    vs!=shaders_.end()&&ps!=shaders_.end()&&vs->second.audited_opaque_phase&&ps->second.audited_opaque_phase&&
                    vs->second.shader.get()==d.vertexShader&&ps->second.shader.get()==d.pixelShader;
                found->second.audited_opaque_phase=exact;
                if(exact) {found->second.opaque_phase_vertex=d.vertexShader;found->second.opaque_phase_pixel=d.pixelShader;}
            }
            ++pipeline_cache_stats_.creations;
            transfer_report_dirty_ = true;
            diagnostic_counters_.pipelines_created = diagnostic_counters_.pipelines_created.value_or(0) + 1;
            if (created)
                *created = true;
        }
        if(diagnostic_pair) {
            if(!found->second.diagnostic_pair_assigned) {
                found->second.diagnostic_pair=gpu_work_queue_->attribution()->InternPair(
                    key.vertex.value_or(std::string_view{}),key.pixel);
                found->second.diagnostic_pair_assigned=true;
            }
            *diagnostic_pair=found->second.diagnostic_pair;
        }
        if(timing_pair) {
            if(!found->second.timing_pair_assigned) {
                found->second.timing_pair=draw_timestamps_->InternPair(
                    key.vertex.value_or(std::string_view{}),key.pixel);
                found->second.timing_pair_assigned=true;
            }
            *timing_pair=found->second.timing_pair;
        }
        if(!operation&&config_.attachmentless_fsi&&key.color_format==-1&&key.depth_format==-1)
            ++attachmentless_fsi_prewarm_prepared_;
        return pipeline.get();
    }
    int32_t PrewarmFsiColorFormat(int32_t canonical) const noexcept {
        // Keep the disk recipe ABI canonical. Choose only this Scene's active
        // host policy during startup, avoiding a doubled742-PSO working set.
        // Real framebuffer creation refusal later uses ordinary Native on-demand
        // compilation with the separately keyed RGBA8 host format.
        return config_.attachmentless_fsi&&attachmentless_fsi_supported_&&!attachmentless_fsi_refused_?-1:canonical;
    }
    NativeOverflowPrewarmResult PrewarmFsiOverflowPipeline(const NativeFsiPipelineRecipe& recipe,
            std::span<const uint32_t> vertex,std::span<const uint32_t> pixel) {
        if(!config_.native_fsi||mutation_serial()||HasPendingGpuWork()||queries_->active()||!deferred_queries_.empty())
            throw std::runtime_error("Overflow pipeline prewarm requires an unexecuted packed Scene");
        if(!ValidNativeFsiPipelineRecipe(recipe)||vertex.size()<5||pixel.size()<5||
           vertex.front()!=0x07230203||pixel.front()!=0x07230203)
            return {NativeOverflowPrewarmStatus::InvalidRecipe};
        // No new derived variant, builtin alternative or source transform enters
        // this path. The supplied canonical spans have the ordinary API contract.
        if(ShaderVariant(false)!=NativeShaderVariant::Unmodified||
           ShaderVariant(true,!recipe.pixel)!=NativeShaderVariant::Unmodified)
            return {NativeOverflowPrewarmStatus::UnsupportedVariant};
        const PipelineLookup key{std::string_view(recipe.vertex),
            recipe.pixel?std::optional<std::string_view>(*recipe.pixel):std::nullopt,
            PipelineRasterKey(recipe.raster),PrewarmFsiColorFormat(recipe.color_format),recipe.depth_format,
            NativeShaderVariant::Unmodified};
        const auto existing=pipelines_.find(key);
        if(existing!=pipelines_.end()&&existing->second.pipeline)
            return {NativeOverflowPrewarmStatus::AlreadyResident};
        const auto resident_vs=shaders_.find(ShaderCacheLookup{recipe.vertex,false,NativeShaderVariant::Unmodified});
        const auto resident_ps=shaders_.find(ShaderCacheLookup{
            recipe.pixel?std::string_view(*recipe.pixel):std::string_view("depth-only"),true,NativeShaderVariant::Unmodified});
        const uint64_t temporary_count=uint64_t(resident_vs==shaders_.end())+uint64_t(resident_ps==shaders_.end());
        const uint64_t vertex_bytes=resident_vs==shaders_.end()?vertex.size_bytes():0;
        const uint64_t pixel_bytes=resident_ps==shaders_.end()?pixel.size_bytes():0;
        if(pixel_bytes>UINT64_MAX-vertex_bytes)return {NativeOverflowPrewarmStatus::ByteCap};
        const auto admission=NativeOverflowPrewarmAdmission(pipelines_.size(),config_.max_pipeline_cache_entries,
            shaders_.size(),config_.max_shader_cache_entries,shader_cache_stats_.bytes,config_.max_shader_cache_bytes,
            temporary_count,vertex_bytes+pixel_bytes);
        if(admission!=NativeOverflowPrewarmStatus::Prepared)return {admission};
        std::unique_ptr<RenderShader> temporary_vs,temporary_ps;
        auto* vs=resident_vs==shaders_.end()?nullptr:resident_vs->second.shader.get();
        auto* ps=resident_ps==shaders_.end()?nullptr:resident_ps->second.shader.get();
        try {
            if(!vs) {
                temporary_vs=CreateShader(vertex.data(),vertex.size_bytes(),"main",RenderShaderFormat::SPIRV);
                if(!temporary_vs||!static_cast<VulkanShader&>(*temporary_vs).vk)
                    return {NativeOverflowPrewarmStatus::ShaderCreationFailed};
                vs=temporary_vs.get();
            }
            if(!ps) {
                temporary_ps=CreateShader(pixel.data(),pixel.size_bytes(),"main",RenderShaderFormat::SPIRV);
                if(!temporary_ps||!static_cast<VulkanShader&>(*temporary_ps).vk)
                    return {NativeOverflowPrewarmStatus::ShaderCreationFailed};
                ps=temporary_ps.get();
            }
        }catch(const std::exception&) {return {NativeOverflowPrewarmStatus::ShaderCreationFailed};}
        RenderGraphicsPipelineDesc descriptor;
        descriptor.pipelineLayout=layout_.get();descriptor.vertexShader=vs;descriptor.pixelShader=ps;
        bool created=false;
        // Pipeline constructs the exact ordinary NativeFsiPipelineDescriptor.
        // The preflight leaves hard capacity, so it cannot evict a cached PSO.
        try {Pipeline(key,descriptor,nullptr,&created);}
        catch(const std::exception&) {return {NativeOverflowPrewarmStatus::PipelineCreationFailed};}
        // Vulkan consumes VkShaderModule objects synchronously at pipeline
        // creation. Only the resulting owned RenderPipeline enters pipelines_.
        // Local module owners end here; shaders_/epochs/accounting are untouched.
        return {created?NativeOverflowPrewarmStatus::Prepared:NativeOverflowPrewarmStatus::AlreadyResident,
            temporary_count,vertex_bytes+pixel_bytes};
    }
    bool StencilMaterialEnabled() const {return (config_.native_stencil_material_cutoff&&bool(config_.native_stencil_material_program))||config_.replay_stencil_material_mode;}
    const std::shared_ptr<const NativeStencilMaterialProgram>& StencilMaterialProgram() const {
        return config_.native_stencil_material_cutoff?config_.native_stencil_material_program:config_.replay_stencil_material_program;
    }
    NativeShaderVariant StencilMaterialVariant() const {
        if(config_.native_stencil_material_cutoff)return NativeShaderVariant::StencilMaterialCutoffV1;
        return config_.replay_stencil_material_mode==1?NativeShaderVariant::ReplayStencilMaterialObserveV1:
            NativeShaderVariant::ReplayStencilMaterialCutoffV1;
    }
    void PrewarmStencilMaterial(const NativeFsiPipelineRecipe& recipe,std::span<const uint32_t> vertex,std::span<const uint32_t> pixel) {
        if(!StencilMaterialEnabled()||recipe.vertex!=kStencilMaterialVertex||!recipe.pixel||
           *recipe.pixel!=kStencilMaterialPixel||recipe.raster!=std::array<uint32_t,6>{3,2,1,4,0,0}||
           recipe.color_format!=int32_t(RenderFormat::R8G8B8A8_UNORM)||recipe.depth_format!=-1)return;
        if(!NativeStencilMaterialCanonicalCertified(vertex,true)||!NativeStencilMaterialCanonicalCertified(pixel,false))
            throw std::runtime_error("Stencil material original prewarm modules refused");
        auto vs=shaders_.find(ShaderCacheLookup{recipe.vertex,false,NativeShaderVariant::Unmodified});
        auto ps=shaders_.find(ShaderCacheLookup{*recipe.pixel,true,NativeShaderVariant::Unmodified});
        if(vs==shaders_.end()||ps==shaders_.end())throw std::runtime_error("Stencil material original modules not resident");
        if(!vs->second.audited_stencil_material||!ps->second.audited_stencil_material)
            throw std::runtime_error("Stencil material resident module identity was not authenticated at creation");
        const auto variant=StencilMaterialVariant();
        const PipelineLookup key{std::string_view(recipe.vertex),std::string_view(*recipe.pixel),PipelineRasterKey(recipe.raster),
            PrewarmFsiColorFormat(recipe.color_format),-1,variant};
        if(pipelines_.find(key)!=pipelines_.end())return;
        const auto& program=*StencilMaterialProgram();
        const uint64_t bytes=program.spirv.size()*sizeof(uint32_t);
        if(stencil_material_stats_.prepared||pipelines_.size()>=config_.max_pipeline_cache_entries||
           shaders_.size()>=config_.max_shader_cache_entries||bytes>config_.max_shader_cache_bytes||
           shader_cache_stats_.bytes>config_.max_shader_cache_bytes-bytes)
            throw std::runtime_error("Stencil material bounded startup capacity refused");
        auto shader=CreateShader(program.spirv.data(),bytes,"main",RenderShaderFormat::SPIRV);
        if(!shader||!static_cast<VulkanShader&>(*shader).vk)throw std::runtime_error("Stencil material module creation failed");
        auto derived=shaders_.emplace(ShaderCacheIdentity{*recipe.pixel,true,variant},CachedShader{std::move(shader),0,bytes}).first;
        cache_epoch_.Mark(vs->second);cache_epoch_.Mark(ps->second);cache_epoch_.Mark(derived->second);
        shader_cache_stats_.bytes+=bytes;++shader_cache_stats_.creations;++shader_cache_stats_.misses;
        RenderGraphicsPipelineDesc p;p.pipelineLayout=layout_.get();p.vertexShader=vs->second.shader.get();p.pixelShader=derived->second.shader.get();
        if(!Pipeline(key,p))throw std::runtime_error("Stencil material startup pipeline failed");
        ++stencil_material_stats_.prepared;
    }
    bool PrewarmStencilMaterialPipeline(const NativeFsiPipelineRecipe& recipe,std::span<const uint32_t> vertex,std::span<const uint32_t> pixel) {
        if(!config_.native_stencil_material_cutoff||!config_.native_stencil_material_program)return false;
        if(!config_.native_fsi||mutation_serial()||HasPendingGpuWork()||queries_->active()||!deferred_queries_.empty())
            throw std::invalid_argument("Stencil material prewarm requires an unexecuted packed Scene");
        if(!NativeStencilMaterialCanonicalCertified(vertex,true)||!NativeStencilMaterialCanonicalCertified(pixel,false))return false;
        if(recipe.vertex!=kStencilMaterialVertex||!recipe.pixel||*recipe.pixel!=kStencilMaterialPixel||
           recipe.raster!=std::array<uint32_t,6>{3,2,1,4,0,0}||
           recipe.color_format!=int32_t(RenderFormat::R8G8B8A8_UNORM)||recipe.depth_format!=-1)return false;
        if(stencil_material_stats_.prepared) {
            const PipelineLookup key{std::string_view(recipe.vertex),std::string_view(*recipe.pixel),PipelineRasterKey(recipe.raster),
                PrewarmFsiColorFormat(recipe.color_format),-1,StencilMaterialVariant()};
            const auto found=pipelines_.find(key);return found!=pipelines_.end()&&bool(found->second.pipeline);
        }
        // Called once after generic startup preparation. Reestablish the exact
        // original pair if earlier cache pressure evicted it; no Draw compiles.
        PrewarmFsiPipeline(recipe,vertex,pixel);PrewarmStencilMaterial(recipe,vertex,pixel);
        return stencil_material_stats_.prepared==1;
    }
    bool StencilMaterialOwnedCube(const DrawInput& o) {
        if(o.Function()!="BeginIndexedVertices")return false;
        const auto args=o.Args();
        if(args[1]!=4||args[2]!=0||args[3]!=8||args[4]!=36||args[5]!=1||args[6]!=12)return false;
        bool valid=true;unsigned slots=0,vertices=0,indices=0;
        o.VertexSlots([&](unsigned slot){valid=valid&&slot==95;++slots;});
        o.Geometry(false,[&](const DrawGeometryInput&){valid=false;});
        o.Geometry(true,[&](const DrawGeometryInput& g){
            if(g.GpuMemory()||g.FetchSlot()){valid=false;return;}
            if(g.Kind()==96&&g.Bytes()==96) {++vertices;const auto data=ReadPayload(g.Payload());valid=valid&&data.bytes().size()==96;}
            else if(g.Kind()==97&&g.Bytes()==72) {++indices;const auto data=ReadPayload(g.Payload());valid=valid&&ContentHash(data.bytes())==kStencilMaterialIndexSha;}
            else valid=false;
        });
        return valid&&slots==1&&vertices==1&&indices==1;
    }
    void PrewarmFsiPipeline(const NativeFsiPipelineRecipe &recipe, std::span<const uint32_t> vertex,
                            std::span<const uint32_t> pixel) {
        if (!config_.native_fsi || mutation_serial() || HasPendingGpuWork() || queries_->active() ||
            !deferred_queries_.empty())
            throw std::runtime_error("Pipeline prewarm requires an unexecuted packed Scene");
        if (!ValidNativeFsiPipelineRecipe(recipe))
            throw std::invalid_argument("Invalid native pipeline recipe");
        const auto module = [&](std::string_view digest, bool fsi, std::span<const uint32_t> code,
                                const RenderShader *protect, NativeShaderVariant variant,
                                bool *audited_vertex = nullptr,bool pinned_atomic = false) {
            const ShaderCacheLookup key{digest, fsi, variant};
            auto found = shaders_.find(key);
            if (found == shaders_.end()) {
                if (code.size() < 5 || code.front() != 0x07230203)
                    throw std::invalid_argument("Invalid prepared SPIR-V");
                const bool color_module_approved=config_.packed_color_render_pass_groups&&
                    variant==NativeShaderVariant::Unmodified&&NativePackedColorModuleCertified(digest,fsi,
                        {reinterpret_cast<const uint8_t*>(code.data()),code.size_bytes()});
                const bool vertex_approved=(config_.builtin_atomic_depth||config_.packed_depth_render_pass_groups)&&!fsi&&
                    AuditedAtomicDepthVertex(digest,{reinterpret_cast<const uint8_t*>(code.data()),code.size_bytes()});
                bool packed_pixel_approved=config_.packed_depth_render_pass_groups&&fsi&&digest=="depth-only"&&
                    ((variant==NativeShaderVariant::Unmodified&&NativePackedDepthPassPixelCertified(
                        {reinterpret_cast<const uint8_t*>(code.data()),code.size_bytes()}))||
                     (variant==NativeShaderVariant::BuiltinAtomicDepthMaxV1&&pinned_atomic));
                const bool readonly_vertex_approved=(config_.builtin_readonly_depth_queries||config_.native_readonly_depth_attachment||config_.retained_shared_depth_queries)&&!fsi&&
                    AuditedReadonlyQueryVertex(digest,{reinterpret_cast<const uint8_t*>(code.data()),code.size_bytes()});
                const bool fp24_vertex_approved=config_.retained_fp24_depth_writer&&!fsi&&
                    AuditedRetainedFp24Vertex(digest,{reinterpret_cast<const uint8_t*>(code.data()),code.size_bytes()});
                bool early_depth_vertex_approved=false;
                if(config_.native_early_depth&&!fsi&&digest==kNativeEarlyDepthVertexSource)try {
                    early_depth_vertex_approved=NativeEarlyDepthVertexCertified(code,digest,kNativeEarlyDepthCanonicalVersion);
                }catch(...) {/* Optional certificate refusal preserves ordinary startup admission. */}
                bool early_depth_catalog_vertex_approved=false;
                if(config_.native_early_depth_catalog&&!fsi&&digest==kNativeEarlyDepthCatalogVertexSource)try {
                    early_depth_catalog_vertex_approved=NativeEarlyDepthCatalogVertexCertified(code,digest,kNativeEarlyDepthCanonicalVersion);
                }catch(...) {/* Optional refusal never changes the canonical vertex module. */}
                bool null_constant_approved=false;
                if(config_.pinned_null_constant_uploads&&variant==NativeShaderVariant::Unmodified&&
                   ((!fsi&&(digest==kAtomicDepthVertexSources[0]||digest==kAtomicDepthVertexSources[1]))||(fsi&&digest=="depth-only")))try {
                    null_constant_approved=fsi&&digest=="depth-only"?NativeNullDepthConstantPixelCertified(code,NativeShaderCompiler::CacheVersion()):
                        !fsi&&NativeNullDepthConstantVertexCertified(code,digest,NativeShaderCompiler::CacheVersion());
                }catch(...) {/* Optional footprint refusal keeps the original full upload. */}
                std::vector<uint32_t> transformed;
                if(variant==NativeShaderVariant::ImplicitCoherentInterlockV1) {
                    transformed=TransformNativeFsiImplicitBarriers(code);
                    code=transformed;
                }
                ReservePersistentCache(
                    shaders_, code.size_bytes(), config_.max_shader_cache_entries,
                    config_.max_shader_cache_bytes, shader_cache_stats_,
                    [](const auto &entry) { return entry.spirv_bytes; },
                    [&](const auto &entry) { return entry.shader.get() == protect; },
                    [&] { FlushPendingDraws(); });
                auto shader =
                    CreateShader(code.data(), code.size_bytes(), "main", RenderShaderFormat::SPIRV);
                if (!shader || !static_cast<VulkanShader *>(shader.get())->vk)
                    throw std::runtime_error("Native prewarm shader module failed");
                found = shaders_
                            .emplace(ShaderCacheIdentity{std::string(digest), fsi, variant},
                                     CachedShader{std::move(shader), 0, code.size_bytes(),vertex_approved,readonly_vertex_approved,fp24_vertex_approved,early_depth_vertex_approved,early_depth_catalog_vertex_approved,null_constant_approved,packed_pixel_approved,color_module_approved})
                            .first;
                if(config_.native_early_depth_material_retention||config_.packed_color_render_pass_groups)
                    found->second.audited_monotonic_depth=NativeMonotonicDepthModuleCertified(digest,fsi,variant,
                        {reinterpret_cast<const uint8_t*>(code.data()),code.size_bytes()});
                if(config_.packed_color_blend_pass_groups)
                    found->second.audited_packed_color_blend=NativePackedColorBlendVariant8ModuleCertified(digest,fsi,variant,
                        {reinterpret_cast<const uint8_t*>(code.data()),code.size_bytes()});
                if(config_.packed_color_action_pass_groups)
                    found->second.audited_packed_color_action=NativePackedColorActionVariant8ModuleCertified(digest,fsi,variant,
                        {reinterpret_cast<const uint8_t*>(code.data()),code.size_bytes()});
                if(config_.retained_opaque_phase) {
                    const auto bytes=std::span(reinterpret_cast<const uint8_t*>(code.data()),code.size_bytes());
                    found->second.audited_opaque_phase=NativeRetainedOpaquePhaseModuleCertified(digest,fsi,variant,bytes);
                    found->second.audited_opaque_phase_canonical=fsi&&variant==NativeShaderVariant::Unmodified&&NativeRetainedOpaquePhaseCanonicalPixelCertified(digest,bytes);
                }
                if(StencilMaterialEnabled()&&variant==NativeShaderVariant::Unmodified&&
                   ((!fsi&&digest==kStencilMaterialVertex)||(fsi&&digest==kStencilMaterialPixel)))
                    found->second.audited_stencil_material=NativeStencilMaterialCanonicalCertified(code,!fsi);
                ++shader_cache_stats_.misses;
                ++shader_cache_stats_.creations;
                shader_cache_stats_.bytes += code.size_bytes();
            } else
                ++shader_cache_stats_.hits;
            cache_epoch_.Mark(found->second);
            transfer_report_dirty_ = true;
            if (audited_vertex)
                *audited_vertex = found->second.audited_atomic_vertex;
            return found->second.shader.get();
        };
        bool audited_vertex = false;
        auto *vs = module(recipe.vertex, false, vertex, nullptr, ShaderVariant(false), &audited_vertex);
        auto *ps = module(recipe.pixel ? std::string_view(*recipe.pixel) : std::string_view("depth-only"),
                          true, pixel, vs, ShaderVariant(true,!recipe.pixel));
        RenderGraphicsPipelineDesc descriptor;
        descriptor.pipelineLayout = layout_.get();
        descriptor.vertexShader = vs;
        descriptor.pixelShader = ps;
        const PipelineLookup key{std::string_view(recipe.vertex),
                                 recipe.pixel ? std::optional<std::string_view>(*recipe.pixel) : std::nullopt,
                                 PipelineRasterKey(recipe.raster), PrewarmFsiColorFormat(recipe.color_format), recipe.depth_format,ShaderVariant(true,!recipe.pixel)};
        Pipeline(key, descriptor);
        if(config_.replay_stencil_material_mode)PrewarmStencilMaterial(recipe,vertex,pixel);

        // A persisted recipe always prepares its generic Native pipeline first.
        // The recipe does not assert dynamic depth/query eligibility. Prepare
        // the separately keyed alternative only from the actual audited VS and
        // pinned builtin; Draw still rechecks every live eligibility condition.
        if (!config_.builtin_atomic_depth || recipe.pixel || !audited_vertex || atomic_builtin_refused_)
            return;
        auto atomic_code = TryBuiltinAtomicDepth(pixel);
        if (!atomic_code) {
            atomic_builtin_refused_ = true;
            ++atomic_depth_stats_.module_refusals;
            return; // Keep the already prepared generic pipeline usable.
        }
        auto atomic_key = key;
        atomic_key.pixel_variant = NativeShaderVariant::BuiltinAtomicDepthMaxV1;
        const ShaderCacheLookup atomic_module{"depth-only", true, atomic_key.pixel_variant};
        const uint64_t atomic_bytes = atomic_code->size() * sizeof(uint32_t);
        // Optional extra warmup must not evict any generic entry at admission.
        // With deliberately small limits, leave the ordinary on-demand path to
        // perform its existing fenced eviction. No startup GPU work is added.
        if ((!pipelines_.contains(atomic_key) && pipelines_.size() >= config_.max_pipeline_cache_entries) ||
            (!shaders_.contains(atomic_module) &&
             (shaders_.size() >= config_.max_shader_cache_entries ||
              atomic_bytes > config_.max_shader_cache_bytes ||
              shader_cache_stats_.bytes > config_.max_shader_cache_bytes - atomic_bytes)))
            return;
        descriptor.pixelShader = module("depth-only", true, *atomic_code, vs, atomic_key.pixel_variant,nullptr,true);
        Pipeline(atomic_key, descriptor);
    }
    bool PrewarmFsiStatePipeline(const NativeFsiStateRecipe& state_recipe,std::span<const uint32_t> vertex) {
        if(!config_.native_fsi||mutation_serial()||HasPendingGpuWork()||queries_->active()||!deferred_queries_.empty())
            throw std::runtime_error("FSI state prewarm requires an unexecuted packed Scene");
        const auto& recipe=state_recipe.generic;
        if(!config_.specialize_fsi_pipeline_state||config_.builtin_readonly_depth_queries||!config_.fsi_state_programs||!ValidFsiStateRecipe(state_recipe))return false;
        if(config_.fsi_state_material_composition&&!FsiStateMaterialCompositionSourceAllowed(
                recipe.vertex,recipe.pixel?std::string_view(*recipe.pixel):std::string_view{},
                config_.fsi_state_inactive_early_catalog,config_.native_early_depth_catalog)) {
            ++fsi_state_stats_.prewarm_refusals;return false;
        }
        if(config_.fsi_state_forward_cfg&&!FsiStateForwardCfgAllowed(state_recipe)) {
            ++fsi_state_stats_.prewarm_refusals;return false;
        }
        const auto& programs=*config_.fsi_state_programs;
        const auto source=recipe.pixel?programs.programs.find(*recipe.pixel):programs.programs.end();
        const auto program=recipe.pixel?(source==programs.programs.end()?nullptr:source->second):programs.depth_only;
        const auto expected_version=config_.fsi_state_forward_cfg?kNativeFsiStateForwardCfgShaderVersion:kNativeFsiStateSpecializationShaderVersion;
        if(!program||program->variant_version!=expected_version||program->compiler_version.empty()||
            (config_.fsi_state_forward_cfg&&program->compiler_version!=kFsiStateForwardCfgBaselineCompiler)||
            program->depth_only!=!recipe.pixel||(recipe.pixel&&program->source_sha256!=*recipe.pixel)) {
            ++fsi_state_stats_.prewarm_refusals;return false;
        }
        const int32_t host_color=PrewarmFsiColorFormat(recipe.color_format);
        const PipelineLookup generic{std::string_view(recipe.vertex),
            recipe.pixel?std::optional<std::string_view>(*recipe.pixel):std::nullopt,
            PipelineRasterKey(recipe.raster),host_color,recipe.depth_format,ShaderVariant(true,!recipe.pixel)};
        const auto original=pipelines_.find(generic);
        const auto vs_entry=shaders_.find(ShaderCacheLookup{recipe.vertex,false,NativeShaderVariant::Unmodified});
        if(original==pipelines_.end()||!original->second.pipeline||vs_entry==shaders_.end()||
            vertex.size()<5||vertex[0]!=0x07230203){++fsi_state_stats_.prewarm_refusals;return false;}
        auto key=generic;key.pixel_variant=FsiStatePixelVariant();key.fsi_state=state_recipe.state;
        const ShaderCacheLookup module_key{recipe.pixel?std::string_view(*recipe.pixel):std::string_view("depth-only"),true,key.pixel_variant};
        const uint64_t bytes=program->spirv.size()*sizeof(uint32_t);
        // Count only missing entries. Preserve every generic owner and all hard
        // limits. Successful optional admissions reserve soft-trim headroom.
        // The hard limits and generic admission/eviction policy remain unchanged.
        const size_t pipeline_limit=config_.max_pipeline_cache_entries;
        const size_t module_limit=config_.max_shader_cache_entries;
        const uint64_t byte_limit=config_.max_shader_cache_bytes;
        const auto candidate=pipelines_.find(key);
        const bool resident=candidate!=pipelines_.end()&&candidate->second.pipeline;
        if((!resident&&fsi_state_stats_.prepared>=kFsiStateMaxPipelines)||
           (candidate==pipelines_.end()&&pipelines_.size()>=pipeline_limit)||
           (!shaders_.contains(module_key)&&(fsi_state_stats_.modules_created>=kFsiStateMaxPipelines||
            bytes>16u*1024*1024-fsi_state_stats_.module_bytes||shaders_.size()>=module_limit||bytes>byte_limit||
            shader_cache_stats_.bytes>byte_limit-bytes))) {++fsi_state_stats_.prewarm_refusals;return false;}
        auto* vs=vs_entry->second.shader.get();cache_epoch_.Mark(vs_entry->second);
        auto ps=shaders_.find(module_key);
        if(ps==shaders_.end()) {
            auto shader=CreateShader(program->spirv.data(),bytes,"main",RenderShaderFormat::SPIRV);
            if(!shader||!static_cast<VulkanShader*>(shader.get())->vk)throw std::runtime_error("FSI state module creation failed");
            ps=shaders_.emplace(ShaderCacheIdentity{std::string(module_key.digest),true,module_key.variant},
                CachedShader{std::move(shader),0,bytes,false}).first;
            if(config_.native_early_depth_material_retention||config_.packed_color_render_pass_groups)
                ps->second.audited_monotonic_depth=NativeMonotonicDepthModuleCertified(module_key.digest,true,module_key.variant,
                    {reinterpret_cast<const uint8_t*>(program->spirv.data()),bytes});
            if(config_.packed_color_blend_pass_groups)
                ps->second.audited_packed_color_blend=NativePackedColorBlendVariant8ModuleCertified(module_key.digest,true,module_key.variant,
                    {reinterpret_cast<const uint8_t*>(program->spirv.data()),bytes});
            if(config_.packed_color_action_pass_groups)
                ps->second.audited_packed_color_action=NativePackedColorActionVariant8ModuleCertified(module_key.digest,true,module_key.variant,
                    {reinterpret_cast<const uint8_t*>(program->spirv.data()),bytes});
            shader_cache_stats_.bytes+=bytes;++shader_cache_stats_.creations;++shader_cache_stats_.misses;
            ++fsi_state_stats_.modules_created;fsi_state_stats_.module_bytes+=bytes;
        } else ++shader_cache_stats_.hits;
        cache_epoch_.Mark(ps->second);
        RenderGraphicsPipelineDesc descriptor;descriptor.pipelineLayout=layout_.get();descriptor.vertexShader=vs;
        descriptor.pixelShader=ps->second.shader.get();
        Pipeline(key,descriptor);
        if(!resident)++fsi_state_stats_.prepared;transfer_report_dirty_=true;return true;
    }
    bool RetainedOpaqueEnabled()const {return config_.retained_opaque_writer||config_.retained_opaque_catalog;}
    NativeShaderVariant RetainedOpaqueVariant()const {return config_.retained_opaque_catalog?
        NativeShaderVariant::RetainedOpaquePackedCatalogV2:NativeShaderVariant::RetainedOpaquePackedAttachmentsV1;}
    const NativeRetainedOpaqueProgram* RetainedOpaqueProgramFor(std::string_view vertex,std::string_view pixel)const {
        if(config_.retained_opaque_catalog) {
            if(!RetainedOpaqueCatalogPair(vertex,pixel)||!config_.retained_opaque_catalog_programs)return nullptr;
            for(const auto& program:config_.retained_opaque_catalog_programs->programs)
                if(program&&program->vertex_sha256==vertex&&program->source_sha256==pixel)return program.get();
            return nullptr;
        }
        return config_.retained_opaque_writer&&vertex==kRetainedOpaqueVertex&&pixel==kRetainedOpaquePixel?
            config_.retained_opaque_program.get():nullptr;
    }
    static SurfaceLayout RetainedColor() {return {1280,720,1280,720,3,false};}
    static SurfaceLayout RetainedDepth() {return {1280,720,1280,0,1,true};}
    const NativeRetainedOpaquePhaseProgram* RetainedOpaquePhaseProgramFor(std::string_view pixel)const {
        if(!config_.retained_opaque_phase||!config_.retained_opaque_phase_programs)return nullptr;
        for(const auto& program:config_.retained_opaque_phase_programs->programs)
            if(program&&program->source_sha256==pixel)return program.get();
        return nullptr;
    }
    static const void* RetainedOpaquePhaseStateOwner(const DrawInput& o)noexcept {
        if(!o.typed)return nullptr;
        return o.typed->lazy_device_state?static_cast<const void*>(o.typed->lazy_device_state.get()):
            static_cast<const void*>(o.typed->device_state.owner.get());
    }
    bool RetainedOpaquePhaseStaticDrawAllowed(const DrawInput& o,std::array<uint32_t,6>& out_raster) {
        if(o.Rectangle()||o.Shader(false).IsNull()||o.Shader(true).IsNull()||
           !RetainedOpaquePhaseProgramFor(o.Shader(true).Digest())||o.Function()!="DrawIndexedPrimitive"||o.Args()[1]!=4)return false;
        bool cpu_geometry=true;o.Geometry(false,[&](const auto& g){if(g.GpuMemory())cpu_geometry=false;});
        if(!cpu_geometry)return false;
        const auto surfaces=o.Surfaces();if(!surfaces[0]||!surfaces[4])return false;
        for(unsigned i=1;i<4;++i)if(surfaces[i])return false;
        const auto c=SurfaceStorage(surfaces[0]),d=SurfaceStorage(surfaces[4]);
        const NativeRetainedOpaqueLayout layout{c.width,c.height,c.pitch,c.base,c.format,uint32_t(c.depth),
            d.width,d.height,d.pitch,d.base,d.format,uint32_t(d.depth)};
        if(layout!=kNativeRetainedOpaqueLayout||(surfaces[0].Info()&0x30000)||(surfaces[4].Info()&0x30000))return false;
        const auto payload=ReadPayload(o.State());const auto raw=payload.bytes();
        if(BE(raw,0x2934)!=0x700766||BE(raw,0x28dc)!=15||BE(raw,0x2938)!=0x10001||(BE(raw,0x293c)&24))return false;
        const auto mode=BE(raw,0x2948);
        for(unsigned face=0;face<2;++face)if(mode&(1u<<(11+face)))
            for(unsigned component=0;component<2;++component)
                if(std::bit_cast<float>(BE(raw,0x2a50+face*8+component*4))!=0.f)return false;
        if(std::bit_cast<float>(BE(raw,0x2918))!=-1.f||std::bit_cast<float>(BE(raw,0x291c))!=1.f)return false;
        std::array<uint32_t,kDrawStateWords> words;
        for(unsigned i=0;i<words.size();++i)words[i]=BE(raw,kDrawStateOffset+i*4);
        words[(0x2934-kDrawStateOffset)/4]=0;words[(0x2948-kDrawStateOffset)/4]&=~(7u<<11);
        const auto state=DecodeDrawState(words,1280,720,4,true);const auto& p=state.pipeline;
        const std::array<uint32_t,6> raster{uint32_t(p.cullMode),uint32_t(p.frontFace),uint32_t(p.depthClipEnabled),uint32_t(p.primitiveTopology),state.specialization,0};
        if(state.discard||state.scissor.left<0||state.scissor.top<0||state.scissor.right>1280||state.scissor.bottom>720||
           state.scissor.left>=state.scissor.right||state.scissor.top>=state.scissor.bottom||
           p.multisampling.sampleCount!=1||p.multisampling.sampleLocationsEnabled||p.alphaToCoverageEnabled||
           !NativeRetainedOpaquePhaseRecipe(o.Shader(false).Digest(),o.Shader(true).Digest(),raster))return false;
        out_raster=raster;return true;
    }
    bool RetainedOpaquePhaseResidentDrawAllowed(const DrawInput& o,const std::array<uint32_t,6>& raster) {
        const auto variant=NativeShaderVariant::RetainedOpaquePhaseConservativeV4;
        const auto vs=shaders_.find(ShaderCacheLookup{o.Shader(false).Digest(),false,NativeShaderVariant::Unmodified});
        const auto ps=shaders_.find(ShaderCacheLookup{o.Shader(true).Digest(),true,variant});
        const PipelineLookup key{o.Shader(false).OptionalDigest(),o.Shader(true).OptionalDigest(),PipelineRasterKey(raster),
            int32_t(RenderFormat::R32_UINT),int32_t(RenderFormat::D32_FLOAT),variant};
        const auto pipeline=pipelines_.find(key);
        return vs!=shaders_.end()&&ps!=shaders_.end()&&vs->second.audited_opaque_phase&&ps->second.audited_opaque_phase&&
            pipeline!=pipelines_.end()&&pipeline->second.pipeline&&pipeline->second.audited_opaque_phase&&
            pipeline->second.opaque_phase_vertex==vs->second.shader.get()&&pipeline->second.opaque_phase_pixel==ps->second.shader.get();
    }
    bool RetainedOpaquePhaseDrawAllowed(const DrawInput& o,std::array<uint32_t,6>* out_raster=nullptr,
            bool require_resident=true,NativeRetainedOpaquePhaseDrawFacts* facts=nullptr) {
        // Inclusive host predicate time (including live residency checks), not
        // GPU time or an additive per-draw stage. No clock/counters when OFF.
        struct PredicateTimingScope {
            State& state;DrawClock::time_point start;
            ~PredicateTimingScope() {
                if(!state.time_cpu_stages_)return;
                ++state.retained_phase_predicate_timing_.calls;
                state.retained_phase_predicate_timing_.elapsed_ms+=DrawMilliseconds(start,DrawClock::now());
            }
        } timing{*this,time_cpu_stages_?DrawClock::now():DrawClock::time_point{}};
        // Preserve live guards at every original call site. The final selected
        // PSO check below still re-reads actual shader/pipeline owners each time.
        if(!config_.retained_opaque_phase||!retained_opaque_writer_||!retained_opaque_writer_->supported()||
           queries_->active()||!config_.debug_pixels.empty())return false;
        if(facts)return facts->Allowed(o.typed,RetainedOpaquePhaseStateOwner(o),out_raster,require_resident,
            [&](auto& raster){return RetainedOpaquePhaseStaticDrawAllowed(o,raster);},
            [&](const auto& raster){return RetainedOpaquePhaseResidentDrawAllowed(o,raster);});
        // JSON and provider-only typed inputs keep their original reads/order.
        std::array<uint32_t,6> raster;
        if(!RetainedOpaquePhaseStaticDrawAllowed(o,raster))return false;
        if(out_raster)*out_raster=raster;
        if(!require_resident)return true;
        return RetainedOpaquePhaseResidentDrawAllowed(o,raster);
    }
    bool PrewarmRetainedOpaquePhasePipeline(const NativeFsiPipelineRecipe& recipe,std::span<const uint32_t> vertex,std::span<const uint32_t> pixel) {
        if(!config_.retained_opaque_phase||!recipe.pixel||!retained_opaque_writer_||!retained_opaque_writer_->supported())return false;
        if(mutation_serial()||HasPendingGpuWork()||queries_->active()||!deferred_queries_.empty())
            throw std::runtime_error("Opaque phase prewarm requires an unexecuted Scene");
        const auto refuse=[&]{++retained_opaque_stats_.prewarm_refusals;return false;};
        const auto* entry=NativeRetainedOpaquePhaseRecipe(recipe.vertex,*recipe.pixel,recipe.raster);
        const auto* program=RetainedOpaquePhaseProgramFor(*recipe.pixel);
        if(!entry||!program||!ValidNativeFsiPipelineRecipe(recipe)||
           recipe.color_format!=int32_t(RenderFormat::R8G8B8A8_UNORM)||recipe.depth_format!=-1||
           program->baseline_version!=NativeShaderCompiler::CacheVersion()||program->variant_version!=NativeRetainedOpaquePhaseConservativeCompilerVersion()||
           program->specialization_mask!=1794)return refuse();
        const auto variant=NativeShaderVariant::RetainedOpaquePhaseConservativeV4;
        const auto bytes=std::span(reinterpret_cast<const uint8_t*>(program->spirv.data()),program->spirv.size()*sizeof(uint32_t));
        if(!NativeRetainedOpaquePhaseModuleCertified(recipe.vertex,false,NativeShaderVariant::Unmodified,
                {reinterpret_cast<const uint8_t*>(vertex.data()),vertex.size_bytes()})||
           !NativeRetainedOpaquePhaseCanonicalPixelCertified(*recipe.pixel,{reinterpret_cast<const uint8_t*>(pixel.data()),pixel.size_bytes()})||
           !NativeRetainedOpaquePhaseModuleCertified(*recipe.pixel,true,variant,bytes)||ContentHash(bytes)!=program->spirv_sha256)return refuse();
        const auto vs=shaders_.find(ShaderCacheLookup{recipe.vertex,false,NativeShaderVariant::Unmodified});
        const auto original_ps=shaders_.find(ShaderCacheLookup{*recipe.pixel,true,NativeShaderVariant::Unmodified});
        const PipelineLookup original{std::string_view(recipe.vertex),std::string_view(*recipe.pixel),PipelineRasterKey(recipe.raster),recipe.color_format,recipe.depth_format};
        const auto original_pipeline=pipelines_.find(original);
        if(vs==shaders_.end()||!vs->second.audited_opaque_phase||original_ps==shaders_.end()||
           !original_ps->second.audited_opaque_phase_canonical||original_pipeline==pipelines_.end()||!original_pipeline->second.pipeline)return refuse();
        const PipelineLookup key{std::string_view(recipe.vertex),std::string_view(*recipe.pixel),PipelineRasterKey(recipe.raster),
            int32_t(RenderFormat::R32_UINT),int32_t(RenderFormat::D32_FLOAT),variant};
        const auto existing=pipelines_.find(key);
        if(existing!=pipelines_.end()&&existing->second.pipeline)return existing->second.audited_opaque_phase;
        const auto pixel_index=NativeRetainedOpaquePhasePixelIndex(*recipe.pixel);
        const auto recipe_index=size_t(entry-kRetainedOpaquePhaseRecipes.data());
        if(!pixel_index)return refuse();
        // Unlike V2, this path appends to all original startup priorities and
        // never evicts an ordinary module/PSO to admit an optional entry.
        if(pipelines_.size()>=config_.max_pipeline_cache_entries)return refuse();
        const ShaderCacheLookup module_key{*recipe.pixel,true,variant};auto module=shaders_.find(module_key);
        if(module==shaders_.end()) {
            if(!retained_opaque_phase_headroom_.CanAdmitModule(*pixel_index,bytes.size())||
               shaders_.size()>=config_.max_shader_cache_entries||bytes.size()>config_.max_shader_cache_bytes||
               shader_cache_stats_.bytes>config_.max_shader_cache_bytes-bytes.size())return refuse();
            auto shader=CreateShader(bytes.data(),bytes.size(),"main",RenderShaderFormat::SPIRV);
            if(!shader||!static_cast<VulkanShader&>(*shader).vk)return refuse();
            module=shaders_.emplace(ShaderCacheIdentity{*recipe.pixel,true,variant},CachedShader{std::move(shader),0,bytes.size()}).first;
            module->second.audited_opaque_phase=true;
            if(!retained_opaque_phase_headroom_.AdmitModule(*pixel_index,bytes.size()))throw std::logic_error("Opaque phase module credit changed");
            shader_cache_stats_.bytes+=bytes.size();++shader_cache_stats_.misses;++shader_cache_stats_.creations;
            ++retained_opaque_stats_.modules_created;retained_opaque_stats_.module_bytes+=bytes.size();
        }
        if(!module->second.audited_opaque_phase||!retained_opaque_phase_headroom_.CanAdmitPipeline(recipe_index))return refuse();
        cache_epoch_.Mark(vs->second);cache_epoch_.Mark(original_ps->second);cache_epoch_.Mark(original_pipeline->second);cache_epoch_.Mark(module->second);
        RenderGraphicsPipelineDesc p;p.pipelineLayout=layout_.get();p.vertexShader=vs->second.shader.get();p.pixelShader=module->second.shader.get();
        p.cullMode=RenderCullMode(recipe.raster[0]);p.frontFace=RenderFrontFace(recipe.raster[1]);p.depthClipEnabled=true;
        p.renderTargetCount=1;p.renderTargetFormat[0]=RenderFormat::R32_UINT;p.renderTargetBlend[0]=RenderBlendDesc::Copy();p.renderTargetBlend[0].renderTargetWriteMask=1;
        p.depthTargetFormat=RenderFormat::D32_FLOAT;p.depthEnabled=true;p.depthWriteEnabled=true;p.depthFunction=RenderComparisonFunction::GREATER_EQUAL;
        const RenderSpecConstant specialization(0,0);p.specConstants=&specialization;p.specConstantsCount=1;
        if(!Pipeline(key,p)||!pipelines_.find(key)->second.audited_opaque_phase)throw std::logic_error("Opaque phase PSO creation not certified");
        if(!retained_opaque_phase_headroom_.AdmitPipeline(recipe_index))throw std::logic_error("Opaque phase PSO credit changed");
        ++retained_opaque_stats_.prepared;return true;
    }
    bool RetainedFp24DepthDrawAllowed(const DrawInput& o,std::array<uint32_t,6>* out_raster=nullptr,bool require_resident=true) {
        if(!config_.retained_fp24_depth_writer||!retained_fp24_depth_writer_||!retained_fp24_depth_writer_->supported()||
           !config_.debug_pixels.empty()||o.Rectangle()||o.Shader(false).IsNull()||!o.Shader(true).IsNull()||
           !NativeRetainedFp24DepthSourceIndex(o.Shader(false).Digest())||o.Function()!="DrawIndexedPrimitive"||o.Args()[1]!=4)return false;
        bool independent=true;o.Textures([&](const auto&){independent=false;});
        o.Geometry(false,[&](const auto& geometry){if(geometry.GpuMemory())independent=false;});
        if(!independent)return false;
        const auto surfaces=o.Surfaces();if(!surfaces[4])return false;
        for(unsigned i=1;i<4;++i)if(surfaces[i])return false;
        const auto depth=SurfaceStorage(surfaces[4]);
        const NativeRetainedFp24DepthLayout key{depth.width,depth.height,depth.pitch,depth.base,depth.format,uint32_t(depth.depth)};
        if(key!=kNativeRetainedFp24DepthLayout)return false;
        const auto payload=ReadPayload(o.State());const auto raw=payload.bytes();
        if(BE(raw,0x2934)!=0x700766||BE(raw,0x28dc)||(BE(raw,0x293c)&24))return false;
        std::array<uint32_t,kDrawStateWords> words;
        for(unsigned i=0;i<words.size();++i)words[i]=BE(raw,kDrawStateOffset+i*4);
        words[(0x2934-kDrawStateOffset)/4]=0;words[(0x2948-kDrawStateOffset)/4]&=~(7u<<11);
        const auto state=DecodeDrawState(words,1280,720,4,true);
        const std::array<uint32_t,6> raster{uint32_t(state.pipeline.cullMode),uint32_t(state.pipeline.frontFace),
            uint32_t(state.pipeline.depthClipEnabled),uint32_t(state.pipeline.primitiveTopology),state.specialization,0};
        NativeFsiConstants fsi{};fsi.depth_control=BE(raw,0x2934);fsi.depth_format=depth.format;fsi.depth_base=depth.base;fsi.pitch=depth.pitch;
        const auto mode=BE(raw,0x2948);
        if(mode&(1u<<11)){fsi.front_slope_bias=std::bit_cast<float>(BE(raw,0x2a50));fsi.front_constant_bias=std::bit_cast<float>(BE(raw,0x2a54));}
        if(mode&(1u<<12)){fsi.back_slope_bias=std::bit_cast<float>(BE(raw,0x2a58));fsi.back_constant_bias=std::bit_cast<float>(BE(raw,0x2a5c));}
        if(state.discard||!NativeRetainedFp24DepthAllowed(fsi,1280,720,surfaces[4].Info(),BE(raw,0x28dc),false,
             o.Shader(false).Digest(),raster,state.viewport.minDepth,state.viewport.maxDepth))return false;
        if(out_raster)*out_raster=raster;
        if(!require_resident)return true;
        const auto vs=shaders_.find(ShaderCacheLookup{o.Shader(false).Digest(),false,NativeShaderVariant::Unmodified});
        const auto ps=shaders_.find(ShaderCacheLookup{"depth-only",true,NativeShaderVariant::RetainedFp24DepthWriterV1});
        const PipelineLookup pipeline{o.Shader(false).OptionalDigest(),std::nullopt,PipelineRasterKey(raster),-1,
            int32_t(RenderFormat::D32_FLOAT),NativeShaderVariant::RetainedFp24DepthWriterV1};
        const auto resident=pipelines_.find(pipeline);
        return vs!=shaders_.end()&&vs->second.audited_retained_fp24_vertex&&ps!=shaders_.end()&&
            resident!=pipelines_.end()&&bool(resident->second.pipeline);
    }
    void MaterializeRetainedFp24Depth() {
        if(!retained_fp24_depth_current_)return;
        auto generation=retained_fp24_depth_current_;auto& authority=generation->authority;
        try {
            authority.BeforeMaterialize(edram_memory_->SurfaceVersion(RetainedDepth()));
            const auto ticket=authority.ticket();
            if(authority.dirty()) {
                auto prepared=retained_fp24_depth_writer_->PrepareStore(edram_memory_->storage_buffer(),generation);
                if(!prepared)throw NativeBatchCompletionError("Cannot prepare authoritative retained FP24 depth export");
                bool recorded=false;
                for(unsigned attempt=0;attempt<2&&!recorded;++attempt)
                    RunTransferScope([&]{recorded=retained_fp24_depth_writer_->Record(*prepared,*transfer_batch_);},0);
                if(!recorded)throw NativeBatchCompletionError("Cannot record authoritative retained FP24 depth export");
                edram_memory_->RecordPackedSurfaceCommit(RetainedDepth());++retained_fp24_depth_stats_.materializations;
                retained_fp24_depth_stats_.maximum_run=std::max(retained_fp24_depth_stats_.maximum_run,retained_fp24_depth_run_);
            }
            authority.Materialized(ticket,true);retained_fp24_depth_current_.reset();retained_fp24_depth_run_=0;retained_fp24_depth_last_source_=UINT32_MAX;
        }catch(const NativeBatchCompletionError&){throw;}
        catch(const std::exception& e){throw NativeBatchCompletionError("Retained FP24 depth export failed: "+std::string(e.what()));}
        catch(...){throw NativeBatchCompletionError("Retained FP24 depth export failed");}
    }
    std::shared_ptr<NativeRetainedFp24DepthGeneration> GetRetainedFp24Depth(uint64_t event) {
        const uint64_t version=edram_memory_->SurfaceVersion(RetainedDepth());
        if(retained_fp24_depth_current_) {
            if(!retained_fp24_depth_current_->authority.Reusable(kNativeRetainedFp24DepthLayout,version))
                throw NativeBatchCompletionError("Retained FP24 depth changed without materialization");
            ++retained_fp24_depth_stats_.reuses;return retained_fp24_depth_current_;
        }
        if(!edram_memory_->SurfaceFullyDefined(RetainedDepth())){++retained_fp24_depth_stats_.source_refusals;return {};}
        if(retained_fp24_depth_generation_==UINT64_MAX){++retained_fp24_depth_stats_.resource_refusals;return {};}
        auto prepared=retained_fp24_depth_writer_->PrepareRestore(edram_memory_->storage_buffer(),++retained_fp24_depth_generation_,version);
        if(!prepared){++retained_fp24_depth_stats_.resource_refusals;return {};}
        bool recorded=false;RunTransferScope([&]{recorded=retained_fp24_depth_writer_->Record(*prepared,*transfer_batch_);},event);
        if(!recorded){++retained_fp24_depth_stats_.resource_refusals;return {};}
        retained_fp24_depth_current_=prepared->generation;++retained_fp24_depth_stats_.restores;return retained_fp24_depth_current_;
    }
    bool PrewarmRetainedFp24DepthPipeline(const NativeFsiPipelineRecipe& recipe,
            std::span<const uint32_t> vertex,std::span<const uint32_t> original_builtin) {
        if(!config_.retained_fp24_depth_writer||!retained_fp24_depth_writer_||!retained_fp24_depth_writer_->supported())return false;
        if(mutation_serial()||HasPendingGpuWork()||queries_->active()||!deferred_queries_.empty())
            throw std::runtime_error("Retained FP24 depth prewarm requires an unexecuted Scene");
        transfer_report_dirty_=true;++retained_fp24_depth_stats_.prewarm_attempts;
        const auto source=NativeRetainedFp24DepthSourceIndex(recipe.vertex);
        if(!source||NativeShaderCompiler::CacheVersion()!=kNativeRetainedFp24DepthBaselineVersion||recipe.pixel||!ValidNativeFsiPipelineRecipe(recipe)||!NativeRetainedFp24DepthRaster(recipe.raster)||
           recipe.color_format!=int32_t(RenderFormat::R8G8B8A8_UNORM)||recipe.depth_format!=-1||vertex.size_bytes()!=10096||
           ContentHash({reinterpret_cast<const uint8_t*>(vertex.data()),vertex.size_bytes()})!=kNativeRetainedFp24DepthSources[*source].spirv||
           original_builtin.size()!=2423||ContentHash({reinterpret_cast<const uint8_t*>(original_builtin.data()),original_builtin.size_bytes()})!=kReadonlyQueryBuiltinSpirv)
            {++retained_fp24_depth_stats_.prewarm_refusals;return false;}
        const auto vs=shaders_.find(ShaderCacheLookup{recipe.vertex,false,NativeShaderVariant::Unmodified});
        const auto ps=shaders_.find(ShaderCacheLookup{"depth-only",true,NativeShaderVariant::Unmodified});
        const PipelineLookup generic{std::string_view(recipe.vertex),std::nullopt,PipelineRasterKey(recipe.raster),
            PrewarmFsiColorFormat(recipe.color_format),-1,NativeShaderVariant::Unmodified};
        const auto original=pipelines_.find(generic);
        if(vs==shaders_.end()||!vs->second.audited_retained_fp24_vertex||ps==shaders_.end()||original==pipelines_.end()||!original->second.pipeline)
            {++retained_fp24_depth_stats_.prewarm_refusals;return false;}
        const PipelineLookup key{std::string_view(recipe.vertex),std::nullopt,PipelineRasterKey(recipe.raster),-1,
            int32_t(RenderFormat::D32_FLOAT),NativeShaderVariant::RetainedFp24DepthWriterV1};
        if(const auto existing=pipelines_.find(key);existing!=pipelines_.end())return bool(existing->second.pipeline);
        const ShaderCacheLookup module_key{"depth-only",true,NativeShaderVariant::RetainedFp24DepthWriterV1};
        auto module=shaders_.find(module_key);const uint64_t bytes=sizeof(readonly_depth_attachment_frag_spv);
        auto headroom=retained_fp24_depth_headroom_;
        if(module==shaders_.end()&&(bytes>64u*1024||headroom.modules()!=0||!headroom.Admit(bytes)))
            {++retained_fp24_depth_stats_.prewarm_refusals;return false;}
        const auto soft=headroom.SoftTargets(true,config_.max_shader_cache_entries,config_.max_shader_cache_bytes);
        const uint64_t pipeline_soft=std::min<uint64_t>(config_.max_pipeline_cache_entries,
            config_.max_pipeline_cache_entries*3/4+std::min<uint64_t>(retained_fp24_depth_stats_.prepared+1,4));
        if(retained_fp24_depth_stats_.prepared>=4||pipelines_.size()>=config_.max_pipeline_cache_entries||pipelines_.size()>=pipeline_soft||
           (module==shaders_.end()&&(shaders_.size()>=config_.max_shader_cache_entries||shaders_.size()>=soft.entries||
            bytes>soft.bytes||shader_cache_stats_.bytes>soft.bytes-bytes)))
            {++retained_fp24_depth_stats_.prewarm_refusals;return false;}
        auto* vertex_shader=vs->second.shader.get();
        cache_epoch_.Mark(vs->second);cache_epoch_.Mark(ps->second);cache_epoch_.Mark(original->second);
        if(module==shaders_.end()) {
            auto shader=CreateShader(readonly_depth_attachment_frag_spv,sizeof(readonly_depth_attachment_frag_spv),"main",RenderShaderFormat::SPIRV);
            if(!shader||!static_cast<VulkanShader&>(*shader).vk){++retained_fp24_depth_stats_.prewarm_refusals;return false;}
            module=shaders_.emplace(ShaderCacheIdentity{"depth-only",true,NativeShaderVariant::RetainedFp24DepthWriterV1},
                CachedShader{std::move(shader),0,bytes,false,false}).first;
            shader_cache_stats_.bytes+=bytes;++shader_cache_stats_.creations;++shader_cache_stats_.misses;retained_fp24_depth_headroom_=headroom;
        }
        cache_epoch_.Mark(module->second);RenderGraphicsPipelineDesc p;p.pipelineLayout=layout_.get();p.vertexShader=vertex_shader;p.pixelShader=module->second.shader.get();
        p.cullMode=RenderCullMode(recipe.raster[0]);p.frontFace=RenderFrontFace(recipe.raster[1]);p.depthClipEnabled=true;
        p.depthEnabled=true;p.depthWriteEnabled=true;p.stencilEnabled=false;p.depthFunction=RenderComparisonFunction::GREATER_EQUAL;p.depthTargetFormat=RenderFormat::D32_FLOAT;
        const RenderSpecConstant specialization(0,0);p.specConstants=&specialization;p.specConstantsCount=1;
        if(!Pipeline(key,p))throw std::logic_error("Retained FP24 depth startup PSO returned null");
        ++retained_fp24_depth_stats_.prepared;return true;
    }
    bool RetainedOpaqueDrawAllowed(const DrawInput& o,std::array<uint32_t,6>* out_raster=nullptr,bool require_resident=true) {
        if(!RetainedOpaqueEnabled()||!retained_opaque_writer_||!retained_opaque_writer_->supported()||
           !config_.debug_pixels.empty()||o.Rectangle()||o.Shader(false).IsNull()||o.Shader(true).IsNull()||
           !RetainedOpaqueProgramFor(o.Shader(false).Digest(),o.Shader(true).Digest())||
           o.Function()!="DrawIndexedPrimitive"||o.Args()[1]!=4)return false;
        const auto surfaces=o.Surfaces();
        if(!surfaces[0]||!surfaces[4])return false;
        for(unsigned i=1;i<4;++i)if(surfaces[i])return false;
        const auto color=SurfaceStorage(surfaces[0]),depth=SurfaceStorage(surfaces[4]);
        const NativeRetainedOpaqueLayout key{color.width,color.height,color.pitch,color.base,color.format,uint32_t(color.depth),
            depth.width,depth.height,depth.pitch,depth.base,depth.format,uint32_t(depth.depth)};
        if(key!=kNativeRetainedOpaqueLayout||(surfaces[0].Info()>>16&3)||(surfaces[4].Info()>>16&3))return false;
        const auto payload=ReadPayload(o.State());const auto raw=payload.bytes();
        if(BE(raw,0x2934)!=0x700766||BE(raw,0x28dc)!=15||
           (BE(raw,0x2938)&0x1fff1fff)!=0x10001||(BE(raw,0x293c)&24))return false;
        const auto mode=BE(raw,0x2948);
        for(unsigned face=0;face<2;++face)if(mode&(1u<<(11+face)))
            for(unsigned component=0;component<2;++component)
                if(std::bit_cast<float>(BE(raw,0x2a50+face*8+component*4))!=0.f)return false;
        const float zs=std::bit_cast<float>(BE(raw,0x2918)),zo=std::bit_cast<float>(BE(raw,0x291c));
        // Actual full-scene parity covers the original1->0 range. The flipped
        // 0->1 case has a color precision mismatch and must remain ordinary Native.
        if(!(zo==1.f&&zs==-1.f))return false;
        std::array<uint32_t,kDrawStateWords> words;
        for(unsigned i=0;i<words.size();++i)words[i]=BE(raw,kDrawStateOffset+i*4);
        words[(0x2934-kDrawStateOffset)/4]=0;words[(0x2948-kDrawStateOffset)/4]&=~(7u<<11);
        const auto state=DecodeDrawState(words,1280,720,4,true);const auto& p=state.pipeline;
        const std::array<uint32_t,6> raster{uint32_t(p.cullMode),uint32_t(p.frontFace),uint32_t(p.depthClipEnabled),uint32_t(p.primitiveTopology),state.specialization,0};
        if(state.discard||raster[0]!=uint32_t(RenderCullMode::BACK)||
           (raster[1]!=uint32_t(RenderFrontFace::CLOCKWISE)&&raster[1]!=uint32_t(RenderFrontFace::COUNTER_CLOCKWISE))||
           raster[2]!=1||raster[3]!=uint32_t(RenderPrimitiveTopology::TRIANGLE_LIST)||raster[4]!=0)return false;
        if(out_raster)*out_raster=raster;
        if(!require_resident)return true;
        const auto variant=RetainedOpaqueVariant();
        const ShaderCacheLookup shader{o.Shader(true).Digest(),true,variant};
        const PipelineLookup pipeline{o.Shader(false).OptionalDigest(),o.Shader(true).OptionalDigest(),PipelineRasterKey(raster),
            int32_t(RenderFormat::R32_UINT),int32_t(RenderFormat::D32_FLOAT),variant};
        const auto ps=shaders_.find(shader),vs=shaders_.find(ShaderCacheLookup{o.Shader(false).Digest(),false,NativeShaderVariant::Unmodified});
        const auto resident=pipelines_.find(pipeline);
        if(ps==shaders_.end()||vs==shaders_.end()||resident==pipelines_.end()||!resident->second.pipeline)return false;
        if(out_raster)*out_raster=raster;return true;
    }
    bool RetainedSharedDepthDrawAllowed(const DrawInput& o,bool require_resident=true) {
        if(!config_.retained_shared_depth_queries||!config_.retained_opaque_catalog||!retained_opaque_current_||
           !retained_opaque_current_->authority.dirty()||!queries_->active()||!config_.debug_pixels.empty()||
           o.Rectangle()||!o.Shader(true).IsNull()||o.Shader(false).IsNull()||
           o.Shader(false).Digest()!=kReadonlyQueryVertexSource||o.Args()[1]!=4||
           (o.Function()!="DrawPrimitive"&&o.Function()!="DrawIndexedPrimitive"))return false;
        const auto surfaces=o.Surfaces();if(!surfaces[4])return false;
        const auto depth=SurfaceStorage(surfaces[4]);
        if(depth.width!=1280||depth.height!=720||depth.pitch!=1280||depth.base||depth.format!=1||!depth.depth||
           (surfaces[4].Info()&0x30000u))return false;
        bool independent=true;
        o.Textures([&](const auto&){independent=false;});
        o.Geometry(false,[&](const auto& geometry){if(geometry.GpuMemory())independent=false;});
        if(!independent)return false;
        const auto payload=ReadPayload(o.State());const auto raw=payload.bytes();
        std::array<uint32_t,kDrawStateWords> words;
        for(unsigned i=0;i<words.size();++i)words[i]=BE(raw,kDrawStateOffset+i*4);
        words[(0x2934-kDrawStateOffset)/4]=0;words[(0x2948-kDrawStateOffset)/4]&=~(7u<<11);
        const auto state=DecodeDrawState(words,1280,720,4,true);
        const std::array<uint32_t,6> raster{uint32_t(state.pipeline.cullMode),uint32_t(state.pipeline.frontFace),
            uint32_t(state.pipeline.depthClipEnabled),uint32_t(state.pipeline.primitiveTopology),state.specialization,0};
        constexpr std::array<uint32_t,6> allowed{3,2,1,4,0,0};
        if(state.discard||raster!=allowed)return false;
        NativeFsiConstants fsi{};fsi.depth_control=BE(raw,0x2934);fsi.depth_format=depth.format;
        fsi.depth_base=depth.base;fsi.pitch=depth.pitch;
        const auto mode=BE(raw,0x2948);
        if(mode&(1u<<11)){fsi.front_slope_bias=std::bit_cast<float>(BE(raw,0x2a50));fsi.front_constant_bias=std::bit_cast<float>(BE(raw,0x2a54));}
        if(mode&(1u<<12)){fsi.back_slope_bias=std::bit_cast<float>(BE(raw,0x2a58));fsi.back_constant_bias=std::bit_cast<float>(BE(raw,0x2a5c));}
        if(!NativeReadonlyDepthAttachmentEligible(fsi,1280,720,BE(raw,0x28dc),false,true,true,
             state.viewport.minDepth,state.viewport.maxDepth,surfaces[4].Info()))return false;
        const auto& generation=*retained_opaque_current_;
        if(!generation.color||!generation.depth||!generation.Framebuffer()||generation.conversion_owner!=retained_opaque_writer_.get()||
           !generation.authority.Reusable(kNativeRetainedOpaqueLayout,edram_memory_->SurfaceVersion(RetainedColor()),
               edram_memory_->SurfaceVersion(RetainedDepth())))return false;
        if(surfaces_maybe_dirty_)for(const auto& [key,image]:surfaces_)if(image.dirty)return false;
        if(!require_resident)return true;
        const auto vs=shaders_.find(ShaderCacheLookup{kReadonlyQueryVertexSource,false,NativeShaderVariant::Unmodified});
        const auto ps=shaders_.find(ShaderCacheLookup{"depth-only",true,NativeShaderVariant::NativeReadonlyDepthD32LoadV2});
        const PipelineLookup key{o.Shader(false).OptionalDigest(),std::nullopt,PipelineRasterKey(allowed),-1,
            int32_t(RenderFormat::D32_FLOAT),NativeShaderVariant::NativeReadonlyDepthD32LoadV2};
        const auto pipeline=pipelines_.find(key);
        return vs!=shaders_.end()&&vs->second.audited_readonly_vertex&&ps!=shaders_.end()&&
            pipeline!=pipelines_.end()&&bool(pipeline->second.pipeline);
    }
    // A retained generation can be fenced yet remain authoritative. Never use
    // HasPendingGpuWork or mutation_serial as a representation/alias proof.
    bool RetainedPrimaryAuthorityForDisjointQuery() {
        if(!config_.retain_primary_across_disjoint_queries)return false;
        if(batch_failed_||draw_group_recording_failed_||(gpu_work_queue_&&gpu_work_queue_->failed()))
            throw NativeBatchCompletionError("Disjoint query after poisoned accepted work");
        if(retained_opaque_current_&&retained_fp24_depth_current_)
            throw NativeBatchCompletionError("Two retained primary writers own the same depth");
        bool dirty=false;
        if(retained_opaque_current_) {
            const auto& generation=*retained_opaque_current_;
            if(!retained_opaque_writer_||!retained_opaque_writer_->ValidateGeneration(generation)||
               !generation.authority.Reusable(kNativeRetainedOpaqueLayout,
                   edram_memory_->SurfaceVersion(RetainedColor()),edram_memory_->SurfaceVersion(RetainedDepth())))
                throw NativeBatchCompletionError("Disjoint query lost exact opaque primary authority");
            dirty=generation.authority.dirty();
        }
        if(retained_fp24_depth_current_) {
            const auto& generation=*retained_fp24_depth_current_;
            if(!retained_fp24_depth_writer_||!retained_fp24_depth_writer_->ValidateGeneration(generation)||
               !generation.authority.Reusable(kNativeRetainedFp24DepthLayout,edram_memory_->SurfaceVersion(RetainedDepth())))
                throw NativeBatchCompletionError("Disjoint query lost exact FP24 primary authority");
            dirty=generation.authority.dirty();
        }
        if(dirty)for(const auto& [key,image]:surfaces_)if(image.dirty)
            throw NativeBatchCompletionError("Disjoint query primary coexists with a dirty legacy interpretation");
        return dirty;
    }
    bool RetainedDisjointQueryDrawAllowed(const DrawInput& o) {
        if(!config_.retain_primary_across_disjoint_queries||!RetainedPrimaryAuthorityForDisjointQuery()||
           !config_.debug_pixels.empty()||o.Rectangle()||o.Shader(false).IsNull()||!o.Shader(true).IsNull()||
           o.Shader(false).Digest()!=kReadonlyQueryVertexSource||o.Args()[1]!=4||
           (o.Function()!="DrawPrimitive"&&o.Function()!="DrawIndexedPrimitive"))return false;
        const auto surfaces=o.Surfaces();if(!surfaces[4])return false;
        // Even CPU-readable geometry marked gpu_memory would perform a hidden
        // ReadPhysical. Refuse all such vertex/header/index owners before Draw.
        bool cpu_geometry=true,no_textures=true;
        o.Geometry(false,[&](const auto& geometry){if(geometry.GpuMemory())cpu_geometry=false;});
        o.Textures([&](const auto&){no_textures=false;});
        const auto depth=SurfaceStorage(surfaces[4]);
        const auto payload=ReadPayload(o.State());const auto raw=payload.bytes();
        std::array<uint32_t,kDrawStateWords> words;
        for(unsigned i=0;i<words.size();++i)words[i]=BE(raw,kDrawStateOffset+i*4);
        words[(0x2934-kDrawStateOffset)/4]=0;words[(0x2948-kDrawStateOffset)/4]&=~(7u<<11);
        const auto state=DecodeDrawState(words,depth.width,depth.height,4,true);
        const std::array<uint32_t,6> raster{uint32_t(state.pipeline.cullMode),uint32_t(state.pipeline.frontFace),
            uint32_t(state.pipeline.depthClipEnabled),uint32_t(state.pipeline.primitiveTopology),state.specialization,0};
        const auto vs=shaders_.find(ShaderCacheLookup{kReadonlyQueryVertexSource,false,NativeShaderVariant::Unmodified});
        const auto ps=shaders_.find(ShaderCacheLookup{"depth-only",true,NativeShaderVariant::NativeReadonlyDepthD32LoadV2});
        const PipelineLookup key{o.Shader(false).OptionalDigest(),std::nullopt,PipelineRasterKey(raster),-1,
            int32_t(RenderFormat::D32_FLOAT),NativeShaderVariant::NativeReadonlyDepthD32LoadV2};
        const auto pipeline=pipelines_.find(key);
        NativeFsiConstants fsi{};fsi.depth_control=BE(raw,0x2934);fsi.depth_format=depth.format;
        fsi.depth_base=depth.base;fsi.pitch=depth.pitch;
        const auto mode=BE(raw,0x2948);
        if(mode&(1u<<11)){fsi.front_slope_bias=std::bit_cast<float>(BE(raw,0x2a50));fsi.front_constant_bias=std::bit_cast<float>(BE(raw,0x2a54));}
        if(mode&(1u<<12)){fsi.back_slope_bias=std::bit_cast<float>(BE(raw,0x2a58));fsi.back_constant_bias=std::bit_cast<float>(BE(raw,0x2a5c));}
        return !state.discard&&depth.depth&&vs!=shaders_.end()&&vs->second.audited_readonly_vertex&&
            ps!=shaders_.end()&&pipeline!=pipelines_.end()&&bool(pipeline->second.pipeline)&&
            NativeRetainedDisjointQueryAllowed(fsi,depth.width,depth.height,surfaces[4].Info(),BE(raw,0x28dc),
                false,queries_->active(),true,o.Shader(false).Digest(),raster,state.viewport.minDepth,state.viewport.maxDepth,
                cpu_geometry,no_textures);
    }
    void CompleteGpuWorkForQueryCompletion() {
        if(!config_.retain_primary_across_disjoint_queries)
            throw std::invalid_argument("Retained query-only completion requires explicit experimental mode");
        const bool dirty=RetainedPrimaryAuthorityForDisjointQuery();
        // Keep the exact generation owner/lease through the fence and compare
        // its full immutable generation+physical-page ticket afterward. A drain
        // may not materialize, replace or promote this primary behind the API.
        const auto opaque=retained_opaque_current_;
        const auto depth=retained_fp24_depth_current_;
        const auto opaque_ticket=opaque?opaque->authority.ticket():NativeRetainedOpaqueAuthority::Ticket{};
        const auto depth_ticket=depth?depth->authority.ticket():NativeRetainedFp24DepthAuthority::Ticket{};
        // The original full fence/collector is unchanged. This method exposes
        // only real query readiness; it never publishes packed/Guest image bytes.
        FlushPendingDraws();
        if(dirty) {
            if(!RetainedPrimaryAuthorityForDisjointQuery()||opaque!=retained_opaque_current_||depth!=retained_fp24_depth_current_)
                throw NativeBatchCompletionError("Query completion changed dirty primary ownership");
            if(opaque) {
                const auto after=opaque->authority.ticket();
                if(after.generation!=opaque_ticket.generation||after.color!=opaque_ticket.color||after.depth!=opaque_ticket.depth||!opaque->authority.dirty())
                    throw NativeBatchCompletionError("Query completion changed the opaque primary ticket");
            }
            if(depth) {
                const auto after=depth->authority.ticket();
                if(after.generation!=depth_ticket.generation||after.depth!=depth_ticket.depth||!depth->authority.dirty())
                    throw NativeBatchCompletionError("Query completion changed the FP24 primary ticket");
            }
        }
        ++disjoint_query_stats_.full_query_completions;disjoint_query_stats_.dirty_query_completions+=dirty;
        transfer_report_dirty_=true;
    }
    bool CanRetainSharedDepthForDeferredQuery() {
        if(!config_.retained_shared_depth_queries||!config_.retained_opaque_catalog||!config_.defer_native_queries||
           !config_.unified_gpu_queue||!config_.async_gpu_submissions||!config_.batch_transfers||
           !retained_opaque_writer_||!retained_opaque_writer_->supported()||!retained_opaque_current_)return false;
        const auto& generation=*retained_opaque_current_;
        if(!generation.color||!generation.depth||!generation.Framebuffer()||
           generation.conversion_owner!=retained_opaque_writer_.get()||!generation.authority.dirty()||
           !generation.authority.Reusable(kNativeRetainedOpaqueLayout,edram_memory_->SurfaceVersion(RetainedColor()),
               edram_memory_->SurfaceVersion(RetainedDepth())))return false;
        if(surfaces_maybe_dirty_)for(const auto& [key,image]:surfaces_)if(image.dirty)return false;
        return true;
    }
    bool PrepareCommittedStateForDeferredQuery(NativeDeferredQueryControl control) {
        if(batch_failed_||(gpu_work_queue_&&gpu_work_queue_->failed())||draw_group_recording_||draw_group_recording_failed_)
            throw NativeBatchCompletionError("Native shared-depth query preparation after failed GPU work");
        if(control!=NativeDeferredQueryControl::Begin&&control!=NativeDeferredQueryControl::End)
            throw std::invalid_argument("Unknown Native deferred query control");
        if(config_.retain_primary_across_disjoint_queries) {
            ++disjoint_query_stats_.query_preparation_calls;
            if(RetainedPrimaryAuthorityForDisjointQuery()) {
                SealDrawCommandGroup();
                if(control==NativeDeferredQueryControl::End)transfer_batch_->Seal();
                ++disjoint_query_stats_.retained_query_preparations;transfer_report_dirty_=true;return true;
            }
            ++disjoint_query_stats_.query_strong_fallbacks;
            FlushCommittedState();SealDrawCommandGroup();return false;
        }
        if(config_.retained_shared_depth_queries)++shared_depth_query_stats_.preparation_calls;
        if(!CanRetainSharedDepthForDeferredQuery()) {
            if(config_.retained_shared_depth_queries)++shared_depth_query_stats_.strong_fallbacks;
            FlushCommittedState();SealDrawCommandGroup();return false;
        }
        SealDrawCommandGroup();
        if(control==NativeDeferredQueryControl::End)transfer_batch_->Seal();
        ++shared_depth_query_stats_.retained_preparations;transfer_report_dirty_=true;return true;
    }
    void ReconcileRetainedOpaquePhaseHeadroom() {
        if(!config_.retained_opaque_phase)return;
        for(size_t i=0;i<kRetainedOpaquePhaseRecipes.size();++i) {
            const auto& r=kRetainedOpaquePhaseRecipes[i];
            const auto pixel=NativeRetainedOpaquePhasePixelIndex(r.pixel);
            if(pixel&&retained_opaque_phase_headroom_.HasModule(*pixel)&&
               shaders_.find(ShaderCacheLookup{r.pixel,true,NativeShaderVariant::RetainedOpaquePhaseConservativeV4})==shaders_.end())
                retained_opaque_phase_headroom_.RetireModule(*pixel);
            const PipelineLookup key{r.vertex,r.pixel,PipelineRasterKey(r.raster),int32_t(RenderFormat::R32_UINT),
                int32_t(RenderFormat::D32_FLOAT),NativeShaderVariant::RetainedOpaquePhaseConservativeV4};
            const auto found=pipelines_.find(key);
            if(retained_opaque_phase_headroom_.HasPipeline(i)&&(found==pipelines_.end()||!found->second.pipeline))
                retained_opaque_phase_headroom_.RetirePipeline(i);
        }
    }
    void AcknowledgeSharedAttachmentConversion(NativeRetainedOpaqueWriter::Prepared& prepared) {
        if(!config_.share_retained_opaque_phase_attachments)return;
        try {retained_opaque_writer_->AcknowledgeSharedAttachmentConversion(prepared);}
        catch(...) {retained_opaque_writer_->FailSharedAttachments();batch_failed_=true;throw NativeBatchCompletionError("Shared phase conversion acceptance failed");}
    }
    void MaterializeRetainedOpaque() {
        MaterializeRetainedFp24Depth();
        if(!retained_opaque_current_)return;
        auto generation=retained_opaque_current_;auto& authority=generation->authority;
        try {
            authority.BeforeMaterialize(edram_memory_->SurfaceVersion(RetainedColor()),edram_memory_->SurfaceVersion(RetainedDepth()));
            const auto ticket=authority.ticket();
            if(authority.dirty()) {
                auto prepared=retained_opaque_writer_->PrepareStore(edram_memory_->storage_buffer(),generation);
                if(!prepared)throw NativeBatchCompletionError("Cannot allocate authoritative retained opaque export");
                bool recorded=false;
                // A pressure-only refusal records nothing for this export.
                // Run's end resets the scope; one fresh retry fits fixed bounds.
                const auto tag=RetainedPhaseTransferTag(NativeGpuWorkKind::RetainedPhaseExport);
                for(unsigned attempt=0;attempt<2&&!recorded;++attempt)
                    RunTransferScope([&]{recorded=retained_opaque_writer_->Record(*prepared,*transfer_batch_);},0,tag?&*tag:nullptr);
                if(!recorded)throw NativeBatchCompletionError("Cannot record authoritative retained opaque export");
                AcknowledgeSharedAttachmentConversion(*prepared);
                edram_memory_->RecordPackedSurfaceCommit(RetainedColor());
                edram_memory_->RecordPackedSurfaceCommit(RetainedDepth());
                ++retained_opaque_stats_.materializations;
                retained_opaque_stats_.maximum_run=std::max(retained_opaque_stats_.maximum_run,retained_opaque_run_);
            }
            if(config_.share_retained_opaque_phase_attachments) {
                try {retained_opaque_writer_->CloseSharedAttachmentGeneration(generation);}
                catch(...) {retained_opaque_writer_->FailSharedAttachments();batch_failed_=true;throw NativeBatchCompletionError("Shared phase generation closure failed");}
            } else authority.Materialized(ticket,true);
            retained_opaque_current_.reset();retained_opaque_run_=0;retained_opaque_last_pair_=UINT32_MAX;
        }catch(const NativeBatchCompletionError&){throw;}
        catch(const std::exception& error){throw NativeBatchCompletionError("Retained opaque export failed: "+std::string(error.what()));}
        catch(...){throw NativeBatchCompletionError("Retained opaque export failed");}
    }
    void PrepareCommittedStateForDraw(const DrawInput& o,NativeRetainedOpaquePhaseDrawFacts* phase_facts=nullptr) {
        // Hardware render targets: host working images are authoritative for
        // draws. Surface() commits an image only when another interpretation
        // touches its tiles; packed consumers (resolve, readback) still flush.
        if(!config_.native_fsi)return;
        std::optional<RetainedPhaseTimingContext> timing_context;
        if(RetainedPhaseGpuTimingEnabled())timing_context.emplace(*this,o);
        // A poisoned preparing tail must still fail even for a continuation.
        if(DrawGroupsEnabled()&&(draw_group_recording_||draw_group_recording_failed_||batch_failed_||
           (gpu_work_queue_&&gpu_work_queue_->failed())))
            throw NativeBatchCompletionError("Native draw command group contains a poisoned preparing tail");
        if(config_.retain_primary_across_disjoint_queries&&
           (retained_fp24_depth_current_||retained_opaque_current_)) {
            ++disjoint_query_stats_.preparation_calls;
            if(RetainedDisjointQueryDrawAllowed(o)) {
                ++disjoint_query_stats_.retained_preparations;transfer_report_dirty_=true;return;
            }
        }
        if(retained_fp24_depth_current_) {
            if(batch_failed_||(gpu_work_queue_&&gpu_work_queue_->failed())||draw_group_recording_||draw_group_recording_failed_)
                throw NativeBatchCompletionError("Retained FP24 depth continuation after failed GPU work");
            if(!RetainedFp24DepthDrawAllowed(o)){FlushCommittedState();return;}
            if(!retained_fp24_depth_current_->authority.Reusable(kNativeRetainedFp24DepthLayout,edram_memory_->SurfaceVersion(RetainedDepth())))
                throw NativeBatchCompletionError("Retained FP24 depth continuation lost physical authority");
            for(const auto& [key,image]:surfaces_)if(image.dirty)
                throw NativeBatchCompletionError("Retained FP24 depth overlaps a dirty Native surface");
            return;
        }
        if(!retained_opaque_current_) {FlushCommittedState();return;}
        const bool shared_candidate=RetainedSharedDepthDrawAllowed(o,false);
        const bool shared_resident=shared_candidate&&RetainedSharedDepthDrawAllowed(o);
        if(!RetainedOpaqueDrawAllowed(o)&&!RetainedOpaquePhaseDrawAllowed(o,nullptr,true,phase_facts)&&!shared_resident) {
            if(shared_candidate) {++shared_depth_query_stats_.candidates;++shared_depth_query_stats_.residency_refusals;transfer_report_dirty_=true;}
            // Only this typed GPU draw may consume an accepted packed export
            // through the same FIFO. Public/CPU/control consumers keep their
            // original full commit and completion contract.
            bool queue_export=config_.queue_retained_exports_for_draws&&o.typed&&
                config_.native_fsi&&config_.async_gpu_submissions&&gpu_work_queue_&&
                transfer_batch_&&!transfer_batch_->in_scope()&&retained_opaque_current_->authority.dirty();
            for(const auto& [key,image]:surfaces_)if(image.dirty)queue_export=false;
            if(!queue_export) {FlushCommittedState();return;}
            const auto before=retained_opaque_stats_.materializations;
            const auto frontier=gpu_work_queue_->accepted_work();
            // This validates exact authority, retains conversion owners before
            // recording, seals before metadata publication and makes accepted
            // recording/admission failures Native-fatal. It does not wait.
            MaterializeRetainedOpaque();
            if(retained_opaque_current_||transfer_batch_->has_recording()||
               retained_opaque_stats_.materializations!=before+1||
               gpu_work_queue_->accepted_work()<=frontier)
                throw NativeBatchCompletionError("Queued retained draw export lost its accepted FIFO frontier");
            ++retained_opaque_stats_.queued_draw_materializations;
            retained_opaque_stats_.queued_draw_pending+=
                gpu_work_queue_->completed_work()<gpu_work_queue_->accepted_work();
            return;
        }
        auto& authority=retained_opaque_current_->authority;
        if(!authority.Reusable(kNativeRetainedOpaqueLayout,edram_memory_->SurfaceVersion(RetainedColor()),edram_memory_->SurfaceVersion(RetainedDepth())))
            throw NativeBatchCompletionError("Retained opaque continuation lost exact physical authority");
        // Only separate retained generations skip their commit. Legacy dirty
        // Surface images must never coexist with stale canonical authority.
        for(const auto& [key,image]:surfaces_)if(image.dirty)
            throw NativeBatchCompletionError("Retained opaque generation overlaps a dirty Native surface");
    }
    std::shared_ptr<NativeRetainedOpaqueGeneration> GetRetainedOpaque(uint64_t event) {
        const auto cv=edram_memory_->SurfaceVersion(RetainedColor()),dv=edram_memory_->SurfaceVersion(RetainedDepth());
        if(retained_opaque_current_) {
            if(!retained_opaque_current_->authority.Reusable(kNativeRetainedOpaqueLayout,cv,dv))
                throw NativeBatchCompletionError("Retained opaque source changed without materialization");
            ++retained_opaque_stats_.reuses;return retained_opaque_current_;
        }
        if(!edram_memory_->SurfaceFullyDefined(RetainedColor())||!edram_memory_->SurfaceFullyDefined(RetainedDepth()))
            {++retained_opaque_stats_.source_refusals;return {};}
        if(retained_opaque_generation_==UINT64_MAX){++retained_opaque_stats_.resource_refusals;return {};}
        bool pressure_polled=false;
        if(config_.poll_retained_opaque_phase_pressure) {
            ++retained_opaque_pressure_stats_.allocation_checks;
            const auto before=retained_opaque_writer_->retained_bytes();
            const auto decision=NativeRetainedOpaquePressurePoll(true,before,retained_opaque_last_pair_bytes_,
                NativeRetainedOpaqueWriter::kMaximumRetainedImageBytes,gpu_work_queue_->inflight_batches());
            if(decision==NativeRetainedOpaquePressureDecision::InvalidBudget) {
                batch_failed_=true;throw NativeBatchCompletionError("Retained opaque image budget invariant changed");
            }
            if(decision==NativeRetainedOpaquePressureDecision::Poll) {
                // Current draw commands/query slots are prepared only later.
                // An older accepted open group is allowed: Poll retires only
                // its already-submitted predecessors, never the newer tail.
                if(batch_failed_||gpu_work_queue_->failed()||draw_group_recording_||draw_group_recording_failed_||
                   transfer_batch_->in_scope()) {
                    batch_failed_=true;throw NativeBatchCompletionError("Retained image pressure poll during failed/preparing work");
                }
                const auto accepted=gpu_work_queue_->accepted_work();
                const auto submissions=gpu_work_queue_->statistics().submissions,waits=gpu_work_queue_->statistics().waits;
                const auto retired=gpu_work_queue_->statistics().poll_retirements;
                ++retained_opaque_pressure_stats_.polls;transfer_report_dirty_=true;
                try {
                    gpu_work_queue_->PollCompleted();
                    const auto after=retained_opaque_writer_->retained_bytes();
                    if(after>before||gpu_work_queue_->accepted_work()!=accepted||
                       gpu_work_queue_->statistics().submissions!=submissions||gpu_work_queue_->statistics().waits!=waits)
                        throw NativeBatchCompletionError("Retained image pressure poll changed submission or allocation ownership");
                    retained_opaque_pressure_stats_.retired_image_bytes+=before-after;
                    retained_opaque_pressure_stats_.unchanged_byte_polls+=before==after;
                    retained_opaque_pressure_stats_.fence_retirements+=gpu_work_queue_->statistics().poll_retirements-retired;
                    pressure_polled=true;
                }catch(const std::exception& error) {
                    batch_failed_=true;throw NativeBatchCompletionError("Retained image pressure poll failed: "+std::string(error.what()));
                }catch(...) {batch_failed_=true;throw NativeBatchCompletionError("Retained image pressure poll failed");}
            }
        }
        auto prepared=retained_opaque_writer_->PrepareRestore(edram_memory_->storage_buffer(),++retained_opaque_generation_,cv,dv);
        if(!prepared){++retained_opaque_stats_.resource_refusals;retained_opaque_pressure_stats_.refused_after_poll+=pressure_polled;return {};}
        const auto tag=RetainedPhaseTransferTag(NativeGpuWorkKind::RetainedPhaseRestore,event);
        bool recorded=false;RunTransferScope([&]{recorded=retained_opaque_writer_->Record(*prepared,*transfer_batch_);},event,tag?&*tag:nullptr);
        if(!recorded){++retained_opaque_stats_.resource_refusals;retained_opaque_pressure_stats_.refused_after_poll+=pressure_polled;return {};}
        AcknowledgeSharedAttachmentConversion(*prepared);
        if(config_.poll_retained_opaque_phase_pressure) {
            // Record revalidated the actual generation and image bounds. Never
            // use nominal pixels or a refused preparation to update this hint.
            retained_opaque_last_pair_bytes_=prepared->generation->color_bytes+prepared->generation->depth_bytes;
            retained_opaque_pressure_stats_.admitted_after_poll+=pressure_polled;
        }
        retained_opaque_current_=prepared->generation;++retained_opaque_stats_.restores;return retained_opaque_current_;
    }
    bool PrewarmRetainedOpaquePipeline(const NativeFsiPipelineRecipe& recipe,std::span<const uint32_t> vertex,std::span<const uint32_t> pixel) {
        if(!RetainedOpaqueEnabled()||!retained_opaque_writer_||!retained_opaque_writer_->supported()||!recipe.pixel)return false;
        const auto* selected=RetainedOpaqueProgramFor(recipe.vertex,*recipe.pixel);
        if(!selected){++retained_opaque_stats_.prewarm_refusals;return false;}
        if(mutation_serial()||HasPendingGpuWork()||queries_->active()||!deferred_queries_.empty())
            throw std::runtime_error("Retained opaque prewarm requires an unexecuted Scene");
        const auto& program=*selected;
        if(!ValidNativeFsiPipelineRecipe(recipe)||recipe.color_format!=int32_t(RenderFormat::R8G8B8A8_UNORM)||recipe.depth_format!=-1||
           !RetainedOpaqueProgramFor(recipe.vertex,*recipe.pixel)||
           recipe.raster[0]!=uint32_t(RenderCullMode::BACK)||
           (recipe.raster[1]!=uint32_t(RenderFrontFace::CLOCKWISE)&&recipe.raster[1]!=uint32_t(RenderFrontFace::COUNTER_CLOCKWISE))||
           recipe.raster[2]!=1||recipe.raster[3]!=uint32_t(RenderPrimitiveTopology::TRIANGLE_LIST)||recipe.raster[4]||recipe.raster[5]||
           program.source_sha256!=*recipe.pixel||program.vertex_sha256!=recipe.vertex||
           program.variant_version!=(config_.retained_opaque_catalog?NativeRetainedOpaqueCatalogCompilerVersion():NativeRetainedOpaqueCompilerVersion())||
           program.baseline_version!="1c697af73d13496ae2424cfd4e65de2856f16bd612bd715b92160403c8bd762b"||
           program.baseline_version!=NativeShaderCompiler::CacheVersion()||program.specialization_mask!=1794||
           ContentHash({reinterpret_cast<const uint8_t*>(program.spirv.data()),program.spirv.size()*sizeof(uint32_t)})!=program.spirv_sha256||
           program.spirv.size()<5||program.spirv.front()!=0x07230203||program.spirv.size()*sizeof(uint32_t)>16u*1024*1024)
            {++retained_opaque_stats_.prewarm_refusals;return false;}
        const PipelineLookup generic_key{std::string_view(recipe.vertex),std::string_view(*recipe.pixel),PipelineRasterKey(recipe.raster),
            recipe.color_format,recipe.depth_format};
        const auto vs=shaders_.find(ShaderCacheLookup{recipe.vertex,false,NativeShaderVariant::Unmodified});
        const auto ps=shaders_.find(ShaderCacheLookup{*recipe.pixel,true,NativeShaderVariant::Unmodified});
        const auto generic=pipelines_.find(generic_key);
        // Optional admission never recreates or evicts a missing original.
        // Generic source/module/PSO preparation is a separate earlier phase.
        if(vertex.size()<5||pixel.size()<5||vertex.front()!=0x07230203||pixel.front()!=0x07230203||
           vs==shaders_.end()||ps==shaders_.end()||generic==pipelines_.end()||!generic->second.pipeline)
            {++retained_opaque_stats_.prewarm_refusals;return false;}
        auto* vertex_shader=vs->second.shader.get();
        cache_epoch_.Mark(vs->second);cache_epoch_.Mark(ps->second);cache_epoch_.Mark(generic->second);
        const PipelineLookup key{std::string_view(recipe.vertex),std::string_view(*recipe.pixel),PipelineRasterKey(recipe.raster),
            int32_t(RenderFormat::R32_UINT),int32_t(RenderFormat::D32_FLOAT),RetainedOpaqueVariant()};
        if(const auto p=pipelines_.find(key);p!=pipelines_.end()&&p->second.pipeline)return true;
        const ShaderCacheLookup module_key{*recipe.pixel,true,RetainedOpaqueVariant()};
        auto module=shaders_.find(module_key);const auto bytes=program.spirv.size()*sizeof(uint32_t);
        // V1 admission and accounting retain the independently reviewed
        // one-module/two-PSO supplement. V2 has its own bounded reservation.
        auto headroom=retained_opaque_headroom_;
        if(!config_.retained_opaque_catalog&&(bytes>128u*1024||
           (module==shaders_.end()&&(headroom.modules()!=0||!headroom.Admit(bytes)))))
            {++retained_opaque_stats_.prewarm_refusals;return false;}
        const auto soft=headroom.SoftTargets(true,config_.max_shader_cache_entries,config_.max_shader_cache_bytes);
        const auto pipeline_credit=std::min<uint64_t>(retained_opaque_stats_.prepared+1,2);
        const auto pipeline_soft=std::min<uint64_t>(config_.max_pipeline_cache_entries,
            config_.max_pipeline_cache_entries*3/4+pipeline_credit);
        const auto catalog_soft=retained_opaque_catalog_headroom_.SoftTargets(true,config_.max_shader_cache_entries,
            config_.max_shader_cache_bytes,config_.max_pipeline_cache_entries);
        const bool refusal=config_.retained_opaque_catalog?
            !retained_opaque_catalog_headroom_.CanAdmitPipeline()||pipelines_.size()>=config_.max_pipeline_cache_entries||
            pipelines_.size()>catalog_soft.pipelines||
            (module==shaders_.end()&&(!retained_opaque_catalog_headroom_.CanAdmitModule(bytes)||
                shaders_.size()>=config_.max_shader_cache_entries||bytes>config_.max_shader_cache_bytes||
                shader_cache_stats_.bytes>config_.max_shader_cache_bytes-bytes||
                shaders_.size()>catalog_soft.modules||shader_cache_stats_.bytes>catalog_soft.bytes)):
            pipelines_.size()>=pipeline_soft||pipelines_.size()>=config_.max_pipeline_cache_entries||
            (module==shaders_.end()&&(shaders_.size()>=soft.entries||shaders_.size()>=config_.max_shader_cache_entries||
                bytes>soft.bytes||shader_cache_stats_.bytes>soft.bytes-bytes));
        if(refusal)
            {++retained_opaque_stats_.prewarm_refusals;return false;}
        if(module==shaders_.end()) {
            auto shader=CreateShader(program.spirv.data(),bytes,"main",RenderShaderFormat::SPIRV);
            if(!shader||!static_cast<VulkanShader&>(*shader).vk){++retained_opaque_stats_.prewarm_refusals;return false;}
            module=shaders_.emplace(ShaderCacheIdentity{*recipe.pixel,true,RetainedOpaqueVariant()},
                CachedShader{std::move(shader),0,bytes,false,false}).first;
            shader_cache_stats_.bytes+=bytes;++shader_cache_stats_.creations;++shader_cache_stats_.misses;
            if(config_.retained_opaque_catalog) {
                if(!retained_opaque_catalog_headroom_.AdmitModule(bytes))throw std::logic_error("Retained catalog module credit changed during serialized admission");
                ++retained_opaque_stats_.modules_created;retained_opaque_stats_.module_bytes+=bytes;
            }else retained_opaque_headroom_=headroom;
        }
        cache_epoch_.Mark(module->second);RenderGraphicsPipelineDesc p;p.pipelineLayout=layout_.get();p.vertexShader=vertex_shader;p.pixelShader=module->second.shader.get();
        p.cullMode=RenderCullMode(recipe.raster[0]);p.frontFace=RenderFrontFace(recipe.raster[1]);p.depthClipEnabled=true;
        p.renderTargetCount=1;p.renderTargetFormat[0]=RenderFormat::R32_UINT;p.renderTargetBlend[0]=RenderBlendDesc::Copy();p.renderTargetBlend[0].renderTargetWriteMask=1;
        p.depthTargetFormat=RenderFormat::D32_FLOAT;p.depthEnabled=true;p.depthWriteEnabled=true;p.depthFunction=RenderComparisonFunction::GREATER_EQUAL;
        const RenderSpecConstant specialization(0,0);p.specConstants=&specialization;p.specConstantsCount=1;
        if(!Pipeline(key,p))throw std::logic_error("Retained opaque startup pipeline creation returned null");
        if(config_.retained_opaque_catalog&&!retained_opaque_catalog_headroom_.AdmitPipeline())
            throw std::logic_error("Retained catalog PSO credit changed during serialized admission");
        ++retained_opaque_stats_.prepared;return true;
    }
    bool PrewarmRetainedSharedDepthQueryPipeline(const NativeFsiPipelineRecipe& recipe,
            std::span<const uint32_t> vertex,std::span<const uint32_t> original_builtin) {
        if(!config_.retained_shared_depth_queries||!config_.retained_opaque_catalog||!retained_opaque_writer_||!retained_opaque_writer_->supported())return false;
        if(mutation_serial()||HasPendingGpuWork()||queries_->active()||!deferred_queries_.empty())
            throw std::runtime_error("Native readonly D32 prewarm requires an unexecuted Scene");
        const std::array<uint32_t,6> allowed{uint32_t(RenderCullMode::BACK),uint32_t(RenderFrontFace::COUNTER_CLOCKWISE),1,
            uint32_t(RenderPrimitiveTopology::TRIANGLE_LIST),0,0};
        if(!ValidNativeFsiPipelineRecipe(recipe)||recipe.vertex!=kReadonlyQueryVertexSource||recipe.pixel||recipe.raster!=allowed||
           recipe.color_format!=int32_t(RenderFormat::R8G8B8A8_UNORM)||recipe.depth_format!=-1||
           !AuditedReadonlyQueryVertex(recipe.vertex,{reinterpret_cast<const uint8_t*>(vertex.data()),vertex.size_bytes()})||
           original_builtin.size()!=2423||ContentHash({reinterpret_cast<const uint8_t*>(original_builtin.data()),original_builtin.size_bytes()})!=kReadonlyQueryBuiltinSpirv) {
            ++shared_depth_query_stats_.prewarm_refusals;return false;
        }
        const auto vs=shaders_.find(ShaderCacheLookup{recipe.vertex,false,NativeShaderVariant::Unmodified});
        const auto ps=shaders_.find(ShaderCacheLookup{"depth-only",true,NativeShaderVariant::Unmodified});
        const PipelineLookup generic{std::string_view(recipe.vertex),std::nullopt,PipelineRasterKey(recipe.raster),
            PrewarmFsiColorFormat(recipe.color_format),-1,NativeShaderVariant::Unmodified};
        const auto original_pipeline=pipelines_.find(generic);
        if(vs==shaders_.end()||!vs->second.audited_readonly_vertex||ps==shaders_.end()||original_pipeline==pipelines_.end()||!original_pipeline->second.pipeline) {
            ++shared_depth_query_stats_.prewarm_refusals;return false;
        }
        const PipelineLookup key{std::string_view(recipe.vertex),std::nullopt,PipelineRasterKey(recipe.raster),-1,
            int32_t(RenderFormat::D32_FLOAT),NativeShaderVariant::NativeReadonlyDepthD32LoadV2};
        const auto existing=pipelines_.find(key);
        if(existing!=pipelines_.end())return bool(existing->second.pipeline);
        const ShaderCacheLookup module_key{"depth-only",true,NativeShaderVariant::NativeReadonlyDepthD32LoadV2};
        auto module=shaders_.find(module_key);
        const auto soft=retained_opaque_catalog_headroom_.SoftTargets(true,config_.max_shader_cache_entries,
            config_.max_shader_cache_bytes,config_.max_pipeline_cache_entries);
        if(shared_depth_query_stats_.prepared>=1||pipelines_.size()>=config_.max_pipeline_cache_entries||pipelines_.size()>soft.pipelines||
           (module==shaders_.end()&&(shaders_.size()>=config_.max_shader_cache_entries||
            sizeof(readonly_depth_attachment_frag_spv)>config_.max_shader_cache_bytes||
            shader_cache_stats_.bytes>config_.max_shader_cache_bytes-sizeof(readonly_depth_attachment_frag_spv)))) {
            ++shared_depth_query_stats_.prewarm_refusals;return false;
        }
        auto* vertex_shader=vs->second.shader.get();cache_epoch_.Mark(vs->second);cache_epoch_.Mark(ps->second);cache_epoch_.Mark(original_pipeline->second);
        if(module==shaders_.end()) {
            if(sizeof(readonly_depth_attachment_frag_spv)>64u*1024||shaders_.size()>soft.modules||shader_cache_stats_.bytes>soft.bytes) {
                ++shared_depth_query_stats_.prewarm_refusals;return false;
            }
            auto shader=CreateShader(readonly_depth_attachment_frag_spv,sizeof(readonly_depth_attachment_frag_spv),"main",RenderShaderFormat::SPIRV);
            if(!shader||!static_cast<VulkanShader&>(*shader).vk) {++shared_depth_query_stats_.prewarm_refusals;return false;}
            module=shaders_.emplace(ShaderCacheIdentity{"depth-only",true,NativeShaderVariant::NativeReadonlyDepthD32LoadV2},
                CachedShader{std::move(shader),0,sizeof(readonly_depth_attachment_frag_spv),false,false}).first;
            shader_cache_stats_.bytes+=sizeof(readonly_depth_attachment_frag_spv);++shader_cache_stats_.creations;++shader_cache_stats_.misses;
            shared_depth_query_stats_.module_bytes=sizeof(readonly_depth_attachment_frag_spv);
        }
        cache_epoch_.Mark(module->second);RenderGraphicsPipelineDesc p;p.pipelineLayout=layout_.get();p.vertexShader=vertex_shader;p.pixelShader=module->second.shader.get();
        p.depthClipEnabled=true;p.cullMode=RenderCullMode::BACK;p.frontFace=RenderFrontFace::COUNTER_CLOCKWISE;
        p.depthEnabled=true;p.depthWriteEnabled=false;p.depthFunction=RenderComparisonFunction::GREATER_EQUAL;p.depthTargetFormat=RenderFormat::D32_FLOAT;
        const RenderSpecConstant specialization(0,0);p.specConstants=&specialization;p.specConstantsCount=1;
        Pipeline(key,p);++shared_depth_query_stats_.prepared;return true;
    }
    void InvalidateEarlyDepth() {
        if(!early_depth_current_)return;
        early_depth_current_->version.Invalidate();early_depth_current_.reset();
        early_depth_stats_.maximum_run=std::max(early_depth_stats_.maximum_run,early_depth_run_);
        if(config_.native_early_depth_catalog) {
            early_depth_catalog_stats_.maximum_run=std::max(early_depth_catalog_stats_.maximum_run,early_depth_run_);
            ++early_depth_catalog_stats_.invalidations;
        }
        early_depth_last_source_=0;early_depth_run_=0;++early_depth_stats_.invalidations;transfer_report_dirty_=true;
    }
    void FinishEarlyDepthReadonlyBoundary(const std::shared_ptr<NativeEarlyDepthGeneration>& generation) noexcept {
        // This is cache lifetime only. Never renew a changed token or alter the
        // immutable lower bound; all original completion/visibility work ran first.
        transfer_report_dirty_=true;
        try {
            const NativeReadonlyDepthLayoutKey key{1280,720,1280,0,1,1};
            if(early_depth_current_==generation&&generation->version.Reusable(key,
               edram_memory_->SurfaceVersion(RetainedDepth()))) {
                ++early_depth_boundary_stats_.retained;return;
            }
            ++early_depth_boundary_stats_.token_refusals;
        }catch(...) {++early_depth_boundary_stats_.check_errors;}
        InvalidateEarlyDepth();
    }
    template<class Function> auto AtEarlyDepthReadonlyBoundary(Function&& function) {
        if(!config_.reuse_early_depth_readonly_boundaries||!early_depth_current_) {
            InvalidateEarlyDepth();return function();
        }
        const auto generation=early_depth_current_;
        ++early_depth_boundary_stats_.checks;
        try {
            if constexpr(std::is_void_v<decltype(function())>) {
                function();FinishEarlyDepthReadonlyBoundary(generation);return;
            }else {
                auto result=function();FinishEarlyDepthReadonlyBoundary(generation);return result;
            }
        }catch(...) {
            ++early_depth_boundary_stats_.failed_boundaries;
            InvalidateEarlyDepth();throw;
        }
    }
    bool PrewarmNativeEarlyDepthPipeline(const NativeFsiPipelineRecipe& recipe,
            std::span<const uint32_t> vertex,std::span<const uint32_t> pixel,
            std::shared_ptr<const NativeEarlyDepthProgram> program) {
        if(!config_.native_early_depth||!early_depth_conversion_||!early_depth_conversion_->supported()||!program)return false;
        if(mutation_serial()||HasPendingGpuWork()||queries_->active()||!deferred_queries_.empty())
            throw std::runtime_error("Early depth preparation requires an unexecuted Native Scene");
        if(!ValidNativeFsiPipelineRecipe(recipe)||recipe.vertex!=kNativeEarlyDepthVertexSource||
           !recipe.pixel||*recipe.pixel!=kNativeEarlyDepthPixelSource||recipe.raster[2]!=1||
           recipe.raster[3]!=uint32_t(RenderPrimitiveTopology::TRIANGLE_LIST)||recipe.raster[5]||
           recipe.color_format!=int32_t(RenderFormat::R8G8B8A8_UNORM)||recipe.depth_format!=-1||
           !NativeEarlyDepthVertexCertified(vertex,recipe.vertex,kNativeEarlyDepthCanonicalVersion)) {
            ++early_depth_stats_.prewarm_refusals;return false;
        }
        const auto authenticated=TryPrepareNativeEarlyDepthShader(pixel,*recipe.pixel,kNativeEarlyDepthCanonicalVersion);
        if(!authenticated||program->source_sha256!=authenticated->source_sha256||
           program->canonical_spirv_sha256!=authenticated->canonical_spirv_sha256||
           program->program_sha256!=authenticated->program_sha256||program->spirv!=authenticated->spirv||
           (early_depth_program_&&early_depth_program_->program_sha256!=program->program_sha256)) {
            ++early_depth_stats_.prewarm_refusals;return false;
        }
        const auto vs=shaders_.find(ShaderCacheLookup{recipe.vertex,false,NativeShaderVariant::Unmodified});
        const auto original=shaders_.find(ShaderCacheLookup{*recipe.pixel,true,NativeShaderVariant::Unmodified});
        const PipelineLookup generic{std::string_view(recipe.vertex),std::string_view(*recipe.pixel),PipelineRasterKey(recipe.raster),
            recipe.color_format,recipe.depth_format,NativeShaderVariant::Unmodified};
        const auto generic_pipeline=pipelines_.find(generic);
        if(vs==shaders_.end()||!vs->second.audited_early_depth_vertex||original==shaders_.end()||
           generic_pipeline==pipelines_.end()||!generic_pipeline->second.pipeline) {
            ++early_depth_stats_.prewarm_refusals;return false;
        }
        const PipelineLookup key{std::string_view(recipe.vertex),std::string_view(*recipe.pixel),PipelineRasterKey(recipe.raster),
            -1,int32_t(RenderFormat::D32_FLOAT),NativeShaderVariant::ConservativeEarlyDepthV1};
        const auto existing=pipelines_.find(key);
        if(existing!=pipelines_.end()&&existing->second.pipeline)return true;
        const ShaderCacheLookup module_key{*recipe.pixel,true,NativeShaderVariant::ConservativeEarlyDepthV1};
        auto ps=shaders_.find(module_key);const uint64_t bytes=program->spirv.size()*sizeof(uint32_t);
        const uint64_t prospective_pipeline=std::min<uint64_t>(config_.max_pipeline_cache_entries,
            config_.max_pipeline_cache_entries*3/4+early_depth_stats_.prepared+early_depth_catalog_stats_.prepared+1);
        const uint64_t prospective_modules=std::min<uint64_t>(config_.max_shader_cache_entries,
            config_.max_shader_cache_entries*3/4+early_depth_catalog_stats_.modules+1);
        const uint64_t prospective_bytes=std::min<uint64_t>(config_.max_shader_cache_bytes,
            config_.max_shader_cache_bytes*3/4+early_depth_catalog_stats_.module_bytes+bytes);
        if(early_depth_stats_.prepared+early_depth_catalog_stats_.prepared>=8||pipelines_.size()>=prospective_pipeline||
           (ps==shaders_.end()&&(!bytes||bytes>128u*1024||shaders_.size()>=prospective_modules||
            bytes>config_.max_shader_cache_bytes||shader_cache_stats_.bytes>config_.max_shader_cache_bytes-bytes||
            shader_cache_stats_.bytes+bytes>prospective_bytes))) {
            ++early_depth_stats_.prewarm_refusals;return false;
        }
        if(ps==shaders_.end()) {
            auto shader=CreateShader(program->spirv.data(),bytes,"main",RenderShaderFormat::SPIRV);
            if(!shader||!static_cast<VulkanShader&>(*shader).vk){++early_depth_stats_.prewarm_refusals;return false;}
            ps=shaders_.emplace(ShaderCacheIdentity{std::string(*recipe.pixel),true,NativeShaderVariant::ConservativeEarlyDepthV1},
                CachedShader{std::move(shader),0,bytes}).first;
            shader_cache_stats_.bytes+=bytes;++shader_cache_stats_.creations;++shader_cache_stats_.misses;
            ++early_depth_stats_.modules;early_depth_stats_.module_bytes=bytes;
        }
        cache_epoch_.Mark(vs->second);cache_epoch_.Mark(ps->second);
        NativeFsiPipelineDescriptor descriptor(recipe.raster,-1,int32_t(RenderFormat::D32_FLOAT),layout_.get(),
            vs->second.shader.get(),ps->second.shader.get());
        descriptor.descriptor.depthEnabled=true;descriptor.descriptor.depthWriteEnabled=false;
        descriptor.descriptor.depthFunction=RenderComparisonFunction::GREATER_EQUAL;
        Pipeline(key,descriptor.descriptor);early_depth_program_=std::move(program);
        ++early_depth_stats_.prepared;transfer_report_dirty_=true;return true;
    }
    bool PrewarmNativeEarlyDepthCatalogPipeline(const NativeFsiPipelineRecipe& recipe,
            std::span<const uint32_t> vertex,std::span<const uint32_t> pixel,
            std::shared_ptr<const NativeEarlyDepthProgram> program) {
        if(!config_.native_early_depth_catalog||!early_depth_conversion_||!early_depth_conversion_->supported()||!program)return false;
        if(mutation_serial()||HasPendingGpuWork()||queries_->active()||!deferred_queries_.empty())
            throw std::runtime_error("Early depth catalogue preparation requires an unexecuted Native Scene");
        if(!ValidNativeFsiPipelineRecipe(recipe)||!recipe.pixel||
           !NativeEarlyDepthCatalogPair(recipe.vertex,*recipe.pixel)||recipe.raster[2]!=1||
           recipe.raster[3]!=uint32_t(RenderPrimitiveTopology::TRIANGLE_LIST)||recipe.raster[5]||
           recipe.color_format!=int32_t(RenderFormat::R8G8B8A8_UNORM)||recipe.depth_format!=-1||
           !NativeEarlyDepthCatalogVertexCertified(vertex,recipe.vertex,kNativeEarlyDepthCanonicalVersion)) {
            ++early_depth_catalog_stats_.prewarm_refusals;return false;
        }
        const auto authenticated=TryPrepareNativeEarlyDepthCatalogShader(pixel,*recipe.pixel,kNativeEarlyDepthCanonicalVersion);
        const auto previous=early_depth_catalog_programs_.find(*recipe.pixel);
        if(!authenticated||program->source_sha256!=authenticated->source_sha256||
           program->canonical_spirv_sha256!=authenticated->canonical_spirv_sha256||
           program->program_sha256!=authenticated->program_sha256||program->spirv!=authenticated->spirv||
           (previous!=early_depth_catalog_programs_.end()&&previous->second->program_sha256!=program->program_sha256)) {
            ++early_depth_catalog_stats_.prewarm_refusals;return false;
        }
        const auto vs=shaders_.find(ShaderCacheLookup{recipe.vertex,false,NativeShaderVariant::Unmodified});
        const auto original=shaders_.find(ShaderCacheLookup{*recipe.pixel,true,NativeShaderVariant::Unmodified});
        const PipelineLookup generic{std::string_view(recipe.vertex),std::string_view(*recipe.pixel),PipelineRasterKey(recipe.raster),
            recipe.color_format,recipe.depth_format,NativeShaderVariant::Unmodified};
        const auto generic_pipeline=pipelines_.find(generic);
        if(vs==shaders_.end()||!vs->second.audited_early_depth_catalog_vertex||original==shaders_.end()||
           generic_pipeline==pipelines_.end()||!generic_pipeline->second.pipeline) {
            ++early_depth_catalog_stats_.prewarm_refusals;return false;
        }
        const PipelineLookup key{std::string_view(recipe.vertex),std::string_view(*recipe.pixel),PipelineRasterKey(recipe.raster),
            -1,int32_t(RenderFormat::D32_FLOAT),NativeShaderVariant::ConservativeEarlyDepthCatalogV2};
        if(const auto existing=pipelines_.find(key);existing!=pipelines_.end()&&existing->second.pipeline)return true;
        const ShaderCacheLookup module_key{*recipe.pixel,true,NativeShaderVariant::ConservativeEarlyDepthCatalogV2};
        auto ps=shaders_.find(module_key);const uint64_t bytes=program->spirv.size()*sizeof(uint32_t);
        const bool new_module=ps==shaders_.end();
        const uint64_t prospective_pipeline=std::min<uint64_t>(config_.max_pipeline_cache_entries,
            config_.max_pipeline_cache_entries*3/4+early_depth_stats_.prepared+early_depth_catalog_stats_.prepared+1);
        const uint64_t prospective_modules=std::min<uint64_t>(config_.max_shader_cache_entries,
            config_.max_shader_cache_entries*3/4+early_depth_stats_.modules+early_depth_catalog_stats_.modules+uint64_t(new_module));
        const uint64_t prospective_bytes=std::min<uint64_t>(config_.max_shader_cache_bytes,
            config_.max_shader_cache_bytes*3/4+early_depth_stats_.module_bytes+early_depth_catalog_stats_.module_bytes+(new_module?bytes:0));
        if(early_depth_stats_.prepared+early_depth_catalog_stats_.prepared>=8||pipelines_.size()>=prospective_pipeline||
           (new_module&&(!bytes||early_depth_catalog_stats_.modules>=2||bytes>kEarlyDepthCatalogMaximumModuleBytes||
            early_depth_catalog_stats_.module_bytes>kEarlyDepthCatalogMaximumModuleBytes-bytes||
            shaders_.size()>=prospective_modules||bytes>config_.max_shader_cache_bytes||
            shader_cache_stats_.bytes>config_.max_shader_cache_bytes-bytes||shader_cache_stats_.bytes+bytes>prospective_bytes))) {
            ++early_depth_catalog_stats_.prewarm_refusals;return false;
        }
        // Own the authenticated source before any module/PSO mutation. A partial
        // startup failure can retain a harmless admitted program but no live path.
        if(previous==early_depth_catalog_programs_.end())
            early_depth_catalog_programs_.emplace(program->source_sha256,program);
        if(new_module) {
            auto shader=CreateShader(program->spirv.data(),bytes,"main",RenderShaderFormat::SPIRV);
            if(!shader||!static_cast<VulkanShader&>(*shader).vk){++early_depth_catalog_stats_.prewarm_refusals;return false;}
            ps=shaders_.emplace(ShaderCacheIdentity{std::string(*recipe.pixel),true,NativeShaderVariant::ConservativeEarlyDepthCatalogV2},
                CachedShader{std::move(shader),0,bytes}).first;
            shader_cache_stats_.bytes+=bytes;++shader_cache_stats_.creations;++shader_cache_stats_.misses;
            ++early_depth_catalog_stats_.modules;early_depth_catalog_stats_.module_bytes+=bytes;
        }
        cache_epoch_.Mark(vs->second);cache_epoch_.Mark(original->second);cache_epoch_.Mark(generic_pipeline->second);cache_epoch_.Mark(ps->second);
        NativeFsiPipelineDescriptor descriptor(recipe.raster,-1,int32_t(RenderFormat::D32_FLOAT),layout_.get(),
            vs->second.shader.get(),ps->second.shader.get());
        descriptor.descriptor.depthEnabled=true;descriptor.descriptor.depthWriteEnabled=false;
        descriptor.descriptor.depthFunction=RenderComparisonFunction::GREATER_EQUAL;
        Pipeline(key,descriptor.descriptor);++early_depth_catalog_stats_.prepared;transfer_report_dirty_=true;return true;
    }
    bool EarlyDepthDrawAllowed(const DrawInput& o,const NativeFsiConstants& fsi,
            const DrawState& state,uint32_t width,uint32_t height) {
        // Diagnostic contract: preserve original clipping/preraster state and
        // admit only Vulkan-defined finite raster samples. Unit viewport bounds
        // are not a certificate that arbitrary guest transforms are finite;
        // cross-pipeline equivalence still requires actual whole-frame parity.
        if(!config_.native_early_depth||!early_depth_conversion_||!early_depth_conversion_->supported()||
           state.discard||queries_->active()||o.Rectangle()||o.Shader(false).IsNull()||o.Shader(true).IsNull()||
           !((o.Shader(false).Digest()==kNativeEarlyDepthVertexSource&&o.Shader(true).Digest()==kNativeEarlyDepthPixelSource)||
             (config_.native_early_depth_catalog&&NativeEarlyDepthCatalogPair(o.Shader(false).Digest(),o.Shader(true).Digest())))||
           o.Function()!="DrawIndexedPrimitive"||o.Args()[1]!=4||width!=1280||height!=720||
           !state.pipeline.depthClipEnabled||state.pipeline.primitiveTopology!=RenderPrimitiveTopology::TRIANGLE_LIST||
           !((state.viewport.minDepth==0.0f&&state.viewport.maxDepth==1.0f)||
             (state.viewport.minDepth==1.0f&&state.viewport.maxDepth==0.0f))||
           fsi.pitch!=1280||fsi.depth_base||fsi.depth_format!=1||(fsi.depth_control&7u)!=6u||
           ((fsi.depth_control>>4)&7u)!=6u||fsi.front_slope_bias!=0.0f||fsi.front_constant_bias!=0.0f||
           fsi.back_slope_bias!=0.0f||fsi.back_constant_bias!=0.0f)return false;
        // Conservative fixed primary footprints are injective and nonwrapping:
        // depth tiles0..719 and only potentially active color0 tiles720..1439.
        const auto surfaces=o.Surfaces();if(!surfaces[4])return false;
        const auto depth_surface=SurfaceStorage(surfaces[4]);
        if(depth_surface.width!=1280||depth_surface.height!=720||depth_surface.pitch!=1280||
           depth_surface.base||depth_surface.format!=1||!depth_surface.depth)return false;
        if(surfaces[4].Info()&0x30000)return false;
        for(unsigned i=0;i<4;++i)if(fsi.color[i].mask) {
            if(i||!surfaces[i]||(surfaces[i].Info()&0x30000)||fsi.color[i].base!=720||
               (fsi.color[i].format!=0&&fsi.color[i].format!=2&&fsi.color[i].format!=3&&fsi.color[i].format!=4&&fsi.color[i].format!=6))return false;
            const auto color=SurfaceStorage(surfaces[i]);
            if(color.width!=1280||color.height!=720||color.pitch!=1280||color.base!=720||color.depth)return false;
        }
        bool independent=true;o.Geometry(false,[&](const auto& g){if(g.GpuMemory())independent=false;});
        return independent;
    }
    std::shared_ptr<NativeEarlyDepthGeneration> GetNativeEarlyDepth(uint64_t event,bool catalogue=false) {
        auto& stats=catalogue?early_depth_catalog_stats_:early_depth_stats_;
        const auto surface=RetainedDepth();const NativeReadonlyDepthLayoutKey key{1280,720,1280,0,1,1};
        const auto version=edram_memory_->SurfaceVersion(surface);
        if(early_depth_current_&&early_depth_current_->version.Reusable(key,version)) {
            ++stats.cache_hits;return early_depth_current_;
        }
        InvalidateEarlyDepth();++stats.cache_misses;
        if(!edram_memory_->SurfaceFullyDefined(surface)){++stats.source_refusals;return {};}
        if(early_depth_generation_==UINT64_MAX){++stats.resource_refusals;return {};}
        auto prepared=early_depth_conversion_->Prepare(edram_memory_->storage_buffer(),surface,version,++early_depth_generation_);
        if(!prepared){++stats.resource_refusals;return {};}
        // Allocate the immutable wrapper before admitting any conversion commands.
        std::shared_ptr<NativeEarlyDepthGeneration> generation;
        try {generation=std::make_shared<NativeEarlyDepthGeneration>(prepared->generation);}
        catch(...) {++stats.resource_refusals;return {};}
        bool recorded=false;RunTransferScope([&]{recorded=early_depth_conversion_->Record(*prepared,*transfer_batch_);},event);
        if(!recorded){++stats.resource_refusals;return {};}
        if(edram_memory_->SurfaceVersion(surface)!=version)
            throw NativeBatchCompletionError("Early depth source changed while accepting its FIFO snapshot");
        early_depth_current_=std::move(generation);return early_depth_current_;
    }
    void PrewarmReadonlyDepthConservative(const PipelineLookup& original_key,RenderGraphicsPipelineDesc p) {
        if(!config_.native_readonly_depth_conservative)return;
        auto key=original_key;key.pixel_variant=NativeShaderVariant::NativeReadonlyDepthLessV1;
        const auto existing=pipelines_.find(key);if(existing!=pipelines_.end()&&existing->second.pipeline)return;
        try {
        auto code=TryNativeReadonlyDepthLess(readonly_depth_attachment_frag_spv);
        if(!code){++readonly_depth_conservative_stats_.module_refusals;return;}
        const ShaderCacheLookup module_key{"depth-only",true,NativeShaderVariant::NativeReadonlyDepthLessV1};
        auto module=shaders_.find(module_key);const uint64_t bytes=code->size()*sizeof(uint32_t);
        if(pipelines_.size()>=config_.max_pipeline_cache_entries*3/4||
           (module==shaders_.end()&&(shaders_.size()>=config_.max_shader_cache_entries||
            bytes>config_.max_shader_cache_bytes||shader_cache_stats_.bytes>config_.max_shader_cache_bytes-bytes))) {
            ++readonly_depth_conservative_stats_.module_refusals;return;
        }
        if(module==shaders_.end()) {
            auto shader=CreateShader(code->data(),bytes,"main",RenderShaderFormat::SPIRV);
            if(!shader||!static_cast<VulkanShader&>(*shader).vk) {
                ++readonly_depth_conservative_stats_.module_refusals;return;
            }
            module=shaders_.emplace(ShaderCacheIdentity{"depth-only",true,NativeShaderVariant::NativeReadonlyDepthLessV1},
                CachedShader{std::move(shader),0,bytes,false,false}).first;
            shader_cache_stats_.bytes+=bytes;++shader_cache_stats_.creations;++shader_cache_stats_.misses;
        }
        cache_epoch_.Mark(module->second);p.pixelShader=module->second.shader.get();
        Pipeline(key,p);++readonly_depth_conservative_stats_.prepared;
        } catch(...) {
            // Optional startup preparation has accepted no GPU work. Preserve
            // the already-prepared original D32 path on allocation/compiler refusal.
            const auto failed=pipelines_.find(key);
            if(failed!=pipelines_.end()&&!failed->second.pipeline)pipelines_.erase(failed);
            ++readonly_depth_conservative_stats_.module_refusals;
        }
    }
    NativeShaderVariant ReadonlyDepthVariant()const noexcept {
        return config_.native_readonly_depth_lower_bound_query?NativeShaderVariant::NativeReadonlyDepthLowerBoundQueryV1:
            NativeShaderVariant::NativeReadonlyDepthD32LoadV2;
    }
    bool PrewarmNativeReadonlyDepthPipeline(const NativeFsiPipelineRecipe& recipe,
            std::span<const uint32_t> vertex,std::span<const uint32_t> original_builtin) {
        if(!config_.native_readonly_depth_attachment||!readonly_depth_conversion_||!readonly_depth_conversion_->supported())return false;
        if(mutation_serial()||HasPendingGpuWork()||queries_->active()||!deferred_queries_.empty())
            throw std::runtime_error("Native readonly D32 prewarm requires an unexecuted Scene");
        const std::array<uint32_t,6> allowed{uint32_t(RenderCullMode::BACK),uint32_t(RenderFrontFace::COUNTER_CLOCKWISE),1,
            uint32_t(RenderPrimitiveTopology::TRIANGLE_LIST),0,0};
        if(!ValidNativeFsiPipelineRecipe(recipe)||recipe.vertex!=kReadonlyQueryVertexSource||recipe.pixel||recipe.raster!=allowed||
           recipe.color_format!=int32_t(RenderFormat::R8G8B8A8_UNORM)||recipe.depth_format!=-1||
           !AuditedReadonlyQueryVertex(recipe.vertex,{reinterpret_cast<const uint8_t*>(vertex.data()),vertex.size_bytes()})||
           original_builtin.size()!=2423||ContentHash({reinterpret_cast<const uint8_t*>(original_builtin.data()),original_builtin.size_bytes()})!=kReadonlyQueryBuiltinSpirv) {
            ++readonly_depth_stats_.prewarm_refusals;return false;
        }
        const auto vs=shaders_.find(ShaderCacheLookup{recipe.vertex,false,NativeShaderVariant::Unmodified});
        const auto ps=shaders_.find(ShaderCacheLookup{"depth-only",true,NativeShaderVariant::Unmodified});
        const PipelineLookup generic{std::string_view(recipe.vertex),std::nullopt,PipelineRasterKey(recipe.raster),
            PrewarmFsiColorFormat(recipe.color_format),-1,NativeShaderVariant::Unmodified};
        const auto original_pipeline=pipelines_.find(generic);
        if(vs==shaders_.end()||!vs->second.audited_readonly_vertex||ps==shaders_.end()||original_pipeline==pipelines_.end()||!original_pipeline->second.pipeline) {
            ++readonly_depth_stats_.prewarm_refusals;return false;
        }
        const auto variant=ReadonlyDepthVariant();
        std::optional<std::vector<uint32_t>> lower_bound_code;
        auto fragment=std::span<const uint32_t>(readonly_depth_attachment_frag_spv);
        if(config_.native_readonly_depth_lower_bound_query) {
            lower_bound_code=TryBuiltinReadonlyQuery(original_builtin);
            if(!lower_bound_code){++readonly_depth_stats_.prewarm_refusals;return false;}
            fragment=*lower_bound_code;
        }
        const auto fragment_bytes=fragment.size_bytes();
        const PipelineLookup key{std::string_view(recipe.vertex),std::nullopt,PipelineRasterKey(recipe.raster),-1,
            int32_t(RenderFormat::D32_FLOAT),variant};
        const auto existing=pipelines_.find(key);
        if(existing!=pipelines_.end())return bool(existing->second.pipeline);
        const ShaderCacheLookup module_key{"depth-only",true,variant};
        auto module=shaders_.find(module_key);
        if(pipelines_.size()>=config_.max_pipeline_cache_entries*3/4||
           (module==shaders_.end()&&(shaders_.size()>=config_.max_shader_cache_entries||
            fragment_bytes>config_.max_shader_cache_bytes||
            shader_cache_stats_.bytes>config_.max_shader_cache_bytes-fragment_bytes))) {
            ++readonly_depth_stats_.prewarm_refusals;return false;
        }
        auto* vertex_shader=vs->second.shader.get();cache_epoch_.Mark(vs->second);cache_epoch_.Mark(ps->second);
        if(module==shaders_.end()) {
            auto shader=CreateShader(fragment.data(),fragment_bytes,"main",RenderShaderFormat::SPIRV);
            if(!shader||!static_cast<VulkanShader&>(*shader).vk) {++readonly_depth_stats_.prewarm_refusals;return false;}
            module=shaders_.emplace(ShaderCacheIdentity{"depth-only",true,variant},
                CachedShader{std::move(shader),0,fragment_bytes,false,false}).first;
            shader_cache_stats_.bytes+=fragment_bytes;++shader_cache_stats_.creations;++shader_cache_stats_.misses;
        }
        cache_epoch_.Mark(module->second);RenderGraphicsPipelineDesc p;p.pipelineLayout=layout_.get();p.vertexShader=vertex_shader;p.pixelShader=module->second.shader.get();
        p.depthClipEnabled=true;p.cullMode=RenderCullMode::BACK;p.frontFace=RenderFrontFace::COUNTER_CLOCKWISE;
        p.depthEnabled=true;p.depthWriteEnabled=false;p.depthFunction=RenderComparisonFunction::GREATER_EQUAL;p.depthTargetFormat=RenderFormat::D32_FLOAT;
        const RenderSpecConstant specialization(0,0);p.specConstants=&specialization;p.specConstantsCount=1;
        Pipeline(key,p);++readonly_depth_stats_.prepared;
        PrewarmReadonlyDepthConservative(key,p);return true;
    }
    std::shared_ptr<NativeReadonlyDepthGeneration> GetNativeReadonlyDepth(const SurfaceLayout& surface,uint64_t event) {
        if(!readonly_depth_conversion_||!readonly_depth_conversion_->supported()||!edram_memory_->SurfaceFullyDefined(surface)) {
            ++readonly_depth_stats_.source_refusals;return {};
        }
        const NativeReadonlyDepthLayoutKey key{surface.width,surface.height,surface.pitch,surface.base,surface.format,uint32_t(surface.depth)};
        const uint64_t version=edram_memory_->SurfaceVersion(surface);
        if(readonly_depth_current_&&readonly_depth_current_->version.Reusable(key,version)) {
            ++readonly_depth_stats_.cache_hits;return readonly_depth_current_;
        }
        ++readonly_depth_stats_.cache_misses;
        if(readonly_depth_generation_==UINT64_MAX) {++readonly_depth_stats_.resource_refusals;return {};}
        auto prepared=readonly_depth_conversion_->Prepare(edram_memory_->storage_buffer(),surface,version,++readonly_depth_generation_);
        if(!prepared) {++readonly_depth_stats_.resource_refusals;return {};}
        bool recorded=false;
        RunTransferScope([&]{recorded=readonly_depth_conversion_->Record(*prepared,*transfer_batch_);},event);
        if(!recorded) {++readonly_depth_stats_.resource_refusals;return {};}
        readonly_depth_current_=prepared->generation;return readonly_depth_current_;
    }
    bool PrewarmReadonlyDepthQueryPipeline(const NativeFsiPipelineRecipe& recipe,
            std::span<const uint32_t> vertex,std::span<const uint32_t> pixel) {
        if(!config_.native_fsi||mutation_serial()||HasPendingGpuWork()||queries_->active()||!deferred_queries_.empty())
            throw std::runtime_error("Read-only query prewarm requires an unexecuted packed Scene");
        if(!config_.builtin_readonly_depth_queries||config_.specialize_fsi_pipeline_state||config_.attachmentless_fsi||
           config_.implicit_fsi_barriers||config_.builtin_atomic_depth||config_.opaque_color_overwrite||
           config_.forward_control_flow||config_.forward_vertex_control_flow||config_.forward_pixel_catalog||
           !ValidNativeFsiPipelineRecipe(recipe)||recipe.vertex!=kReadonlyQueryVertexSource||recipe.pixel||
           recipe.raster[3]!=uint32_t(RenderPrimitiveTopology::TRIANGLE_LIST)||recipe.raster[5]||
           recipe.color_format!=int32_t(RenderFormat::R8G8B8A8_UNORM)||recipe.depth_format!=-1)return false;
        ++readonly_query_stats_.prewarm_attempts;
        try {
        const auto vb=std::span(reinterpret_cast<const uint8_t*>(vertex.data()),vertex.size_bytes());
        if(!AuditedReadonlyQueryVertex(recipe.vertex,vb)) {++readonly_query_stats_.vertex_refusals;++readonly_query_stats_.prewarm_refusals;return false;}
        const auto vs_entry=shaders_.find(ShaderCacheLookup{recipe.vertex,false,NativeShaderVariant::Unmodified});
        const auto original_ps=shaders_.find(ShaderCacheLookup{"depth-only",true,NativeShaderVariant::Unmodified});
        const PipelineLookup generic_key{std::string_view(recipe.vertex),std::nullopt,PipelineRasterKey(recipe.raster),
            PrewarmFsiColorFormat(recipe.color_format),recipe.depth_format,NativeShaderVariant::Unmodified};
        const auto original_pipeline=pipelines_.find(generic_key);
        if(vs_entry==shaders_.end()||!vs_entry->second.audited_readonly_vertex||original_ps==shaders_.end()||
           original_pipeline==pipelines_.end()||!original_pipeline->second.pipeline) {++readonly_query_stats_.prewarm_refusals;return false;}
        auto key=generic_key;key.pixel_variant=NativeShaderVariant::BuiltinReadonlyDepthQueryV2;
        const auto previous_pipeline=pipelines_.find(key);
        const bool existing=previous_pipeline!=pipelines_.end()&&bool(previous_pipeline->second.pipeline);
        if(!existing&&(readonly_query_stats_.prepared>=128||pipelines_.size()>=config_.max_pipeline_cache_entries*3/4)) {
            ++readonly_query_stats_.prewarm_refusals;return false;
        }
        const ShaderCacheLookup module_key{"depth-only",true,key.pixel_variant};
        auto candidate=shaders_.find(module_key);
        std::optional<std::vector<uint32_t>> code;
        if(candidate==shaders_.end()) {
            code=TryBuiltinReadonlyQuery(pixel);
            if(!code) {++readonly_query_stats_.module_refusals;++readonly_query_stats_.prewarm_refusals;return false;}
            const uint64_t bytes=code->size()*4;
            if(shaders_.size()>=config_.max_shader_cache_entries||bytes>config_.max_shader_cache_bytes||
               shader_cache_stats_.bytes>config_.max_shader_cache_bytes-bytes||!readonly_query_headroom_.CanAdmit(bytes)) {
                ++readonly_query_stats_.prewarm_refusals;return false;
            }
        } else if(pixel.size()!=2423||ContentHash({reinterpret_cast<const uint8_t*>(pixel.data()),pixel.size_bytes()})!=kReadonlyQueryBuiltinSpirv) {
            ++readonly_query_stats_.module_refusals;++readonly_query_stats_.prewarm_refusals;return false;
        }
        auto* vs=vs_entry->second.shader.get();auto* ps=original_ps->second.shader.get();
        // Stamp before insertion: unordered_map iterators can be invalidated.
        cache_epoch_.Mark(vs_entry->second);cache_epoch_.Mark(original_ps->second);
        if(candidate==shaders_.end()) {
            const uint64_t bytes=code->size()*4;
            // Admission above already fits hard limits: never evict an original
            // startup module to admit this optional single-module alternative.
            ReservePersistentCache(shaders_,bytes,config_.max_shader_cache_entries,config_.max_shader_cache_bytes,shader_cache_stats_,
                [](const auto& e){return e.spirv_bytes;},[&](const auto& e){return AsyncPinned(e.shader.get())||e.shader.get()==vs||e.shader.get()==ps;},[&]{FlushPendingDraws();});
            auto module=CreateShader(code->data(),bytes,"main",RenderShaderFormat::SPIRV);
            if(!module||!static_cast<VulkanShader*>(module.get())->vk)throw std::runtime_error("Read-only query module creation failed");
            candidate=shaders_.emplace(ShaderCacheIdentity{"depth-only",true,key.pixel_variant},CachedShader{std::move(module),0,bytes,false,false}).first;
            ++shader_cache_stats_.creations;++shader_cache_stats_.misses;shader_cache_stats_.bytes+=bytes;
            if(!readonly_query_headroom_.Admit(bytes))throw std::logic_error("Read-only query headroom accounting failed");
        } else ++shader_cache_stats_.hits;
        cache_epoch_.Mark(candidate->second);
        RenderGraphicsPipelineDesc descriptor;descriptor.pipelineLayout=layout_.get();descriptor.vertexShader=vs;descriptor.pixelShader=candidate->second.shader.get();
        Pipeline(key,descriptor);if(!existing)++readonly_query_stats_.prepared;
        transfer_report_dirty_=true;return true;
        } catch(...) {
            ++readonly_query_stats_.prewarm_refusals;
            transfer_report_dirty_=true;
            throw; // Startup caller logs refusal; gameplay only does resident lookup.
        }
    }
    RenderShader* ResidentReadonlyDepthQuery(const PipelineLookup& key,const RenderShader* current_vertex) {
        const auto vs=shaders_.find(ShaderCacheLookup{kReadonlyQueryVertexSource,false,NativeShaderVariant::Unmodified});
        if(vs==shaders_.end()||!vs->second.audited_readonly_vertex||vs->second.shader.get()!=current_vertex) {
            ++readonly_query_stats_.vertex_refusals;return nullptr;
        }
        const auto ps=shaders_.find(ShaderCacheLookup{"depth-only",true,key.pixel_variant});
        const auto pipeline=pipelines_.find(key);
        if(ps==shaders_.end()||pipeline==pipelines_.end()||!pipeline->second.pipeline) {
            ++readonly_query_stats_.residency_refusals;return nullptr;
        }
        cache_epoch_.Mark(vs->second);cache_epoch_.Mark(ps->second);cache_epoch_.Mark(pipeline->second);
        return ps->second.shader.get();
    }
    bool PrewarmOpaqueColorOverwritePipeline(const NativeFsiPipelineRecipe& recipe,
        std::span<const uint32_t> vertex) {
        if(!config_.native_fsi||mutation_serial()||HasPendingGpuWork()||queries_->active()||
           !deferred_queries_.empty())throw std::runtime_error("Opaque prewarm requires an unexecuted packed Scene");
        if(!config_.opaque_color_overwrite||!opaque_program_ready_||config_.implicit_fsi_barriers||
           !ValidNativeFsiPipelineRecipe(recipe)||recipe.vertex!=kOpaqueColorOverwriteVertex||
           !recipe.pixel||*recipe.pixel!=kOpaqueColorOverwritePixel||!OpaqueColorOverwriteRasterKey(recipe.raster))
            return false;
        const auto vs_entry=shaders_.find(ShaderCacheLookup{recipe.vertex,false,ShaderVariant(false)});
        const PipelineLookup generic_key{std::string_view(recipe.vertex),std::string_view(*recipe.pixel),
            PipelineRasterKey(recipe.raster),PrewarmFsiColorFormat(recipe.color_format),recipe.depth_format,ShaderVariant(true)};
        const auto generic_pipeline=pipelines_.find(generic_key);
        if(vs_entry==shaders_.end()||generic_pipeline==pipelines_.end()||!generic_pipeline->second.pipeline) {
            ++opaque_overwrite_stats_.prewarm_refusals;return false;
        }
        // The caller's already-canonical VS span must describe this exact
        // cached module. It is not compiled or retained by this method.
        if(vertex.size()<5||vertex[0]!=0x07230203) {
            ++opaque_overwrite_stats_.prewarm_refusals;return false;
        }
        auto candidate_key=generic_key;
        candidate_key.pixel_variant=NativeShaderVariant::OpaqueFormat3FullWordOverwriteV1;
        const ShaderCacheLookup shader_key{kOpaqueColorOverwritePixel,true,candidate_key.pixel_variant};
        const auto& code=config_.opaque_color_overwrite_program->spirv;
        const uint64_t bytes=code.size()*sizeof(uint32_t);
        if((!pipelines_.contains(candidate_key)&&pipelines_.size()>=config_.max_pipeline_cache_entries)||
           (!shaders_.contains(shader_key)&&(shaders_.size()>=config_.max_shader_cache_entries||
            bytes>config_.max_shader_cache_bytes||shader_cache_stats_.bytes>config_.max_shader_cache_bytes-bytes))) {
            ++opaque_overwrite_stats_.prewarm_refusals;return false;
        }
        auto* vs=vs_entry->second.shader.get();
        // Stamp before optional insertion; retain no VS iterator across it.
        cache_epoch_.Mark(vs_entry->second);
        auto candidate=shaders_.find(shader_key);
        if(candidate==shaders_.end()) {
            ReservePersistentCache(shaders_,bytes,config_.max_shader_cache_entries,config_.max_shader_cache_bytes,
                shader_cache_stats_,[](const auto& entry){return entry.spirv_bytes;},
                [&](const auto& entry){return AsyncPinned(entry.shader.get())||entry.shader.get()==vs;},[&]{FlushPendingDraws();});
            auto shader=CreateShader(code.data(),bytes,"main",RenderShaderFormat::SPIRV);
            if(!shader||!static_cast<VulkanShader*>(shader.get())->vk)
                throw std::runtime_error("Opaque Native prewarm module creation failed");
            candidate=shaders_.emplace(ShaderCacheIdentity{std::string(kOpaqueColorOverwritePixel),true,
                candidate_key.pixel_variant},CachedShader{std::move(shader),0,bytes,false}).first;
            shader_cache_stats_.bytes+=bytes;++shader_cache_stats_.misses;++shader_cache_stats_.creations;
        } else ++shader_cache_stats_.hits;
        cache_epoch_.Mark(candidate->second);
        RenderGraphicsPipelineDesc descriptor;descriptor.pipelineLayout=layout_.get();
        descriptor.vertexShader=vs;descriptor.pixelShader=candidate->second.shader.get();
        Pipeline(candidate_key,descriptor);++opaque_overwrite_stats_.prepared;
        transfer_report_dirty_=true;return true;
    }
    bool PrewarmForwardControlFlowPipeline(const NativeFsiPipelineRecipe& recipe,
        std::span<const uint32_t> vertex) {
        if(!config_.native_fsi||mutation_serial()||HasPendingGpuWork()||queries_->active()||
           !deferred_queries_.empty())throw std::runtime_error("Forward-CFG prewarm requires an unexecuted packed Scene");
        if(!config_.forward_control_flow||!forward_program_ready_||config_.implicit_fsi_barriers||config_.opaque_color_overwrite||
           !ValidNativeFsiPipelineRecipe(recipe)||recipe.vertex!=kForwardControlFlowVertex||
           !recipe.pixel||*recipe.pixel!=kForwardControlFlowPixel||!ForwardControlFlowRasterKey(recipe.raster)||
           recipe.color_format!=int32_t(RenderFormat::R8G8B8A8_UNORM)||recipe.depth_format!=-1)
            return false;
        const auto vs_entry=shaders_.find(ShaderCacheLookup{recipe.vertex,false,ShaderVariant(false)});
        const PipelineLookup generic_key{std::string_view(recipe.vertex),std::string_view(*recipe.pixel),
            PipelineRasterKey(recipe.raster),PrewarmFsiColorFormat(recipe.color_format),recipe.depth_format,ShaderVariant(true)};
        const auto generic_pipeline=pipelines_.find(generic_key);
        if(vs_entry==shaders_.end()||generic_pipeline==pipelines_.end()||!generic_pipeline->second.pipeline) {
            ++forward_control_flow_stats_.prewarm_refusals;return false;
        }
        // The caller's already-canonical VS span must describe this exact
        // cached module. It is not compiled or retained by this method.
        if(vertex.size()<5||vertex[0]!=0x07230203) {
            ++forward_control_flow_stats_.prewarm_refusals;return false;
        }
        auto candidate_key=generic_key;
        candidate_key.pixel_variant=NativeShaderVariant::ForwardOnlyGuestControlFlowV1;
        const ShaderCacheLookup shader_key{kForwardControlFlowPixel,true,candidate_key.pixel_variant};
        const auto& code=config_.forward_control_flow_program->spirv;
        const uint64_t bytes=code.size()*sizeof(uint32_t);
        if((!pipelines_.contains(candidate_key)&&pipelines_.size()>=config_.max_pipeline_cache_entries)||
           (!shaders_.contains(shader_key)&&(shaders_.size()>=config_.max_shader_cache_entries||
            bytes>config_.max_shader_cache_bytes||shader_cache_stats_.bytes>config_.max_shader_cache_bytes-bytes))) {
            ++forward_control_flow_stats_.prewarm_refusals;return false;
        }
        auto* vs=vs_entry->second.shader.get();
        // Stamp before optional insertion; retain no VS iterator across it.
        cache_epoch_.Mark(vs_entry->second);
        auto candidate=shaders_.find(shader_key);
        if(candidate==shaders_.end()) {
            ReservePersistentCache(shaders_,bytes,config_.max_shader_cache_entries,config_.max_shader_cache_bytes,
                shader_cache_stats_,[](const auto& entry){return entry.spirv_bytes;},
                [&](const auto& entry){return AsyncPinned(entry.shader.get())||entry.shader.get()==vs;},[&]{FlushPendingDraws();});
            auto shader=CreateShader(code.data(),bytes,"main",RenderShaderFormat::SPIRV);
            if(!shader||!static_cast<VulkanShader*>(shader.get())->vk)
                throw std::runtime_error("Forward-CFG Native prewarm module creation failed");
            candidate=shaders_.emplace(ShaderCacheIdentity{std::string(kForwardControlFlowPixel),true,
                candidate_key.pixel_variant},CachedShader{std::move(shader),0,bytes,false}).first;
            shader_cache_stats_.bytes+=bytes;++shader_cache_stats_.misses;++shader_cache_stats_.creations;
        } else ++shader_cache_stats_.hits;
        cache_epoch_.Mark(candidate->second);
        RenderGraphicsPipelineDesc descriptor;descriptor.pipelineLayout=layout_.get();
        descriptor.vertexShader=vs;descriptor.pixelShader=candidate->second.shader.get();
        Pipeline(candidate_key,descriptor);++forward_control_flow_stats_.prepared;
        transfer_report_dirty_=true;return true;
    }
    bool PrewarmForwardVertexPipeline(const NativeFsiPipelineRecipe& recipe,std::span<const uint32_t> vertex) {
        if(!config_.native_fsi||mutation_serial()||HasPendingGpuWork()||queries_->active()||!deferred_queries_.empty())
            throw std::runtime_error("Forward-VS prewarm requires an unexecuted packed Scene");
        if(!config_.forward_vertex_control_flow||!forward_vertex_program_ready_||
           config_.implicit_fsi_barriers||config_.opaque_color_overwrite||config_.forward_control_flow||config_.builtin_atomic_depth||
           !ValidNativeFsiPipelineRecipe(recipe)||!ForwardVertexAllowed(recipe.vertex,recipe.raster)||
           recipe.color_format!=int32_t(RenderFormat::R8G8B8A8_UNORM)||recipe.depth_format!=-1)return false;
        const ShaderCacheLookup generic_vs{recipe.vertex,false,NativeShaderVariant::Unmodified};
        const ShaderCacheLookup generic_ps{recipe.pixel?std::string_view(*recipe.pixel):std::string_view("depth-only"),true,NativeShaderVariant::Unmodified};
        const auto vs_entry=shaders_.find(generic_vs),ps_entry=shaders_.find(generic_ps);
        const PipelineLookup generic_key{std::string_view(recipe.vertex),recipe.pixel?std::optional<std::string_view>(*recipe.pixel):std::nullopt,
            PipelineRasterKey(recipe.raster),PrewarmFsiColorFormat(recipe.color_format),recipe.depth_format,NativeShaderVariant::Unmodified};
        const auto generic_pipeline=pipelines_.find(generic_key);
        if(vs_entry==shaders_.end()||ps_entry==shaders_.end()||generic_pipeline==pipelines_.end()||!generic_pipeline->second.pipeline||
           vertex.size()<5||vertex[0]!=0x07230203) {++forward_vertex_stats_.prewarm_refusals;return false;}
        auto candidate_key=generic_key;candidate_key.vertex_variant=NativeShaderVariant::ForwardStraightVertexControlFlowV1;
        const bool existing=pipelines_.contains(candidate_key);
        const ShaderCacheLookup shader_key{kForwardVertexSource,false,candidate_key.vertex_variant};
        const auto& code=config_.forward_vertex_program->spirv;const uint64_t bytes=code.size()*4;
        if((!existing&&(forward_vertex_stats_.prepared>=kForwardVertexMaxPipelines||pipelines_.size()>=config_.max_pipeline_cache_entries))||
           (!shaders_.contains(shader_key)&&(shaders_.size()>=config_.max_shader_cache_entries||bytes>config_.max_shader_cache_bytes||
            shader_cache_stats_.bytes>config_.max_shader_cache_bytes-bytes))) {++forward_vertex_stats_.prewarm_refusals;return false;}
        auto* original_vs=vs_entry->second.shader.get();auto* ps=ps_entry->second.shader.get();
        // Mark both generic modules before insertion; map iterators may rehash.
        cache_epoch_.Mark(vs_entry->second);cache_epoch_.Mark(ps_entry->second);
        auto candidate=shaders_.find(shader_key);
        if(candidate==shaders_.end()) {
            ReservePersistentCache(shaders_,bytes,config_.max_shader_cache_entries,config_.max_shader_cache_bytes,shader_cache_stats_,
                [](const auto& entry){return entry.spirv_bytes;},
                [&](const auto& entry){return AsyncPinned(entry.shader.get())||entry.shader.get()==original_vs||entry.shader.get()==ps;},[&]{FlushPendingDraws();});
            auto module=CreateShader(code.data(),bytes,"main",RenderShaderFormat::SPIRV);
            if(!module||!static_cast<VulkanShader*>(module.get())->vk)throw std::runtime_error("Forward-VS module creation failed");
            candidate=shaders_.emplace(ShaderCacheIdentity{std::string(kForwardVertexSource),false,candidate_key.vertex_variant},
                CachedShader{std::move(module),0,bytes,false}).first;
            shader_cache_stats_.bytes+=bytes;++shader_cache_stats_.misses;++shader_cache_stats_.creations;
        } else ++shader_cache_stats_.hits;
        cache_epoch_.Mark(candidate->second);
        RenderGraphicsPipelineDesc descriptor;descriptor.pipelineLayout=layout_.get();descriptor.vertexShader=candidate->second.shader.get();descriptor.pixelShader=ps;
        Pipeline(candidate_key,descriptor);if(!existing)++forward_vertex_stats_.prepared;
        transfer_report_dirty_=true;return true;
    }
    bool PrewarmForwardPixelCatalogPipeline(const NativeFsiPipelineRecipe& recipe,std::span<const uint32_t> vertex) {
        if(!config_.native_fsi||mutation_serial()||HasPendingGpuWork()||queries_->active()||!deferred_queries_.empty())
            throw std::runtime_error("Forward pixel catalog prewarm requires an unexecuted packed Scene");
        if(!config_.forward_pixel_catalog||!forward_pixel_catalog_ready_||
           config_.implicit_fsi_barriers||config_.opaque_color_overwrite||config_.forward_control_flow||config_.forward_vertex_control_flow||config_.builtin_atomic_depth||
           !ValidNativeFsiPipelineRecipe(recipe)||!recipe.pixel||!ForwardPixelCatalogAllowed(*recipe.pixel,recipe.raster)||
           recipe.color_format!=int32_t(RenderFormat::R8G8B8A8_UNORM)||recipe.depth_format!=-1)return false;
        const auto program=config_.forward_pixel_catalog_programs->programs.find(*recipe.pixel);
        if(program==config_.forward_pixel_catalog_programs->programs.end()) {++forward_pixel_catalog_stats_.prewarm_refusals;return false;}
        const ShaderCacheLookup generic_vs{recipe.vertex,false,NativeShaderVariant::Unmodified},generic_ps{*recipe.pixel,true,NativeShaderVariant::Unmodified};
        const auto vs_entry=shaders_.find(generic_vs),ps_entry=shaders_.find(generic_ps);
        const PipelineLookup generic_key{std::string_view(recipe.vertex),std::string_view(*recipe.pixel),PipelineRasterKey(recipe.raster),
            PrewarmFsiColorFormat(recipe.color_format),recipe.depth_format,NativeShaderVariant::Unmodified};
        const auto generic_pipeline=pipelines_.find(generic_key);
        auto& source_stats=forward_pixel_catalog_source_stats_.at(*recipe.pixel);
        if(vs_entry==shaders_.end()||ps_entry==shaders_.end()||generic_pipeline==pipelines_.end()||!generic_pipeline->second.pipeline||
           vertex.size()<5||vertex[0]!=0x07230203) {++forward_pixel_catalog_stats_.prewarm_refusals;++source_stats.prewarm_refusals;return false;}
        auto candidate_key=generic_key;candidate_key.pixel_variant=NativeShaderVariant::ForwardColorPixelCatalogV1;
        const bool existing=pipelines_.contains(candidate_key);
        const ShaderCacheLookup shader_key{*recipe.pixel,true,candidate_key.pixel_variant};
        const auto& code=program->second->spirv;const uint64_t bytes=code.size()*4;
        if((!existing&&(forward_pixel_catalog_stats_.prepared>=kForwardPixelCatalogMaxPipelines||pipelines_.size()>=config_.max_pipeline_cache_entries))||
           (!shaders_.contains(shader_key)&&(!forward_pixel_catalog_headroom_.CanAdmit(bytes)||shaders_.size()>=config_.max_shader_cache_entries||bytes>config_.max_shader_cache_bytes||
            shader_cache_stats_.bytes>config_.max_shader_cache_bytes-bytes))) {++forward_pixel_catalog_stats_.prewarm_refusals;++source_stats.prewarm_refusals;return false;}
        auto* vs=vs_entry->second.shader.get();auto* original_ps=ps_entry->second.shader.get();
        // Stamp both original modules before an insertion can rehash the map.
        cache_epoch_.Mark(vs_entry->second);cache_epoch_.Mark(ps_entry->second);
        auto candidate=shaders_.find(shader_key);
        if(candidate==shaders_.end()) {
            ReservePersistentCache(shaders_,bytes,config_.max_shader_cache_entries,config_.max_shader_cache_bytes,shader_cache_stats_,
                [](const auto& entry){return entry.spirv_bytes;},
                [&](const auto& entry){return AsyncPinned(entry.shader.get())||entry.shader.get()==vs||entry.shader.get()==original_ps;},[&]{FlushPendingDraws();});
            auto module=CreateShader(code.data(),bytes,"main",RenderShaderFormat::SPIRV);
            if(!module||!static_cast<VulkanShader*>(module.get())->vk)throw std::runtime_error("Forward pixel catalog module creation failed");
            candidate=shaders_.emplace(ShaderCacheIdentity{*recipe.pixel,true,candidate_key.pixel_variant},CachedShader{std::move(module),0,bytes,false}).first;
            shader_cache_stats_.bytes+=bytes;++shader_cache_stats_.misses;++shader_cache_stats_.creations;
            if(!forward_pixel_catalog_headroom_.Admit(bytes))throw std::logic_error("Forward pixel catalog admission accounting failed");
        } else ++shader_cache_stats_.hits;
        cache_epoch_.Mark(candidate->second);
        RenderGraphicsPipelineDesc descriptor;descriptor.pipelineLayout=layout_.get();descriptor.vertexShader=vs;descriptor.pixelShader=candidate->second.shader.get();
        Pipeline(candidate_key,descriptor);if(!existing){++forward_pixel_catalog_stats_.prepared;++source_stats.prepared;}
        transfer_report_dirty_=true;return true;
    }
    std::unique_ptr<RenderBuffer> Upload(std::span<const uint8_t> data,RenderBufferFlags flags) {
        if(data.empty()||data.size()>16*1024*1024) throw std::runtime_error("Invalid buffer extent");
        auto b=device_->createBuffer(RenderBufferDesc::UploadBuffer(data.size(),flags));
        std::memcpy(b->map(),data.data(),data.size());validation::Flush(*b);b->unmap();return b;
    }
    RetiredBufferKey VertexBufferKey(uint64_t bytes) const {
        return {bytes,RenderBufferFlag::CONSTANT|RenderBufferFlag::DEVICE_ADDRESSABLE,
            uint32_t(RenderHeapType::UPLOAD),false,static_cast<const VulkanDevice*>(device_)};
    }
    std::unique_ptr<RenderBuffer> UploadVertex(std::span<const uint8_t> data) {
        if(data.empty()||data.size()>16u*1024*1024)throw std::runtime_error("Invalid vertex upload extent");
        auto b=vertex_buffer_pool_?vertex_buffer_pool_->Take(VertexBufferKey(data.size())):nullptr;
        const bool reused=bool(b);
        if(!b)b=device_->createBuffer(RenderBufferDesc::UploadBuffer(data.size(),
            RenderBufferFlag::CONSTANT|RenderBufferFlag::DEVICE_ADDRESSABLE));
        if(!b)throw std::runtime_error("Vertex upload allocation failed");
        const auto& buffer=static_cast<const VulkanBuffer&>(*b);
        if(buffer.vk==VK_NULL_HANDLE||!buffer.allocation)
            throw std::runtime_error("Vertex upload allocation failed");
        if(!reused)++vertex_buffer_allocations_;
        auto* mapped=b->map();if(!mapped)throw std::runtime_error("Vertex upload mapping failed");
        try {std::memcpy(mapped,data.data(),data.size());validation::Flush(*b);}
        catch(...) {b->unmap();throw;}
        b->unmap();if(reused)++vertex_buffer_reuses_;return b;
    }
    RenderBuffer* UploadDraw(DrawUpload& upload,std::span<const uint8_t> data,RenderBufferFlags flags) {
        if(data.empty()||data.size()>16*1024*1024)throw std::runtime_error("Invalid draw upload extent");
        // Only the acquired, retired slot can be overwritten. Other slots may
        // still be referenced by sealed command lists awaiting their batch.
        if(data.size()>upload.capacity) {
            const auto capacity=std::bit_ceil(data.size());
            upload.buffer=device_->createBuffer(RenderBufferDesc::UploadBuffer(capacity,flags));
            upload.capacity=capacity;++draw_upload_allocations_;
        } else ++draw_upload_reuses_;
        void* mapped=upload.buffer->map();if(!mapped)throw std::runtime_error("Draw upload mapping failed");
        std::memcpy(mapped,data.data(),data.size());validation::Flush(*upload.buffer);upload.buffer->unmap();
        return upload.buffer.get();
    }
    RenderBuffer* UploadConstants(DrawUpload& upload,std::span<const uint8_t> guest,
                                  std::span<const uint8_t> shared,ConstantDeltaPacker::Writes* delta_writes=nullptr,
                                  bool pinned_null=false) {
        const auto bytes=PackedDrawConstantSize(guest,shared);
        // The mirror belongs to this actual allocation and owns only CPU bits.
        // At most128 draw slots each retain one8192-byte mirror.
        if(config_.delta_constant_uploads&&!upload.constant_delta)
            upload.constant_delta=std::make_unique<ConstantDeltaPacker>();
        // Draw acquires this slot only after its previous submission fence.
        // Keep UploadDraw's allocation/flush semantics while packing directly
        // into the mapped upload, avoiding a zeroed temporary and full copy.
        if(bytes>upload.capacity) {
            if(upload.constant_delta)upload.constant_delta->Invalidate();
            const auto capacity=std::bit_ceil(bytes);
            upload.Unmap();
            upload.buffer=device_->createBuffer(RenderBufferDesc::UploadBuffer(capacity,
                RenderBufferFlag::CONSTANT|RenderBufferFlag::DEVICE_ADDRESSABLE));
            upload.capacity=capacity;++draw_upload_allocations_;
        } else ++draw_upload_reuses_;
        if(!upload.mapped)upload.mapped=static_cast<uint8_t*>(upload.buffer->map());
        auto* mapped=upload.mapped;
        if(!mapped)throw std::runtime_error("Draw constant mapping failed");
        {
            if(pinned_null) {
                // This writer bypasses the delta mirror; never trust it afterwards.
                if(upload.constant_delta)upload.constant_delta->Invalidate();
                PackPinnedNullDrawConstants(guest,shared,{mapped,bytes});
                validation::Flush(*upload.buffer);
            } else if(config_.delta_constant_uploads) {
                const auto writes=upload.constant_delta->Pack(guest,shared,{mapped,bytes},
                    [&]{validation::Flush(*upload.buffer);});
                if(delta_writes)*delta_writes=writes;
            } else {
                PackDrawConstants(guest,shared,{mapped,bytes});
                validation::Flush(*upload.buffer);
            }
        }
        return upload.buffer.get();
    }
    void RecordFullCpuOverlay(const DrawInput& draw,const DrawTextureInput& texture,
                              const TextureLayout& layout,uint64_t version,size_t base_bytes,size_t mip_bytes,
                              const TextureOverlayStats& overlay,int32_t anisotropy) noexcept {
        if(!config_.diagnose_full_cpu_texture_overlays)return;
        auto& diagnostic=full_cpu_overlay_diagnostic_;++diagnostic.misses;
        const auto sampled=overlay.sampled_bytes[0]+overlay.sampled_bytes[1];
        if(!sampled){++diagnostic.empty_footprints;return;}
        if(overlay.sampled_bytes!=overlay.owned_region_bytes){++diagnostic.partial_overlays;return;}
        ++diagnostic.full_overlays;
        // Describe legality only. Existing normalized GPU float decoding has
        // not been proved equivalent to every CPU upload/sampler interpretation.
        const bool storage_layout=layout.dimension==1&&layout.layers==1&&layout.depth==1&&
            !layout.min_mip&&!layout.max_mip&&layout.levels.size()==1&&
            layout.block.block_width==1&&layout.block.block_height==1&&
            !(layout.base_address&3u);
        const uint32_t format=layout.format>=27&&layout.format<=29?layout.format+3:
            layout.format==54?7:layout.format;
        const bool format_supported=format==2||format==6||format==7||format==22||format==23||
            format==24||format==25||format==26||format==30||format==31||format==32||
            format==36||format==37||format==38;
        diagnostic.storage_layout_supported+=storage_layout;
        diagnostic.format_decoder_supported+=format_supported;
        diagnostic.load_shared_supported+=storage_layout&&format_supported;
        for(size_t i=0;i<diagnostic.layouts;++i)if(diagnostic.fetches[i]==layout.fetch)return;
        if(diagnostic.layouts==FullCpuOverlayDiagnostic::kMaximumLayouts) {
            ++diagnostic.layout_record_refusals;return;
        }
        try {
            bool original_binding_supported=false,decoded_binding_supported=false;
            std::string original_binding_error,decoded_binding_error;
            try {NativeTextureFormat(layout);TextureSampler(layout,anisotropy,config_.max_sampler_anisotropy);
                original_binding_supported=true;}
            catch(const std::exception& error){original_binding_error=std::string(error.what()).substr(0,512);}
            auto decoded_layout=layout;decoded_layout.fetch[0]&=~(255u<<2);
            try {NativeTextureFormat(decoded_layout);TextureSampler(decoded_layout,anisotropy,config_.max_sampler_anisotropy);
                decoded_binding_supported=true;}
            catch(const std::exception& error){decoded_binding_error=std::string(error.what()).substr(0,512);}
            Json levels=Json::array();
            for(size_t i=0;i<std::min<size_t>(layout.levels.size(),16);++i) {
                const auto& level=layout.levels[i];
                levels.push_back({{"mip",level.mip},{"layer",level.layer},{"width",level.width},
                    {"height",level.height},{"depth",level.depth},{"offset",level.offset},{"mips",level.mips},
                    {"pitch_blocks",level.pitch_blocks},{"slice_rows",level.slice_rows},
                    {"packed_offset",std::array{level.x,level.y,level.z}}});
            }
            Json alias=nullptr;
            if(layout.dimension==1) {
                const auto identity=texture.Identity().ToJson();
                const auto key=Json::array({identity,(layout.fetch[0]>>2)&255u}).dump();
                const auto found=sampled_resolves_.find(key);
                if(found!=sampled_resolves_.end()&&found->second.current) {
                    const auto& generation=found->second.current;
                    alias={{"version",generation->version},{"bytes",generation->bytes},
                        {"owners",generation.use_count()},{"bindings",generation->bindings.size()}};
                }
            }
            Json record={{"first_frame",draw.Frame()},{"first_event",draw.Event()},{"slot",texture.Slot()},
                {"source",texture.SourceText()},{"fetch",layout.fetch},{"format",layout.format},
                {"base_address",layout.base_address},{"mip_address",layout.mip_address},
                {"base_payload_bytes",base_bytes},{"mip_payload_bytes",mip_bytes},
                {"base_payload",texture.BasePayload().OptionalDigest()?Json(std::string(*texture.BasePayload().OptionalDigest())):Json(nullptr)},
                {"mip_payload",texture.MipPayload().OptionalDigest()?Json(std::string(*texture.MipPayload().OptionalDigest())):Json(nullptr)},
                {"cpu_texture_version",version},{"dimension",layout.dimension},{"width",layout.width},
                {"height",layout.height},{"depth",layout.depth},{"layers",layout.layers},{"tiled",layout.tiled},
                {"endian",layout.endian},{"min_mip",layout.min_mip},{"max_mip",layout.max_mip},
                {"block",std::array{layout.block.block_width,layout.block.block_height,layout.block.bytes,layout.block.components}},
                {"level_count",layout.levels.size()},{"levels",std::move(levels)},
                {"sampled_region_bytes",overlay.sampled_bytes},{"owned_region_bytes",overlay.owned_region_bytes},
                {"footprint_ranges",overlay.footprint_ranges},{"footprint_first",overlay.footprint_first},
                {"footprint_end",overlay.footprint_end},{"readback_bytes",overlay.readback_bytes},
                {"copy_regions",overlay.copy_regions},{"resident_same_identity_alias_generation",std::move(alias)},
                {"existing_load_shared",{{"storage_layout_supported",storage_layout},{"format_decoder_supported",format_supported},
                    {"fully_defined",storage_layout&&format_supported?Json(resolve_memory_->FullyDefined(layout,layout.base_address)):Json(nullptr)},
                    {"original_binding_supported",original_binding_supported},{"decoded_binding_supported",decoded_binding_supported},
                    {"original_binding_error",original_binding_error},{"decoded_binding_error",decoded_binding_error},
                    {"sampling_precision_equivalence_proven",false},{"pixel_signs",(layout.fetch[0]>>2)&255u},
                    {"exponent_field",(layout.fetch[3]>>13)&63u},{"number_format",layout.fetch[3]&1u}}}};
            diagnostic.records.push_back(std::move(record));
            diagnostic.fetches[diagnostic.layouts++]=layout.fetch;
        } catch(...) {++diagnostic.record_errors;} // Optional metadata cannot change render authority.
    }
public:
    Json report={{"complete",false},{"mode","synchronous offscreen native replay"},{"draws",0},{"clears",0},{"resolves",0},{"gpu_texture_reads",0},{"artifacts",Json::array()},
        {"limitations",{"No live gameplay submission or presentation","Float render storage does not reproduce all Xenos render-target precision",
            "Uncaptured initial history and untouched atlas regions require an initial-state capture",
            "EDRAM currently supports single-sample surfaces; multisample surfaces fall back to single-sample storage"}}};
    State(SceneExecutorConfig config,RenderDevice* device,RenderCommandQueue* queue)
        :config_(std::move(config)),capture_(config_.capture),compiled_(config_.compiled),output_(config_.output),
         diagnostic_zero_history_(config_.diagnostic_zero_history),
         detailed_reports_(config_.detailed_reports||!output_.empty()),
         time_cpu_stages_(config_.time_cpu_stages||config_.time_draw_recording||config_.diagnose_texture_eviction_reloads),
         device_(device),queue_(queue) {
        if(config_.diagnose_texture_eviction_reloads)texture_eviction_reload_census_=std::make_unique<NativeTextureEvictionReloadCensus>();
        if(!config_.texture_probe_directory.empty()||!config_.texture_probe_hashes.empty()) {
            texture_probe_=std::make_unique<NativeTextureProbe>(config_.texture_probe_directory,config_.texture_probe_hashes);
            report["texture_probe"]=texture_probe_->Status();
        }
        if(config_.max_deferred_queries<1||config_.max_deferred_queries>NativeQueryTracker::kMaxDeferredQueries)
            throw std::invalid_argument("Native logical query capacity must be between 1 and 128");
        if(config_.max_inflight_batches!=2&&config_.max_inflight_batches!=4)
            throw std::invalid_argument("Native asynchronous fence window must be 2 or 4");
        if(config_.max_draw_batch_size<1||config_.max_draw_batch_size>64)
            throw std::runtime_error("Native draw batch size must be between 1 and 64");
        if(config_.max_stale_alias_bytes>AliasRetention::kMaxBytes)
            throw std::invalid_argument("Native stale alias budget exceeds 128 MiB");
        if(!config_.max_pipeline_cache_entries||config_.max_pipeline_cache_entries>2048||
           !NativeFsiMaterialShaderCacheBoundsValid(config_.fsi_state_cache_headroom,
               config_.fsi_state_material_composition,config_.specialize_fsi_pipeline_state,
               config_.max_shader_cache_entries,config_.max_shader_cache_bytes))
            throw std::invalid_argument("Native persistent pipeline/shader cache bounds are invalid");
        if(config_.max_dormant_texture_bytes>2048ull*1024*1024||config_.max_dormant_texture_entries>4096||
           config_.max_dormant_texture_epochs>7200||
           config_.max_dormant_vertex_bytes>64u*1024*1024||config_.max_dormant_vertex_entries>2048||
           config_.max_dormant_cache_epochs>120)
            throw std::invalid_argument("Native dormant resource cache exceeds its bound");
        if(!NativeFsiHoistModeKnown(config_.fsi_hoist_mode))
            throw std::invalid_argument("Unknown Native FSI hoist mode");
        if(config_.fsi_hoist_mode!=NativeFsiHoistMode::Disabled) {
            const auto expected=config_.fsi_hoist_mode==NativeFsiHoistMode::ImmutableState?
                kNativeFsiHoistStateShaderVersion:kNativeFsiHoistIncomingDepthShaderVersion;
            if(!config_.native_fsi||config_.exact_fsi_stores||config_.implicit_fsi_barriers||config_.builtin_atomic_depth||
               config_.builtin_readonly_depth_queries||config_.opaque_color_overwrite||config_.forward_control_flow||
               config_.forward_vertex_control_flow||config_.forward_pixel_catalog||config_.specialize_fsi_pipeline_state||
               config_.fsi_state_forward_cfg||config_.retained_opaque_writer||config_.retained_opaque_catalog||
               config_.retained_fp24_depth_writer||config_.retained_shared_depth_queries||config_.attachmentless_fsi||
               config_.queue_retained_exports_for_draws||config_.reuse_retained_opaque_resources||
               config_.fsi_hoist_version!=expected||(!config_.fsi_shader&&!config_.shader_snapshot))
                throw std::invalid_argument("FSI hoist requires its separate provider and exclusive non-null Native shader mode");
        }
        if(config_.exact_fsi_stores&&(!config_.native_fsi||config_.fsi_hoist_mode!=NativeFsiHoistMode::Disabled||config_.implicit_fsi_barriers||config_.builtin_atomic_depth||
           config_.builtin_readonly_depth_queries||config_.opaque_color_overwrite||config_.forward_control_flow||
           config_.forward_vertex_control_flow||config_.forward_pixel_catalog||config_.specialize_fsi_pipeline_state||
           config_.fsi_state_forward_cfg||config_.retained_opaque_writer||config_.retained_opaque_catalog||
           config_.retained_fp24_depth_writer||config_.retained_shared_depth_queries||config_.attachmentless_fsi||
           config_.queue_retained_exports_for_draws||config_.reuse_retained_opaque_resources||
           config_.exact_fsi_stores_version!=kNativeFsiExactStoresShaderVersion||
           (!config_.fsi_shader&&!config_.shader_snapshot)))
            throw std::invalid_argument("Exact FSI stores require their separate provider and exclusive Native shader mode");
        if(config_.opaque_color_overwrite&&config_.native_fsi&&!config_.implicit_fsi_barriers&&
           config_.opaque_color_overwrite_program) {
            const auto& program=*config_.opaque_color_overwrite_program;
            opaque_program_ready_=program.source_sha256==kOpaqueColorOverwritePixel&&
                program.compiler_version==kOpaqueColorOverwriteBaselineCompiler&&
                program.variant_version==kNativeOpaqueShaderVersion&&program.spirv.size()>=5&&
                program.spirv.size()*4<=16u*1024*1024&&program.spirv[0]==0x07230203&&
                program.compiled_spirv_sha256==ContentHash({
                    reinterpret_cast<const uint8_t*>(program.spirv.data()),program.spirv.size()*4});
        }
        if(config_.forward_control_flow&&config_.native_fsi&&!config_.implicit_fsi_barriers&&!config_.opaque_color_overwrite&&
           config_.forward_control_flow_program) {
            const auto& program=*config_.forward_control_flow_program;
            forward_program_ready_=program.source_sha256==kForwardControlFlowPixel&&
                program.compiler_version==kForwardControlFlowBaselineCompiler&&
                program.variant_version==kNativeForwardShaderVersion&&program.spirv.size()>=5&&
                program.spirv.size()*4<=16u*1024*1024&&program.spirv[0]==0x07230203&&
                program.compiled_spirv_sha256==ContentHash({
                    reinterpret_cast<const uint8_t*>(program.spirv.data()),program.spirv.size()*4});
        }
        if(config_.forward_vertex_control_flow&&config_.native_fsi&&!config_.implicit_fsi_barriers&&!config_.opaque_color_overwrite&&!config_.forward_control_flow&&!config_.builtin_atomic_depth&&
           config_.forward_vertex_program) {
            const auto& program=*config_.forward_vertex_program;
            forward_vertex_program_ready_=program.source_sha256==kForwardVertexSource&&
                program.compiler_version==kForwardVertexBaselineCompiler&&
                program.variant_version==kNativeForwardVertexShaderVersion&&program.control_instructions==14&&
                program.terminal==12&&program.padding_clauses==1&&program.spirv.size()>=5&&
                program.spirv.size()*4<=16u*1024*1024&&program.spirv[0]==0x07230203&&
                program.compiled_spirv_sha256==ContentHash({
                    reinterpret_cast<const uint8_t*>(program.spirv.data()),program.spirv.size()*4});
        }
        if(config_.forward_pixel_catalog&&config_.native_fsi&&!config_.implicit_fsi_barriers&&!config_.opaque_color_overwrite&&
           !config_.forward_control_flow&&!config_.forward_vertex_control_flow&&!config_.builtin_atomic_depth&&config_.forward_pixel_catalog_programs) {
            const auto& catalog=config_.forward_pixel_catalog_programs->programs;
            bool valid=!catalog.empty()&&catalog.size()<=kForwardPixelCatalogMaxPrograms;uint64_t bytes=0;
            for(const auto& [source,program]:catalog) {
                if(!program){valid=false;continue;}
                valid=valid&&ForwardPixelCatalogSource(source)&&source==program->source_sha256&&
                    program->compiler_version==kForwardPixelCatalogBaselineCompiler&&program->variant_version==kNativeForwardPixelCatalogShaderVersion&&
                    program->output_mask==1&&program->control_instructions>0&&program->control_instructions<=64&&
                    program->spirv.size()>=5&&program->spirv.size()*4<=16u*1024*1024&&program->spirv[0]==0x07230203&&
                    program->compiled_spirv_sha256==ContentHash({reinterpret_cast<const uint8_t*>(program->spirv.data()),program->spirv.size()*4});
                bytes+=program->spirv.size()*4;
            }
            forward_pixel_catalog_ready_=valid&&bytes<=16u*1024*1024;
            if(forward_pixel_catalog_ready_)for(const auto& [source,program]:catalog)forward_pixel_catalog_source_stats_.try_emplace(source);
        }
        if(config_.retained_opaque_phase_render_pass_groups&&
           (!config_.retained_opaque_phase||!config_.fsi_state_material_composition||!config_.native_fsi||
            !config_.unified_gpu_queue||!config_.async_gpu_submissions||!config_.batch_transfers||
            !config_.defer_native_queries||config_.multi_draw_command_buffers))
            throw std::invalid_argument("Retained phase pass groups require canonical asynchronous phase rendering without generic grouping");
        if(config_.share_retained_opaque_phase_attachments&&
           (!config_.retained_opaque_phase||!config_.share_retained_opaque_phase_scratch||config_.reuse_retained_opaque_resources))
            throw std::invalid_argument("Shared phase attachments require the exact phase FIFO and shared scratch");
        if(config_.share_retained_opaque_phase_scratch&&(!config_.retained_opaque_phase||config_.reuse_retained_opaque_resources))
            throw std::invalid_argument("Shared opaque phase scratch requires the separate phase and fresh image ownership");
        if(config_.poll_retained_opaque_phase_pressure&&(!config_.retained_opaque_phase||config_.reuse_retained_opaque_resources))
            throw std::invalid_argument("Retained image pressure polling requires the separate phase and fresh image ownership");
        if(config_.retained_opaque_phase) {
            // A separate composition policy; legacy writer exclusions stay
            // intact and ordinary shipping paths remain enabled outside it.
            if(!config_.native_fsi||!config_.fsi_state_material_composition||!config_.specialize_fsi_pipeline_state||
               !config_.unified_gpu_queue||!config_.async_gpu_submissions||!config_.batch_transfers||!config_.defer_native_queries||
               config_.retained_opaque_writer||config_.retained_opaque_catalog||config_.retained_fp24_depth_writer||
               config_.retained_shared_depth_queries||config_.retained_d32_composition||config_.retain_primary_across_disjoint_queries||
               config_.native_early_depth_material_retention||config_.reuse_early_depth_readonly_boundaries||config_.reuse_retained_opaque_resources)
                throw std::invalid_argument("Opaque phase requires canonical asynchronous material composition and separate authority");
            if(config_.retained_opaque_phase_programs) {
                std::array<bool,kRetainedOpaquePhaseMaxPrograms> seen{};uint64_t bytes=0;
                for(const auto& program:config_.retained_opaque_phase_programs->programs) {
                    if(!program)throw std::invalid_argument("Null opaque phase program");
                    const auto index=NativeRetainedOpaquePhasePixelIndex(program->source_sha256);
                    const auto code=std::span(reinterpret_cast<const uint8_t*>(program->spirv.data()),program->spirv.size()*sizeof(uint32_t));
                    if(!index||seen[*index]||program->baseline_version!=NativeShaderCompiler::CacheVersion()||
                       program->variant_version!=NativeRetainedOpaquePhaseConservativeCompilerVersion()||program->specialization_mask!=1794||
                       !NativeRetainedOpaquePhaseModuleCertified(program->source_sha256,true,NativeShaderVariant::RetainedOpaquePhaseConservativeV4,code)||
                       ContentHash(code)!=program->spirv_sha256||code.size()>kRetainedOpaquePhaseMaxModuleBytes-bytes)
                        throw std::invalid_argument("Uncertified or duplicate opaque phase program");
                    seen[*index]=true;bytes+=code.size();
                }
            }
        }
        if(config_.attachmentless_fsi&&!config_.native_fsi)
            throw std::invalid_argument("Attachmentless FSI requires packed Native rasterization");
        if(config_.native_early_depth_material_retention&&(!config_.native_fsi||!config_.native_early_depth||
           config_.attachmentless_fsi||config_.implicit_fsi_barriers||config_.retained_opaque_writer||
           config_.retained_opaque_catalog||config_.retained_fp24_depth_writer||config_.retained_shared_depth_queries))
            throw std::invalid_argument("Early depth material retention requires canonical packed Native draws and immutable early depth");
        if(config_.builtin_readonly_depth_queries&&(!config_.native_fsi||config_.specialize_fsi_pipeline_state))
            throw std::invalid_argument("Read-only query builtin requires packed Native FSI");
        if(config_.builtin_atomic_depth&&!config_.native_fsi)
            throw std::invalid_argument("Atomic depth requires packed Native rasterization");
        if(config_.fsi_state_forward_cfg&&!config_.specialize_fsi_pipeline_state)
            throw std::invalid_argument("FSI forward composition requires exact state specialization mode");
        if(config_.fsi_state_inactive_early_catalog&&!config_.fsi_state_material_composition)
            throw std::invalid_argument("Inactive early catalogue admission requires material composition");
        if(config_.fsi_state_material_composition&&(!config_.specialize_fsi_pipeline_state||
           !config_.native_fsi||!config_.unified_gpu_queue||!config_.async_gpu_submissions||
           !config_.batch_transfers||!config_.defer_native_queries||config_.fsi_state_forward_cfg||
           config_.multi_draw_command_buffers||
           config_.attachmentless_fsi||config_.implicit_fsi_barriers||config_.builtin_atomic_depth||
           config_.builtin_readonly_depth_queries||config_.opaque_color_overwrite||
           config_.forward_control_flow||config_.forward_vertex_control_flow||config_.forward_pixel_catalog||
           config_.exact_fsi_stores||config_.fsi_hoist_mode!=NativeFsiHoistMode::Disabled||
           config_.retained_opaque_writer||config_.retained_opaque_catalog||config_.retained_fp24_depth_writer||
           config_.retained_shared_depth_queries||config_.retained_d32_composition||config_.retain_primary_across_disjoint_queries||
           config_.reuse_retained_opaque_resources||(config_.queue_retained_exports_for_draws&&!config_.retained_opaque_phase)||
           StencilMaterialEnabled()||config_.replay_fragment_invocations||!config_.debug_pixels.empty()))
            throw std::invalid_argument("FSI material composition requires canonical asynchronous Native materials and depth/query paths");
        if(config_.specialize_fsi_pipeline_state&&(!config_.native_fsi||config_.builtin_readonly_depth_queries||config_.implicit_fsi_barriers||
           config_.builtin_atomic_depth||config_.opaque_color_overwrite||config_.forward_control_flow||
           config_.forward_vertex_control_flow||config_.forward_pixel_catalog))
            throw std::invalid_argument("FSI state specialization requires exclusive packed Native shader mode");
        if(config_.implicit_fsi_barriers&&!config_.native_fsi)
            throw std::invalid_argument("Implicit FSI barriers require packed native rasterization");
        if(config_.implicit_fsi_barriers)
            report["shader_variant"]=std::string(kImplicitFsiVariantIdentity);
        // Hardware render targets batch like FSI: each draw keeps its own
        // command list, framebuffer and image references until its fence.
        submission_capacity_=NativeSubmissionCapacity(config_.async_submission_capacity,config_.async_gpu_submissions);
        if(config_.async_pipelines&&device_)async_pipelines_.Start(4);
        pending_draws_.reserve(submission_capacity_.lists);
        diagnostic_counters_.completed_frames.reserve(65);
        if(!output_.empty())fs::create_directories(output_);
        if(device_) {
            report["mode"]=config_.present?"live native execution with presentation callback":"live native execution";
            report["limitations"][0]="Independent gameplay rendering, control/query replacement and presentation equivalence remain unverified";
        }
        if(bool(device_)!=bool(queue_))throw std::runtime_error("Native device and queue must be supplied together");
        if(device_&&config_.native_fsi&&!config_.ordered_pixel_interlock_enabled)
            throw std::runtime_error("Native packed raster operations require enabled fragment pixel interlock");
        if(!device_) {
            if(config_.native_fsi) {
                owned_fsi_device_=std::make_unique<FsiDevice>(config_.restore_depth_stencil_export,false,
                    config_.native_readonly_depth_read_only_pass);
                device_=&owned_fsi_device_->device();
                // FsiDevice only succeeds after enabling ordered pixel interlock.
                config_.ordered_pixel_interlock_enabled=true;
                config_.shader_stencil_export_enabled=owned_fsi_device_->stencil_export_enabled();
            }
            else {interface_=CreateVulkanInterface();owned_device_=interface_?interface_->createDevice():nullptr;device_=owned_device_.get();config_.shader_stencil_export_enabled=false;}
            if(!device_)throw std::runtime_error("Vulkan unavailable");
            // Both standalone device constructors enable their supported core
            // features. Borrowed gameplay devices provide enabled limits in
            // config, since support alone does not prove a feature is enabled.
            const auto& vk=static_cast<const VulkanDevice&>(*device_);
            VkPhysicalDeviceFeatures features{};
            vkGetPhysicalDeviceFeatures(vk.physicalDevice,&features);
            config_.storage_image_extended_formats_enabled=features.shaderStorageImageExtendedFormats;
            config_.max_sampler_anisotropy=features.samplerAnisotropy?
                uint32_t(vk.physicalDeviceProperties.limits.maxSamplerAnisotropy):0;
            owned_queue_=device_->createCommandQueue(RenderCommandListType::DIRECT);queue_=owned_queue_.get();
        }
        // Identify the existing renderer device in replay/live diagnostics.
        // CPU Vulkan devices qualify for FSI too; comparisons need this proof.
        {
            const auto& properties=static_cast<const VulkanDevice&>(*device_).physicalDeviceProperties;
            report["device"]={{"name",properties.deviceName},{"type",uint32_t(properties.deviceType)},
                {"vendor_id",properties.vendorID},{"device_id",properties.deviceID},
                {"driver_version",properties.driverVersion},{"api_version",properties.apiVersion},
                {"timestamp_period_ns",properties.limits.timestampPeriod},
                {"software",properties.deviceType==VK_PHYSICAL_DEVICE_TYPE_CPU}};
        }
        if(config_.attachmentless_fsi) {
            const auto& limits=static_cast<const VulkanDevice&>(*device_).physicalDeviceProperties.limits;
            attachmentless_fsi_supported_=NativeAttachmentlessFsiEligible(true,1,1,limits);
            if(!attachmentless_fsi_supported_) {
                attachmentless_fsi_refused_=true;++attachmentless_fsi_refusals_;
            }
        }
        if(!device_->getCapabilities().descriptorIndexing)
            throw std::runtime_error("Native texture arrays require enabled partially-bound descriptor indexing");
        commands_=queue_->createCommandList();fence_=device_->createCommandFence();
        queries_=std::make_unique<NativeQueryTracker>(*device_,submission_capacity_.lists,config_.host_query_reset);
        if(config_.native_readonly_depth_lower_bound_query&&(!config_.native_readonly_depth_attachment||
           config_.native_readonly_depth_skip_edram_barrier||config_.native_readonly_depth_conservative||
           config_.retained_d32_composition||config_.retained_shared_depth_queries||
           config_.retain_primary_across_disjoint_queries||config_.retained_fp24_depth_writer||
           config_.retained_opaque_writer||config_.retained_opaque_catalog))
            throw std::invalid_argument("Lower-bound queries require exclusive readonly depth and real packed-buffer dependencies");
        if(config_.native_readonly_depth_read_only_pass&&(!config_.native_readonly_depth_attachment||
           config_.retained_d32_composition||config_.retained_shared_depth_queries||
           config_.retain_primary_across_disjoint_queries||config_.retained_fp24_depth_writer||
           config_.retained_opaque_writer||config_.retained_opaque_catalog))
            throw std::invalid_argument("Preserved read-only depth pass requires the original exclusive D32 query path");
        if(config_.native_readonly_depth_no_write_metadata&&(!config_.native_readonly_depth_attachment||
           config_.retained_d32_composition||
           config_.retained_shared_depth_queries||config_.retain_primary_across_disjoint_queries||
           config_.retained_fp24_depth_writer||config_.retained_opaque_writer||config_.retained_opaque_catalog))
            throw std::invalid_argument("Readonly no-write metadata requires the original exclusive D32 query path");
        if(config_.native_readonly_depth_conservative&&(!config_.native_readonly_depth_attachment||
           config_.retained_d32_composition||config_.retained_shared_depth_queries||
           config_.retain_primary_across_disjoint_queries||config_.retained_fp24_depth_writer||
           config_.retained_opaque_writer||config_.retained_opaque_catalog))
            throw std::invalid_argument("Conservative readonly depth requires the original exclusive D32 query path");
        if(config_.pinned_null_constant_uploads&&(!config_.native_fsi||config_.delta_constant_uploads))
            throw std::invalid_argument("Pinned null constant uploads require Native FSI and original full-upload fallback");
        if(config_.submission_worker&&(!config_.async_gpu_submissions||!config_.unified_gpu_queue))
            throw std::invalid_argument("Native submission worker requires the asynchronous unified queue");
        // Hardware render targets (native_fsi off) use the same unified queue;
        // host working images stay authoritative until a packed consumer.
        if(config_.async_gpu_submissions&&!config_.unified_gpu_queue)
            throw std::invalid_argument("Asynchronous native work requires the unified queue");
        if(config_.unified_gpu_queue&&!config_.batch_transfers)
            throw std::invalid_argument("Unified native GPU work requires transfer batching");
        if(config_.raw_gpu_float_cpu_textures&&!config_.unified_gpu_queue)
            throw std::invalid_argument("Raw float CPU textures require the unified native GPU queue");
        if(config_.queued_volume_alias_loads&&!config_.unified_gpu_queue)
            throw std::invalid_argument("Queued volume aliases require the unified native GPU queue");
        if(config_.queue_cpu_memory_writes&&!config_.unified_gpu_queue)
            throw std::invalid_argument("Queued CPU memory writes require the unified native GPU queue");
        if(config_.retire_completed_query_prefixes&&(!config_.defer_native_queries||!config_.unified_gpu_queue))
            throw std::invalid_argument("Native query prefix retirement requires deferred unified work");
        if(config_.gpu_physical_timestamps&&(!config_.unified_gpu_queue||config_.gpu_draw_timestamps||
           !config_.gpu_physical_interval||config_.gpu_physical_first>=config_.gpu_physical_end||config_.gpu_physical_end==UINT32_MAX))
            throw std::invalid_argument("Physical GPU timing requires unified work, valid frame sampling and no per-draw timer");
        if(config_.gpu_batch_attribution&&(!config_.gpu_timestamps||!config_.unified_gpu_queue))
            throw std::invalid_argument("GPU batch attribution requires unified GPU timestamps");
        if(config_.replay_fragment_invocations&&(!config_.native_fsi||!config_.unified_gpu_queue||
           !config_.async_gpu_submissions||config_.specialize_fsi_pipeline_state))
            throw std::invalid_argument("Fragment census requires standalone asynchronous packed Native replay without FSI state specialization");
#ifndef GOW3_NATIVE_FRAGMENT_CENSUS_REPLAY
        if(config_.replay_fragment_invocations)
            throw std::invalid_argument("Fragment census is not compiled into this Scene target");
#endif
        if(config_.native_stencil_material_cutoff&&config_.replay_stencil_material_mode)
            throw std::invalid_argument("Stencil material runtime and census modes are exclusive");
#ifndef GOW3_NATIVE_STENCIL_MATERIAL_REPLAY
        if(config_.replay_stencil_material_mode)
            throw std::invalid_argument("Stencil material diagnostic is not compiled into this Scene target");
#endif
        if(StencilMaterialEnabled()) {
            const auto& program=StencilMaterialProgram();
            const auto mode=config_.native_stencil_material_cutoff?NativeStencilMaterialMode::Uninstrumented:
                NativeStencilMaterialMode(config_.replay_stencil_material_mode);
            if(config_.replay_stencil_material_mode>2||!config_.native_fsi||
               (config_.replay_stencil_material_mode&&!owned_fsi_device_)||
               !config_.ordered_pixel_interlock_enabled||!config_.unified_gpu_queue||!config_.async_gpu_submissions||
               config_.attachmentless_fsi||config_.implicit_fsi_barriers||config_.specialize_fsi_pipeline_state||
               config_.exact_fsi_stores||config_.fsi_hoist_mode!=NativeFsiHoistMode::Disabled||
               config_.retained_opaque_writer||config_.retained_opaque_catalog||config_.retained_fp24_depth_writer||
               config_.opaque_color_overwrite||config_.forward_control_flow||config_.forward_vertex_control_flow||
               config_.forward_pixel_catalog||!config_.debug_pixels.empty()||!program||
               !program->Authenticated()||program->mode!=mode||program->source!=kStencilMaterialPixel||
               program->canonical_version!=kStencilMaterialCanonicalVersion||NativeShaderCompiler::CacheVersion()!=kStencilMaterialCanonicalVersion||
               program->derived_version!=NativeStencilMaterialCompilerVersion()||!NativeStencilMaterialDerivedCertified(program->spirv,mode))
                throw std::invalid_argument("Stencil material requires exclusive canonical Native state and authenticated program");
#ifdef GOW3_NATIVE_STENCIL_MATERIAL_REPLAY
            if(config_.replay_stencil_material_mode) {
                // Only diagnostic modes add binding1, counters and readback.
                // Owned FsiDevice actually enables fragmentStoresAndAtomics.
                descriptor_descs_[4]=RenderDescriptorSetDesc(stencil_material_ranges_.data(),2);
                stencil_material_stats_.samples=Json::array();
            }
#endif
        }
        if(config_.gpu_draw_timestamps&&!config_.unified_gpu_queue)
            throw std::invalid_argument("Draw timestamps require the unified GPU queue");
        if(config_.gpu_timestamps&&!config_.unified_gpu_queue)
            throw std::invalid_argument("Native GPU timestamps require the unified queue");
        if((config_.packed_depth_render_pass_groups||config_.packed_color_render_pass_groups||config_.fsi_draw_pass_groups)&&(!config_.native_fsi||!config_.ordered_pixel_interlock_enabled||
           !config_.unified_gpu_queue||!config_.async_gpu_submissions||config_.multi_draw_command_buffers||
           config_.attachmentless_fsi||config_.implicit_fsi_barriers||
           config_.builtin_readonly_depth_queries||(config_.specialize_fsi_pipeline_state&&!config_.fsi_state_material_composition)||config_.fsi_state_forward_cfg||
           config_.exact_fsi_stores||config_.fsi_hoist_mode!=NativeFsiHoistMode::Disabled||
           config_.retained_opaque_writer||config_.retained_opaque_catalog||config_.retained_fp24_depth_writer||
           config_.retained_shared_depth_queries||config_.retained_d32_composition||config_.retain_primary_across_disjoint_queries||
           config_.opaque_color_overwrite||config_.forward_control_flow||config_.forward_vertex_control_flow||config_.forward_pixel_catalog||
           !config_.debug_pixels.empty()))
            throw std::invalid_argument("Packed pass groups require certified primary writers and the bounded asynchronous Native queue");
        if(config_.packed_color_blend_pass_groups&&(!config_.packed_color_render_pass_groups||
           !config_.fsi_state_material_composition||!config_.specialize_fsi_pipeline_state))
            throw std::invalid_argument("Packed blend pass groups require parent packed groups and material state composition");
        if(config_.packed_color_action_pass_groups&&(!config_.packed_color_render_pass_groups||
           !config_.fsi_state_material_composition||!config_.specialize_fsi_pipeline_state))
            throw std::invalid_argument("Packed action pass groups require parent packed groups and material state composition");
        if(config_.readonly_query_render_pass_groups&&(!config_.native_fsi||!config_.native_readonly_depth_attachment||
           !config_.native_readonly_depth_read_only_pass||!config_.native_readonly_depth_skip_edram_barrier||
           !config_.native_readonly_depth_no_write_metadata||!config_.host_query_reset||
           !config_.defer_native_queries||!config_.unified_gpu_queue||!config_.async_gpu_submissions||
           config_.multi_draw_command_buffers||config_.native_readonly_depth_conservative||
           config_.native_readonly_depth_lower_bound_query||config_.retained_opaque_writer||
           config_.retained_opaque_catalog||config_.retained_fp24_depth_writer||config_.retained_shared_depth_queries||
           config_.retained_d32_composition||config_.retain_primary_across_disjoint_queries||!config_.debug_pixels.empty()))
            throw std::invalid_argument("Readonly query pass groups require immutable D32, host resets and the asynchronous Native queue");
        if(config_.multi_draw_command_buffers&&(!config_.unified_gpu_queue||!config_.async_gpu_submissions))
            throw std::invalid_argument("Native draw command groups require packed rasterization and the asynchronous unified queue");
        if(config_.retained_d32_composition&&(!config_.native_fsi||!config_.native_readonly_depth_attachment||
           !config_.defer_native_queries||!config_.unified_gpu_queue||!config_.async_gpu_submissions||!config_.batch_transfers||
           (bool(RetainedOpaqueEnabled())==bool(config_.retained_fp24_depth_writer))||config_.retained_shared_depth_queries||
           config_.native_readonly_depth_skip_edram_barrier||config_.fsi_hoist_mode!=NativeFsiHoistMode::Disabled||config_.exact_fsi_stores||
           !config_.debug_pixels.empty()))
            throw std::invalid_argument("Retained D32 composition requires one primary writer and canonical deferred Native D32 queries");
        if(config_.retain_primary_across_disjoint_queries&&(!config_.retained_d32_composition||!config_.native_fsi||!config_.native_readonly_depth_attachment||
           !config_.defer_native_queries||!config_.unified_gpu_queue||!config_.async_gpu_submissions||!config_.batch_transfers||
           (bool(RetainedOpaqueEnabled())==bool(config_.retained_fp24_depth_writer))||config_.retained_shared_depth_queries||
           config_.native_readonly_depth_skip_edram_barrier||config_.fsi_hoist_mode!=NativeFsiHoistMode::Disabled||config_.exact_fsi_stores||
           !config_.debug_pixels.empty()))
            throw std::invalid_argument("Disjoint retained query requires one exact primary writer and canonical deferred D32 Native queries");
        if(config_.retained_shared_depth_queries&&(!config_.retained_opaque_catalog||!config_.defer_native_queries||
           !config_.native_fsi||!config_.unified_gpu_queue||!config_.async_gpu_submissions||!config_.batch_transfers||
           config_.native_readonly_depth_attachment||config_.builtin_readonly_depth_queries||config_.retained_opaque_writer))
            throw std::invalid_argument("Shared retained depth queries require the exclusive deferred asynchronous V2 path");
        if(config_.retained_opaque_writer&&config_.retained_opaque_catalog)
            throw std::invalid_argument("Retained opaque writer variants are mutually exclusive");
        if(config_.reuse_retained_opaque_resources&&(!RetainedOpaqueEnabled()||!config_.native_fsi||
           !config_.batch_transfers||!config_.unified_gpu_queue||!config_.async_gpu_submissions))
            throw std::invalid_argument("Retained resource reuse requires the asynchronous unified Native retained writer");
        if(config_.queue_retained_exports_for_draws&&((!RetainedOpaqueEnabled()&&!config_.retained_opaque_phase)||!config_.native_fsi||
           !config_.unified_gpu_queue||!config_.async_gpu_submissions||!config_.batch_transfers))
            throw std::invalid_argument("Queued retained draw exports require the asynchronous unified Native writer");
        if(config_.retained_opaque_catalog&&config_.retained_opaque_catalog_programs) {
            const auto& programs=config_.retained_opaque_catalog_programs->programs;
            if(programs.size()>kRetainedOpaqueCatalogMaxPrograms)throw std::invalid_argument("Retained opaque catalog exceeds startup source bound");
            for(size_t i=0;i<programs.size();++i) {
                if(!programs[i]||!RetainedOpaqueCatalogPair(programs[i]->vertex_sha256,programs[i]->source_sha256))
                    throw std::invalid_argument("Retained opaque catalog contains unaudited source");
                for(size_t j=0;j<i;++j)if(programs[j]->source_sha256==programs[i]->source_sha256)
                    throw std::invalid_argument("Retained opaque catalog contains duplicate pixel source");
            }
        }
        if(RetainedOpaqueEnabled()&&(!config_.native_fsi||!config_.unified_gpu_queue||!config_.async_gpu_submissions||
           (config_.native_readonly_depth_attachment&&!config_.retained_d32_composition)||config_.attachmentless_fsi||config_.specialize_fsi_pipeline_state||config_.fsi_state_forward_cfg||
           config_.implicit_fsi_barriers||config_.builtin_atomic_depth||config_.builtin_readonly_depth_queries||
           config_.opaque_color_overwrite||config_.forward_control_flow||config_.forward_vertex_control_flow||config_.forward_pixel_catalog))
            throw std::invalid_argument("Retained opaque writer requires the exclusive asynchronous Native path");
        if(config_.retained_fp24_depth_writer&&(!config_.native_fsi||!config_.unified_gpu_queue||!config_.async_gpu_submissions||!config_.batch_transfers||
           config_.retained_opaque_writer||config_.retained_opaque_catalog||config_.retained_shared_depth_queries||config_.reuse_retained_opaque_resources||config_.queue_retained_exports_for_draws||
           (config_.native_readonly_depth_attachment&&!config_.retained_d32_composition)||config_.attachmentless_fsi||config_.specialize_fsi_pipeline_state||config_.fsi_state_forward_cfg||
           config_.implicit_fsi_barriers||config_.builtin_atomic_depth||config_.builtin_readonly_depth_queries||config_.multi_draw_command_buffers||
           config_.opaque_color_overwrite||config_.forward_control_flow||config_.forward_vertex_control_flow||config_.forward_pixel_catalog))
            throw std::invalid_argument("Retained FP24 depth writer requires its exclusive asynchronous Native path");
        if(config_.reuse_early_depth_readonly_boundaries&&(!config_.native_early_depth||config_.native_early_depth_catalog))
            throw std::invalid_argument("Early depth readonly-boundary reuse requires V1 with catalogue disabled");
        if(config_.native_early_depth_catalog&&!config_.native_early_depth)
            throw std::invalid_argument("Early depth catalogue requires the independently keyed shipping aid");
        // The grouped atomic writer changes only the two pinned null-PS depth
        // families. It closes before query/material consumers and retains their
        // source barriers and packed-write version invalidations. Other atomic
        // compositions remain outside this validated path.
        if(config_.native_early_depth&&(!config_.native_fsi||!config_.unified_gpu_queue||!config_.async_gpu_submissions||!config_.batch_transfers||
           config_.attachmentless_fsi||(config_.specialize_fsi_pipeline_state&&!config_.fsi_state_material_composition)||config_.fsi_state_forward_cfg||config_.implicit_fsi_barriers||
           (config_.builtin_atomic_depth&&!config_.packed_depth_render_pass_groups)||config_.builtin_readonly_depth_queries||config_.retained_opaque_writer||config_.retained_opaque_catalog||
           config_.retained_fp24_depth_writer||config_.retained_shared_depth_queries||config_.retained_d32_composition||config_.retain_primary_across_disjoint_queries||
           (config_.native_readonly_depth_skip_edram_barrier&&!config_.native_readonly_depth_read_only_pass)||
           config_.exact_fsi_stores||config_.fsi_hoist_mode!=NativeFsiHoistMode::Disabled||
           config_.opaque_color_overwrite||config_.forward_control_flow||config_.forward_vertex_control_flow||config_.forward_pixel_catalog||!config_.debug_pixels.empty()))
            throw std::invalid_argument("Early depth aid requires its exclusive asynchronous canonical Native path");
        if(config_.native_readonly_depth_skip_edram_barrier&&!config_.native_readonly_depth_attachment)
            throw std::invalid_argument("Skipping packed EDRAM requires the audited Native D32 query path");
        if(config_.native_readonly_depth_attachment&&(!config_.native_fsi||!config_.unified_gpu_queue||!config_.async_gpu_submissions||
           config_.attachmentless_fsi||(config_.specialize_fsi_pipeline_state&&!config_.fsi_state_material_composition)||config_.fsi_state_forward_cfg||
           config_.implicit_fsi_barriers||(config_.builtin_atomic_depth&&!config_.packed_depth_render_pass_groups)||config_.builtin_readonly_depth_queries||
           config_.opaque_color_overwrite||config_.forward_control_flow||config_.forward_vertex_control_flow||config_.forward_pixel_catalog))
            throw std::invalid_argument("Native readonly D32 requires the exclusive asynchronous packed query path");
        if(config_.unified_gpu_queue)gpu_work_queue_=std::make_unique<NativeGpuWorkQueue>(*device_,*queue_,
            [this](const auto& completion){RetireGpuWork(completion);},config_.async_gpu_submissions,config_.gpu_timestamps,submission_capacity_.lists,config_.gpu_batch_attribution,config_.max_inflight_batches,config_.submission_worker,config_.gpu_physical_timestamps,
            config_.gpu_physical_first,config_.gpu_physical_end,config_.gpu_physical_interval);
        if(DrawGroupsEnabled())gpu_work_queue_->ConfigureDrawCommandGroups([this]{SealDrawCommandGroup();});
        if(config_.gpu_draw_timestamps)try {
            draw_timestamps_=std::make_unique<NativeDrawTimestamps>(*device_,*queue_,submission_capacity_.lists,config_.gpu_draw_timestamps_every_frame);
        } catch(...) {/* Optional diagnostic allocation must not prevent rendering. */}
#ifdef GOW3_NATIVE_FRAGMENT_CENSUS_REPLAY
        if(config_.replay_fragment_invocations)try {
            // An owned standalone device supplies its actual enabled feature;
            // physical support or a borrowed-device function pointer is insufficient.
            fragment_census_=std::make_unique<NativeFragmentInvocationCensus>(*device_,*queue_,submission_capacity_.lists,
                owned_fsi_device_&&owned_fsi_device_->pipeline_statistics_enabled());
        }catch(...) {/* Save explicit unavailable status; do not alter rendering. */}
#endif
        if(config_.gpu_timestamps)try {
            presentation_gpu_timings_=std::make_unique<NativeSynchronousGpuTimings>(*device_,*queue_);
        }catch(...) {/* Optional diagnostics never prevent rendering. */}
        if(config_.batch_transfers)transfer_batch_=std::make_unique<NativeTransferBatch>(*device_,*queue_,gpu_work_queue_.get());
        transfers_=std::make_unique<PassTransfer>(*device_,*queue_,fullscreen_spv,resolve_spv,restore_depth_spv,transfer_batch_.get(),
            config_.restore_depth_stencil_export&&config_.shader_stencil_export_enabled?
                std::span<const uint32_t>(restore_depth_stencil_spv):std::span<const uint32_t>{});
        transfers_->SetDescriptorPoolEnabled(config_.recycle_pass_transfer_descriptors);
        transfers_->SetRenderPassCacheEnabled(config_.recycle_pass_transfer_render_passes);
        if(config_.recycle_vertex_buffers)vertex_buffer_pool_=std::make_unique<RetiredBufferPool<RenderBuffer>>();
        resolve_memory_=std::make_unique<ResolveMemory>(*device_,*queue_,memory_store_spv,memory_load_spv,512u*1024*1024,0,transfer_batch_.get(),config_.compact_cpu_texture_version_pages,config_.block_page_versions,config_.range_texture_versions,config_.coalesced_texture_ranges,config_.reuse_defined_ranges);
        resolve_memory_->SetCpuWriteUploadReuseEnabled(config_.reuse_cpu_write_upload);
        resolve_memory_->SetCpuUploadReceiptForwardingEnabled(config_.forward_cpu_upload_receipts);
        resolve_memory_->SetQueuedVolumeLoadsEnabled(config_.queued_volume_alias_loads);
        resolve_memory_->SetImagePoolEnabled(config_.recycle_resolve_images);
        resolve_memory_->SetDescriptorPoolEnabled(config_.recycle_resolve_descriptors);
        resolve_memory_->SetIdentityViewReuseEnabled(config_.reuse_resolve_identity_views);
        if(config_.canonical_resolve_stores&&config_.native_fsi)
            canonical_resolve_stats_.configured=resolve_memory_->EnableCanonicalStoreReceipt(memory_store_canonical_spv);
        if(config_.direct_atlas_depth_resolves&&config_.native_fsi)
            direct_atlas_depth_resolve_stats_.configured=resolve_memory_->EnableAtlasRgbaDepthStore(
                memory_store_rgba_stencil_spv);
        if(config_.raw_gpu_float_cpu_textures)resolve_memory_->EnableRawFloatCpuTextures(raw_float_texture_spv);
        if(config_.compact_resolve_channels)
            resolve_memory_->EnableCompactLoadFormats(memory_load_r32_spv,memory_load_rg32_spv,
                config_.storage_image_extended_formats_enabled);
        edram_memory_=std::make_unique<ResolveMemory>(*device_,*queue_,memory_store_spv,memory_load_spv,2048*5120,0,transfer_batch_.get(),false,config_.block_page_versions,false,false,config_.reuse_defined_ranges);
        edram_memory_->SetDescriptorPoolEnabled(config_.recycle_resolve_descriptors);
        edram_memory_->SetIdentityViewReuseEnabled(config_.reuse_resolve_identity_views);
        if(!config_.native_fsi&&transfer_batch_)edram_memory_->EnableDirectColorRestore(fullscreen_spv,memory_restore_frag_spv);
        if(config_.packed_primary_stencil_clear&&config_.native_fsi)
            packed_primary_stencil_clear_stats_.configured=edram_memory_->EnablePackedStencilClear(
                packed_primary_stencil_clear_spv);
        if(config_.packed_secondary_stencil_clear&&config_.native_fsi)
            packed_secondary_stencil_clear_stats_.configured=edram_memory_->EnablePackedSecondaryStencilClear(
                packed_secondary_stencil_clear_spv);
        if(config_.fused_atlas_depth_clear&&config_.native_fsi)
            fused_atlas_depth_clear_stats_.configured=edram_memory_->EnableAtlasDepthClear(
                atlas_depth_clear_spv,atlas_depth_clear_host_spv);
        if(RetainedOpaqueEnabled()||config_.retained_opaque_phase)try {
            // A Writer owns one immutable conversion pair for its entire life.
            // The V4 phase cannot admit V3 pixels or reuse legacy half-depth
            // attachments; other retained modes keep their original encoding.
            const std::span<const uint32_t> restore=config_.retained_opaque_phase?
                std::span<const uint32_t>(retained_opaque_phase_restore_spv):std::span<const uint32_t>(retained_opaque_restore_spv);
            const std::span<const uint32_t> store=config_.retained_opaque_phase?
                std::span<const uint32_t>(retained_opaque_phase_store_spv):std::span<const uint32_t>(retained_opaque_store_spv);
            retained_opaque_writer_=std::make_unique<NativeRetainedOpaqueWriter>(*device_,restore,store,config_.reuse_retained_opaque_resources,
                config_.share_retained_opaque_phase_scratch?transfer_batch_.get():nullptr,
                config_.share_retained_opaque_phase_attachments?gpu_work_queue_.get():nullptr);
        }catch(const std::exception&) {/* Optional startup refusal; no recorded commands. */}
        if(config_.retained_fp24_depth_writer)try {
            retained_fp24_depth_writer_=std::make_unique<NativeRetainedFp24DepthWriter>(*device_,retained_fp24_depth_restore_spv,retained_fp24_depth_store_spv);
        }catch(const std::exception&) {/* Optional startup failure has no recorded GPU work. */}
        if(config_.fused_packed_atlas_resolves&&config_.direct_atlas_depth_resolves&&config_.native_fsi) {
            fused_packed_atlas_resolve_=std::make_unique<NativeFusedPackedAtlasResolve>(
                *device_,*queue_,fullscreen_spv,fused_packed_atlas_resolve_spv,transfer_batch_.get());
            fused_packed_atlas_resolve_stats_.configured=direct_atlas_depth_resolve_stats_.configured&&
                fused_packed_atlas_resolve_->supported();
        }
        if(config_.native_early_depth)try {
            early_depth_conversion_=std::make_unique<NativeReadonlyDepthConversion>(*device_,early_depth_lower_bound_comp_spv);
        }catch(...) {/* Optional startup refusal uses ordinary canonical Native. */}
        if(config_.native_readonly_depth_attachment)try {
            readonly_depth_conversion_=std::make_unique<NativeReadonlyDepthConversion>(*device_,
                config_.native_readonly_depth_lower_bound_query?std::span<const uint32_t>(early_depth_lower_bound_comp_spv):
                    std::span<const uint32_t>(readonly_depth_attachment_comp_spv),
                config_.native_readonly_depth_read_only_pass);
        }catch(const std::exception&) {/* No command exists: optional startup resource refusal uses ordinary Native. */}
        const RenderPushConstantRange push(0,0,0,24,RenderShaderStageFlag::VERTEX|RenderShaderStageFlag::PIXEL);
        layout_=device_->createPipelineLayout(RenderPipelineLayoutDesc(&push,1,descriptor_descs_.data(),config_.native_fsi?5:4));
        if(config_.native_fsi) {
            report["raster_operations"]="ordered native packed EDRAM";
            report["limitations"][1]="Single-sample interlock supports packed fixed/7e3/depth/stencil and polygon offset; gamma, extended half precision and full early-depth ordering remain unsupported";
        }
    }
    void SaveReport() {if(output_.empty())return;ReportQueries();ReportTransfers();std::ofstream f(output_/"replay.json");f<<report.dump(2)<<'\n';if(!f)throw std::runtime_error("Cannot write replay report");}
    void Preview(RenderTexture& source,const std::string& name,bool depth=false) {
        if(output_.empty())return;
        NativeTransferBatch::Suspension synchronous(transfer_batch_.get());
        FlushPendingDraws();
        const auto& desc=static_cast<VulkanTexture&>(source).desc;
        auto target=device_->createTexture(RenderTextureDesc::ColorTarget(desc.width,desc.height,RenderFormat::R32G32B32A32_FLOAT));
        const RenderRect r(0,0,desc.width,desc.height);
        transfers_->Resolve(source,*target,r,r);
        auto bytes=validation::ReadImage(*device_,*queue_,*target,16);
        if(config_.debug_raw_images) {
            std::ofstream raw(output_/(name+".rgba32f"),std::ios::binary);
            if constexpr(std::endian::native==std::endian::big)
                for(size_t i=0;i<bytes.size();i+=4)std::reverse(bytes.begin()+i,bytes.begin()+i+4);
            raw.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());
            if constexpr(std::endian::native==std::endian::big)
                for(size_t i=0;i<bytes.size();i+=4)std::reverse(bytes.begin()+i,bytes.begin()+i+4);
            if(!raw)throw std::runtime_error("Cannot write native RGBA image diagnostics");
            report["raw_artifacts"].push_back({{"name",name+".rgba32f"},{"width",desc.width},{"height",desc.height},
                                               {"format","rgba32_float_little_endian"}});
        }
        float lo=INFINITY,hi=-INFINITY; size_t nonzero=0;
        for(size_t i=0;i<bytes.size();i+=16) {
            std::array<float,4> c;std::memcpy(c.data(),bytes.data()+i,16);
            for(unsigned k=0;k<(depth?1:3);++k) {
                if(!std::isfinite(c[k])) throw std::runtime_error("Non-finite native image output");
                lo=std::min(lo,c[k]);hi=std::max(hi,c[k]);
            }
            nonzero+=c[0]!=0||(!depth&&(c[1]!=0||c[2]!=0));
        }
        std::ofstream f(output_/(name+".ppm"),std::ios::binary);f<<"P6\n"<<desc.width<<' '<<desc.height<<"\n255\n";
        for(size_t i=0;i<bytes.size();i+=16) {
            std::array<float,4> c;std::memcpy(c.data(),bytes.data()+i,16);
            for(unsigned k=0;k<3;++k) {float v=depth?(hi>lo?(c[0]-lo)/(hi-lo):c[0]):c[k];f.put(char(std::lround(std::clamp(v,0.f,1.f)*255)));}
        }
        if(!f) throw std::runtime_error("Cannot write native preview");
        report["artifacts"].push_back({{"name",name+".ppm"},{"width",desc.width},{"height",desc.height},{"min",lo},{"max",hi},{"nonzero_pixels",nonzero},{"depth_normalized_for_preview",depth}});
    }
    void ReportTransfers() {
        if(!transfer_report_dirty_)return;
#ifdef GOW3_NATIVE_STENCIL_MATERIAL_REPLAY
        if(config_.replay_stencil_material_mode) {
            const auto& s=stencil_material_stats_;
            report["stencil_material_cutoff"]={{"enabled",true},{"mode",config_.replay_stencil_material_mode==1?"observe":"cutoff"},
                {"derived_version",config_.replay_stencil_material_program->derived_version},{"prepared",s.prepared},
                {"candidates",s.candidates},{"selected",s.selected},{"refusals",s.refusals},{"query_refusals",s.query_refusals},
                {"budget_refusals",s.budget_refusals},{"accepted",s.accepted},{"retired",s.retired},{"pending",s.accepted-s.retired},
                {"samples",s.samples},{"refusal_reasons",s.refusal_reasons},{"counter_scope","Nonhelper storage atomics before material/alpha discard; exact retired per-draw uint32 counters, not pipeline invocations"}};
        }
#endif
        if(config_.native_stencil_material_cutoff) {
            const auto& s=stencil_material_stats_;
            report["native_stencil_material_cutoff"]={{"enabled",true},{"requested",true},{"mode","uninstrumented"},
                {"program_ready",bool(config_.native_stencil_material_program)},
                {"derived_version",config_.native_stencil_material_program?config_.native_stencil_material_program->derived_version:std::string{}},
                {"prepared",s.prepared},{"candidates",s.candidates},{"selected",s.selected},{"refusals",s.refusals},
                {"query_refusals",s.query_refusals},{"refusal_reasons",s.refusal_reasons},
                {"counter_descriptors",0},{"counter_buffers",0},{"counter_readbacks",0}};
        }
        const auto& counters=diagnostic_counters_;
        report["cpu_stage_timings"]={{"enabled",time_cpu_stages_},
            {"scope","Per-draw, per-texture and Runtime operation host stages only. Disabled stage durations are null; creation-on-miss, completion and presentation timers are unchanged."}};
        const auto stage_ms=[this](double value)->Json {return time_cpu_stages_?Json(value):Json(nullptr);};
        if(time_cpu_stages_)report["cpu_stage_timings"]["retained_phase_draw_allowed"]={
            {"calls",retained_phase_predicate_timing_.calls},{"elapsed_ms",retained_phase_predicate_timing_.elapsed_ms},
            {"scope","Cumulative inclusive host time for every predicate invocation, including refusals and exceptions. Contains live shader/PSO lookups; overlaps draw setup/finish and boundary preparation, never additive GPU cost."}};
        if(time_cpu_stages_) {
            const auto& timing=retained_phase_bookkeeping_timing_;
            auto& accepted=report["cpu_stage_timings"]["retained_phase_accepted_bookkeeping"];
            accepted={{"calls",timing.inclusive.calls},{"elapsed_ms",timing.inclusive.elapsed_ms},
                {"scope","Cumulative inclusive host time after selected phase GPU work is accepted: four primary version expressions (post-write receipt or SurfaceVersion), RecordSurfaceWrite, AcceptedDraw, and accepted-draw counters/recipe lookup. Includes exceptional exits and nested query timer overhead; overlaps draw finish, never additive GPU cost."}};
            constexpr std::array<const char*,4> names{"pre_color","pre_depth","post_color","post_depth"};
            for(size_t i=0;i<names.size();++i)accepted["surface_versions"][names[i]]={
                {"calls",timing.surface_versions[i].calls},{"elapsed_ms",timing.surface_versions[i].elapsed_ms}};
            accepted["surface_versions_scope"]="Inclusive duration of each primary-version expression (including exceptional exits), nested within accepted bookkeeping. Pre-write expressions query SurfaceVersion; post-write expressions use a checked assignment receipt or the original SurfaceVersion query. Original call sites and function-argument evaluation semantics are preserved.";
        }
        if(config_.packed_zero_depth_clear)report["packed_zero_depth_clear"]={
            {"considered",packed_zero_clear_stats_.considered},{"selected",packed_zero_clear_stats_.selected},
            {"fallback",packed_zero_clear_stats_.fallback}};
        if(config_.packed_primary_stencil_clear) {
            const auto& stats=packed_primary_stencil_clear_stats_;
            report["packed_primary_stencil_clear"]={{"enabled",true},{"configured",stats.configured},
                {"identity","canonical-fp24-primary-stencil-clear-normalized-v1"},
                {"source_layout",std::array<uint32_t,6>{1280,720,1280,0,1,1}},
                {"candidates",stats.candidates},{"selected",stats.selected},
                {"packed_sources",stats.selected},{"query_refusals",stats.query_refusals},
                {"current_host_refusals",stats.current_host_refusals},
                {"provenance_refusals",stats.provenance_refusals},{"resource_refusals",stats.resource_refusals},
                {"source_pixels",stats.source_pixels},{"clear_pixels",stats.clear_pixels},
                {"counter_scope","Full packed FP24 normalization per selected stale/absent clear, rectangle-only stencil replacement. Current or dirty hosts use the original Native image path; counts are not paired restore savings or exclusive timing."}};
        }
        if(config_.packed_secondary_stencil_clear) {
            const auto& stats=packed_secondary_stencil_clear_stats_;
            report["packed_secondary_stencil_clear"]={{"enabled",true},{"configured",stats.configured},
                {"identity","canonical-fp24-secondary-stencil-clear-normalized-v1"},
                {"source_layout",std::array<uint32_t,6>{640,360,640,1440,1,1}},
                {"candidates",stats.candidates},{"selected",stats.selected},
                {"packed_sources",stats.selected},{"query_refusals",stats.query_refusals},
                {"current_host_refusals",stats.current_host_refusals},
                {"provenance_refusals",stats.provenance_refusals},{"resource_refusals",stats.resource_refusals},
                {"source_pixels",stats.source_pixels},{"clear_pixels",stats.clear_pixels},
                {"counter_scope","Full packed FP24 normalization per selected stale/absent clear, rectangle-only stencil replacement. Current or dirty hosts use the original Native image path; counts are not paired restore savings or exclusive timing."}};
        }
        if(config_.fused_atlas_depth_clear) {
            uint64_t receipt_bytes=0,receipts=0;
            for(const auto& [key,image]:surfaces_)if(image.atlas_clear_receipt.source) {
                ++receipts;receipt_bytes+=static_cast<const VulkanTexture&>(
                    *image.atlas_clear_receipt.source).allocationInfo.size;
            }
            const auto& stats=fused_atlas_depth_clear_stats_;
            report["fused_atlas_depth_clear"]={{"enabled",true},{"configured",stats.configured},
                {"identity","canonical-unorm24-atlas-depth-one-receipt-v1"},
                {"source_layout",std::array<uint32_t,6>{1120,688,1120,1440,0,1}},
                {"candidates",stats.candidates},{"selected",stats.selected},
                {"provenance_refusals",stats.provenance_refusals},{"resource_refusals",stats.resource_refusals},
                {"packed_sources",stats.packed_sources},{"host_sources",stats.host_sources},
                {"source_pixels",stats.source_pixels},{"clear_pixels",stats.clear_pixels},
                {"deferred_host_materializations",stats.deferred_host_materializations},
                {"deferred_host_discards",stats.deferred_host_discards},
                {"retained_receipts",receipts},{"retained_receipt_bytes",receipt_bytes},
                {"peak_receipt_bytes",stats.peak_receipt_bytes},
                {"maximum_deferred_receipts",1},{"maximum_receipt_bytes",kNativeAtlasDepthClearMaxReceiptBytes},
                {"receipt_ownership_scope","Receipt gauges/peak cover Scene cache owners only, not all fence-owned transfer packets. New and delayed images are separately charged to the existing bounded transfer scratch budget."},
                {"counter_scope","Full source roundtrip per selected clear; deferred same-version host restoration is counted separately. Packed history remains authoritative; counters are not exclusive timing."}};
        }
        if(config_.direct_atlas_depth_resolves) {
            const auto& stats=direct_atlas_depth_resolve_stats_;
            report["direct_atlas_depth_resolves"]={{"enabled",true},{"configured",stats.configured},
                {"identity","canonical-unorm24-atlas-direct-rgba-resolve-v1"},
                {"source_layout",std::array<uint32_t,6>{1120,688,1120,1440,0,1}},
                {"candidates",stats.candidates},{"selected",stats.selected},
                {"packed_sources",stats.packed_sources},{"receipt_sources",stats.receipt_sources},
                {"current_host_refusals",stats.current_host_refusals},{"provenance_refusals",stats.provenance_refusals},
                {"resource_refusals",stats.resource_refusals},{"layout_refusals",stats.layout_refusals},
                {"query_refusals",stats.query_refusals},{"source_pixels",stats.source_pixels},
                {"packed_decode_pixels",stats.packed_decode_pixels},{"resolve_pixels",stats.resolve_pixels},
                {"prospective_restore_calls",stats.selected},{"prospective_restore_pixels",stats.source_pixels},
                {"counter_scope","Selected counts direct source resolves (including optional fused MRT). packed_decode_pixels counts only actual full RGBA loads; logical source_pixels includes fused MRT. Prospective restores describe stale/receipt state, not matched baseline work. Physical packing, destination repair, sampling and transfer scopes remain canonical."}};
        }
        if(config_.fused_packed_atlas_resolves) {
            const auto& s=fused_packed_atlas_resolve_stats_;
            report["fused_packed_atlas_resolves"]={{"enabled",true},{"configured",s.configured},
                {"identity","canonical-packed-unorm24-atlas-resolve-mrt-v1"},
                {"source_layout",std::array<uint32_t,6>{1120,688,1120,1440,0,1}},
                {"candidates",s.candidates},{"selected",s.selected},{"receipt_refusals",s.receipt_refusals},
                {"current_host_refusals",s.current_host_refusals},{"query_refusals",s.query_refusals},
                {"layout_refusals",s.layout_refusals},{"provenance_refusals",s.provenance_refusals},
                {"resource_refusals",s.resource_refusals},{"source_pixels",s.source_pixels},{"resolve_pixels",s.resolve_pixels},
                {"counter_scope","Selected operations replace one full canonical RGBA decode plus Resolve with one MRT. Logical source area is not GPU timing. Scratch only initializes the copied rectangle; original physical store, destination repair, packed history and host/receipt ownership stay canonical."}};
        }
        if(config_.direct_packed_color_resolves) {
            const auto& stats=direct_packed_color_resolve_stats_;
            report["direct_packed_color_resolves"]={{"enabled",true},{"configured",config_.native_fsi},
                {"identity","canonical-unorm10-primary-direct-color-resolve-v1"},
                {"source_layout",std::array<uint32_t,6>{1280,720,1280,720,2,1}},
                {"candidates",stats.candidates},{"selected",stats.selected},{"packed_sources",stats.packed_sources},
                {"query_refusals",stats.query_refusals},{"current_host_refusals",stats.current_host_refusals},
                {"provenance_refusals",stats.provenance_refusals},{"source_pixels",stats.source_pixels},
                {"resolve_pixels",stats.resolve_pixels},{"prospective_copy_calls",stats.selected},
                {"prospective_copy_pixels",stats.source_pixels},
                {"counter_scope","Exact absent/stale clean source bypass of the RGBA32F identity host copy. These are candidate opportunities, not matched baseline savings: repeated same-version resolves can decode again and later host consumers still restore. Full packed decode, real UNorm10 resolve, destination repair and physical store remain unchanged."}};
        }
        if(config_.atlas_depth_region_restores)report["atlas_depth_region_restores"]={
            {"enabled",true},{"identity","canonical-unorm24-atlas-source-region-v1"},
            {"source_layout",std::array<uint32_t,6>{1120,688,1120,1440,0,1}},
            {"maximum_source_extent",std::array<uint32_t,2>{kNativeAtlasDepthRegionMaxWidth,kNativeAtlasDepthRegionMaxHeight}},
            {"requests",atlas_depth_region_stats_.requests},{"selected",atlas_depth_region_stats_.selected},
            {"refusals",atlas_depth_region_stats_.refusals},{"partial_restores",atlas_depth_region_stats_.partial_restores},
            {"restored_pixels",atlas_depth_region_stats_.restored_pixels},
            {"full_extent_pixels",atlas_depth_region_stats_.full_extent_pixels},
            {"current_source_reads",atlas_depth_region_stats_.current_source_reads},
            {"counter_scope","Source restoration only; full-extent pixels are reference extents for these restorations, not measured OFF work or exclusive timing. Full allocations and all destination/consumer stages remain."}};
        report["hardware_render_passes"]={{"started",hardware_pass_stats_.started},{"continued",hardware_pass_stats_.continued}};
        {
            Json layouts=Json::array();
            for(const auto& l:msaa_fallback_.layouts)layouts.push_back({{"width",l[0]},{"height",l[1]},
                {"surface_info",l[2]},{"edram_base",l[3]},{"format",l[4]},{"depth",bool(l[5])}});
            report["msaa_fallback"]={{"enabled",config_.msaa_single_sample_fallback},
                {"surfaces",msaa_fallback_.surfaces},{"layouts",std::move(layouts)}};
        }
        report["surface_conversions"]={{"commits",surface_conversions_.commits},{"commit_pixels",surface_conversions_.commit_pixels},
            {"restores",surface_conversions_.restores},{"restore_pixels",surface_conversions_.restore_pixels},
            {"first_restores",surface_conversions_.first_restores},{"direct_color_restores",edram_memory_?edram_memory_->direct_color_restores():0},{"restore_keys",surface_conversions_.restore_keys},
            {"commit_keys",surface_conversions_.commit_keys}};
        if(config_.skip_full_color_clear_restore)report["full_color_clear_restore"]={
            {"requests",full_color_clear_restore_stats_.requests},
            {"full_overwrites",full_color_clear_restore_stats_.full_overwrites},
            {"skipped_restores",full_color_clear_restore_stats_.skipped_restores},
            {"skipped_restore_pixels",full_color_clear_restore_stats_.skipped_restore_pixels}};
        if(config_.retain_primary_across_disjoint_queries)report["retained_disjoint_queries"]={
            {"enabled",true},{"identity",kNativeRetainedDisjointQueryIdentity},
            {"secondary_depth_layout",std::array<uint32_t,6>{640,360,640,1440,1,1}},
            {"secondary_tile_interval",kNativeRetainedDisjointQueryTiles},
            {"preparation_calls",disjoint_query_stats_.preparation_calls},{"retained_preparations",disjoint_query_stats_.retained_preparations},
            {"accepted_draws",disjoint_query_stats_.accepted_draws},{"opaque_preserved_draws",disjoint_query_stats_.opaque_preserved_draws},
            {"depth_preserved_draws",disjoint_query_stats_.depth_preserved_draws},{"late_fallbacks",disjoint_query_stats_.late_fallbacks},
            {"query_preparation_calls",disjoint_query_stats_.query_preparation_calls},{"retained_query_preparations",disjoint_query_stats_.retained_query_preparations},
            {"query_strong_fallbacks",disjoint_query_stats_.query_strong_fallbacks},{"accepted_begins",disjoint_query_stats_.accepted_begins},
            {"accepted_ends",disjoint_query_stats_.accepted_ends},{"last_end_required_work",disjoint_query_stats_.last_end_required_work},
            {"last_end_completed_work",disjoint_query_stats_.last_end_completed_work},{"full_query_completions",disjoint_query_stats_.full_query_completions},
            {"dirty_query_completions",disjoint_query_stats_.dirty_query_completions},
            {"counter_scope","Exact accepted separate-image D32 queries while dirty primary tokens remain unchanged; no same-image query sharing or fragment-cost claim"}};
        if(config_.fsi_hoist_mode!=NativeFsiHoistMode::Disabled)report["fsi_immutable_state_hoist"]={
            {"enabled",true},{"mode",unsigned(config_.fsi_hoist_mode)},{"variant_version",config_.fsi_hoist_version},
            {"selected_draws",fsi_hoist_stats_.selected},{"queried_draws",fsi_hoist_stats_.queried},
            {"immutable_state_load_draws",fsi_hoist_stats_.selected},
            {"incoming_depth_state_draws",fsi_hoist_stats_.incoming_depth_state},
            {"canonical_null_pixel_shader",true},
            {"counters_scope","Accepted non-null derived FSI draws and host depth-or-stencil state; fragment execution and hoisted instruction count are not measured"}};
        if(config_.exact_fsi_stores)report["exact_fsi_stores"]={{"enabled",true},{"variant_version",config_.exact_fsi_stores_version},
            {"selected_draws",exact_fsi_stores_stats_.selected},{"queried_draws",exact_fsi_stores_stats_.queried},
            {"depth_identity_candidate_draws",exact_fsi_stores_stats_.depth_identity_candidates},
            {"depth_identity_certified_draws",exact_fsi_stores_stats_.depth_identity_certified},
            {"depth_identity_uncertified_draws",exact_fsi_stores_stats_.depth_identity_uncertified},
            {"color_overwrite_state_draws",exact_fsi_stores_stats_.color_overwrite_draws},
            {"color_overwrite_state_targets",exact_fsi_stores_stats_.color_overwrite_targets},
            {"canonical_null_pixel_shader",true},{"counters_scope","Accepted non-null exact-FSI draws and host state eligibility; fragment branch execution is not measured"}};
        report["draws"]=counters.draws;report["clears"]=counters.clears;
        report["resolves"]=counters.resolves;report["gpu_texture_reads"]=counters.gpu_texture_reads;
        if(counters.pipelines_created)report["pipelines_created"]=*counters.pipelines_created;
        if(counters.direct_fixed_source_resolves)report["direct_fixed_source_resolves"]=*counters.direct_fixed_source_resolves;
        if(counters.anisotropy) {
            report["anisotropy_override"]=*counters.anisotropy;
            report["max_sampler_anisotropy"]=config_.max_sampler_anisotropy;
            report["texture_filtering"]={{"override",*counters.anisotropy},
                {"maximum_sampler_anisotropy",config_.max_sampler_anisotropy}};
        }
        if(counters.last_accepted_event)report["last_accepted_event"]=*counters.last_accepted_event;
        if(counters.last_completed_event)report["last_completed_event"]=*counters.last_completed_event;
        if(!counters.completed_frames.empty())report["completed_frames"]=counters.completed_frames;
        if(index_snapshot_) {
            const auto& saved=*index_snapshot_;const auto& stats=saved.statistics;
            report["index_cache"]={{"hits",stats.hits},{"misses",stats.misses},
                {"gpu_bypasses",stats.gpu_bypasses},{"changed_sources",stats.changed_sources},
                {"admitted",stats.admitted},{"capacity_misses",stats.capacity_misses},
                {"retired",stats.retired},{"resident",saved.resident},
                {"retained_bytes",saved.retained_bytes},{"index_bytes",saved.index_bytes},
                {"maximum_entries",1024},{"maximum_retained_bytes",32u*1024*1024}};
        }
        if(binding_snapshot_) {
            const auto& saved=*binding_snapshot_;
            report["texture_binding_cache"]={{"hits",saved.hits},{"created",saved.created},
                {"transient_created",saved.transient},{"resident",saved.resident},
                {"maximum_resident",4096},{"maximum_per_image",16}};
        }
        if(batch_snapshot_) {
            const auto& saved=*batch_snapshot_;
            report["draw_batches"]={{"submissions",saved.submissions},{"draws",saved.draws},
                {"largest_batch",saved.largest},{"maximum_draws",config_.max_draw_batch_size}};
        }
        if(config_.native_fsi) {
            report["fsi_raster_attachment"]={{"retention_enabled",config_.retain_fsi_raster_extent},
                {"creations",fsi_attachment_creations_},{"larger_extent_reuses",fsi_attachment_extent_reuses_},
                {"maximum_retained_bytes",32u*1024*1024}};
            report["dummy_fsi_render_pass"]={{"enabled",config_.discard_dummy_fsi_attachment},
                {"installed",discard_dummy_fsi_installed_},{"attempts",discard_dummy_fsi_attempts_},
                {"installs",discard_dummy_fsi_installs_},{"refusals",discard_dummy_fsi_refusals_},
                {"last_result",int32_t(discard_dummy_fsi_last_result_)},{"selected_draws",discard_dummy_fsi_draws_},
                {"queried_draws",discard_dummy_fsi_queried_},{"no_pixel_shader_draws",discard_dummy_fsi_no_pixel_},
                {"discarded_draws",discard_dummy_fsi_discarded_}};
            report["attachmentless_fsi_render_pass"]={{"enabled",config_.attachmentless_fsi},
                {"supported",attachmentless_fsi_supported_},{"active",attachmentless_fsi_active_},
                {"refused",attachmentless_fsi_refused_},{"attempts",attachmentless_fsi_attempts_},
                {"installs",attachmentless_fsi_installs_},{"refusals",attachmentless_fsi_refusals_},
                {"selected_draws",attachmentless_fsi_draws_},{"queried_draws",attachmentless_fsi_queried_},
                {"no_pixel_shader_draws",attachmentless_fsi_no_pixel_},{"discarded_draws",attachmentless_fsi_discarded_},
                {"barrier_draws",attachmentless_fsi_barriers_},{"prewarm_attempts",attachmentless_fsi_prewarm_attempts_},
                {"prewarm_prepared",attachmentless_fsi_prewarm_prepared_},{"prewarm_refusals",attachmentless_fsi_prewarm_refusals_}};
            if(fsi_framebuffer_) {
                report["fsi_raster_attachment"]["width"]=fsi_raster_width_;
                report["fsi_raster_attachment"]["height"]=fsi_raster_height_;
            }
        }
        const auto footprints=resolve_memory_->CpuTextureFootprintStatistics();
        report["cpu_texture_footprint_cache"]={{"coalesced_ranges",config_.coalesced_texture_ranges},{"hits",footprints.hits},{"misses",footprints.misses},
            {"admissions",footprints.admissions},{"evictions",footprints.evictions},
            {"entries",footprints.entries},{"peak_entries",footprints.peak_entries},
            {"maximum_entries",footprints.maximum_entries}};
        const auto& physical_definitions=resolve_memory_->definition_statistics();
        const auto& packed_definitions=edram_memory_->definition_statistics();
        report["cpu_texture_footprint_cache"]["defined_coverage"]={
            {"enabled",config_.reuse_defined_ranges},
            {"physical_calls",physical_definitions.calls},{"physical_ranges",physical_definitions.ranges},{"physical_covered",physical_definitions.covered},
            {"packed_calls",packed_definitions.calls},{"packed_ranges",packed_definitions.ranges},{"packed_covered",packed_definitions.covered}};
        const auto versions=resolve_memory_->CpuTextureVersionStatistics();
        report["cpu_texture_footprint_cache"]["page_versions"]={
            {"union_enabled",versions.page_union_enabled},{"calls",versions.calls},
            {"memo_hits",versions.memo_hits},{"rescans",versions.rescans},
            {"range_tracking_enabled",versions.range_tracking_enabled},{"range_hits",versions.range_hits},
            {"union_rescans",versions.page_union_rescans},
            {"baseline_page_visits",versions.baseline_page_visits},
            {"performed_page_visits",versions.performed_page_visits},
            {"avoided_page_visits",versions.baseline_page_visits-versions.performed_page_visits}};
        if(config_.share_cpu_texture_sampling_variants) {
            const auto& s=cpu_texture_sampling_;
            report["cpu_texture_sampling_storage"]={{"enabled",true},{"lookups",s.lookups},
                {"image_hits",s.image_hits},{"image_creations",s.image_creations},{"variant_image_hits",s.variant_image_hits},
                {"reserved_binding_refusals",s.reserved_binding_refusals},{"zero_bias_cached",s.zero_bias_cached},
                {"nonzero_bias_binding_limit",12},{"maximum_bindings_per_image",16},{"maximum_cached_bindings",4096}};
        }
        if(config_.diagnose_full_cpu_texture_overlays) {
            const auto& diagnostic=full_cpu_overlay_diagnostic_;
            report["full_cpu_texture_overlays"]={{"enabled",true},{"cpu_texture_misses",diagnostic.misses},
                {"empty_footprints",diagnostic.empty_footprints},{"partial_overlays",diagnostic.partial_overlays},
                {"full_overlays",diagnostic.full_overlays},{"storage_layout_supported",diagnostic.storage_layout_supported},
                {"format_decoder_supported",diagnostic.format_decoder_supported},{"load_shared_supported",diagnostic.load_shared_supported},
                {"recorded_layouts",diagnostic.layouts},{"maximum_recorded_layouts",FullCpuOverlayDiagnostic::kMaximumLayouts},
                {"layout_record_refusals",diagnostic.layout_record_refusals},{"record_errors",diagnostic.record_errors},
                {"records",diagnostic.records}};
        }
        if(texture_snapshot_) {
            const auto& saved=*texture_snapshot_;const auto& timing=saved.timing;
            report["texture_updates"]={{"frame",saved.frame},{"cpu_stage_timings_enabled",time_cpu_stages_},{"cpu_uploads",timing.uploads},
                {"cpu_payload_bytes",timing.upload_bytes},{"gpu_overlay_bytes",timing.overlay_bytes},
                {"overlay_copy_regions",timing.overlay_regions},{"overlay_readback_bytes",timing.overlay_readback_bytes},
                {"gpu_alias_loads",timing.alias_loads},{"version_ms",stage_ms(timing.version)},{"payload_ms",stage_ms(timing.payload)},
                {"overlay_ms",stage_ms(timing.overlay)},{"decode_ms",stage_ms(timing.decode)},{"upload_ms",stage_ms(timing.upload)},
                {"gpu_alias_load_ms",stage_ms(timing.alias_load)},{"binding_ms",stage_ms(timing.binding)},{"other_ms",stage_ms(saved.other)}};
        }
        if(config_.pinned_null_constant_uploads) {
            const auto& stats=pinned_null_constant_stats_;
            report["pinned_null_constant_uploads"]={{"enabled",true},{"retired_draws",stats.retired_draws},
                {"candidates",stats.candidates},{"selected",stats.selected},{"refusals",stats.refusals},
                {"float_prefix_bytes",kNativeNullDepthFloatPrefixBytes},{"shared_bytes",sizeof(NativeFsiShaderConstants)},
                {"allocation_payload_bytes",kFloatConstantBytes+sizeof(NativeFsiShaderConstants)},
                {"float_baseline_bytes",stats.retired_draws*kFloatConstantBytes},
                {"float_written_bytes",stats.retired_draws*kFloatConstantBytes-stats.selected*(kFloatConstantBytes-kNativeNullDepthFloatPrefixBytes)},
                {"float_omitted_bytes",stats.selected*(kFloatConstantBytes-kNativeNullDepthFloatPrefixBytes)},
                {"shared_written_bytes",stats.retired_draws*sizeof(NativeFsiShaderConstants)},
                {"full_range_flushes",stats.retired_draws},
                {"scope","Exact module-certified host stores, accounted at original draw fence retirement; full shared ABI/allocation/flush/ownership unchanged. No GPU byte or FPS inference."}};
        }
        size_t constant_delta_mirrors=0;
        for(const auto& slot:std::span(draw_slots_).first(submission_capacity_.lists))
            constant_delta_mirrors+=bool(slot.constants.constant_delta);
        report["constant_delta_uploads"]={{"enabled",config_.delta_constant_uploads},
            {"block_bytes",ConstantDeltaPacker::kBlockBytes},{"mirror_bytes",sizeof(ConstantDeltaPacker)},
            {"resident_mirrors",constant_delta_mirrors},{"metadata_bytes",constant_delta_mirrors*sizeof(ConstantDeltaPacker)},
            {"maximum_metadata_bytes",submission_capacity_.lists*sizeof(ConstantDeltaPacker)},
            {"submitted_draws",constant_delta_stats_.submitted_draws},
            {"submitted_float_baseline_bytes",constant_delta_stats_.submitted_draws*kFloatConstantBytes},
            {"submitted_float_written_bytes",constant_delta_stats_.float_written_bytes},
            {"submitted_float_unchanged_blocks",constant_delta_stats_.unchanged_blocks},
            {"submitted_shared_written_bytes",constant_delta_stats_.shared_written_bytes},
            {"submitted_full_range_flushes",constant_delta_stats_.submitted_draws},
            {"scope","Host stores for draw slots retired by original submission fences; full mapped-range flush/reservation/ABI unchanged. No GPU transfer-byte or performance inference."}};
        if(resource_snapshot_) {
            const auto& saved=*resource_snapshot_;
            report["draw_resource_reuse"]={{"descriptor_sets_created",saved.descriptors},
                {"upload_buffers_created",saved.allocations},{"upload_buffer_reuses",saved.reuses},
                {"index_capacity",saved.index_capacity},{"constant_capacity",saved.constant_capacity},
                {"rectangle_capacity",saved.rectangle_capacity},{"maximum_upload_capacity",16*1024*1024}};
        }
        if(draw_snapshot_) {
            const auto& saved=*draw_snapshot_;const auto& timing=saved.timing;
            report["draw_timings"]={{"frame",saved.frame},{"cpu_stage_timings_enabled",time_cpu_stages_},{"draws",timing.count},
                {"setup_ms",stage_ms(timing.setup)},{"textures_ms",stage_ms(timing.textures)},
                {"submit_wait_ms",timing.submit_wait},{"finish_ms",stage_ms(timing.finish)}};
        }
        auto state_coverage=Json::array();
        for(const auto& [key,value]:fsi_state_coverage_)state_coverage.push_back({
            {"vertex",key.vertex?Json(*key.vertex):Json(nullptr)},{"pixel",key.pixel?Json(*key.pixel):Json(nullptr)},
            {"raster",std::get<std::array<uint32_t,6>>(key.raster)},{"fsi_state",key.fsi_state->words},
            {"candidates",value.candidates},{"queried_candidates",value.queried_candidates},{"selected",value.selected},{"queried_selected",value.queried}});
        report["fsi_state_specialization"]={{"enabled",config_.specialize_fsi_pipeline_state},
            {"composition_enabled",config_.fsi_state_forward_cfg},
            {"identity",config_.fsi_state_forward_cfg?kFsiStateForwardCfgIdentity:std::string_view(kFsiStateSpecializationIdentity)},
            {"variant_version",config_.fsi_state_forward_cfg?kNativeFsiStateForwardCfgShaderVersion:kNativeFsiStateSpecializationShaderVersion},
            {"candidates",fsi_state_stats_.candidates},{"selected",fsi_state_stats_.selected},
            {"queried_selected",fsi_state_stats_.queried_selected},{"residency_refusals",fsi_state_stats_.residency_refusals},
            {"prepared",fsi_state_stats_.prepared},{"prewarm_refusals",fsi_state_stats_.prewarm_refusals},
            {"modules_created",fsi_state_stats_.modules_created},{"module_bytes",fsi_state_stats_.module_bytes},
            {"observation_refusals",fsi_state_stats_.observation_refusals},
            {"resident_handoffs",fsi_state_stats_.resident_handoffs},{"coverage_pointer_hits",fsi_state_stats_.coverage_pointer_hits},
            {"gameplay_compilations",0},{"gameplay_pipeline_creations",0},{"coverage",std::move(state_coverage)}};
        if(config_.fsi_state_material_composition)
            report["fsi_state_material_composition"]={{"enabled",true},{"identity",kFsiStateMaterialCompositionPolicy},
                {"inactive_early_catalog_enabled",config_.fsi_state_inactive_early_catalog},
                {"inactive_catalog_admitted",fsi_state_material_stats_.inactive_catalog_admitted},
                {"inactive_catalog_selected",fsi_state_material_stats_.inactive_catalog_selected},
                {"examined",fsi_state_material_stats_.examined},{"source_refusals",fsi_state_material_stats_.source_refusals},
                {"query_refusals",fsi_state_material_stats_.query_refusals},{"state_refusals",fsi_state_material_stats_.state_refusals},
                {"admitted",fsi_state_material_stats_.admitted}};
        report["pipeline_cache"]={{"resident",pipelines_.size()},{"maximum",config_.max_pipeline_cache_entries},
            {"hits",pipeline_cache_stats_.hits},{"misses",pipeline_cache_stats_.misses},
            {"creations",pipeline_cache_stats_.creations},{"evictions",pipeline_cache_stats_.evictions},
            {"pressure_drains",pipeline_cache_stats_.pressure_drains},{"creation_timing",pipeline_creation_timing_.Report()}};
        report["shader_cache"]={{"resident",shaders_.size()},{"maximum",config_.max_shader_cache_entries},
            {"spirv_bytes",shader_cache_stats_.bytes},{"maximum_spirv_bytes",config_.max_shader_cache_bytes},
            {"hits",shader_cache_stats_.hits},{"misses",shader_cache_stats_.misses},
            {"creations",shader_cache_stats_.creations},{"evictions",shader_cache_stats_.evictions},
            {"pressure_drains",shader_cache_stats_.pressure_drains},{"input_timing",shader_input_timing_.Report()},
            {"module_timing",shader_module_timing_.Report()}};
        const auto& lookup=shaders_.statistics();
        report["shader_lookup_lookaside"]={{"enabled",shaders_.enabled()},{"slots",ShaderLookupCache<CachedShader>::kSlots},
            {"lookups",lookup.lookups},{"hits",lookup.hits},{"tree_lookups",lookup.tree_lookups},{"invalidations",lookup.invalidations}};
        const auto lookaside_report=[](const auto& map) {
            const auto& stats=map.statistics();
            return Json{{"enabled",map.enabled()},{"lookups",stats.lookups},{"hits",stats.hits},
                {"tree_lookups",stats.tree_lookups},{"invalidations",stats.invalidations}};
        };
        report["pipeline_lookup_lookaside"]=lookaside_report(pipelines_);
        report["cpu_image_lookup_lookaside"]=lookaside_report(textures_);
        const auto residency_report=[](const auto& cache,const DormantCache& policy) {
            const auto& stats=policy.statistics();const auto occupied=policy.occupancy(cache);const auto& limits=policy.limits();
            return Json{{"resident",occupied.resident_entries},{"allocation_bytes",stats.resident_bytes},
                {"extra_dormant_entries",occupied.dormant_entries},{"extra_dormant_bytes",occupied.dormant_bytes},
                {"maximum_extra_dormant_entries",limits.entries},{"maximum_extra_dormant_bytes",limits.bytes},
                {"maximum_idle_epochs",limits.idle_epochs},{"hits",stats.hits},{"misses",stats.misses},
                {"creations",stats.creations},{"dormant_hits",stats.dormant_hits},{"evictions",stats.evictions},
                {"budget_evictions",stats.budget_evictions},{"age_evictions",stats.age_evictions},
                {"binding_evictions",stats.binding_evictions},{"wrap_evictions",stats.wrap_evictions},
                {"peak_allocation_bytes",stats.peak_resident_bytes},{"peak_extra_dormant_bytes",stats.peak_dormant_bytes},
                {"peak_extra_dormant_entries",stats.peak_dormant_entries}};
        };
        const auto page_version_report=[](const NativePageVersionTable& table) {
            const auto& s=table.statistics();
            return Json{{"enabled",table.enabled()},{"pages",table.size()},
                {"block_pages",NativePageVersionTable::kBlockPages},
                {"writes",s.writes},{"reads",s.reads},{"logical_written_pages",s.logical_written_pages},
                {"logical_read_pages",s.logical_read_pages},{"uniform_writes",s.uniform_writes},
                {"summary_reads",s.summary_reads},{"materialized_pages",s.materialized_pages},
                {"written_pages",s.written_pages},{"scanned_pages",s.scanned_pages}};
        };
        report["page_versions"]={{"physical",page_version_report(resolve_memory_->PageVersions())},
            {"edram",page_version_report(edram_memory_->PageVersions())}};
        const auto& alias_state=resolve_memory_->gpu_alias_state_statistics();
        report["gpu_alias_state_cache"]={{"enabled",config_.cache_gpu_alias_states},
            {"requests",alias_state.requests},{"hits",alias_state.hits},{"scans",alias_state.scans},
            {"ranges_scanned",alias_state.ranges_scanned},{"pages_scanned",alias_state.pages_scanned},
            {"cache_clears",alias_state.cache_clears},{"serial_wraps",alias_state.serial_wraps},
            {"maximum_entries",2048},{"memo_bytes_per_entry",sizeof(NativeGpuAliasStateMemo)}};
        const auto& layouts=texture_layout_cache_.statistics();
        report["texture_layout_cache"]={{"enabled",config_.cache_texture_layouts},
            {"ways",texture_layout_cache_.ways()},
            {"hits",layouts.hits},{"misses",layouts.misses},{"admissions",layouts.admissions},
            {"evictions",layouts.evictions},{"bypasses",layouts.bypasses},{"entries",layouts.entries},
            {"retained_level_bytes",layouts.level_bytes},{"peak_level_bytes",layouts.peak_level_bytes},
            {"maximum_entries",TextureLayoutCache::kEntries},
            {"maximum_level_bytes",TextureLayoutCache::kMaximumLevelBytes},
            {"maximum_levels_per_entry",TextureLayoutCache::kMaximumLevels},
            {"fixed_storage_bytes",sizeof(TextureLayoutCache)}};
        const auto& resets=queries_->reset_statistics();
        report["query_resets"]={{"configured",config_.host_query_reset},{"host_enabled",queries_->host_reset_enabled()},
            {"host_calls",resets.host_calls},{"host_slots",resets.host_slots},
            {"gpu_calls",resets.gpu_calls},{"gpu_calls_avoided",resets.gpu_calls_avoided}};
        report["query_prefix_retirement"]={{"enabled",config_.retire_completed_query_prefixes},
            {"completed",query_prefix_completions_},{"completed_while_work_pending",query_prefix_while_pending_},
            {"pressure_waits",query_prefix_pressure_}};
        report["texture_cache"]=residency_report(textures_,texture_cache_);
        report["cpu_texture_drains"]={{"enabled",config_.defer_cpu_texture_drains},
            {"candidates",cpu_texture_drains_.candidates},{"pure_cpu",cpu_texture_drains_.pure_cpu},
            {"pending_pure_cpu",cpu_texture_drains_.pending_pure_cpu},{"overlays",cpu_texture_drains_.overlays}};
        const auto& image_slots=cpu_image_slots_.statistics();
        report["cpu_image_slot_lookup"]={{"enabled",config_.cpu_image_slot_lookup},{"slots",CpuImageSlots::kSlots},
            {"lookups",image_slots.lookups},{"hits",image_slots.hits},{"tree_lookups",image_slots.tree_lookups},
            {"slot_bypasses",image_slots.slot_bypasses},{"stores",image_slots.stores},
            {"resets",image_slots.resets},{"invalidated_slots",image_slots.invalidated_slots},
            {"non_draw_resets",image_slots.non_draw_resets},{"non_cpu_resets",image_slots.non_cpu_resets},
            {"erase_resets",image_slots.erase_resets}};
        if(config_.diagnose_texture_eviction_reloads) {
            const auto counters=[](const NativeTextureCensusCounters& v) {
                return Json{{"creations",v.creations},{"recent_reloads",v.recent_reloads},
                    {"not_recent_or_untrackable",v.not_recent_or_untrackable},{"untrackable_creations",v.untrackable_creations},
                    {"evictions",v.evictions},{"untrackable_evictions",v.untrackable_evictions},
                    {"entry_budget_evictions",v.entry_budget_evictions},{"byte_budget_evictions",v.byte_budget_evictions},
                    {"both_budget_evictions",v.both_budget_evictions},{"disabled_budget_evictions",v.disabled_budget_evictions},
                    {"age_evictions",v.age_evictions},{"binding_evictions",v.binding_evictions},{"wrap_evictions",v.wrap_evictions},
                    {"reload_entry_budget",v.reload_entry_budget},{"reload_byte_budget",v.reload_byte_budget},
                    {"reload_both_budget",v.reload_both_budget},{"reload_disabled_budget",v.reload_disabled_budget},
                    {"reload_age",v.reload_age},{"reload_binding",v.reload_binding},{"reload_wrap",v.reload_wrap},
                    {"created_allocation_bytes",v.created_allocation_bytes},{"evicted_allocation_bytes",v.evicted_allocation_bytes},
                    {"creation_ms",v.creation_ms},{"reload_ms",v.reload_ms},{"flush_pending_ms",v.flush_pending_ms},
                    {"eviction_ms",v.eviction_ms},{"payload_ms",v.payload_ms},{"overlay_ms",v.overlay_ms},
                    {"decode_ms",v.decode_ms},{"upload_ms",v.upload_ms},
                    {"maximum_creation_ms",v.maximum_creation_ms},{"maximum_reload_ms",v.maximum_reload_ms},
                    {"maximum_eviction_ms",v.maximum_eviction_ms}};
            };
            const auto& census=*texture_eviction_reload_census_;
            std::array<const NativeTextureEvictionReloadCensus::Frame*,NativeTextureEvictionReloadCensus::kMaximumFrames> ordered{};
            std::size_t count=0;for(const auto& frame:census.frames)if(frame.valid)ordered[count++]=&frame;
            std::sort(ordered.begin(),ordered.begin()+count,[](const auto* a,const auto* b) {
                return std::tie(a->completed_frame_epoch,a->frame)<std::tie(b->completed_frame_epoch,b->frame);
            });
            Json frames=Json::array();for(std::size_t i=0;i<count;++i)frames.push_back({
                {"frame",ordered[i]->frame},{"completed_frame_epoch",ordered[i]->completed_frame_epoch},
                {"counters",counters(ordered[i]->counters)}});
            report["texture_eviction_reload_census"]={{"enabled",true},{"total",counters(census.total)},
                {"frames",std::move(frames)},{"maximum_frame_records",NativeTextureEvictionReloadCensus::kMaximumFrames},
                {"maximum_recent_tombstones",NativeTextureEvictionReloadCensus::kMaximumTombstones},
                {"fixed_census_bytes",sizeof(NativeTextureEvictionReloadCensus)},
                {"completed_frame_epoch",census.completed_frame_epoch()},{"tombstones_overwritten",census.tombstones_overwritten},
                {"metadata_failures",census.metadata_failures},{"overflowed",census.overflowed},
                {"ordinary_cache_creations",texture_cache_.statistics().creations},
                {"ordinary_cache_evictions",texture_cache_.statistics().evictions},
                {"scope","Successful CPU image creations and fenced original cache erasures only. Exact fetch/base/mip/version tombstones are bounded to512 recent eviction events; absent match is not proof of first use. Frames use original operation frame plus diagnostic completed-frame epoch. Timers are inclusive host preparation/wait/destruction; upload includes allocation, binding and recording. Stage times and reload time are subsets of creation time, never additive GPU costs. No policy, lifetime or limits changed."}};
        }
        report["vertex_cache"]=residency_report(vertex_buffers_,vertex_cache_);
        const auto vertex_pool=vertex_buffer_pool_?vertex_buffer_pool_->statistics():RetiredBufferPool<RenderBuffer>::Statistics{};
        report["vertex_buffer_pool"]={{"enabled",config_.recycle_vertex_buffers},
            {"actual_allocations",vertex_buffer_allocations_},{"actual_reuses",vertex_buffer_reuses_},
            {"hits",vertex_pool.hits},{"misses",vertex_pool.misses},{"returns",vertex_pool.returns},
            {"evictions",vertex_pool.evictions},{"rejected",vertex_pool.rejected},
            {"idle_buffers",vertex_pool.idle_buffers},{"idle_allocation_bytes",vertex_pool.idle_bytes},
            {"peak_idle_buffers",vertex_pool.peak_idle_buffers},{"peak_idle_allocation_bytes",vertex_pool.peak_idle_bytes},
            {"maximum_idle_buffers",RetiredBufferPool<RenderBuffer>::kMaxBuffers},
            {"maximum_idle_allocation_bytes",RetiredBufferPool<RenderBuffer>::kMaxBytes}};
        const auto& canonical=canonical_resolve_stats_;
        report["canonical_resolve_stores"]={{"enabled",config_.canonical_resolve_stores},
            {"configured",canonical.configured},{"stores",canonical.stores},
            {"adopted",canonical.adopted},{"discarded",canonical.discarded},
            {"budget_refusals",canonical.budget_refusals},{"receipt_bytes",canonical.receipt_bytes},
            {"peak_receipt_bytes",canonical.peak_receipt_bytes},
            {"maximum_receipt_bytes",kCanonicalResolveReceiptBytes}};
        const auto& volume=resolve_memory_->queued_volume_load_statistics();
        report["queued_volume_alias_loads"]={{"enabled",volume.enabled},{"requests",volume.requests},
            {"selected",volume.selected},{"refusals",volume.refusals},
            {"synchronous_preflight",volume.synchronous_preflight},
            {"decoded_slices",volume.decoded_slices},{"assemblies",volume.assemblies},
            {"maximum_layers",ResolveMemory::kMaxQueuedVolumeLayers},
            {"maximum_combined_image_bytes",ResolveMemory::kMaxQueuedVolumeImageBytes}};
        const auto& compact=resolve_memory_->compact_load_statistics();
        report["compact_resolve_channels"]={{"enabled",config_.compact_resolve_channels},
            {"r32_supported",compact.r32_supported},{"rg32_supported",compact.rg32_supported},
            {"r32_loads",compact.r32_loads},{"rg32_loads",compact.rg32_loads},{"rgba32_loads",compact.rgba32_loads},
            {"logical_output_bytes",compact.logical_output_bytes},{"logical_bytes_avoided",compact.logical_bytes_avoided}};
        const auto image_pool=resolve_memory_->ImagePoolStatistics();
        report["resolve_image_pool"]={{"enabled",config_.recycle_resolve_images},
            {"hits",image_pool.hits},{"misses",image_pool.misses},{"returns",image_pool.returns},
            {"admitted",image_pool.admitted},{"evictions",image_pool.evictions},{"rejected",image_pool.rejected},
            {"idle_images",image_pool.idle_images},{"idle_allocation_bytes",image_pool.idle_bytes},
            {"peak_idle_images",image_pool.peak_idle_images},{"peak_idle_allocation_bytes",image_pool.peak_idle_bytes},
            {"maximum_idle_images",ResolveMemory::ImagePool::kMaxImages},
            {"maximum_idle_allocation_bytes",ResolveMemory::ImagePool::kMaxBytes}};
        const auto descriptor_stats=[](const ResolveMemory::DescriptorPool::Statistics& value) {
            return Json{{"enabled",value.enabled},{"requests",value.requests},{"hits",value.hits},
                {"misses",value.misses},{"allocations",value.allocations},{"factory_failures",value.factory_failures},
                {"capacity_fallbacks",value.capacity_fallbacks},{"returns",value.returns},
                {"uncached_destructions",value.uncached_destructions},{"live_sets",value.live_sets},
                {"leased_sets",value.leased_sets},{"idle_sets",value.idle_sets},
                {"peak_live_sets",value.peak_live_sets},{"maximum_sets",value.maximum_sets},
                {"synchronous_bypasses",value.synchronous_bypasses}};
        };
        report["resolve_descriptor_pool"]={{"enabled",config_.recycle_resolve_descriptors},
            {"physical",descriptor_stats(resolve_memory_->DescriptorPoolStatistics())},
            {"edram",descriptor_stats(edram_memory_->DescriptorPoolStatistics())}};
        report["pass_transfer_descriptor_pool"]=descriptor_stats(transfers_->DescriptorPoolStatistics());
        const auto pass_cache=transfers_->RenderPassCacheStatistics();
        report["pass_transfer_render_pass_cache"]={{"enabled",pass_cache.enabled},{"requests",pass_cache.requests},
            {"hits",pass_cache.hits},{"misses",pass_cache.misses},{"creations",pass_cache.creations},
            {"unsupported_bypasses",pass_cache.unsupported_bypasses},{"capacity_bypasses",pass_cache.capacity_bypasses},
            {"create_failures",pass_cache.create_failures},{"cached_entries",pass_cache.cached_entries},
            {"maximum_entries",pass_cache.maximum_entries}};
        const auto identity_view_stats=[](const ResolveMemory::IdentityViewReuseStatistics& value) {
            return Json{{"enabled",value.enabled},{"load_requests",value.load_requests},
                {"load_reused",value.load_reused},{"load_explicit_views",value.load_explicit_views},
                {"load_refusals",value.load_refusals},{"store_bypasses",value.store_bypasses}};
        };
        report["resolve_identity_views"]={{"enabled",config_.reuse_resolve_identity_views},
            {"physical",identity_view_stats(resolve_memory_->identity_view_reuse_statistics())},
            {"edram",identity_view_stats(edram_memory_->identity_view_reuse_statistics())}};
        const auto& aliases=*alias_retention_;
        report["alias_generations"]={{"created",aliases.created},{"reused",aliases.reused},
            {"retired",aliases.retired},{"stale_bytes",aliases.bytes},{"stale_generations",aliases.generations},
            {"stale_bindings",aliases.bindings},{"peak_stale_bytes",aliases.peak_bytes},
            {"peak_stale_generations",aliases.peak_generations},{"peak_stale_bindings",aliases.peak_bindings},
            {"pressure_drains",aliases.pressure_drains},{"maximum_stale_bytes",config_.max_stale_alias_bytes},
            {"maximum_stale_generations",AliasRetention::kMaxGenerations},
            {"maximum_stale_bindings",AliasRetention::kMaxBindings}};
        if(transfer_batch_) {
            const auto& stats=transfer_batch_->statistics();
            report["transfer_batches"]={{"stages",stats.stages},{"submissions",stats.submissions},
                {"waits",stats.waits},{"maximum_stages",stats.maximum_stages},
                {"synchronous_fallbacks",stats.synchronous_fallbacks},{"queued_lists",stats.queued_lists}};
            if(gpu_work_queue_) {
                const auto& work=gpu_work_queue_->statistics();
                report["gpu_work_queue"]={{"capacity",gpu_work_queue_->capacity()},{"async_batch_lists",gpu_work_queue_->async_batch_lists()},
                    {"submissions",work.submissions},{"waits",work.waits},
                    {"fence_polls",work.fence_polls},{"poll_retirements",work.poll_retirements},
                    {"accepted_work",gpu_work_queue_->accepted_work()},{"completed_work",gpu_work_queue_->completed_work()},
                    {"draw_lists",work.draw_lists},{"transfer_lists",work.transfer_lists},
                    {"maximum_lists",work.maximum_lists},{"maximum_scratch_bytes",work.maximum_scratch_bytes},
                    {"maximum_inflight_batches",work.maximum_inflight_batches},{"inflight_batch_limit",gpu_work_queue_->inflight_batch_limit()}};
                report["draw_command_groups"]={{"enabled",DrawGroupsEnabled()},
                    {"logical_grouped_draws",work.grouped_draws},{"closed_groups",work.draw_command_groups},
                    {"maximum_group_draws",work.maximum_group_draws},{"physical_lists",work.physical_lists},
                    {"maximum_physical_lists_per_submission",work.maximum_physical_lists_per_submission},
                    {"maximum_group_limit",std::min(config_.max_draw_batch_size,NativeDrawCommandGroups::kMaximumDraws)},
                    {"standalone_preparation_draws",group_preparation_standalone_},
                    {"readonly_barriers_deferred",group_readonly_deferred_},{"readonly_state_changed",group_readonly_changed_},
                    {"recording_failed",draw_group_recording_failed_}};
                report["retained_opaque_phase_render_pass_groups"]={{"enabled",config_.retained_opaque_phase_render_pass_groups},
                    {"eligible",retained_phase_pass_stats_.eligible},{"selected",retained_phase_pass_stats_.selected},
                    {"render_passes_avoided",retained_phase_pass_stats_.continued},
                    {"color_barriers_avoided",retained_phase_pass_stats_.continued},
                    {"depth_barriers_avoided",retained_phase_pass_stats_.continued},
                    {"closed_render_pass_groups",retained_phase_pass_stats_.groups},
                    {"preparation_refusals",retained_phase_pass_stats_.preparation_refusals},
                    {"timestamp_refusals",retained_phase_pass_stats_.timestamp_refusals},
                    {"generation_boundaries",retained_phase_pass_stats_.generation_boundaries},
                    {"attachment_state_boundaries",retained_phase_pass_stats_.attachment_state_boundaries},
                    {"sampled_barriers_avoided",retained_phase_pass_stats_.sampled_barriers_avoided},
                    {"maximum_group_draws",retained_phase_pass_stats_.maximum_group_draws}};
                report["readonly_query_render_pass_groups"]={{"enabled",config_.readonly_query_render_pass_groups},
                    {"eligible",readonly_query_pass_stats_.eligible},{"selected",readonly_query_pass_stats_.selected},
                    {"render_passes_avoided",readonly_query_pass_stats_.continued},
                    {"closed_render_pass_groups",readonly_query_pass_stats_.groups},
                    {"preparation_refusals",readonly_query_pass_stats_.preparation_refusals},
                    {"query_controls_preserved",readonly_query_pass_stats_.query_controls_preserved},
                    {"maximum_group_draws",readonly_query_pass_stats_.maximum_group_draws}};
                report["packed_color_render_pass_groups"]={{"enabled",config_.packed_color_render_pass_groups},
                    {"candidates",packed_color_pass_stats_.candidates},{"eligible",packed_color_pass_stats_.eligible},
                    {"selected",packed_color_pass_stats_.selected},{"refusals",packed_color_pass_stats_.refusals},
                    {"query_refusals",packed_color_pass_stats_.query_refusals},
                    {"timestamp_refusals",packed_color_pass_stats_.timestamp_refusals},
                    {"preparation_refusals",packed_color_pass_stats_.preparation_refusals},
                    {"closed_render_pass_groups",packed_color_pass_stats_.groups},
                    {"render_passes_avoided",packed_color_pass_stats_.continued},
                    {"edram_barriers_avoided",packed_color_pass_stats_.continued},
                    {"dummy_barriers_avoided",packed_color_pass_stats_.continued},
                    {"sampled_barriers_avoided",packed_color_pass_stats_.sampled_barriers_avoided},
                    {"maximum_group_draws",packed_color_pass_stats_.maximum_group_draws},
                    {"variant8_candidates",packed_color_pass_stats_.variant8_candidates},
                    {"variant8_eligible",packed_color_pass_stats_.variant8_eligible},
                    {"variant8_selected",packed_color_pass_stats_.variant8_selected},
                    {"variant8_render_passes_avoided",packed_color_pass_stats_.variant8_continued},
                    {"variant8_module_refusals",packed_color_pass_stats_.variant8_module_refusals},
                    {"variant8_pipeline_refusals",packed_color_pass_stats_.variant8_pipeline_refusals}};
                report["packed_color_blend_pass_groups"]={{"enabled",config_.packed_color_blend_pass_groups},
                    {"candidates",packed_color_blend_pass_stats_.candidates},
                    {"eligible",packed_color_blend_pass_stats_.eligible},
                    {"selected",packed_color_blend_pass_stats_.selected},
                    {"refusals",packed_color_blend_pass_stats_.refusals},
                    {"module_refusals",packed_color_blend_pass_stats_.module_refusals},
                    {"pipeline_refusals",packed_color_blend_pass_stats_.pipeline_refusals},
                    {"query_refusals",packed_color_blend_pass_stats_.query_refusals},
                    {"timestamp_refusals",packed_color_blend_pass_stats_.timestamp_refusals},
                    {"preparation_refusals",packed_color_blend_pass_stats_.preparation_refusals},
                    {"alpha_selected",packed_color_blend_pass_stats_.alpha_selected},
                    {"additive_selected",packed_color_blend_pass_stats_.additive_selected},
                    {"old_to_blend_continued",packed_color_blend_pass_stats_.old_to_blend_continued},
                    {"blend_to_old_continued",packed_color_blend_pass_stats_.blend_to_old_continued},
                    {"render_passes_avoided",packed_color_blend_pass_stats_.continued}};
                report["packed_color_action_pass_groups"]={{"enabled",config_.packed_color_action_pass_groups},
                    {"candidates",packed_color_action_pass_stats_.candidates},{"eligible",packed_color_action_pass_stats_.eligible},
                    {"selected",packed_color_action_pass_stats_.selected},{"refusals",packed_color_action_pass_stats_.refusals},
                    {"module_refusals",packed_color_action_pass_stats_.module_refusals},
                    {"pipeline_refusals",packed_color_action_pass_stats_.pipeline_refusals},
                    {"query_refusals",packed_color_action_pass_stats_.query_refusals},
                    {"timestamp_refusals",packed_color_action_pass_stats_.timestamp_refusals},
                    {"preparation_refusals",packed_color_action_pass_stats_.preparation_refusals},
                    {"render_passes_avoided",packed_color_action_pass_stats_.continued}};
                report["async_shaders"]={{"enabled",bool(config_.shader_ready)},{"skipped_draws",async_shader_skipped_draws_}};
                report["async_pipelines"]={{"enabled",config_.async_pipelines},{"started",async_pipelines_.started},
                    {"skipped_draws",async_pipelines_.skipped_draws},{"adopted",async_pipelines_.adopted},
                    {"budget_waits",async_pipelines_.budget_waits},{"budget_builds",async_pipelines_.budget_builds},
                    {"failures",async_pipelines_.failures},
                    {"sync_fsi_state",async_pipelines_.sync_fsi_state},{"sync_variant",async_pipelines_.sync_variant},
                    {"sync_recorded",async_pipelines_.sync_recorded},{"sync_resident_fsi",async_pipelines_.sync_resident_fsi},
                    {"sync_other",async_pipelines_.sync_other},{"skipped_recorded",async_pipelines_.skipped_recorded}};
                report["fsi_draw_pass_groups"]={{"enabled",config_.fsi_draw_pass_groups},
                    {"candidates",fsi_draw_pass_stats_.candidates},{"shape_refusals",fsi_draw_pass_stats_.shape_refusals},
                    {"plane_refusals",fsi_draw_pass_stats_.plane_refusals},{"eligible",fsi_draw_pass_stats_.eligible},
                    {"selected",fsi_draw_pass_stats_.selected},{"boundaries",fsi_draw_pass_stats_.boundaries},
                    {"closed_render_pass_groups",fsi_draw_pass_stats_.groups},
                    {"render_passes_avoided",fsi_draw_pass_stats_.continued},
                    {"maximum_group_draws",fsi_draw_pass_stats_.maximum_group_draws},
                    {"sampled_barriers_avoided",fsi_draw_pass_stats_.sampled_barriers_avoided},
                    {"queries_enabled",config_.fsi_draw_pass_queries},{"queried_selected",fsi_draw_pass_stats_.queried_selected},
                    {"query_controls_preserved",fsi_draw_pass_stats_.query_controls_preserved}};
                report["packed_depth_render_pass_groups"]={{"enabled",config_.packed_depth_render_pass_groups},
                    {"candidates",packed_depth_pass_stats_.candidates},{"eligible",packed_depth_pass_stats_.eligible},
                    {"selected",packed_depth_pass_stats_.selected},{"refusals",packed_depth_pass_stats_.refusals},
                    {"query_refusals",packed_depth_pass_stats_.query_refusals},
                    {"timestamp_refusals",packed_depth_pass_stats_.timestamp_refusals},
                    {"preparation_refusals",packed_depth_pass_stats_.preparation_refusals},
                    {"closed_render_pass_groups",packed_depth_pass_stats_.groups},
                    {"render_passes_avoided",packed_depth_pass_stats_.continued},
                    {"edram_barriers_avoided",packed_depth_pass_stats_.continued},
                    {"dummy_barriers_avoided",packed_depth_pass_stats_.continued},
                    {"maximum_group_draws",packed_depth_pass_stats_.maximum_group_draws},
                    {"atomic_selected",packed_depth_pass_stats_.atomic_selected},
                    {"atomic_render_passes_avoided",packed_depth_pass_stats_.atomic_continued},
                    {"access_model_boundaries",packed_depth_pass_stats_.access_model_boundaries}};
                report["gpu_work_queue"]["submission_worker"]={
                    {"enabled",work.submission_worker_enabled},{"driver_receipts",work.driver_receipts},
                    {"receipt_wait_ms",work.driver_receipt_wait_ms},
                    {"scope","Driver receipts precede fence polling/reset; original FIFO and fence capacity unchanged"}};
                if(work.reason_counters_enabled) {
                    static constexpr std::array<const char*,size_t(NativeGpuWorkQueue::SubmissionReason::Count)> submit_names{
                        "external","draw_batch_target","draw_ownership_pressure","staging_capacity",
                        "reservation_pressure","frontier_coverage","explicit_drain","coherence_progress"};
                    static constexpr std::array<const char*,size_t(NativeGpuWorkQueue::WaitReason::Count)> wait_names{
                        "external","inflight_fence_pressure","reservation_pressure","draw_ownership_pressure",
                        "frontier_coverage","explicit_drain"};
                    auto submit_reasons=Json::object(),wait_reasons=Json::object(),wait_reason_ms=Json::object();
                    for(size_t i=0;i<submit_names.size();++i) {
                        const auto& count=work.submission_reasons[i];
                        submit_reasons[submit_names[i]]={{"submissions",count.submissions},{"lists",count.lists},
                            {"draws",count.draws},{"transfers",count.transfers}};
                    }
                    for(size_t i=0;i<wait_names.size();++i) {
                        wait_reasons[wait_names[i]]=work.wait_reasons[i];
                        wait_reason_ms[wait_names[i]]=work.wait_reason_ms[i];
                    }
                    report["gpu_work_queue"]["reason_counters"]={
                        {"enabled",true},{"reserve_calls",work.reserve_calls},
                        {"reserve_list_pressure",work.reserve_list_pressure},{"reserve_scratch_pressure",work.reserve_scratch_pressure},
                        {"inflight_fence_pressure",work.inflight_fence_pressure},{"draw_slot_pressure",work.draw_slot_pressure},
                        {"draw_upload_pressure",work.draw_upload_pressure},{"explicit_drain_calls",work.explicit_drain_calls},
                        {"frontier_wait_calls",work.frontier_wait_calls},{"submissions",std::move(submit_reasons)},
                        {"waits",std::move(wait_reasons)},{"wait_ms",std::move(wait_reason_ms)}};
                } else report["gpu_work_queue"]["reason_counters"]={{"enabled",false}};
                if(work.timings_enabled)report["gpu_timings"]={
                    {"supported",work.timestamps_supported},{"status",work.timestamp_status},
                    {"valid_bits",work.timestamp_valid_bits},{"period_ns",work.timestamp_period_ns},
                    {"intervals",work.gpu_intervals},{"unavailable",work.gpu_unavailable},
                    {"inclusive_interval_sum_ms",work.gpu_interval_sum_ms},
                    {"submit_ms",work.submit_ms},{"fence_wait_ms",work.fence_wait_ms},
                    {"driver_receipt_wait_ms",work.driver_receipt_wait_ms},
                    {"resource_retirement_ms",work.resource_retirement_ms},
                    {"completion_callback_ms",work.completion_callback_ms},
                    {"timestamp_record_ms",work.timestamp_record_ms},{"timestamp_read_ms",work.timestamp_read_ms}};
            }
        }
        if(config_.gpu_timestamps)report["presentation_gpu_timings"]=presentation_gpu_timings_?
            presentation_gpu_timings_->Report():Json{{"requested",true},{"enabled",false},{"status","unavailable diagnostic allocation"}};
        if(config_.gpu_batch_attribution)report["gpu_batch_attribution"]=GpuBatchAttributionReport(false);
        if(config_.gpu_physical_timestamps)report["gpu_physical_timings"]=gpu_work_queue_->PhysicalTimingReport(false);
        if(config_.builtin_readonly_depth_queries)report["builtin_readonly_queries"]={
            {"identity",kBuiltinReadonlyQueryIdentity},{"candidates",readonly_query_stats_.candidates},
            {"selected",readonly_query_stats_.selected},{"queried_selected",readonly_query_stats_.queried_selected},
            {"prepared",readonly_query_stats_.prepared},{"prewarm_attempts",readonly_query_stats_.prewarm_attempts},
            {"prewarm_refusals",readonly_query_stats_.prewarm_refusals},{"vertex_refusals",readonly_query_stats_.vertex_refusals},
            {"module_refusals",readonly_query_stats_.module_refusals},{"residency_refusals",readonly_query_stats_.residency_refusals},
            {"headroom_modules",readonly_query_headroom_.modules()},{"headroom_bytes",readonly_query_headroom_.bytes()}};
        if(config_.builtin_atomic_depth)report["builtin_atomic_depth"]={
            {"identity",kBuiltinAtomicDepthIdentity},{"candidates",atomic_depth_stats_.candidates},
            {"selected",atomic_depth_stats_.selected},{"vertex_refusals",atomic_depth_stats_.vertex_refusals},
            {"module_refusals",atomic_depth_stats_.module_refusals}};
        if(config_.opaque_color_overwrite) {
            report["opaque_color_overwrite"]={{"identity",kOpaqueColorOverwriteIdentity},
                {"program_ready",opaque_program_ready_},{"prepared",opaque_overwrite_stats_.prepared},
                {"candidates",opaque_overwrite_stats_.candidates},{"selected",opaque_overwrite_stats_.selected},
                {"residency_refusals",opaque_overwrite_stats_.residency_refusals},
                {"prewarm_refusals",opaque_overwrite_stats_.prewarm_refusals}};
            if(config_.opaque_color_overwrite_program) {
                const auto& program=*config_.opaque_color_overwrite_program;
                report["opaque_color_overwrite"]["variant_version"]=program.variant_version;
                report["opaque_color_overwrite"]["compiled_spirv_sha256"]=program.compiled_spirv_sha256;
            }
        }
        if(config_.forward_control_flow) {
            report["forward_control_flow"]={{"identity",kForwardControlFlowIdentity},
                {"program_ready",forward_program_ready_},{"prepared",forward_control_flow_stats_.prepared},
                {"candidates",forward_control_flow_stats_.candidates},{"selected",forward_control_flow_stats_.selected},
                {"queried_selected",forward_control_flow_stats_.queried_selected},
                {"residency_refusals",forward_control_flow_stats_.residency_refusals},
                {"prewarm_refusals",forward_control_flow_stats_.prewarm_refusals}};
            if(config_.forward_control_flow_program) {
                const auto& program=*config_.forward_control_flow_program;
                report["forward_control_flow"]["control_instructions"]=program.control_instructions;
                report["forward_control_flow"]["baseline_hlsl_sha256"]=program.baseline_hlsl_sha256;
                report["forward_control_flow"]["modified_hlsl_sha256"]=program.modified_hlsl_sha256;
                report["forward_control_flow"]["variant_version"]=program.variant_version;
                report["forward_control_flow"]["compiled_spirv_sha256"]=program.compiled_spirv_sha256;
            }
        }
        if(config_.forward_vertex_control_flow) {
            report["forward_vertex_control_flow"]={{"identity",kForwardVertexIdentity},
                {"program_ready",forward_vertex_program_ready_},{"prepared",forward_vertex_stats_.prepared},
                {"candidates",forward_vertex_stats_.candidates},{"selected",forward_vertex_stats_.selected},
                {"queried_selected",forward_vertex_stats_.queried_selected},
                {"residency_refusals",forward_vertex_stats_.residency_refusals},
                {"prewarm_refusals",forward_vertex_stats_.prewarm_refusals}};
            if(config_.forward_vertex_program) {
                const auto& program=*config_.forward_vertex_program;
                report["forward_vertex_control_flow"]["control_instructions"]=program.control_instructions;
                report["forward_vertex_control_flow"]["terminal"]=program.terminal;
                report["forward_vertex_control_flow"]["padding_clauses"]=program.padding_clauses;
                report["forward_vertex_control_flow"]["pipeline_limit"]=kForwardVertexMaxPipelines;
                report["forward_vertex_control_flow"]["baseline_hlsl_sha256"]=program.baseline_hlsl_sha256;
                report["forward_vertex_control_flow"]["modified_hlsl_sha256"]=program.modified_hlsl_sha256;
                report["forward_vertex_control_flow"]["variant_version"]=program.variant_version;
                report["forward_vertex_control_flow"]["compiled_spirv_sha256"]=program.compiled_spirv_sha256;
            }
        }
        if(config_.forward_pixel_catalog) {
            report["forward_pixel_catalog"]={{"identity",kForwardPixelCatalogIdentity},{"program_ready",forward_pixel_catalog_ready_},
                {"program_limit",kForwardPixelCatalogMaxPrograms},{"pipeline_limit",kForwardPixelCatalogMaxPipelines},
                {"prepared",forward_pixel_catalog_stats_.prepared},{"candidates",forward_pixel_catalog_stats_.candidates},
                {"selected",forward_pixel_catalog_stats_.selected},{"queried_selected",forward_pixel_catalog_stats_.queried_selected},
                {"residency_refusals",forward_pixel_catalog_stats_.residency_refusals},{"prewarm_refusals",forward_pixel_catalog_stats_.prewarm_refusals},
                {"sources",Json::object()}};
            report["forward_pixel_catalog"]["programs"]=config_.forward_pixel_catalog_programs?config_.forward_pixel_catalog_programs->programs.size():0;
            const auto soft=forward_pixel_catalog_headroom_.SoftTargets(forward_pixel_catalog_ready_,config_.max_shader_cache_entries,config_.max_shader_cache_bytes);
            report["forward_pixel_catalog"]["cache_headroom"]={{"admitted_modules",forward_pixel_catalog_headroom_.modules()},
                {"admitted_spirv_bytes",forward_pixel_catalog_headroom_.bytes()},{"soft_entry_target",soft.entries},{"soft_byte_target",soft.bytes}};
            for(const auto& [source,stats]:forward_pixel_catalog_source_stats_) {
                const auto& program=*config_.forward_pixel_catalog_programs->programs.at(source);
                report["forward_pixel_catalog"]["sources"][source]={{"prepared",stats.prepared},{"candidates",stats.candidates},
                    {"selected",stats.selected},{"queried_selected",stats.queried_selected},{"residency_refusals",stats.residency_refusals},
                    {"prewarm_refusals",stats.prewarm_refusals},{"control_instructions",program.control_instructions},
                    {"output_mask",program.output_mask},{"variant_version",program.variant_version},
                    {"compiled_spirv_sha256",program.compiled_spirv_sha256},{"baseline_hlsl_sha256",program.baseline_hlsl_sha256},
                    {"modified_hlsl_sha256",program.modified_hlsl_sha256}};
            }
        }
        if(config_.gpu_draw_timestamps)report["gpu_draw_timings"]=GpuDrawTimingReport(false);
        if(config_.restore_depth_stencil_export||config_.detailed_reports) {
            const auto& restores=transfers_->restore_statistics();
            report["depth_stencil_restores"]={{"requested",config_.restore_depth_stencil_export},{"extension_enabled",config_.shader_stencil_export_enabled},
                {"calls",restores.calls},{"legacy_calls",restores.legacy_calls},{"export_calls",restores.export_calls},
                {"draws",restores.draws},{"stencil_clears",restores.stencil_clears},{"export_failures",restores.export_failures}};
        }
        if(config_.coalesce_draw_barriers||config_.skip_readonly_sample_barriers||config_.precise_fsi_barriers)
            report["draw_barriers"]={{"sampled_emitted",draw_barriers_.sampled_emitted},
                {"sampled_skipped",draw_barriers_.sampled_skipped},{"precise_fsi",draw_barriers_.precise_fsi},
                {"precise_fallback",draw_barriers_.precise_fallback},{"merged",draw_barriers_.merged},
                {"separate",draw_barriers_.separate}};
        if(config_.skip_unused_descriptor_sets)
            report["unused_descriptor_sets"]={{"enabled",true},{"draws",unused_descriptors_.draws},
                {"selected_draws",unused_descriptors_.selected_draws},{"unknown_draws",unused_descriptors_.unknown_draws},
                {"omitted_set_bindings",unused_descriptors_.omitted_set_bindings},
                {"required_set_bindings",unused_descriptors_.required_set_bindings}};
        if(config_.batch_draw_descriptors)
            report["draw_descriptors"]={{"draws",draw_descriptors_.draws},
                {"writes",draw_descriptors_.writes},{"update_calls",draw_descriptors_.update_calls},
                {"bind_calls",draw_descriptors_.bind_calls}};
        if(config_.cache_draw_descriptors)
            report["draw_descriptor_content_cache"]={{"requested",true},{"enabled",config_.batch_draw_descriptors},
                {"requires_descriptor_batching",!config_.batch_draw_descriptors},
                {"draws",descriptor_contents_.draws},{"compared",descriptor_contents_.compared},
                {"skipped_images",descriptor_contents_.skipped_images},{"skipped_samplers",descriptor_contents_.skipped_samplers},
                {"skipped_images_by_set",descriptor_contents_.skipped_images_by_set},
                {"image_write_requests",descriptor_contents_.image_write_requests},{"sampler_write_requests",descriptor_contents_.sampler_write_requests},
                {"committed_writes",descriptor_contents_.committed_writes},{"update_calls",descriptor_contents_.update_calls},
                {"caches",descriptor_contents_.caches},{"metadata_bytes",descriptor_contents_.caches*sizeof(NativeDescriptorContentCache)},
                {"maximum_metadata_bytes",submission_capacity_.lists*sizeof(NativeDescriptorContentCache)},
                {"allocation_refusals",descriptor_contents_.allocation_refusals},{"set_identity_resets",descriptor_contents_.set_identity_resets},
                {"aborts",descriptor_contents_.aborts},{"generation_refusals",descriptor_contents_.generation_refusals},
                {"invalid_handle_refusals",descriptor_contents_.invalid_handle_refusals}};
        if(config_.time_draw_recording||config_.reset_draw_command_pool)
            report["draw_recording"]={{"timings_enabled",config_.time_draw_recording},
                {"begins",draw_recording_.begins},{"pool_resets",draw_recording_.pool_resets},
                {"ends",draw_recording_.ends},{"begin_ms",draw_recording_.begin_ms},
                {"pool_reset_ms",draw_recording_.pool_reset_ms},{"end_ms",draw_recording_.end_ms},
                {"enqueue_submit_ms",draw_recording_.enqueue_submit_ms},
                {"enqueue_wait_ms",draw_recording_.enqueue_wait_ms},
                {"enqueue_host_ms",std::max(0.0,draw_recording_.enqueue_submit_ms-draw_recording_.enqueue_wait_ms)}};
        // Refresh only on observation. A failed JSON allocation leaves dirty
        // set, so the next failure-report attempt retries current typed values.
        const auto& cpu_writes=resolve_memory_->cpu_write_statistics();
        const auto& raw_float=resolve_memory_->raw_float_cpu_statistics();
        report["raw_gpu_float_cpu_textures"]={{"enabled",config_.raw_gpu_float_cpu_textures},
            {"configured",raw_float.configured},{"supported",raw_float.supported},
            {"copies",raw_float.copies},{"queued",raw_float.queued},{"synchronous",raw_float.synchronous},
            {"logical_bytes",raw_float.logical_bytes},{"scratch_allocation_bytes",raw_float.scratch_allocation_bytes}};
        report["cpu_memory_writes"]={{"queue_enabled",config_.queue_cpu_memory_writes},
            {"queued_uploads",cpu_writes.queued_uploads},{"queued_invalidations",cpu_writes.queued_invalidations},
            {"synchronous_uploads",cpu_writes.synchronous_uploads},
            {"synchronous_invalidations",cpu_writes.synchronous_invalidations},
            {"uploaded_bytes",cpu_writes.uploaded_bytes}};
        if(config_.forward_cpu_upload_receipts) {
            const auto& receipts=resolve_memory_->cpu_upload_receipt_statistics();
            report["cpu_upload_receipts"]={{"requested",receipts.requested},{"enabled",receipts.enabled},{"checked_queue",receipts.checked_queue},
                {"unchecked_queue_refusals",receipts.unchecked_queue_refusals},
                {"maximum_pages",NativeCpuUploadReceipts::kSlots},
                {"maximum_payload_bytes",NativeCpuUploadReceipts::kMaximumPayloadBytes},
                {"storage_bytes",receipts.storage_bytes},{"allocation_refusals",receipts.allocation_refusals},
                {"write_attempts",receipts.write_attempts},{"admitted_pages",receipts.admitted_pages},
                {"admitted_bytes",receipts.admitted_bytes},{"queued_refusals",receipts.queued_refusals},
                {"unsupported_refusals",receipts.unsupported_refusals},{"version_errors",receipts.version_errors},
                {"slot_replacements",receipts.slot_replacements},{"retained_pages",receipts.retained_pages},
                {"serial_wrap_clears",receipts.serial_wrap_clears},{"attempts",receipts.attempts},
                {"hits",receipts.hits},{"refusals",receipts.refusals},{"pages_checked",receipts.pages_checked},
                {"pages",receipts.pages},{"bytes",receipts.bytes},{"missing_pages",receipts.missing_pages},
                {"stale_pages",receipts.stale_pages},{"invalid_fragments",receipts.invalid_fragments},
                {"scope","Whole overlays from exact completed CPU-upload pages; original ownership/uploads/flushes unchanged"}};
        }
        if(config_.reuse_cpu_write_upload) {
            const auto& reuse=resolve_memory_->cpu_write_upload_reuse_statistics();
            report["cpu_write_upload_reuse"]={{"enabled",reuse.enabled},{"checked_queue",reuse.checked_queue},
                {"poisoned",reuse.poisoned},{"capacity_bytes",4096},{"allocations",reuse.allocations},
                {"reuses",reuse.reuses},{"uploads",reuse.uploads},{"uploaded_bytes",reuse.uploaded_bytes},
                {"submissions",reuse.submissions},{"waits",reuse.waits},{"queued_bypasses",reuse.queued_bypasses},
                {"unsupported_bypasses",reuse.unsupported_bypasses},{"unchecked_queue_bypasses",reuse.unchecked_queue_bypasses}};
        }
        const auto fp24=retained_fp24_depth_writer_?retained_fp24_depth_writer_->statistics():NativeRetainedFp24DepthWriter::Statistics{};
        Json fp24_sources=Json::array();for(size_t i=0;i<kNativeRetainedFp24DepthSources.size();++i)
            fp24_sources.push_back({{"vertex",kNativeRetainedFp24DepthSources[i].source},{"selected",retained_fp24_depth_source_selected_[i]}});
        report["retained_fp24_depth_writer"]={{"enabled",config_.retained_fp24_depth_writer},{"identity",kNativeRetainedFp24DepthIdentity},
            {"supported",retained_fp24_depth_writer_&&retained_fp24_depth_writer_->supported()},
            {"prepared",retained_fp24_depth_stats_.prepared},{"maximum_pipelines",4},{"maximum_sources",2},
            {"modules_created",retained_fp24_depth_headroom_.modules()},{"module_bytes",retained_fp24_depth_headroom_.bytes()},
            {"candidates",retained_fp24_depth_stats_.candidates},{"selected",retained_fp24_depth_stats_.selected},
            {"queried_selected",retained_fp24_depth_stats_.queried_selected},{"reuses",retained_fp24_depth_stats_.reuses},
            {"restores",retained_fp24_depth_stats_.restores},{"materializations",retained_fp24_depth_stats_.materializations},
            {"current_run",retained_fp24_depth_run_},{"maximum_run",std::max(retained_fp24_depth_stats_.maximum_run,retained_fp24_depth_run_)},
            {"shader_switches",retained_fp24_depth_stats_.shader_switches},{"sources",std::move(fp24_sources)},
            {"prewarm_attempts",retained_fp24_depth_stats_.prewarm_attempts},{"prewarm_refusals",retained_fp24_depth_stats_.prewarm_refusals},
            {"source_refusals",retained_fp24_depth_stats_.source_refusals},{"resource_refusals",retained_fp24_depth_stats_.resource_refusals},
            {"residency_refusals",retained_fp24_depth_stats_.residency_refusals},{"conversion_restores",fp24.restores},{"conversion_exports",fp24.exports},
            {"allocation_refusals",fp24.allocation_refusals},{"budget_refusals",fp24.budget_refusals},
            {"retained_image_and_scratch_bytes",retained_fp24_depth_writer_?retained_fp24_depth_writer_->retained_bytes():0},
            {"peak_retained_image_and_scratch_bytes",retained_fp24_depth_writer_?retained_fp24_depth_writer_->peak_retained_bytes():0},
            {"maximum_retained_image_and_scratch_bytes",NativeRetainedFp24DepthWriter::kMaximumRetainedBytes},
            {"depth_attachment_load_op",uint32_t(VK_ATTACHMENT_LOAD_OP_LOAD)},{"depth_attachment_store_op",uint32_t(VK_ATTACHMENT_STORE_OP_STORE)},
            {"framebuffer_depth_attachment_readonly",false},{"pipeline_depth_write_enable",true},{"pipeline_stencil_enable",false},
            {"host_load_store_op_none_supported",static_cast<const VulkanDevice*>(device_)->loadStoreOpNoneSupported},
            {"supported_viewport_depth_ranges",{{1.f,0.f}}}};
        const auto d32=readonly_depth_conversion_?readonly_depth_conversion_->statistics():NativeReadonlyDepthConversion::Statistics{};
        const auto budget=readonly_depth_conversion_?readonly_depth_conversion_->budget():NativeReadonlyDepthConversion::BudgetSnapshot{};
        if(config_.native_early_depth) {
            const auto resources=early_depth_conversion_?early_depth_conversion_->statistics():NativeReadonlyDepthConversion::Statistics{};
            const auto bytes=early_depth_conversion_?early_depth_conversion_->budget():NativeReadonlyDepthConversion::BudgetSnapshot{};
            report["native_early_depth"]={{"enabled",true},{"supported",early_depth_conversion_&&early_depth_conversion_->supported()},
                {"variant",NativeEarlyDepthShaderVersion()},{"canonical_shader_version",kNativeEarlyDepthCanonicalVersion},
                {"prepared",early_depth_stats_.prepared},{"modules",early_depth_stats_.modules},{"module_bytes",early_depth_stats_.module_bytes},
                {"candidates",early_depth_stats_.candidates},{"selected",early_depth_stats_.selected},{"queried_selected",early_depth_stats_.queried_selected},
                {"prewarm_refusals",early_depth_stats_.prewarm_refusals},{"source_refusals",early_depth_stats_.source_refusals},
                {"resource_refusals",early_depth_stats_.resource_refusals},{"residency_refusals",early_depth_stats_.residency_refusals},
                {"cache_hits",early_depth_stats_.cache_hits},{"cache_misses",early_depth_stats_.cache_misses},
                {"promotions",early_depth_stats_.promotions},{"invalidations",early_depth_stats_.invalidations},{"maximum_run",early_depth_stats_.maximum_run},
                {"conversions",resources.conversions},{"budget_refusals",resources.budget_refusals},{"allocation_refusals",resources.allocation_refusals},
                {"retained_image_bytes",bytes.bytes},{"peak_retained_image_bytes",bytes.peak},{"hardware_depth_write",false},
                {"canonical_packed_epilogue_authoritative",true},{"sample_count",1},{"defined_finite_raster_samples_only",true},
                {"maximum_pipelines",8},{"maximum_module_bytes",128u*1024}};
        }
        if(config_.native_early_depth_material_retention) {
            const auto& s=early_depth_material_stats_;
            report["native_early_depth_material_retention"]={{"enabled",true},
                {"candidates",s.candidates},{"admitted",s.admitted},{"selected",s.selected},
                {"query_refusals",s.query_refusals},{"module_refusals",s.module_refusals},
                {"state_refusals",s.state_refusals},{"layout_refusals",s.layout_refusals},
                {"draw_refusals",s.draw_refusals},{"token_refusals",s.token_refusals},{"late_refusals",s.late_refusals},
                {"scope","Certified accepted monotonic material writes preserve an immutable lower bound; original shaders, packed output, barriers and queries remain authoritative"}};
            if(detailed_reports_) {
                auto rows=Json::array();
                for(const auto& r:early_depth_material_refusals_)
                    rows.push_back({{"vertex",r.vertex},{"pixel",r.pixel},
                        {"vertex_variant",uint32_t(r.vertex_variant)},{"pixel_variant",uint32_t(r.pixel_variant)},
                        {"vertex_present",r.certificates[0]},{"pixel_present",r.certificates[1]},
                        {"vertex_certificate_present",r.certificates[2]},{"pixel_certificate_present",r.certificates[3]},
                        {"vertex_certificate_matches",r.certificates[4]},{"pixel_certificate_matches",r.certificates[5]},
                        {"pipeline_present",r.certificates[6]},{"pipeline_matches",r.certificates[7]},
                        {"pipeline_certified",r.certificates[8]},{"state",r.state},
                        {"first_frame",r.first_frame},{"last_frame",r.last_frame},
                        {"first_event",r.first_event},{"last_event",r.last_event},{"count",r.count}});
                report["native_early_depth_material_retention"]["module_refusal_details"]={
                    {"limit",64},{"rows",std::move(rows)},
                    {"overflow",early_depth_material_refusal_overflow_},
                    {"errors",early_depth_material_refusal_errors_}};
            }
        }
        if(config_.reuse_early_depth_readonly_boundaries) {
            const auto bytes=early_depth_conversion_?early_depth_conversion_->budget():NativeReadonlyDepthConversion::BudgetSnapshot{};
            report["native_early_depth_guard_reuse"]={{"enabled",true},{"checks",early_depth_boundary_stats_.checks},
                {"retained",early_depth_boundary_stats_.retained},{"token_refusals",early_depth_boundary_stats_.token_refusals},
                {"check_errors",early_depth_boundary_stats_.check_errors},{"failed_boundaries",early_depth_boundary_stats_.failed_boundaries},
                {"retained_image_bytes",bytes.bytes},{"peak_retained_image_bytes",bytes.peak},{"maximum_retained_image_bytes",32u*1024*1024},
                {"scope","V1 immutable cache only; post-work exact primary layout/token; original full completion and query semantics unchanged"},
                {"changed_token_promotion",false},{"catalogue_enabled",false}};
        }
        if(config_.native_early_depth_catalog) {
            const auto resources=early_depth_conversion_?early_depth_conversion_->statistics():NativeReadonlyDepthConversion::Statistics{};
            const auto bytes=early_depth_conversion_?early_depth_conversion_->budget():NativeReadonlyDepthConversion::BudgetSnapshot{};
            Json sources=Json::array();
            for(uint32_t i=0;i<kNativeEarlyDepthCatalogPixelSources.size();++i)
                sources.push_back({{"vertex",kNativeEarlyDepthCatalogVertexSource},{"pixel",kNativeEarlyDepthCatalogPixelSources[i]},
                    {"selected",early_depth_catalog_source_selected_[i]}});
            report["native_early_depth_catalog"]={{"enabled",true},{"supported",early_depth_conversion_&&early_depth_conversion_->supported()},
                {"variant",NativeEarlyDepthCatalogVersion()},{"canonical_shader_version",kNativeEarlyDepthCanonicalVersion},
                {"prepared",early_depth_catalog_stats_.prepared},{"modules",early_depth_catalog_stats_.modules},{"module_bytes",early_depth_catalog_stats_.module_bytes},
                {"total_prepared",early_depth_stats_.prepared+early_depth_catalog_stats_.prepared},
                {"total_module_bytes",early_depth_stats_.module_bytes+early_depth_catalog_stats_.module_bytes},
                {"total_selected",early_depth_stats_.selected+early_depth_catalog_stats_.selected},
                {"conversion_scope","shared immutable lower-bound image across V1 and independently certified V2 draws"},
                {"continuity_scope","maximum_run, invalidations and shader_switches describe the shared physical generation; selected/hit/miss counts are V2-only"},
                {"candidates",early_depth_catalog_stats_.candidates},{"selected",early_depth_catalog_stats_.selected},
                {"queried_selected",early_depth_catalog_stats_.queried_selected},{"prewarm_refusals",early_depth_catalog_stats_.prewarm_refusals},
                {"source_refusals",early_depth_catalog_stats_.source_refusals},{"resource_refusals",early_depth_catalog_stats_.resource_refusals},
                {"residency_refusals",early_depth_catalog_stats_.residency_refusals},{"cache_hits",early_depth_catalog_stats_.cache_hits},
                {"cache_misses",early_depth_catalog_stats_.cache_misses},{"promotions",early_depth_catalog_stats_.promotions},
                {"invalidations",early_depth_catalog_stats_.invalidations},{"maximum_run",early_depth_catalog_stats_.maximum_run},
                {"shader_switches",early_depth_catalog_shader_switches_},{"sources",std::move(sources)},
                {"conversions",resources.conversions},{"budget_refusals",resources.budget_refusals},{"allocation_refusals",resources.allocation_refusals},
                {"retained_image_bytes",bytes.bytes},{"peak_retained_image_bytes",bytes.peak},{"maximum_retained_image_bytes",32u*1024*1024},
                {"maximum_total_pipelines",8},{"maximum_added_modules",2},{"maximum_added_module_bytes",kEarlyDepthCatalogMaximumModuleBytes},
                {"hardware_depth_write",false},{"canonical_packed_epilogue_authoritative",true},{"defined_finite_raster_samples_only",true}};
        }
        report["native_readonly_depth_no_write_metadata"]={{"enabled",config_.native_readonly_depth_no_write_metadata},
            {"prepared",readonly_no_write_stats_.prepared},{"selected",readonly_no_write_stats_.selected},
            {"refusals",readonly_no_write_stats_.refusals},{"host_invalidations",readonly_no_write_stats_.host_invalidations},
            {"receipt_invalidations",readonly_no_write_stats_.receipt_invalidations}};
        const bool read_only_pass=readonly_depth_conversion_&&readonly_depth_conversion_->read_only_pass();
        report["native_readonly_depth_attachment"]={{"enabled",config_.native_readonly_depth_attachment},
            {"lower_bound_query",config_.native_readonly_depth_lower_bound_query},
            {"lower_bound_read_barriers",readonly_query_read_barriers_},
            {"lower_bound_read_barriers_avoided",readonly_query_read_reuses_},
            {"read_only_pass_configured",config_.native_readonly_depth_read_only_pass},
            {"read_only_pass_active",read_only_pass},
            {"read_only_depth_barriers_avoided",readonly_depth_read_barriers_avoided_},
            {"host_store_op_none_supported",static_cast<const VulkanDevice*>(device_)->storeOpNoneSupported},
            {"conservative_configured",config_.native_readonly_depth_conservative},
            {"conservative_identity",kNativeReadonlyDepthLessIdentity},
            {"conservative_prepared",readonly_depth_conservative_stats_.prepared},
            {"conservative_selected",readonly_depth_conservative_stats_.selected},
            {"conservative_module_refusals",readonly_depth_conservative_stats_.module_refusals},
            {"conservative_residency_refusals",readonly_depth_conservative_stats_.residency_refusals},
            {"skip_edram_barrier_enabled",config_.native_readonly_depth_skip_edram_barrier},
            {"skipped_edram_barriers",readonly_depth_stats_.skipped_edram_barriers},
            {"retained_edram_barriers",readonly_depth_stats_.retained_edram_barriers},
            {"supported",readonly_depth_conversion_&&readonly_depth_conversion_->supported()},
            {"attachment_load_op",uint32_t(VK_ATTACHMENT_LOAD_OP_LOAD)},
            {"attachment_store_op",uint32_t(read_only_pass?VK_ATTACHMENT_STORE_OP_NONE:VK_ATTACHMENT_STORE_OP_STORE)},
            {"attachment_policy",read_only_pass?"Explicit LOAD+STORE_NONE and DEPTH_READ; PSO writes disabled":
                "Plume regular depth framebuffer: explicit LOAD+STORE; PSO writes disabled"},
            {"framebuffer_depth_attachment_readonly",read_only_pass},{"pipeline_depth_write_enable",false},
            {"host_load_store_op_none_supported",static_cast<const VulkanDevice*>(device_)->loadStoreOpNoneSupported},
            {"identity",config_.native_readonly_depth_lower_bound_query?"native-readonly-query-full-fp24-predecessor-exact-packed-discard-v1":
                "native-readonly-depth-d32-v2-exact-half-zero-bias-load-store"},
            {"supported_viewport_depth_ranges",{{0.f,1.f},{1.f,0.f}}},
            {"supported_raster",{uint32_t(RenderCullMode::BACK),uint32_t(RenderFrontFace::COUNTER_CLOCKWISE),1,
                uint32_t(RenderPrimitiveTopology::TRIANGLE_LIST),0,0}},{"prepared",readonly_depth_stats_.prepared},
            {"candidates",readonly_depth_stats_.candidates},{"selected",readonly_depth_stats_.selected},{"queried_selected",readonly_depth_stats_.queried_selected},
            {"cache_hits",readonly_depth_stats_.cache_hits},{"cache_misses",readonly_depth_stats_.cache_misses},
            {"source_refusals",readonly_depth_stats_.source_refusals},{"residency_refusals",readonly_depth_stats_.residency_refusals},
            {"prewarm_refusals",readonly_depth_stats_.prewarm_refusals},{"resource_refusals",readonly_depth_stats_.resource_refusals},
            {"promotions",readonly_depth_stats_.promotions},{"promotion_refusals",readonly_depth_stats_.promotion_refusals},
            {"conversions",d32.conversions},{"budget_refusals",d32.budget_refusals},{"allocation_refusals",d32.allocation_refusals},
            {"scratch_allocation_bytes",d32.scratch_bytes},{"retained_image_bytes",budget.bytes},{"peak_retained_image_bytes",budget.peak},
            {"maximum_retained_image_bytes",NativeReadonlyDepthConversion::kMaximumRetainedImageBytes}};
        if(config_.retained_shared_depth_queries)report["retained_shared_depth_queries"]={
            {"enabled",true},{"prepared",shared_depth_query_stats_.prepared},{"module_bytes",shared_depth_query_stats_.module_bytes},
            {"candidates",shared_depth_query_stats_.candidates},{"selected",shared_depth_query_stats_.selected},
            {"queried_selected",shared_depth_query_stats_.queried_selected},{"promotions",shared_depth_query_stats_.promotions},
            {"residency_refusals",shared_depth_query_stats_.residency_refusals},{"framebuffer_refusals",shared_depth_query_stats_.framebuffer_refusals},
            {"prewarm_refusals",shared_depth_query_stats_.prewarm_refusals},{"preparation_calls",shared_depth_query_stats_.preparation_calls},
            {"retained_preparations",shared_depth_query_stats_.retained_preparations},{"strong_fallbacks",shared_depth_query_stats_.strong_fallbacks},
            {"accepted_begins",shared_depth_query_stats_.accepted_begins},{"accepted_ends",shared_depth_query_stats_.accepted_ends},
            {"last_end_required_work",shared_depth_query_stats_.last_end_required_work},{"last_end_completed_work",shared_depth_query_stats_.last_end_completed_work},
            {"policy","Same authoritative D32 generation; no-color LOAD/STORE framebuffer; unchanged late FragDepth; original real queries and metadata tokens"}};
        if(RetainedOpaqueEnabled()||config_.retained_opaque_phase)report[config_.retained_opaque_phase?"retained_opaque_phase":config_.retained_opaque_catalog?"retained_opaque_catalog":"retained_opaque_writer"]={{"enabled",true},{"supported",retained_opaque_writer_&&retained_opaque_writer_->supported()},
            {"variant_version",config_.retained_opaque_program?Json(config_.retained_opaque_program->variant_version):Json(nullptr)},
            {"baseline_version",config_.retained_opaque_program?Json(config_.retained_opaque_program->baseline_version):Json(nullptr)},
            {"spirv_sha256",config_.retained_opaque_program?Json(config_.retained_opaque_program->spirv_sha256):Json(nullptr)},
            {"modules_created",retained_opaque_headroom_.modules()},{"module_bytes",retained_opaque_headroom_.bytes()},
            {"prepared",retained_opaque_stats_.prepared},{"candidates",retained_opaque_stats_.candidates},{"selected",retained_opaque_stats_.selected},{"queried_selected",retained_opaque_stats_.queried_selected},
            {"reuses",retained_opaque_stats_.reuses},{"restores",retained_opaque_stats_.restores},{"materializations",retained_opaque_stats_.materializations},{"maximum_run",std::max(retained_opaque_run_,retained_opaque_stats_.maximum_run)},
            {"queue_exports_for_typed_draws",config_.queue_retained_exports_for_draws},
            {"queued_draw_materializations",retained_opaque_stats_.queued_draw_materializations},
            {"queued_draw_pending",retained_opaque_stats_.queued_draw_pending},
            {"source_refusals",retained_opaque_stats_.source_refusals},{"resource_refusals",retained_opaque_stats_.resource_refusals},{"residency_refusals",retained_opaque_stats_.residency_refusals},{"prewarm_refusals",retained_opaque_stats_.prewarm_refusals},
            {"retained_image_bytes",retained_opaque_writer_?retained_opaque_writer_->retained_bytes():0},{"peak_retained_image_bytes",retained_opaque_writer_?retained_opaque_writer_->peak_bytes():0},
            {"maximum_retained_image_bytes",NativeRetainedOpaqueWriter::kMaximumRetainedImageBytes},{"supported_viewport_depth_ranges",Json::array({Json::array({1.f,0.f})})}};
        if((RetainedOpaqueEnabled()||config_.retained_opaque_phase)&&retained_opaque_writer_) {
            auto& writer=report[config_.retained_opaque_phase?"retained_opaque_phase":config_.retained_opaque_catalog?"retained_opaque_catalog":"retained_opaque_writer"];
            const auto& r=retained_opaque_writer_->statistics();const auto pools=retained_opaque_writer_->pool_statistics();
            const auto pool=[](const auto& s)->Json {return {{"hits",s.hits},{"misses",s.misses},{"returns",s.returns},
                {"admitted",s.admitted},{"evictions",s.evictions},{"rejected",s.rejected},{"idle_resources",s.idle_images},
                {"idle_bytes",s.idle_bytes},{"peak_idle_resources",s.peak_idle_images},{"peak_idle_bytes",s.peak_idle_bytes}};};
            writer["resource_pool"]={{"enabled",retained_opaque_writer_->reuses_resources()},
                {"attachment_allocations",r.attachment_allocations},{"scratch_allocations",r.scratch_allocations},
                {"attachment_reuses",r.attachment_reuses},{"scratch_reuses",r.scratch_reuses},
                {"reused_restore_layouts",r.reused_restore_layouts},{"pooled_restores",r.pooled_restores},{"pooled_exports",r.pooled_exports},
                {"allocation_refusals",r.allocation_refusals},{"budget_refusals",r.budget_refusals},
                {"actual_active_and_idle_bytes",retained_opaque_writer_->retained_bytes()},
                {"peak_actual_active_and_idle_bytes",retained_opaque_writer_->peak_bytes()},
                {"maximum_actual_active_and_idle_bytes",NativeRetainedOpaqueWriter::kMaximumRetainedImageBytes},
                {"attachments",pool(pools.attachments)},{"scratch",pool(pools.scratch)}};
            if(config_.retained_opaque_phase)writer["shared_attachments"]={{"enabled",retained_opaque_writer_->shares_attachments()},
                {"allocations",r.shared_attachment_allocations},{"reuses",r.shared_attachment_reuses},
                {"closes",r.shared_attachment_closes},{"clean_closes",r.shared_attachment_clean_closes},
                {"refusals",r.shared_attachment_refusals},{"failures",r.shared_attachment_failures},
                {"poisoned",retained_opaque_writer_->shared_attachments_failed()},
                {"immutable_bindings",true},{"image_budget_unchanged",true}};
            if(config_.retained_opaque_phase)writer["shared_scratch"]={{"enabled",retained_opaque_writer_->shares_scratch()},
                {"allocations",r.shared_scratch_allocations},{"reuses",r.shared_scratch_reuses},
                {"restores",r.shared_scratch_restores},{"exports",r.shared_scratch_exports},
                {"refusals",r.shared_scratch_refusals},{"recording_failures",r.shared_scratch_recording_failures},
                {"bytes",r.shared_scratch_bytes},{"maximum_transfer_bytes",NativeTransferBatch::kMaxImageBytes},
                {"image_budget_unchanged",true},{"immutable_bindings",true}};
        }
        if(config_.retained_opaque_phase) {
            auto& stats=report["retained_opaque_phase"];
            const auto& pressure=retained_opaque_pressure_stats_;
            stats["image_pressure_poll"]={{"enabled",config_.poll_retained_opaque_phase_pressure},
                {"last_actual_pair_bytes",retained_opaque_last_pair_bytes_},{"allocation_checks",pressure.allocation_checks},
                {"polls",pressure.polls},{"fence_retirements",pressure.fence_retirements},
                {"retired_image_bytes",pressure.retired_image_bytes},{"unchanged_byte_polls",pressure.unchanged_byte_polls},
                {"admitted_after_poll",pressure.admitted_after_poll},{"refused_after_poll",pressure.refused_after_poll},
                {"maximum_image_bytes",NativeRetainedOpaqueWriter::kMaximumRetainedImageBytes}};
            stats["variant_version"]=NativeRetainedOpaquePhaseConservativeCompilerVersion();
            stats["shader_variant"]=uint32_t(NativeShaderVariant::RetainedOpaquePhaseConservativeV4);
            stats["depth_representation"]="fp24-cellfloor-unit-tail-v1";
            stats["baseline_version"]=NativeShaderCompiler::CacheVersion();
            stats["modules_created"]=retained_opaque_stats_.modules_created;stats["module_bytes"]=retained_opaque_stats_.module_bytes;
            stats["maximum_programs"]=kRetainedOpaquePhaseMaxPrograms;stats["maximum_pipelines"]=kRetainedOpaquePhaseMaxPipelines;
            stats["shader_switches"]=retained_opaque_stats_.shader_switches;stats["current_run"]=retained_opaque_run_;
            stats["selected_by_recipe"]=retained_opaque_phase_selected_;
            stats["primary_write_receipts"]={{"selected",phase_primary_write_receipts_},
                {"fallbacks",phase_primary_write_receipt_fallbacks_},
                {"scope","Only two post-write primary version queries; both original writes/pre-queries and live authority remain unchanged."}};
            stats["resident_module_credit"]=retained_opaque_phase_headroom_.modules();
            stats["resident_byte_credit"]=retained_opaque_phase_headroom_.bytes();
            stats["resident_pipeline_credit"]=retained_opaque_phase_headroom_.pipelines();
            stats["policy"]="Exact certified packed-output materials; authoritative R32_UINT/D32; ordinary Native before unknown consumers";
        }
        if(config_.retained_opaque_catalog) {
            auto& stats=report["retained_opaque_catalog"];
            stats["variant_version"]=NativeRetainedOpaqueCatalogCompilerVersion();stats["baseline_version"]=NativeShaderCompiler::CacheVersion();
            stats["modules_created"]=retained_opaque_stats_.modules_created;stats["module_bytes"]=retained_opaque_stats_.module_bytes;
            stats["shader_switches"]=retained_opaque_stats_.shader_switches;stats["current_run"]=retained_opaque_run_;
            const auto soft=retained_opaque_catalog_headroom_.SoftTargets(true,config_.max_shader_cache_entries,
                config_.max_shader_cache_bytes,config_.max_pipeline_cache_entries);
            stats["cache_headroom"]={{"admitted_modules",retained_opaque_catalog_headroom_.modules()},
                {"admitted_spirv_bytes",retained_opaque_catalog_headroom_.bytes()},{"admitted_pipelines",retained_opaque_catalog_headroom_.pipelines()},
                {"soft_module_target",soft.modules},{"soft_byte_target",soft.bytes},{"soft_pipeline_target",soft.pipelines}};
            stats["maximum_programs"]=kRetainedOpaqueCatalogMaxPrograms;stats["maximum_pipelines"]=kRetainedOpaqueCatalogMaxPipelines;
            Json programs=Json::array();
            for(size_t i=0;i<kRetainedOpaqueCatalogPairs.size();++i) {
                const auto& pair=kRetainedOpaqueCatalogPairs[i];const auto* program=RetainedOpaqueProgramFor(pair.vertex,pair.pixel);
                const auto& count=retained_opaque_pair_stats_[i];
                programs.push_back({{"vertex",pair.vertex},{"pixel",pair.pixel},{"spirv_sha256",program?Json(program->spirv_sha256):Json(nullptr)},
                    {"candidates",count.candidates},{"selected",count.selected},{"queried_selected",count.queried_selected}});
            }
            stats["programs"]=std::move(programs);
        }
        report["diagnostic_serializations"]=diagnostic_serializations_+1;
        ++diagnostic_serializations_;transfer_report_dirty_=false;
    }
    Json GpuBatchAttributionReport(bool details) const {
        if(!gpu_work_queue_||!gpu_work_queue_->attribution())return {{"enabled",false}};
        return ReportNativeGpuBatchAttribution(*gpu_work_queue_->attribution(),details);
    }
    Json GpuDrawTimingReport(bool details) const {
        if(draw_timestamps_)return draw_timestamps_->Report(details);
        if(config_.gpu_draw_timestamps)return {{"enabled",true},{"supported",false},
            {"incomplete",true},{"status","unavailable: diagnostic initialization failed"}};
        return {{"enabled",false}};
    }
    Json FragmentInvocationReport() const {
#ifdef GOW3_NATIVE_FRAGMENT_CENSUS_REPLAY
        if(fragment_census_)return fragment_census_->Report();
#endif
        if(config_.replay_fragment_invocations)return {{"enabled",true},{"supported",false},{"incomplete",true},
            {"status","unavailable: standalone diagnostic initialization failed or not compiled"}};
        return {{"enabled",false}};
    }
    void RunTransferScope(const std::function<void()>& work,uint64_t event,const NativeGpuWorkTag* tag=nullptr) {
        readonly_query_read_owner_.reset();
        transfer_report_dirty_=true;
        SealDrawCommandGroup();
        try {transfer_batch_->Run(work,event,tag);}
        catch(const NativeTransferBatchError& error) {throw NativeBatchCompletionError(error.what());}
    }
    bool QueueCpuWrite(const Json& o) const {
        if(!NativeCpuWriteQueueEligible(config_.queue_cpu_memory_writes,bool(gpu_work_queue_),
                o.at("address").get<uint32_t>(),o.at("bytes").get<uint32_t>()))return false;
        for(const auto& pending:deferred_queries_)
            if(NativeCpuWriteOverlapsQueryReport(o.at("address"),o.at("bytes"),pending.query.result.end_address))return false;
        return true;
    }
    void CpuMemoryWrite(const Json& o,bool queue_write) {
        if(queue_write&&!transfer_batch_->in_scope()) {
            // An exception after Invalidate is authoritative mutation even if
            // a later payload lookup fails before recording the upload stage.
            RunTransferScope([&]{CpuMemoryWrite(o,true);},o.at("event"));return;
        }
        const uint32_t address=o.at("address"),count=o.at("bytes");
        resolve_memory_->InvalidateCpuWrite(address,count,queue_write);
        if(!o.at("payload").is_null()) {
            auto bytes=Payload(o.at("payload"));
            if(bytes.size()!=count)throw std::runtime_error("CPU memory update extent mismatch");
            resolve_memory_->InitializeCpuWrite(address,bytes,queue_write);
        }
    }
    bool TryFusedAtlasDepthClear(const Json& o,unsigned flags,const RenderRect& r) {
        const auto& surfaces=o.at("surfaces");
        if(!config_.native_fsi||flags!=0x10||!surfaces.contains("10")||
           !o.at("depth").is_number())return false;
        const auto& identity=surfaces.at("10");
        const auto storage=SurfaceStorage(identity);
        const NativeFusedAtlasDepthClearInput input{config_.native_fsi,queries_->active(),storage.depth,
            storage.width,storage.height,storage.pitch,storage.base,storage.format,
            identity.at("surface_info"),flags,o.at("depth").get<float>(),
            {r.left,r.top,r.right,r.bottom}};
        if(!NativeFusedAtlasDepthClearAllowed(input))return false;
        ++fused_atlas_depth_clear_stats_.candidates;
        // Validate the otherwise unused scalar fields exactly as the original
        // Clear does before recording any optional compute commands.
        if(!o.at("color").is_null())(void)o.at("color").get<std::array<float,4>>();
        (void)o.at("stencil").get<uint32_t>();
        MaterializeRetainedOpaque();
        const auto key=SurfaceKey(identity);const auto tiles=SurfaceTiles(storage);
        frame_surfaces_.insert(key);
        // Same ordered alias commits as Surface(), before observing provenance.
        // Do not add a blanket flush of unrelated dirty working images.
        for(auto& [other,image]:surfaces_)if(other!=key&&image.dirty&&(image.tiles&tiles).any()) {
            const auto previous=SurfaceStorage(image.identity);
            edram_memory_->StoreSurface(*image.texture,previous);
            image.version=edram_memory_->SurfaceVersion(previous);image.dirty=false;
        }
        const auto version=edram_memory_->SurfaceVersion(storage);
        auto found=surfaces_.find(key);
        if(found!=surfaces_.end()&&found->second.atlas_clear_receipt.source&&
           !NativeFusedAtlasDepthClearReceiptCurrent(true,found->second.atlas_clear_receipt.packed_version,version)) {
            found->second.atlas_clear_receipt={};
            ++fused_atlas_depth_clear_stats_.deferred_host_discards;
        }
        bool other_dirty=false;
        for(const auto& [other,image]:surfaces_)if(other!=key&&image.dirty)other_dirty=true;
        const auto source=NativeFusedAtlasDepthClearChooseSource(
            edram_memory_->SurfaceFullyDefined(storage),found!=surfaces_.end()&&bool(found->second.texture),
            found!=surfaces_.end()&&found->second.dirty,
            found!=surfaces_.end()?found->second.version:0,version,
            found!=surfaces_.end()&&bool(found->second.atlas_clear_receipt.source),other_dirty);
        if(source==NativeFusedAtlasDepthClearSource::Refuse) {
            ++fused_atlas_depth_clear_stats_.provenance_refusals;return false;
        }
        if(!fused_atlas_depth_clear_stats_.configured) {
            ++fused_atlas_depth_clear_stats_.resource_refusals;return false;
        }
        // Allocate all Scene bookkeeping before acceptance. An absent host
        // entry need not allocate a D32 image until a genuine image consumer.
        const bool inserted=found==surfaces_.end();
        if(inserted) {
            found=surfaces_.try_emplace(key).first;
            found->second.identity=identity;found->second.tiles=tiles;
        }
        auto& entry=found->second;
        ResolveMemory::AtlasDepthClearReceipt receipt;
        if(!edram_memory_->ClearAtlasDepthOneFromPacked(storage,r,receipt,
                source==NativeFusedAtlasDepthClearSource::Host?entry.texture.get():nullptr)) {
            if(inserted)surfaces_.erase(found);
            ++fused_atlas_depth_clear_stats_.resource_refusals;return false;
        }
        if(!receipt.source||receipt.packed_version<=version||
           receipt.packed_version!=edram_memory_->SurfaceVersion(storage)) {
            batch_failed_=true;
            throw NativeBatchCompletionError("Fused atlas clear accepted an invalid host receipt");
        }
        // Every receipt has this same full packed footprint. A new full Define
        // invalidates older receipts; drop them without touching dirty hosts.
        for(auto& [other,image]:surfaces_)if(image.atlas_clear_receipt.source) {
            image.atlas_clear_receipt={};++fused_atlas_depth_clear_stats_.deferred_host_discards;
        }
        entry.atlas_clear_receipt=std::move(receipt);
        const auto retained_bytes=static_cast<const VulkanTexture&>(*entry.atlas_clear_receipt.source).allocationInfo.size;
        fused_atlas_depth_clear_stats_.peak_receipt_bytes=std::max(
            fused_atlas_depth_clear_stats_.peak_receipt_bytes,retained_bytes);
        // Leave the old host token untouched. Same-version image reads must
        // restore the pre-encode receipt; changed packed versions use full load.
        ++mutation_serial_;++fused_atlas_depth_clear_stats_.selected;
        if(source==NativeFusedAtlasDepthClearSource::Host)++fused_atlas_depth_clear_stats_.host_sources;
        else ++fused_atlas_depth_clear_stats_.packed_sources;
        fused_atlas_depth_clear_stats_.source_pixels+=kNativeFusedAtlasDepthClearPixels;
        fused_atlas_depth_clear_stats_.clear_pixels+=uint64_t(r.right-r.left)*uint64_t(r.bottom-r.top);
        return true;
    }
    bool TryPackedPrimaryStencilClear(const Json& o,unsigned flags,const RenderRect& r) {
        const auto& attachments=o.at("surfaces");
        if(!config_.native_fsi||flags!=0x20||!attachments.contains("10")||
           !o.at("depth").is_number())return false;
        const auto& identity=attachments.at("10");
        const auto storage=SurfaceStorage(identity);
        const NativePackedPrimaryStencilClearInput input{config_.native_fsi,queries_->active(),storage.depth,
            storage.width,storage.height,storage.pitch,storage.base,storage.format,
            identity.at("surface_info"),flags,o.at("depth").get<float>(),
            {r.left,r.top,r.right,r.bottom}};
        if(!NativePackedPrimaryStencilClearLayoutAllowed(input))return false;
        // Malformed otherwise-unused fields retain original Surface/Clear
        // validation and side-effect order instead of taking the compute path.
        uint32_t stencil=0;
        try {
            if(!o.at("color").is_null())(void)o.at("color").get<std::array<float,4>>();
            stencil=o.at("stencil").get<uint32_t>();
        } catch(const std::exception&) {return false;}
        auto& stats=packed_primary_stencil_clear_stats_;++stats.candidates;
        if(input.query_active){++stats.query_refusals;return false;}
        MaterializeRetainedOpaque();
        const auto key=SurfaceKey(identity);const auto tiles=SurfaceTiles(storage);
        frame_surfaces_.insert(key);
        // Verbatim Surface() order: commit dirty overlapping aliases before
        // observing the packed publication token; no unrelated flush is added.
        for(auto& [other,image]:surfaces_)if(other!=key&&image.dirty&&(image.tiles&tiles).any()) {
            const auto previous=SurfaceStorage(image.identity);
            edram_memory_->StoreSurface(*image.texture,previous);
            image.version=edram_memory_->SurfaceVersion(previous);image.dirty=false;
        }
        const auto version=edram_memory_->SurfaceVersion(storage);
        const auto found=surfaces_.find(key);
        bool other_dirty=false;
        for(const auto& [other,image]:surfaces_)if(other!=key&&image.dirty)other_dirty=true;
        const auto source=NativePackedPrimaryStencilClearChooseSource(
            edram_memory_->SurfaceFullyDefined(storage),found!=surfaces_.end()&&bool(found->second.texture),
            found!=surfaces_.end()&&found->second.dirty,
            found!=surfaces_.end()?found->second.version:0,version,
            found!=surfaces_.end()&&bool(found->second.atlas_clear_receipt.source),other_dirty);
        if(source==NativePackedPrimaryStencilClearSource::CurrentHost) {
            ++stats.current_host_refusals;return false;
        }
        if(source!=NativePackedPrimaryStencilClearSource::Packed) {
            ++stats.provenance_refusals;return false;
        }
        if(!stats.configured||!edram_memory_->ClearPrimaryStencilFromPacked(storage,r,stencil)) {
            ++stats.resource_refusals;return false;
        }
        // Full Define must advance the packed token after accepted compute.
        // Keep an existing D32 token OLD: its next genuine consumer takes the
        // unchanged full restore from canonical normalized packed contents.
        if(edram_memory_->SurfaceVersion(storage)<=version) {
            batch_failed_=true;
            throw NativeBatchCompletionError("Packed primary stencil clear did not publish a new version");
        }
        ++mutation_serial_;++stats.selected;
        stats.source_pixels+=kNativePackedPrimaryStencilClearPixels;
        stats.clear_pixels+=uint64_t(r.right-r.left)*uint64_t(r.bottom-r.top);
        return true;
    }
    bool TryPackedSecondaryStencilClear(const Json& o,unsigned flags,const RenderRect& r) {
        const auto& attachments=o.at("surfaces");
        if(!config_.native_fsi||flags!=0x20||!attachments.contains("10")||
           !o.at("depth").is_number())return false;
        const auto& identity=attachments.at("10");
        const auto storage=SurfaceStorage(identity);
        const NativePackedSecondaryStencilClearInput input{config_.native_fsi,queries_->active(),storage.depth,
            storage.width,storage.height,storage.pitch,storage.base,storage.format,
            identity.at("surface_info"),flags,o.at("depth").get<float>(),
            {r.left,r.top,r.right,r.bottom}};
        if(!NativePackedSecondaryStencilClearLayoutAllowed(input))return false;
        // Malformed otherwise-unused fields retain original Surface/Clear
        // validation and side-effect order instead of taking the compute path.
        uint32_t stencil=0;
        try {
            if(!o.at("color").is_null())(void)o.at("color").get<std::array<float,4>>();
            stencil=o.at("stencil").get<uint32_t>();
        } catch(const std::exception&) {return false;}
        auto& stats=packed_secondary_stencil_clear_stats_;++stats.candidates;
        if(input.query_active){++stats.query_refusals;return false;}
        MaterializeRetainedOpaque();
        const auto key=SurfaceKey(identity);const auto tiles=SurfaceTiles(storage);
        frame_surfaces_.insert(key);
        // Verbatim Surface() order: commit dirty overlapping aliases before
        // observing the packed publication token; no unrelated flush is added.
        for(auto& [other,image]:surfaces_)if(other!=key&&image.dirty&&(image.tiles&tiles).any()) {
            const auto previous=SurfaceStorage(image.identity);
            edram_memory_->StoreSurface(*image.texture,previous);
            image.version=edram_memory_->SurfaceVersion(previous);image.dirty=false;
        }
        const auto version=edram_memory_->SurfaceVersion(storage);
        const auto found=surfaces_.find(key);
        bool other_dirty=false;
        for(const auto& [other,image]:surfaces_)if(other!=key&&image.dirty)other_dirty=true;
        const auto source=NativePackedSecondaryStencilClearChooseSource(
            edram_memory_->SurfaceFullyDefined(storage),found!=surfaces_.end()&&bool(found->second.texture),
            found!=surfaces_.end()&&found->second.dirty,
            found!=surfaces_.end()?found->second.version:0,version,
            found!=surfaces_.end()&&bool(found->second.atlas_clear_receipt.source),other_dirty);
        if(source==NativePackedSecondaryStencilClearSource::CurrentHost) {
            ++stats.current_host_refusals;return false;
        }
        if(source!=NativePackedSecondaryStencilClearSource::Packed) {
            ++stats.provenance_refusals;return false;
        }
        if(!stats.configured||!edram_memory_->ClearSecondaryStencilFromPacked(storage,r,stencil)) {
            ++stats.resource_refusals;return false;
        }
        // Full Define must advance the packed token after accepted compute.
        // Keep an existing D32 token OLD: its next genuine consumer takes the
        // unchanged full restore from canonical normalized packed contents.
        if(edram_memory_->SurfaceVersion(storage)<=version) {
            batch_failed_=true;
            throw NativeBatchCompletionError("Packed secondary stencil clear did not publish a new version");
        }
        ++mutation_serial_;++stats.selected;
        stats.source_pixels+=kNativePackedSecondaryStencilClearPixels;
        stats.clear_pixels+=uint64_t(r.right-r.left)*uint64_t(r.bottom-r.top);
        return true;
    }
    void Clear(const Json& o) {
        if(transfer_batch_&&!transfer_batch_->in_scope()) {
            NativeGpuWorkTag tag;
            if(config_.gpu_batch_attribution||config_.gpu_physical_timestamps){tag.kind=NativeGpuWorkKind::Clear;try{tag.event=o.at("event");tag.frame=o.at("frame");}catch(...){}}
            RunTransferScope([&]{Clear(o);FlushCommittedState(true);},o.at("event"),(config_.gpu_batch_attribution||config_.gpu_physical_timestamps)?&tag:nullptr);return;
        }
        const unsigned flags=o.at("flags");
        if(flags&~0x3fu)throw std::runtime_error("Special clear flags not implemented");
        auto r=Rectangle(o.at("rectangle"));if(r.isEmpty())return;
        const auto& surfaces=o.at("surfaces");
        if(config_.packed_primary_stencil_clear&&TryPackedPrimaryStencilClear(o,flags,r)) {
            transfer_report_dirty_=true;++diagnostic_counters_.clears;return;
        }
        if(config_.packed_secondary_stencil_clear&&TryPackedSecondaryStencilClear(o,flags,r)) {
            transfer_report_dirty_=true;++diagnostic_counters_.clears;return;
        }
        if(config_.fused_atlas_depth_clear&&TryFusedAtlasDepthClear(o,flags,r)) {
            transfer_report_dirty_=true;++diagnostic_counters_.clears;return;
        }
        if(config_.packed_zero_depth_clear) {
            ++packed_zero_clear_stats_.considered;
            if(config_.native_fsi&&flags==0x30&&surfaces.contains("10")&&
               o.at("depth").is_number()&&o.at("stencil")==0) {
                const auto& identity=surfaces.at("10");
                const auto storage=SurfaceStorage(identity);
                const PackedZeroDepthClearInput input{config_.native_fsi,storage.depth,flags,
                    storage.width,storage.height,storage.pitch,storage.base,storage.format,
                    identity.at("surface_info"),{r.left,r.top,r.right,r.bottom},
                    o.at("depth"),o.at("stencil")};
                if(PackedZeroDepthClearAllowed(input)) {
                    // Dirty host images, including differently interpreted aliases,
                    // must commit before the canonical packed bytes are overwritten.
                    // Version increments leave every retained host view stale.
                    // Original Clear validates color even for depth-only flags.
                    if(!o.at("color").is_null())(void)o.at("color").get<std::array<float,4>>();
                    FlushCommittedState();
                    const auto key=SurfaceKey(identity);
                    if(surfaces_.contains(key))frame_surfaces_.insert(key);
                    ++mutation_serial_;edram_memory_->ClearPackedDepthZero(storage);
                    ++packed_zero_clear_stats_.selected;
                    transfer_report_dirty_=true;++diagnostic_counters_.clears;return;
                }
            }
            ++packed_zero_clear_stats_.fallback;
        }
        bool discard_full_color=false;
        auto attachment=[&](const std::string& k)->RenderTexture* {
            return &Surface(surfaces.at(k),true,discard_full_color?&r:nullptr);
        };
        RenderTexture* depth=flags&0x30?attachment("10"):nullptr;
        auto color=o.at("color").is_null()?std::array<float,4>{}:o.at("color").get<std::array<float,4>>();
        if(config_.skip_full_color_clear_restore&&config_.native_fsi&&!(flags&0x30)&&(flags&0xf)) {
            // PassTransfer validates depth even for color-only clears. Refuse
            // omission on malformed/range-invalid scalar input, preserving the
            // original validation path rather than leaving a stale image dirty.
            const auto depth_value=o.find("depth"),stencil_value=o.find("stencil");
            if(depth_value!=o.end()&&depth_value->is_number()&&stencil_value!=o.end()) {
                const float value=depth_value->get<float>();
                const bool stencil_ok=stencil_value->is_number_unsigned()?
                    stencil_value->get<uint64_t>()<=UINT32_MAX:
                    stencil_value->is_number_integer()&&stencil_value->get<int64_t>()>=0&&
                    stencil_value->get<int64_t>()<=UINT32_MAX;
                discard_full_color=std::isfinite(value)&&value>=0.f&&value<=1.f&&stencil_ok;
            }
        }
        bool did_depth=false;
        // Clear flags select bound attachments; unbound MRT slots are ignored
        // by the guest D3D implementation as well.
        for(unsigned i=0;i<4;++i)if((flags&(1u<<i))&&surfaces.contains(std::to_string(6+i))) {
            ++mutation_serial_;
            transfers_->Clear(attachment(std::to_string(6+i)),depth,true,!did_depth&&(flags&0x10),!did_depth&&(flags&0x20),r,
                {color[0],color[1],color[2],color[3]},o.at("depth"),o.at("stencil"));did_depth=true;
        }
        if(depth&&!did_depth){++mutation_serial_;transfers_->Clear(nullptr,depth,false,flags&0x10,flags&0x20,r,{},o.at("depth"),o.at("stencil"));}
        transfer_report_dirty_=true;++diagnostic_counters_.clears;
    }
    std::shared_ptr<RenderTexture> DirectAtlasDepthResolveSource(const Json& o,const Json& source,
            const TextureLayout& destination,const RenderRect& src,const RenderRect& dst,
            NativeDirectAtlasDepthResolveSource& selected_source,RenderTexture& resolve_destination,
            bool& fused_resolve) {
        auto& stats=direct_atlas_depth_resolve_stats_;++stats.candidates;
        auto& fused=fused_packed_atlas_resolve_stats_;const bool want_fused=config_.fused_packed_atlas_resolves;
        if(want_fused)++fused.candidates;
        if(queries_->active()) {++stats.query_refusals;if(want_fused)++fused.query_refusals;return {};}
        const auto storage=SurfaceStorage(source);
        NativeDirectAtlasDepthResolveInput input;
        input.native_fsi=config_.native_fsi;input.query_active=queries_->active();
        input.source_width=storage.width;input.source_height=storage.height;input.source_pitch=storage.pitch;
        input.source_base=storage.base;input.source_format=storage.format;input.source_depth=storage.depth;
        input.source_surface_info=source.at("surface_info");
        input.destination_width=destination.width;input.destination_height=destination.height;
        input.destination_format=destination.format;input.destination_dimension=destination.dimension;
        input.destination_layers=destination.layers;input.destination_endian=destination.endian;
        input.destination_level_count=uint32_t(destination.levels.size());
        input.destination_min_mip=destination.min_mip;input.destination_max_mip=destination.max_mip;
        input.destination_block_width=destination.block.block_width;
        input.destination_block_height=destination.block.block_height;input.destination_block_bytes=destination.block.bytes;
        input.destination_tiled=destination.tiled;
        if(destination.levels.size()==1)input.destination_pitch=destination.levels.front().pitch_blocks;
        input.flags=o.at("flags");input.level=o.at("level");input.source_slot=o.at("source_slot");
        input.exponent_bias=o.at("exponent_bias");input.destination_swap=ResolveDestinationSwap(destination.fetch,input.source_slot);
        input.source_rectangle={src.left,src.top,src.right,src.bottom};
        input.destination_rectangle={dst.left,dst.top,dst.right,dst.bottom};
        // The rejected partial restoration experiment is not composed here.
        if(config_.atlas_depth_region_restores||!NativeDirectAtlasDepthResolveAllowed(input)) {
            ++stats.layout_refusals;if(want_fused)++fused.layout_refusals;return {};
        }
        if(!stats.configured||!resolve_memory_->SupportsAtlasRgbaDepthStore()) {
            ++stats.resource_refusals;if(want_fused)++fused.resource_refusals;return {};
        }
        MaterializeRetainedOpaque();
        const auto key=SurfaceKey(source);const auto tiles=SurfaceTiles(storage);
        frame_surfaces_.insert(key);
        // Identical ordering to Surface(): commit only dirty overlapping aliases
        // before observing full packed provenance. Do not flush unrelated work.
        for(auto& [other,image]:surfaces_)if(other!=key&&image.dirty&&(image.tiles&tiles).any()) {
            const auto previous=SurfaceStorage(image.identity);
            edram_memory_->StoreSurface(*image.texture,previous);
            image.version=edram_memory_->SurfaceVersion(previous);image.dirty=false;
        }
        const auto version=edram_memory_->SurfaceVersion(storage);
        auto found=surfaces_.find(key);
        if(found!=surfaces_.end()&&found->second.atlas_clear_receipt.source&&
           !NativeFusedAtlasDepthClearReceiptCurrent(true,found->second.atlas_clear_receipt.packed_version,version)) {
            found->second.atlas_clear_receipt={};
            ++fused_atlas_depth_clear_stats_.deferred_host_discards;
        }
        selected_source=NativeDirectAtlasDepthResolveChooseSource(
            edram_memory_->SurfaceFullyDefined(storage),found!=surfaces_.end()&&bool(found->second.texture),
            found!=surfaces_.end()&&found->second.dirty,found!=surfaces_.end()?found->second.version:0,version,
            found!=surfaces_.end()&&bool(found->second.atlas_clear_receipt.source),
            found!=surfaces_.end()?found->second.atlas_clear_receipt.packed_version:0);
        if(selected_source==NativeDirectAtlasDepthResolveSource::CurrentHost) {
            ++stats.current_host_refusals;if(want_fused)++fused.current_host_refusals;return {};
        }
        if(selected_source==NativeDirectAtlasDepthResolveSource::Refuse) {
            ++stats.provenance_refusals;if(want_fused)++fused.provenance_refusals;return {};
        }
        if(selected_source==NativeDirectAtlasDepthResolveSource::Receipt) {
            if(want_fused)++fused.receipt_refusals;
            auto rgba=found->second.atlas_clear_receipt.source;
            // Its producing compute packet may have retired. This new resolve
            // packet independently owns the immutable pre-encode float image.
            if(transfer_batch_&&transfer_batch_->active())transfer_batch_->KeepImage(rgba,
                static_cast<const VulkanTexture&>(*rgba).allocationInfo.size);
            return rgba; // Preserve receipt/token for future generic consumers.
        }
        if(want_fused) {
            if(!NativeFusedPackedAtlasRectangleAllowed({src.left,src.top,src.right,src.bottom},
                    {dst.left,dst.top,dst.right,dst.bottom}))++fused.layout_refusals;
            else {
                std::shared_ptr<RenderTexture> stencil;
                if(fused.configured&&fused_packed_atlas_resolve_->Resolve(
                        edram_memory_->storage_buffer(),resolve_destination,dst,stencil)) {
                    fused_resolve=true;return stencil;
                }
                ++fused.resource_refusals;
            }
        }
        // Read-only complete canonical decode. No new cached receipt and no
        // promotion of the old D32 token: later same-token generic reads decode
        // identical bytes; changed-token reads refresh in both implementations.
        return edram_memory_->LoadSurfaceShared(storage);
    }
    std::shared_ptr<RenderTexture> DirectPackedColorResolveSource(const Json& o,const Json& source,
            const TextureLayout& destination,const RenderRect& src,const RenderRect& dst) {
        const auto storage=SurfaceStorage(source);
        NativeDirectPackedColorResolveInput input;
        input.native_fsi=config_.native_fsi;input.source_depth=storage.depth;
        input.source_width=storage.width;input.source_height=storage.height;input.source_pitch=storage.pitch;
        input.source_base=storage.base;input.source_format=storage.format;input.source_surface_info=source.at("surface_info");
        input.destination_width=destination.width;input.destination_height=destination.height;
        input.destination_format=destination.format;input.destination_dimension=destination.dimension;
        input.destination_layers=destination.layers;input.destination_endian=destination.endian;
        input.destination_level_count=uint32_t(destination.levels.size());
        input.destination_min_mip=destination.min_mip;input.destination_max_mip=destination.max_mip;
        input.destination_block_width=destination.block.block_width;
        input.destination_block_height=destination.block.block_height;input.destination_block_bytes=destination.block.bytes;
        input.destination_tiled=destination.tiled;
        if(destination.levels.size()==1)input.destination_pitch=destination.levels.front().pitch_blocks;
        input.flags=o.at("flags");input.level=o.at("level");input.source_slot=o.at("source_slot");
        input.exponent_bias=o.at("exponent_bias");input.destination_swap=ResolveDestinationSwap(destination.fetch,input.source_slot);
        input.source_rectangle={src.left,src.top,src.right,src.bottom};
        input.destination_rectangle={dst.left,dst.top,dst.right,dst.bottom};
        if(!NativeDirectPackedColorResolveAllowed(input))return {};
        auto& stats=direct_packed_color_resolve_stats_;++stats.candidates;
        if(queries_->active()){++stats.query_refusals;return {};}
        MaterializeRetainedOpaque();
        const auto key=SurfaceKey(source);const auto tiles=SurfaceTiles(storage);
        frame_surfaces_.insert(key);
        // Preserve Surface()'s ordering and only its dirty overlapping commits.
        for(auto& [other,image]:surfaces_)if(other!=key&&image.dirty&&(image.tiles&tiles).any()) {
            const auto previous=SurfaceStorage(image.identity);
            edram_memory_->StoreSurface(*image.texture,previous);
            image.version=edram_memory_->SurfaceVersion(previous);image.dirty=false;
        }
        const auto version=edram_memory_->SurfaceVersion(storage);
        const auto found=surfaces_.find(key);
        const auto selected=NativeDirectPackedColorResolveChooseSource(
            edram_memory_->SurfaceFullyDefined(storage),found!=surfaces_.end()&&bool(found->second.texture),
            found!=surfaces_.end()&&found->second.dirty,found!=surfaces_.end()?found->second.version:0,
            version,found!=surfaces_.end()&&bool(found->second.atlas_clear_receipt.source));
        if(selected==NativeDirectPackedColorResolveSource::CurrentHost) {
            ++stats.current_host_refusals;return {};
        }
        if(selected!=NativeDirectPackedColorResolveSource::Packed) {
            ++stats.provenance_refusals;return {};
        }
        // Same canonical full decode and packet-owned immutable image as the
        // first half of Surface(). Retain no receipt and leave any host token
        // OLD so future generic consumers take their unchanged full restore.
        return edram_memory_->LoadSurfaceShared(storage);
    }
    void DiscardCanonicalResolveReceipt(uint32_t address) {
        const auto found=canonical_resolve_receipts_.find(address);
        if(found==canonical_resolve_receipts_.end())return;
        canonical_resolve_stats_.receipt_bytes-=found->second.bytes;
        ++canonical_resolve_stats_.discarded;
        canonical_resolve_receipts_.erase(found);
    }
    bool StoreCanonicalResolve(RenderTexture& image,const TextureLayout& layout,
                               const RenderRect& rectangle,uint32_t address) {
        if(!canonical_resolve_stats_.configured)return false;
        // A receipt is speculative until a matching sampled view consumes it.
        // It never replaces the authoritative physical memory or working image.
        DiscardCanonicalResolveReceipt(address);
        const uint64_t estimate=uint64_t(layout.width)*layout.height*16;
        auto& stats=canonical_resolve_stats_;
        if(estimate>kCanonicalResolveReceiptBytes-stats.receipt_bytes||
           canonical_resolve_receipts_.size()>=kCanonicalResolveReceiptCount) {
            ++stats.budget_refusals;return false;
        }
        auto receipt=resolve_memory_->TryStoreCanonical(image,layout,rectangle,address);
        if(!receipt)return false; // Refused before any store was accepted.
        ++stats.stores;
        const uint64_t bytes=static_cast<const VulkanTexture&>(*receipt->image).allocationInfo.size;
        // Physical Store has already succeeded. An unexpectedly large driver
        // allocation may discard only the optional image; never repeat Store.
        if(bytes>kCanonicalResolveReceiptBytes-stats.receipt_bytes) {
            ++stats.budget_refusals;++stats.discarded;return true;
        }
        canonical_resolve_receipts_.emplace(address,CanonicalResolveReceipt{std::move(*receipt),bytes});
        stats.receipt_bytes+=bytes;
        stats.peak_receipt_bytes=std::max(stats.peak_receipt_bytes,stats.receipt_bytes);
        return true;
    }
    std::shared_ptr<RenderTexture> TakeCanonicalResolve(const TextureLayout& layout,uint32_t address) {
        const auto found=canonical_resolve_receipts_.find(address);
        if(found==canonical_resolve_receipts_.end()||
           !resolve_memory_->CanonicalStoreReceiptMatches(found->second.receipt,layout,address))return {};
        auto image=std::move(found->second.receipt.image);
        canonical_resolve_stats_.receipt_bytes-=found->second.bytes;
        ++canonical_resolve_stats_.adopted;
        canonical_resolve_receipts_.erase(found);
        return image;
    }
    void Resolve(const Json& o) {
        if(transfer_batch_&&!transfer_batch_->in_scope()) {
            NativeGpuWorkTag tag;
            if(config_.gpu_batch_attribution||config_.gpu_physical_timestamps){tag.kind=NativeGpuWorkKind::Resolve;try{tag.event=o.at("event");tag.frame=o.at("frame");}catch(...){}}
            RunTransferScope([&]{Resolve(o);},o.at("event"),(config_.gpu_batch_attribution||config_.gpu_physical_timestamps)?&tag:nullptr);return;
        }
        if(o.at("level")!=0||(o.at("flags").get<unsigned>()&0x2300))throw std::runtime_error("Unsupported resolve flags/mip");
        const auto& identity=o.at("destination");const auto key=TextureKey(identity); const std::string base=identity.at("base");
        frame_resolved_.insert(key);
        const auto dst=Rectangle(o.at("destination_rectangle"));
        const bool full=dst.left==0&&dst.top==0&&dst.right==identity.at("width")&&dst.bottom==identity.at("height");
        const uint32_t address=std::stoul(base,nullptr,16);
        auto fetch=o.at("destination_fetch").get<std::array<uint32_t,6>>();
        fetch[1]=(fetch[1]&0xfff)|address;
        auto layout=DescribeTexture(fetch);
        if(layout.format==2)for(unsigned channel=0;channel<4;++channel)
            if(((fetch[0]>>(2+channel*2))&3)==1)
                throw std::runtime_error("Signed 8-bit resolve conversion is not supported");
        // The observed R8 copy uses native byte order. The reference's 8-bit
        // resolve ignores its endian bits, so do not infer the multi-byte
        // formats' byte-swap behavior for an unverified R8 state.
        if(layout.format==2&&layout.endian)
            throw std::runtime_error("Nonzero 8-bit resolve endianness is unverified");
        // An atlas may legitimately write only the rectangles sampled later.
        // Retain unwritten pixels, but record the unproven input dependency.
        // They are never replaced with CPU snapshots or a fabricated clear.
        if(!full&&!resolve_memory_->FullyDefined(layout,address))report["uninitialized_resolve_regions"].push_back({{"event",o.at("event")},{"identity",identity},{"written_rectangle",o.at("destination_rectangle")}});
        auto& dest=resolved_[key];
        if(!dest.texture) {
            dest.texture=device_->createTexture(RenderTextureDesc::ColorTarget(layout.width,layout.height,NativeTextureFormat(layout)));
            dest.identity=identity;
        }
        if(!full&&dest.version!=resolve_memory_->Version(layout,address)&&resolve_memory_->AnyDefined(layout,address)) {
            auto restored=resolve_memory_->LoadShared(layout,address);
            const RenderRect whole(0,0,layout.width,layout.height);
            transfers_->Resolve(*restored,*dest.texture,whole,whole);
            if(detailed_reports_)report["texture_alias_restores"].push_back({{"event",o.at("event")},{"identity",identity}});
        }
        const unsigned slot=o.at("source_slot");const auto& source=o.at("surfaces").at(std::to_string(6+slot));
        auto conversion=PassTransfer::Conversion::Color;
        if(identity.at("format")==2)conversion=PassTransfer::Conversion::UNorm8;
        if(identity.at("format")==7)conversion=PassTransfer::Conversion::UNorm10;
        if(slot==4)conversion=source.at("format")==0?PassTransfer::Conversion::DepthUNorm24:PassTransfer::Conversion::DepthFloat24;
        const auto src=Rectangle(o.at("source_rectangle"));
        if(config_.fused_packed_atlas_resolves&&o.contains("copy_dest_swap")&&
           o.at("copy_dest_swap")!=ResolveDestinationSwap(fetch,slot))
            throw std::runtime_error("Resolve destination swap disagrees with captured guest resource");
        std::shared_ptr<RenderTexture> quantized_source,atlas_rgba_source,packed_color_source;
        auto atlas_rgba_kind=NativeDirectAtlasDepthResolveSource::Refuse;
        bool fused_atlas_resolve=false;
        const unsigned source_format=source.at("format");
        const bool fixed_source=slot!=4&&(source_format==4||source_format==5);
        const bool direct_fixed_source=config_.native_fsi&&fixed_source;
        RenderTexture* src_image=nullptr;
        bool selected_atlas_region=false;
        if(direct_fixed_source) {
            // FSI raster writes already live in packed EDRAM. The fixed16
            // resolve below reads that storage directly, so restoring an
            // unused host attachment would add a load and fullscreen transfer.
            // Standalone clear->resolve may still have dirty host clear images;
            // commit them, including aliases, before loading the packed source.
            FlushCommittedState();
        } else {
            if(config_.direct_atlas_depth_resolves&&slot==4)
                atlas_rgba_source=DirectAtlasDepthResolveSource(o,source,layout,src,dst,atlas_rgba_kind,*dest.texture,fused_atlas_resolve);
            if(config_.direct_packed_color_resolves&&slot==0)
                packed_color_source=DirectPackedColorResolveSource(o,source,layout,src,dst);
            bool source_region=false;
            if(!atlas_rgba_source&&!packed_color_source&&config_.atlas_depth_region_restores) {
                ++atlas_depth_region_stats_.requests;
                if(config_.native_fsi&&slot==4&&!queries_->active()) {
                    const auto storage=SurfaceStorage(source);
                    NativeAtlasDepthRegionInput input;
                    input.source_width=storage.width;input.source_height=storage.height;input.source_pitch=storage.pitch;
                    input.source_base=storage.base;input.source_format=storage.format;input.source_depth=storage.depth;
                    input.source_surface_info=source.at("surface_info");
                    input.destination_width=layout.width;input.destination_height=layout.height;
                    input.destination_format=layout.format;input.destination_dimension=layout.dimension;
                    input.destination_layers=layout.layers;input.destination_endian=layout.endian;
                    input.destination_level_count=uint32_t(layout.levels.size());
                    input.destination_min_mip=layout.min_mip;input.destination_max_mip=layout.max_mip;
                    input.destination_block_width=layout.block.block_width;input.destination_block_height=layout.block.block_height;
                    input.destination_block_bytes=layout.block.bytes;input.destination_tiled=layout.tiled;
                    if(layout.levels.size()==1)input.destination_pitch=layout.levels.front().pitch_blocks;
                    input.flags=o.at("flags");input.level=o.at("level");input.source_slot=slot;
                    input.exponent_bias=o.at("exponent_bias");input.destination_swap=ResolveDestinationSwap(fetch,slot);
                    input.source_rectangle={src.left,src.top,src.right,src.bottom};
                    input.destination_rectangle={dst.left,dst.top,dst.right,dst.bottom};
                    source_region=NativeAtlasDepthRegionAllowed(input)&&edram_memory_->SurfaceFullyDefined(storage);
                }
                if(!source_region)++atlas_depth_region_stats_.refusals;
            }
            src_image=packed_color_source?packed_color_source.get():atlas_rgba_source?atlas_rgba_source.get():
                &Surface(source,false,nullptr,source_region?&src:nullptr);
            selected_atlas_region=source_region;
        }
        if(fixed_source) {
            // Float working attachments may contain values outside the guest
            // fixed16 range. Resolve must read the encoded EDRAM values before
            // applying its exponent, including signed alpha used by DOF.
            // This does not replace per-fragment destination quantization.
            const auto storage=SurfaceStorage(source);
            if(!direct_fixed_source&&surfaces_.at(SurfaceKey(source)).dirty)
                edram_memory_->StoreSurface(*src_image,storage);
            quantized_source=edram_memory_->LoadSurfaceShared(storage);
            if(detailed_reports_)report["fixed_source_resolves"].push_back(o.at("event"));
            if(direct_fixed_source) {
                transfer_report_dirty_=true;
                diagnostic_counters_.direct_fixed_source_resolves=diagnostic_counters_.direct_fixed_source_resolves.value_or(0)+1;
            }
        }
        const bool destination_swap=ResolveDestinationSwap(fetch,slot);
        if(o.contains("copy_dest_swap")&&o.at("copy_dest_swap")!=destination_swap)
            throw std::runtime_error("Resolve destination swap disagrees with captured guest resource");
        if(!fused_atlas_resolve)transfers_->Resolve(quantized_source?*quantized_source:*src_image,*dest.texture,src,dst,o.at("exponent_bias"),conversion,
            destination_swap?0x60a:0x688);
        if(destination_swap&&detailed_reports_)report["resolve_destination_swaps"].push_back(o.at("event"));
        if(atlas_rgba_source) {
            // Resolve has already accepted the destination write. A failed
            // physical store is fatal, never an optional fallback or omission.
            if(!resolve_memory_->StoreDepthFromRgbaStencil(*dest.texture,layout,dst,address,
                    *atlas_rgba_source,src.left-dst.left,src.top-dst.top)) {
                batch_failed_=true;
                throw NativeBatchCompletionError("Direct atlas resolve accepted no physical depth/stencil store");
            }
            auto& stats=direct_atlas_depth_resolve_stats_;++stats.selected;
            if(atlas_rgba_kind==NativeDirectAtlasDepthResolveSource::Receipt)++stats.receipt_sources;
            else {++stats.packed_sources;if(!fused_atlas_resolve)stats.packed_decode_pixels+=kNativeDirectAtlasDepthResolvePixels;}
            if(fused_atlas_resolve) {
                auto& fused=fused_packed_atlas_resolve_stats_;++fused.selected;
                fused.source_pixels+=kNativeDirectAtlasDepthResolvePixels;
                fused.resolve_pixels+=uint64_t(dst.right-dst.left)*uint64_t(dst.bottom-dst.top);
                if(detailed_reports_)report["fused_packed_atlas_resolve_events"].push_back(o.at("event"));
            }
            stats.source_pixels+=kNativeDirectAtlasDepthResolvePixels;
            stats.resolve_pixels+=uint64_t(src.right-src.left)*uint64_t(src.bottom-src.top);
        } else if(slot==4)resolve_memory_->StoreDepth(*dest.texture,layout,dst,address,*src_image,src.left-dst.left,src.top-dst.top);
        else if(!StoreCanonicalResolve(*dest.texture,layout,dst,address))
            resolve_memory_->Store(*dest.texture,layout,dst,address);
        dest.version=resolve_memory_->Version(layout,address);
        if(packed_color_source) {
            auto& stats=direct_packed_color_resolve_stats_;++stats.selected;++stats.packed_sources;
            stats.source_pixels+=kNativeDirectPackedColorResolvePixels;
            stats.resolve_pixels+=uint64_t(src.right-src.left)*uint64_t(src.bottom-src.top);
            if(detailed_reports_)report["direct_packed_color_resolve_events"].push_back(o.at("event"));
        }
        if(selected_atlas_region)++atlas_depth_region_stats_.selected;
        transfer_report_dirty_=true;++diagnostic_counters_.resolves;
        if(!output_.empty())Preview(*dest.texture,"resolve-"+std::to_string(o.at("event").get<unsigned>()),slot==4);
    }
    // Async shaders: checked before any state, slot or command work, so a
    // skipped draw has no side effects (like a culled draw; an active query
    // simply counts no samples for it).
    bool DrawShadersReady(const DrawInput& o) {
        bool ready=true;
        for(const bool pixel:{false,true}) {
            const auto input=o.Shader(pixel);
            if(input.IsNull()||!input.typed||!input.typed->owner)continue;
            const bool fsi=pixel&&config_.native_fsi;
            if(shaders_.find(ShaderCacheLookup{input.Digest(),fsi,ShaderVariant(fsi,false)})!=shaders_.end())continue;
            // Evaluate both stages so both compilations start together.
            ready&=config_.shader_ready(*input.typed,fsi);
        }
        return ready;
    }
    void Draw(const DrawInput& o) {
        // The guest render thread wrote this draw's device-state copy on
        // another core moments ago. Start pulling the register block that
        // Draw decodes (booleans/loops through the draw-state words) before
        // shader/state preparation instead of stalling on it below. A pure
        // hint: it performs no architectural read and changes no state.
        if(o.typed&&o.typed->lazy_device_state) {
            if(const auto& bytes=o.typed->lazy_device_state->Owner();bytes&&bytes->size()>=kDrawStateOffset+kDrawStateWords*4) {
                const auto* base=bytes->data();
                for(size_t offset=0x2780;offset<kDrawStateOffset+kDrawStateWords*4;offset+=64)__builtin_prefetch(base+offset);
            }
        }
        if(config_.shader_ready&&!DrawShadersReady(o)) {++async_shader_skipped_draws_;return;}
        std::optional<RetainedPhaseTimingContext> timing_context;
        if(RetainedPhaseGpuTimingEnabled())timing_context.emplace(*this,o);
        struct EarlyDepthFailureGuard {
            State& owner;int exceptions=owner.config_.native_early_depth_material_retention?std::uncaught_exceptions():0;
            ~EarlyDepthFailureGuard() noexcept {
                if(owner.config_.native_early_depth_material_retention&&std::uncaught_exceptions()>exceptions)
                    owner.InvalidateEarlyDepth();
            }
        } early_depth_failure_guard{*this};
        if(config_.diagnose_texture_eviction_reloads)try {SetTextureCensusFrame(o.Frame());}
        catch(...) {texture_census_frame_valid_=false;texture_eviction_reload_census_->MetadataFailure();}
        // No DrawFacts payload construction on the normal phase-OFF path.
        // No state hashing/materialization occurs merely to create the scope.
        std::optional<NativeRetainedOpaquePhaseDrawFacts> phase_facts;
        NativeRetainedOpaquePhaseDrawFacts* phase_facts_ptr=nullptr;
        if(config_.retained_opaque_phase&&o.typed) {
            if(const auto* owner=RetainedOpaquePhaseStateOwner(o))
                phase_facts_ptr=&phase_facts.emplace(o.typed,owner);
        }
        PrepareCommittedStateForDraw(o,phase_facts_ptr);
        if(batch_failed_||draw_group_recording_failed_)
            throw NativeBatchCompletionError("Native draw after failed accepted work");
        if(gpu_work_queue_)gpu_work_queue_->Reserve();
        const auto draw_start=time_cpu_stages_?DrawClock::now():DrawClock::time_point{};
        const auto wait_before=draw_timing_.submit_wait;
        const uint32_t slot_count=config_.async_gpu_submissions?submission_capacity_.lists:config_.max_draw_batch_size;
        if(pending_draws_.size()>=slot_count)RelieveDrawPressure();
        auto& draw_slot=draw_slots_[next_draw_slot_];
        next_draw_slot_=(next_draw_slot_+1)%slot_count;
        // O(1) form of std::find(pending_draws_, &draw_slot) != end.
        if(draw_slot.pending_occurrences)
            throw NativeGpuWorkError("Native draw slot reused before its completion fence");
        if(next_draw_token_==UINT64_MAX)throw NativeGpuWorkError("Native draw ownership generation exhausted");
        draw_slot.token=++next_draw_token_;draw_slot.queried=false;
        if(!DrawGroupsEnabled()&&!draw_slot.commands)draw_slot.commands=queue_->createCommandList();
        auto* commands=DrawGroupsEnabled()?nullptr:draw_slot.commands.get();
        const auto raw_payload=ViewPayload(o.State());
        const auto raw=raw_payload.bytes();
        std::array<uint32_t,kDrawStateWords> words;
        // One extent check (BE's exact exception) instead of one per word, so
        // the byte-swap loop is a straight, vectorizable copy.
        if(kDrawStateOffset+words.size()*4>raw.size())throw std::runtime_error("Truncated captured word");
        for(unsigned i=0;i<words.size();++i)words[i]=std::byteswap(LoadUnalignedU32(raw.data()+kDrawStateOffset+i*4));
        const auto args=o.Args();
        const auto& function=o.Function();const bool up=function.starts_with("Begin");
        unsigned count=function=="BeginIndexedVertices"?args[4]:up?args[2]:args[3],primitive=args[1];
        const bool rectangle=o.Rectangle();
        if(!count||count>4*1024*1024)throw std::runtime_error("Invalid draw count");
        if(primitive!=4&&primitive!=6&&primitive!=13&&!(primitive==8&&rectangle))throw std::runtime_error("Unsupported scene primitive "+std::to_string(primitive));
        struct DrawAttachment {DrawSurfaceInput identity;std::optional<SurfaceLayout> storage;};
        std::array<DrawAttachment,5> attachments;
        const auto surfaces=o.Surfaces();
        for(size_t slot=0;slot<attachments.size();++slot)attachments[slot].identity=surfaces[slot];
        const auto storage=[&](size_t slot)->const SurfaceLayout& {
            auto& attachment=attachments[slot];
            if(!attachment.identity)throw std::runtime_error("Missing native draw attachment");
            if(!attachment.storage)attachment.storage=SurfaceStorage(attachment.identity);
            return *attachment.storage;
        };
        const bool pixel_shader_present=!o.Shader(true).IsNull();
        const bool color=pixel_shader_present&&attachments[0].identity&&(BE(raw,0x28dc)&15);
        for(unsigned slot=1;slot<4;++slot)if(!config_.native_fsi&&attachments[slot].identity&&(BE(raw,0x28dc)&(15u<<(slot*4))))
            throw std::runtime_error("Multiple color attachments require native scene support");
        if(!color&&!attachments[4].identity)throw std::runtime_error("Draw has no native attachment");
        const size_t extent_slot=color?0:4;
        const auto& extent=attachments[extent_slot].identity;
        const uint32_t extent_width=extent.Width(),extent_height=extent.Height();
        auto decoded_words=words;
        if(config_.native_fsi) {
            decoded_words[(0x2934-kDrawStateOffset)/4]=0;
            decoded_words[(0x2948-kDrawStateOffset)/4]&=~(7u<<11);
        }
        auto state=DecodeDrawState(decoded_words,extent_width,extent_height,rectangle?8:4,config_.native_fsi);
        NativeFsiShaderConstants fsi_shared;auto& shared=fsi_shared.shared;
        shared.half_pixel=state.half_pixel;shared.alpha_threshold=state.alpha_reference;
        if(config_.native_fsi) {
            auto& raster=fsi_shared.raster;
            raster.blend_constant=state.blend_constant;
            raster.pitch=storage(extent_slot).pitch;
            raster.stencil_front=BE(raw,0x2900);raster.stencil_back=BE(raw,0x28fc);
            const uint32_t mode=BE(raw,0x2948);
            if(mode&(1u<<11)) {raster.front_slope_bias=std::bit_cast<float>(BE(raw,0x2a50))/16.f;raster.front_constant_bias=std::bit_cast<float>(BE(raw,0x2a54));}
            if(mode&(1u<<12)) {raster.back_slope_bias=std::bit_cast<float>(BE(raw,0x2a58))/16.f;raster.back_constant_bias=std::bit_cast<float>(BE(raw,0x2a5c));}
            if(!std::isfinite(raster.front_slope_bias)||!std::isfinite(raster.front_constant_bias)||!std::isfinite(raster.back_slope_bias)||!std::isfinite(raster.back_constant_bias))
                throw std::runtime_error("Non-finite native FSI polygon offset");
            if(attachments[4].identity) {
                const auto& d=storage(4);
                if(d.pitch!=raster.pitch)throw std::runtime_error("Native FSI attachment pitches differ");
                raster.depth_base=d.base;raster.depth_format=d.format;raster.depth_control=BE(raw,0x2934);
            }
            constexpr uint32_t blend_offsets[]{0x2938,0x2958,0x295c,0x2960};
            for(unsigned slot=0;slot<4;++slot)if(attachments[slot].identity) {
                const auto& layout=storage(slot);
                if(layout.pitch!=raster.pitch)throw std::runtime_error("Native FSI attachment pitches differ");
                auto format=layout.format==10?2u:layout.format==12?3u:layout.format;
                if(format!=0&&format!=2&&format!=3&&format!=4&&format!=5&&format!=6&&format!=7&&format!=14&&format!=15)
                    throw std::runtime_error("Native FSI color format is unsupported");
                raster.color[slot]={layout.base,format,pixel_shader_present?(BE(raw,0x28dc)>>(slot*4))&15u:0u,BE(raw,blend_offsets[slot])};
                if(state.pipeline.renderTargetBlend[slot].blendEnabled)raster.blend_enabled|=1u<<slot;
            }
            if(config_.exact_fsi_stores&&pixel_shader_present&&
               !(attachments[extent_slot].identity.Info()&0x30000u)&&
               NativeFsiExactDepthStoreNoAliases(raster,extent_width,extent_height))
                raster.reserved=kNativeFsiExactDepthIdentityFlag;
            // Clears and host working images may still own newer bytes. Commit
            // them before the native fragment shader reads packed destinations.
            if(surfaces_maybe_dirty_) {
                for(auto& [key,image]:surfaces_)if(image.dirty) {
                    FlushPendingDraws();
                    const auto layout=SurfaceStorage(image.identity);edram_memory_->StoreSurface(*image.texture,layout);
                    image.version=edram_memory_->SurfaceVersion(layout);image.dirty=false;
                }
                surfaces_maybe_dirty_=false;
            }
        }
        for(unsigned slot=0;slot<4;++slot)if(attachments[slot].identity) {
            const auto info=attachments[slot].identity.FormatInfo();
            shared.color_scale[slot]=std::ldexp(1.f,int32_t(((info>>20)&63)^32)-32);
        }
        if(0x27a0+32*4>raw.size())throw std::runtime_error("Truncated captured word");
        for(unsigned i=0;i<8;++i)shared.booleans[i]=std::byteswap(LoadUnalignedU32(raw.data()+0x2780+i*4));
        for(unsigned i=0;i<32;++i)shared.loops[i]=std::byteswap(LoadUnalignedU32(raw.data()+0x27a0+i*4));
        std::array<DrawGeometryInput,98> geometry{};
        std::array<DrawGeometryInput,96> streams{};
        bool packed_pass_cpu_geometry=true;
        o.Geometry(up,[&](const DrawGeometryInput& g) {
            if((config_.packed_depth_render_pass_groups||config_.packed_color_render_pass_groups||config_.readonly_query_render_pass_groups||config_.native_early_depth_material_retention||config_.fsi_draw_pass_groups)&&g.GpuMemory())packed_pass_cpu_geometry=false;
            const auto kind=g.Kind();
            if(kind<geometry.size())geometry[kind]=g;
            if(const auto slot=g.FetchSlot())if(*slot<streams.size())streams[*slot]=g;
        });
        auto geometry_block=[&](unsigned kind)->const DrawGeometryInput& {
            const auto& block=geometry.at(kind);
            if(!block)throw std::out_of_range("Missing native geometry block");
            return block;
        };
        auto stream_block=[&](unsigned slot)->const DrawGeometryInput& {
            const auto& block=streams.at(slot);
            if(!block)throw std::out_of_range("Missing native vertex stream");
            return block;
        };
        RenderBuffer* rectangle_vertices=nullptr;
        if(rectangle) {
            if(function!="BeginVertices"||primitive!=8||count!=3||args[3]!=16||!o.Shader(false).Matches("9d8adccbaa369d9fa2106a03a54a997ec417e12fcbdb4bed7e67c36dfadf8a01"))
                throw std::runtime_error("Unsupported rectangle vertex program/layout");
            const auto payload=ReadPayload(geometry_block(96).Payload());
            const auto bytes=payload.bytes();if(bytes.size()!=48)throw std::runtime_error("Invalid rectangle payload");
            std::array<float,16> positions;
            for(unsigned i=0;i<12;++i){positions[i]=std::bit_cast<float>(BE(bytes,i*4));if(!std::isfinite(positions[i]))throw std::runtime_error("Invalid rectangle vertex");}
            for(unsigned i=0;i<4;++i)positions[12+i]=positions[4+i]+positions[8+i]-positions[i];
            rectangle_vertices=UploadDraw(draw_slot.rectangle,{reinterpret_cast<const uint8_t*>(positions.data()),sizeof(positions)},RenderBufferFlag::VERTEX);
        }
        o.VertexSlots([&](unsigned slot) {
            if(slot>=96||(up&&slot!=95))throw std::runtime_error("Unsupported vertex fetch slot");
            const auto& g=up?geometry_block(96):stream_block(slot);
            const bool gpu_memory=g.GpuMemory();
            std::vector<uint8_t> native_bytes;
            if(gpu_memory)native_bytes=ReadPhysical(g.Address(),g.Bytes());
            const auto native_digest=gpu_memory?picosha2::hash256_hex_string(native_bytes):std::string{};
            const std::string_view key=gpu_memory?std::string_view(native_digest):g.Payload().Digest();
            auto found_vertex=vertex_buffers_.find(key);
            if(found_vertex==vertex_buffers_.end())found_vertex=vertex_buffers_.emplace(
                std::piecewise_construct,std::forward_as_tuple(key),std::forward_as_tuple()).first;
            auto& entry=found_vertex->second;
            vertex_cache_.Access(entry.residency,bool(entry.buffer));
            auto& b=entry.buffer;
            if(!b) {
                if(gpu_memory)b=UploadVertex(native_bytes);
                else {
                    const auto payload=ReadPayload(g.Payload());
                    b=UploadVertex(payload.bytes());
                }
                vertex_cache_.Created(entry.residency,static_cast<const VulkanBuffer&>(*b).allocationInfo.size);
            }
            shared.vertex_streams[slot]={b->getDeviceAddress(),g.Bytes(),up?2:g.Endian()};
        });
        IndexGeometryParameters index_parameters;
        index_parameters.count=count;index_parameters.primitive=primitive;
        index_parameters.up_padding=up;index_parameters.rectangle=rectangle;
        index_parameters.strip_restart=bool(BE(raw,0x2948)&(1u<<21));
        std::optional<OwnedPayload> index_payload;
        // Operation-owned CPU index bytes are borrowed; see ViewPayload.
        const OwnedPayload::Snapshot* index_borrowed=nullptr;
        std::string_view index_digest;
        if(function=="DrawPrimitive"||function=="BeginVertices")
            index_parameters.first_index=up?0u:args[2];
        else {
            if(up) {
                if(args[2])throw std::runtime_error("Unsupported UP minimum index");
                index_parameters.minimum_index=args[2];
                index_parameters.width=args[5]&4?4:2;
                index_parameters.endian=index_parameters.width==4?2:1;
            } else {
                const auto header=ViewPayload(geometry_block(30).Payload());const auto flags=BE(header.bytes(),0);
                index_parameters.width=flags>>31?4:2;index_parameters.endian=(flags>>29)&3;
            }
            const auto& g=geometry_block(up?97:31);
            if(g.GpuMemory()) {
                index_parameters.source=IndexGeometrySource::GpuMemory;
                index_payload.emplace(ReadPhysical(g.Address(),g.Bytes()));
            } else {
                index_parameters.source=IndexGeometrySource::CpuPayload;
                index_digest=g.Payload().Digest();
                // Read on cache hits too: absent or malformed provider input
                // remains observable, and a different owner compares bytes.
                index_borrowed=BorrowedPayloadOwner(g.Payload());
                if(!index_borrowed)index_payload.emplace(ReadPayload(g.Payload()));
            }
        }
        const auto index_source=index_borrowed?std::span<const uint8_t>(**index_borrowed):
            index_payload?index_payload->bytes():std::span<const uint8_t>();
        const OwnedPayload::Snapshot no_index_owner;
        const auto& index_owner=index_borrowed?*index_borrowed:index_payload?index_payload->snapshot():no_index_owner;
        const IndexGeometryLookup index_key{index_parameters,index_digest};
        count=IndexGeometryCount(index_source,index_parameters);
        const size_t index_bytes=size_t(count)*4;
        size_t index_upload_bytes=0;
        auto* cached_indices=index_cache_.Find(index_key,index_source,index_owner);
        RenderBuffer* index_buffer=cached_indices?cached_indices->get():nullptr;
        if(!index_buffer) {
            const auto indices=BuildIndexGeometry(index_source,index_parameters);
            const std::span<const uint8_t> upload(reinterpret_cast<const uint8_t*>(indices.data()),index_bytes);
            if(index_cache_.CanAdmit(index_key,index_source.size(),index_bytes)) {
                // Only an admitted cache entry takes a reference to a borrowed owner.
                auto owner=index_borrowed?OwnedPayload::Snapshot(*index_borrowed):
                    index_payload?std::move(*index_payload).TakeSnapshot():OwnedPayload::Snapshot{};
                cached_indices=index_cache_.Insert(index_key,std::move(owner),index_bytes,
                    [&] {return Upload(upload,RenderBufferFlag::INDEX);});
                if(cached_indices)index_buffer=cached_indices->get();
            }
            if(!index_buffer) {
                index_buffer=UploadDraw(draw_slot.indices,upload,RenderBufferFlag::INDEX);
                index_upload_bytes=index_bytes;
            }
        }
        auto& descriptors=draw_slot.descriptors;
        const unsigned descriptor_count=config_.native_fsi?5:4;
        for(unsigned i=0;i<descriptor_count;++i)if(!descriptors[i]) {
            descriptors[i]=device_->createDescriptorSet(descriptor_descs_[i]);++draw_descriptor_allocations_;
            if(i<4&&config_.cache_draw_descriptors&&config_.batch_draw_descriptors) {
                draw_slot.descriptor_set_generations[i]=descriptor_generations_.Issue();
                if(!draw_slot.descriptor_set_generations[i])++descriptor_contents_.generation_refusals;
            }
            if(i==4)descriptors[i]->setBuffer(0,&edram_memory_->storage_buffer());
        }
        std::optional<NativeDrawDescriptorBatch> descriptor_batch;
        if(config_.batch_draw_descriptors)
            descriptor_batch.emplace(static_cast<VulkanDevice*>(device_)->vk,vkUpdateDescriptorSets);
        auto descriptor_target=[&](unsigned set) {
            const auto& descriptor=static_cast<const VulkanDescriptorSet&>(*descriptors[set]);
            const auto& mapping=*descriptor.setLayout;
            return NativeDescriptorTarget{descriptor.vk,mapping.setBindings,
                mapping.descriptorIndexBases,mapping.descriptorBindingIndices};
        };
        std::optional<NativeDescriptorContentTransaction> descriptor_content_transaction;
        if(config_.cache_draw_descriptors&&descriptor_batch) {
            // Cache only metadata. Refused metadata allocation leaves the same
            // ordinary Native descriptor writes and all image owners untouched.
            if(!draw_slot.descriptor_contents&&!draw_slot.descriptor_contents_allocation_refused) {
                try {
                    draw_slot.descriptor_contents=std::make_unique<NativeDescriptorContentCache>();
                    ++descriptor_contents_.caches;
                }catch(const std::bad_alloc&) {
                    draw_slot.descriptor_contents_allocation_refused=true;++descriptor_contents_.allocation_refusals;
                }
            }
            bool usable=bool(draw_slot.descriptor_contents);
            for(unsigned i=0;i<4;++i) {
                if(!draw_slot.descriptor_set_generations[i])usable=false;
                if(!descriptors[i]) {usable=false;++descriptor_contents_.invalid_handle_refusals;continue;}
                const auto& descriptor=static_cast<const VulkanDescriptorSet&>(*descriptors[i]);
                if(descriptor.vk==VK_NULL_HANDLE||!descriptor.setLayout) {usable=false;++descriptor_contents_.invalid_handle_refusals;}
            }
            if(usable) {
                for(unsigned i=0;i<4;++i)if(draw_slot.descriptor_contents->Set(i,
                    draw_slot.descriptor_set_generations[i],static_cast<const VulkanDescriptorSet&>(*descriptors[i]).vk))++descriptor_contents_.set_identity_resets;
                descriptor_content_transaction.emplace(*draw_slot.descriptor_contents,&descriptor_contents_.aborts);
                ++descriptor_contents_.draws;
            }
        }
        // These arrays use PARTIALLY_BOUND and every active slot is overwritten
        // below. Dormant descriptors may refer to retired views: Vulkan explicitly
        // permits invalid descriptors that are not dynamically used. The previous
        // use of this slot has completed, so updates cannot race a pending read.
        auto& transient_bindings=draw_slot.transient_bindings;
        const int32_t anisotropy=config_.anisotropy_override?config_.anisotropy_override():-1;
        transfer_report_dirty_=true;diagnostic_counters_.anisotropy=anisotropy;
        // A null JSON value owns no allocation. Build provenance only when
        // requested, retaining the existing publish-after-draw behavior.
        unsigned gpu_reads=0;bool packed_pass_texture_free=true;Json alias_reads;
        const auto texture_start=time_cpu_stages_?DrawClock::now():DrawClock::time_point{};
        const auto texture_wait_before=draw_timing_.submit_wait;
        uint32_t timing_frame=UINT32_MAX;
        if(draw_timestamps_)try{timing_frame=o.Frame();}catch(...){draw_timestamps_->RejectMetadata();}
        std::optional<NativeDrawTimestampScope> draw_timing_scope;
        std::array<RenderTexture*,32> deferred_readonly_barriers{};
        uint32_t deferred_readonly_count=0;
        auto BeginRecording=[&](RenderCommandList& list) {
            const auto start=config_.time_draw_recording?DrawClock::now():DrawClock::time_point{};
            if(config_.reset_draw_command_pool) {
                // The chosen leader is this retired slot's private command
                // buffer. Followers retain their own immutable resource slots.
                auto& vk_commands=static_cast<VulkanCommandList&>(list);
                if(vk_commands.queue!=queue_||vk_commands.activeRenderPass!=VK_NULL_HANDLE)
                    throw NativeGpuWorkError("Native retired draw pool has foreign queue or active render pass");
                BeginRetiredNativeDrawPool(static_cast<VulkanDevice*>(device_)->vk,vk_commands.commandPool,
                    vk_commands.vk,vkResetCommandPool,vkBeginCommandBuffer,&draw_recording_,config_.time_draw_recording);
            } else list.begin();
            if(config_.time_draw_recording)draw_recording_.begin_ms+=DrawMilliseconds(start,DrawClock::now());
            if(config_.time_draw_recording||config_.reset_draw_command_pool)++draw_recording_.begins;
        };
        auto BeginTiming=[&] {
            if(!draw_timing_scope)draw_timing_scope.emplace(draw_timestamps_.get(),
                NativeDrawTimestamps::Token{draw_timestamps_?uint32_t(&draw_slot-draw_slots_.data()):0,draw_slot.token},
                timing_frame,*commands);
        };
        auto EmitDeferredReadonly=[&] {
            for(uint32_t i=0;i<deferred_readonly_count;++i)
                commands->barriers(RenderBarrierStage::GRAPHICS,
                    RenderTextureBarrier(deferred_readonly_barriers[i],RenderTextureLayout::SHADER_READ));
            deferred_readonly_count=0;
        };
        auto BeginStandalone=[&] {
            if(commands)return;
            // Nothing for this preparing draw has been recorded. Prior groups
            // can now close before an upload/transition or later transfer wait.
            SealDrawCommandGroup();
            if(!draw_slot.commands)draw_slot.commands=queue_->createCommandList();
            commands=draw_slot.commands.get();BeginRecording(*commands);BeginTiming();
            ++group_preparation_standalone_;
            // Preserve order/duplicates if an earlier immutable read barrier
            // precedes the first upload or layout-changing barrier in this draw.
            EmitDeferredReadonly();
        };
        if(!DrawGroupsEnabled()) {BeginRecording(*commands);BeginTiming();}
        o.Textures([&](const DrawTextureInput& t) {
            packed_pass_texture_free=false;
            const unsigned slot=t.Slot();const auto fetch=t.Fetch();
            TextureLayout uncached_layout;
            const auto& layout=config_.cache_texture_layouts?
                texture_layout_cache_.Get(fetch,uncached_layout):
                (uncached_layout=DescribeTexture(fetch));
            RenderTexture* image=nullptr;
            TextureBindingCache* binding_cache=nullptr;
            TextureBinding* uploaded_binding=nullptr;
            bool decoded_gpu=false;
            const bool cpu_snapshot=t.SourceIs("cpu_snapshot");
            if(!cpu_snapshot)cpu_image_slots_.ResetSlot(slot,CpuImageSlots::ResetReason::NonCpuTexture);
            if(cpu_snapshot) {
                const auto version_start=time_cpu_stages_?DrawClock::now():DrawClock::time_point{};
                const auto version=resolve_memory_->CpuTextureVersion(layout);
                const CpuImageLookup key{CpuTextureStorageSamplingKey(fetch,config_.share_cpu_texture_sampling_variants),t.BasePayload().OptionalDigest(),
                    t.MipPayload().OptionalDigest(),version};
                auto found=cpu_image_slots_.Find(slot,key);
                if(found==textures_.end()) {
                    found=textures_.emplace(
                        CpuImageIdentity{key.fetch,OwnDigest(key.base),OwnDigest(key.mips),key.version},UploadedImage{}).first;
                    cpu_image_slots_.Remember(slot,found);
                }
                texture_cache_.Access(found->second.residency,bool(found->second.texture.GetImage()));
                auto& uploaded=found->second;auto& texture=uploaded.texture;
                binding_cache=&uploaded.bindings;
                if(config_.share_cpu_texture_sampling_variants) {
                    ++cpu_texture_sampling_.lookups;
                    if(texture.GetImage()) {
                        ++cpu_texture_sampling_.image_hits;
                        if(fetch!=uploaded.initial_fetch)++cpu_texture_sampling_.variant_image_hits;
                    }
                }
                // Include immutable image-key construction and cache lookup;
                // these costs occur on hits as well as on texture updates.
                if(time_cpu_stages_)texture_timing_.version+=DrawMilliseconds(version_start,DrawClock::now());
                if(!texture.GetImage()) {
                    std::optional<NativeTextureEvictionReloadCensus::Creation> census_creation;
                    if(config_.diagnose_texture_eviction_reloads&&texture_census_frame_valid_)
                        census_creation=texture_eviction_reload_census_->BeginCreation(key,texture_census_frame_,
                            {texture_timing_.payload,texture_timing_.overlay,texture_timing_.decode,texture_timing_.upload});
                    bool raw=false;
                    if(config_.raw_gpu_float_cpu_textures&&RawFloatCpuTextureLayout(layout)&&
                        !t.BasePayload().IsNull()&&t.MipPayload().IsNull()&&
                        !(texture_probe_&&key.base&&texture_probe_->Selected(*key.base))) {
                        // Preserve source presence/extent validation. Raw GPU
                        // ownership replaces contents, never malformed inputs.
                        const auto payload=ReadPayload(t.BasePayload());
                        if(!RawFloatCpuTexturePayload(payload.bytes().size()))
                            throw std::runtime_error("CPU texture payload misses sampled blocks");
                        if(resolve_memory_->CanLoadRawFloatCpuTexture(layout,payload.bytes().size())) {
                            NativeTexture next;next.raw_gpu_image=device_->createTexture(
                                RenderTextureDesc::Texture2D(4096,1,1,RenderFormat::R32G32B32A32_FLOAT));
                            if(!next.raw_gpu_image)throw std::runtime_error("Raw float CPU image allocation failed");
                            // Keep original floating-format sign/number/swizzle,
                            // sampler and exponent semantics. No decoded view.
                            next.binding=BindTexture(*device_,*next.raw_gpu_image,layout,false,
                                anisotropy,config_.max_sampler_anisotropy);
                            NativeGpuWorkTag tag;
                            if(config_.gpu_batch_attribution||config_.gpu_physical_timestamps){tag.kind=NativeGpuWorkKind::AliasLoad;try{tag.event=o.Event();tag.frame=o.Frame();}catch(...){}}
                            RunTransferScope([&]{resolve_memory_->LoadRawFloatCpuTexture(next.raw_gpu_image,layout);},
                                0,(config_.gpu_batch_attribution||config_.gpu_physical_timestamps)?&tag:nullptr);
                            texture=std::move(next);texture_cache_.Created(uploaded.residency,TextureAllocationBytes(texture));
                            ++texture_timing_.uploads;texture_timing_.overlay_bytes+=kRawFloatCpuTextureBytes;
                            if(census_creation)texture_eviction_reload_census_->EndCreation(*census_creation,uploaded.residency.allocation_bytes,
                                {texture_timing_.payload,texture_timing_.overlay,texture_timing_.decode,texture_timing_.upload});
                            raw=true;
                        }
                    }
                    if(!raw) {
                    NativeTextureEvictionReloadCensus::Clock::time_point census_flush_begin;
                    if(census_creation)census_flush_begin=NativeTextureEvictionReloadCensus::Clock::now();
                    if(!config_.defer_cpu_texture_drains)FlushPendingDraws();
                    if(census_creation)census_creation->flush_pending_ms+=NativeTextureEvictionReloadCensus::Milliseconds(
                        census_flush_begin,NativeTextureEvictionReloadCensus::Clock::now());
                    ++texture_timing_.uploads;
                    const auto payload_start=time_cpu_stages_?DrawClock::now():DrawClock::time_point{};
                    auto base=t.BasePayload().IsNull()?std::vector<uint8_t>{}:Payload(t.BasePayload());
                    auto mips=t.MipPayload().IsNull()?std::vector<uint8_t>{}:Payload(t.MipPayload());
                    if(time_cpu_stages_)texture_timing_.payload+=DrawMilliseconds(payload_start,DrawClock::now());
                    texture_timing_.upload_bytes+=base.size()+mips.size();
                    TextureOverlayStats overlay_stats;
                    const auto overlay_start=time_cpu_stages_?DrawClock::now():DrawClock::time_point{};
                    const bool pending_before_overlay=HasPendingGpuWork();
                    std::function<void()> before_readback;
                    if(config_.defer_cpu_texture_drains)before_readback=[this]{FlushPendingDraws();};
                    const auto overlaid=resolve_memory_->OverlayCpuTexture(layout,base,mips,&overlay_stats,before_readback);
                    if(config_.defer_cpu_texture_drains) {
                        ++cpu_texture_drains_.candidates;
                        if(overlaid)++cpu_texture_drains_.overlays;
                        else {++cpu_texture_drains_.pure_cpu;cpu_texture_drains_.pending_pure_cpu+=pending_before_overlay;}
                    }
                    if(time_cpu_stages_)texture_timing_.overlay+=DrawMilliseconds(overlay_start,DrawClock::now());
                    texture_timing_.overlay_bytes+=overlaid;
                    texture_timing_.overlay_regions+=overlay_stats.copy_regions;
                    texture_timing_.overlay_readback_bytes+=overlay_stats.readback_bytes;
                    RecordFullCpuOverlay(o,t,layout,version,base.size(),mips.size(),overlay_stats,anisotropy);
                    if(overlaid&&detailed_reports_)report["mixed_cpu_gpu_textures"].push_back({{"event",o.Event()},{"slot",slot},
                        {"gpu_bytes",overlaid},{"version",version}});
                    if(texture_probe_&&key.base&&texture_probe_->Selected(*key.base)) {
                        texture_probe_->Record(fetch,version,o.Frame(),o.Event(),slot,
                            *key.base,key.mips.value_or(std::string_view{}),overlaid,base,mips);
                        report["texture_probe"]=texture_probe_->Status();
                    }
                    const auto decode_start=time_cpu_stages_?DrawClock::now():DrawClock::time_point{};
                    auto decoded=DecodeTexture(fetch,base,mips);
                    const auto upload_start=time_cpu_stages_?DrawClock::now():DrawClock::time_point{};
                    if(time_cpu_stages_)texture_timing_.decode+=DrawMilliseconds(decode_start,upload_start);
                    if(DrawGroupsEnabled())BeginStandalone();
                    texture=UploadTexture(*device_,*commands,decoded,anisotropy,config_.max_sampler_anisotropy);
                    texture_cache_.Created(uploaded.residency,TextureAllocationBytes(texture));
                    if(time_cpu_stages_)texture_timing_.upload+=DrawMilliseconds(upload_start,DrawClock::now());
                    if(census_creation)texture_eviction_reload_census_->EndCreation(*census_creation,uploaded.residency.allocation_bytes,
                        {texture_timing_.payload,texture_timing_.overlay,texture_timing_.decode,texture_timing_.upload});
                    }
                    // The upload binding was made from this draw's full fetch.
                    // Never adopt a previous sampling variant on a storage hit.
                    if(config_.share_cpu_texture_sampling_variants) {
                        uploaded_binding=&texture.binding;
                        uploaded.initial_fetch=fetch;++cpu_texture_sampling_.image_creations;
                    }
                }
                image=texture.GetImage();
                if(!config_.share_cpu_texture_sampling_variants)uploaded_binding=&texture.binding;
            } else if(t.SourceIs("gpu_resolve")||t.SourceIs("gpu_alias_or_previous_frame")) {
                const auto identity=t.Identity();
                const uint32_t address=identity.Address();
                std::optional<NativeGpuAliasState> alias_state;
                if(config_.cache_gpu_alias_states)alias_state=resolve_memory_->InspectTextureState(layout,address);
                if(!(alias_state?alias_state->any_defined:resolve_memory_->AnyDefined(layout,address))) {
                    report["missing_gpu_input"]={{"event",o.Event()},{"slot",slot},{"identity",identity.ToJson()}};
                    if(!diagnostic_zero_history_)throw std::runtime_error("Missing prior-frame GPU texture at "+identity.BaseText()+", slot "+std::to_string(slot));
                    // Fault-injection test only: run later passes with an
                    // explicitly synthetic input. Never enabled by default or
                    // accepted as evidence of frame equivalence/completion.
                    FlushPendingDraws();
                    auto seed=device_->createTexture(RenderTextureDesc::ColorTarget(layout.width,layout.height,NativeTextureFormat(layout)));
                    const RenderRect r(0,0,layout.width,layout.height);
                    transfers_->Clear(seed.get(),nullptr,true,false,false,r,{},0);
                    resolve_memory_->Store(*seed,layout,r,address);
                    report["diagnostic_synthetic_history"].push_back(report["missing_gpu_input"]);
                    report["valid_for_equivalence"]=false;
                    // Diagnostic seeding changed physical ownership; never use
                    // the pre-Store missing-state snapshot for full/version.
                    if(alias_state)alias_state=resolve_memory_->InspectTextureState(layout,address);
                }
                if(!(alias_state?alias_state->fully_defined:resolve_memory_->FullyDefined(layout,address))) {
                    // An allocation with one initialized texel is not a valid
                    // sampled image. Without a proven sampling footprint the
                    // shader may access any texel, including untouched aliases.
                    // Stop before uploading an image from undefined GPU bytes.
                    report["reads_of_partial_images"].push_back({{"event",o.Event()},{"slot",slot},{"identity",identity.ToJson()}});
                    report["valid_for_equivalence"]=false;
                    throw std::runtime_error("Partially defined GPU texture at "+identity.BaseText()+", slot "+std::to_string(slot));
                }
                // Sampling signed bytes is a different interpretation from
                // writing a resolve destination. Never reuse its UNORM image
                // as a signed view simply because both address the same bytes.
                const auto cached=identity.FindOrCreate(sampled_alias_index_,(fetch[0]>>2)&255u);
                frame_sampled_.insert(cached->first);auto& alias=cached->second;
                const auto version=alias_state?alias_state->version:resolve_memory_->Version(layout,address);
                if(!alias.current||alias.current->version!=version) {
                    const bool queued_2d=gpu_work_queue_&&layout.dimension==1&&layout.layers==1;
                    const bool queued_volume=gpu_work_queue_&&resolve_memory_->CanQueueVolumeLoad(layout,address);
                    // Cache ownership may move while older draws retain exact
                    // immutable image+bindings. Unsupported conversion retains
                    // synchronous behavior. Stale resources keep their bound.
                    if(!queued_2d&&!queued_volume)FlushPendingDraws();
                    if(alias.current&&alias.current.use_count()>1&&
                       !alias_retention_->Fits(alias.current->bytes,alias.current->bindings.size(),config_.max_stale_alias_bytes)) {
                        ++alias_retention_->pressure_drains;FlushPendingDraws();
                        if(alias.current.use_count()>1&&
                           !alias_retention_->Fits(alias.current->bytes,alias.current->bindings.size(),config_.max_stale_alias_bytes))
                            throw std::runtime_error("Native alias budget cannot retire a preparing draw's texture");
                    }
                    const auto alias_start=time_cpu_stages_?DrawClock::now():DrawClock::time_point{};
                    auto next=std::make_shared<AliasGeneration>();
                    next->bytes=uint64_t(layout.width)*layout.height*layout.layers*16;
                    next->version=version;
                    if(config_.canonical_resolve_stores&&queued_2d)
                        next->texture=TakeCanonicalResolve(layout,address);
                    if(next->texture) {
                        // The producer packet owns the compute write. This
                        // generation and the draw below own the immutable image;
                        // the existing sampled-image barrier orders the read.
                    } else if(queued_2d||queued_volume) {
                        ++texture_timing_.alias_loads;
                        // The canonical decode reads already-owned physical
                        // bytes. Seal it before this draw; its SHADER_READ
                        // barrier below orders the compute result without a
                        // CPU wait. Optional volume assembly uses the same FIFO.
                        NativeGpuWorkTag tag;
                        if(config_.gpu_batch_attribution||config_.gpu_physical_timestamps){tag.kind=NativeGpuWorkKind::AliasLoad;try{tag.event=o.Event();tag.frame=o.Frame();}catch(...){}}
                        RunTransferScope([&]{next->texture=resolve_memory_->LoadShared(layout,address);},0,(config_.gpu_batch_attribution||config_.gpu_physical_timestamps)?&tag:nullptr);
                    } else {
                        ++texture_timing_.alias_loads;
                        next->texture=resolve_memory_->Load(layout,address);
                    }
                    // Loading can relieve FIFO pressure and retire old draws.
                    // Charge only owners still present after that exact fence.
                    if(alias.current) {
                        if(alias.current.use_count()>1)alias.current->RetainStale(alias_retention_);
                        cached_texture_bindings_-=alias.current->bindings.size();
                    }
                    alias.current=std::move(next);++alias_retention_->created;
                    if(time_cpu_stages_)texture_timing_.alias_load+=DrawMilliseconds(alias_start,DrawClock::now());
                    if(detailed_reports_)alias_reads.push_back({{"event",o.Event()},{"slot",slot},{"identity",identity.ToJson()}});
                } else ++alias_retention_->reused;
                draw_slot.KeepAlias(alias.current);
                binding_cache=&alias.current->bindings;
                image=alias.current->texture.get();decoded_gpu=true;++gpu_reads;
            } else throw std::runtime_error("Texture slot "+std::to_string(slot)+": "+t.SourceText());
            const auto binding_start=time_cpu_stages_?DrawClock::now():DrawClock::time_point{};
            const auto& binding=BindCachedTexture(*binding_cache,*image,layout,decoded_gpu,anisotropy,transient_bindings,uploaded_binding,
                cpu_snapshot&&config_.share_cpu_texture_sampling_variants);
            // Uploads and canonical alias conversions establish their own write
            // transitions. Sampled generations are immutable while any current
            // or pending draw owns them; pool reuse first records COMPUTE/GENERAL.
            // Exact GRAPHICS/SHADER_READ therefore has no new access/layout work.
            // Do not generalize to EDRAM, writable images or mixed stage masks.
            const auto& sampled_image=static_cast<const VulkanTexture&>(*image);
            if(!config_.skip_readonly_sample_barriers||NativeSampleBarrierRequired(
                    sampled_image.textureLayout,sampled_image.barrierStages)) {
                if(DrawGroupsEnabled()&&!commands&&
                   !NativeSampleBarrierRequired(sampled_image.textureLayout,sampled_image.barrierStages)) {
                    // Only the exact immutable GRAPHICS/SHADER_READ state may
                    // be deferred: recording this read/read barrier changes no
                    // tracking consulted by later conversion or readback work.
                    // Alias generations are pinned above; CPU cache images are
                    // protected for the current frame. Keep duplicates/order.
                    if(deferred_readonly_count==deferred_readonly_barriers.size())
                        throw std::runtime_error("Native draw exceeds deferred sampled-image barrier bound");
                    deferred_readonly_barriers[deferred_readonly_count++]=image;
                    ++group_readonly_deferred_;
                } else {
                    if(DrawGroupsEnabled())BeginStandalone();
                    commands->barriers(RenderBarrierStage::GRAPHICS,RenderTextureBarrier(image,RenderTextureLayout::SHADER_READ));
                }
                ++draw_barriers_.sampled_emitted;
            } else ++draw_barriers_.sampled_skipped;
            const unsigned image_set=binding.dimension==2?1:binding.dimension==3?2:0;
            if(descriptor_batch) {
                const auto view=binding.view?static_cast<const VulkanTextureView&>(*binding.view).vk:
                    sampled_image.imageView;
                bool image_write=true,sampler_write=true;
                if(descriptor_content_transaction) {
                    ++descriptor_contents_.compared;
                    const auto content=NativeDrawDescriptorContent(descriptor_target(image_set),slot,
                        binding.descriptor_generation,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,view);
                    if(view==VK_NULL_HANDLE) {
                        descriptor_content_transaction->ForceWrite(image_set,slot);++descriptor_contents_.invalid_handle_refusals;
                    } else image_write=descriptor_content_transaction->NeedsWrite(image_set,slot,content);
                    if(!image_write) {
                        ++descriptor_contents_.skipped_images;
                        ++descriptor_contents_.skipped_images_by_set[image_set];
                    }
                }
                if(image_write) {
                    descriptor_batch->Texture(descriptor_target(image_set),slot,view);
                    if(descriptor_content_transaction)++descriptor_contents_.image_write_requests;
                }
                if(binding.sampler) {
                    const auto sampler=static_cast<const VulkanSampler&>(*binding.sampler).vk;
                    if(descriptor_content_transaction) {
                        ++descriptor_contents_.compared;
                        const auto content=NativeDrawDescriptorContent(descriptor_target(3),slot,
                            binding.descriptor_generation,VK_DESCRIPTOR_TYPE_SAMPLER,VK_NULL_HANDLE,sampler);
                        if(sampler==VK_NULL_HANDLE) {
                            descriptor_content_transaction->ForceWrite(3,slot);++descriptor_contents_.invalid_handle_refusals;
                        } else sampler_write=descriptor_content_transaction->NeedsWrite(3,slot,content);
                        if(!sampler_write)++descriptor_contents_.skipped_samplers;
                    }
                    if(sampler_write) {
                        descriptor_batch->Sampler(descriptor_target(3),slot,sampler);
                        if(descriptor_content_transaction)++descriptor_contents_.sampler_write_requests;
                    }
                }
            } else {
                descriptors[image_set]->setTexture(slot,image,RenderTextureLayout::SHADER_READ,binding.view.get());
                descriptors[3]->setSampler(slot,binding.sampler.get());
            }
            shared.textures_2d[slot]=shared.textures_3d[slot]=shared.textures_cube[slot]=shared.samplers[slot]=slot;
            shared.texture_properties[slot]=binding.properties;
            if(time_cpu_stages_)texture_timing_.binding+=DrawMilliseconds(binding_start,DrawClock::now());
        });
        if(descriptor_batch) {
            // Only prior accepted draws may retire during texture preparation.
            // This slot's images/views/samplers remain owned until its own fence;
            // transfer command lists use different descriptor sets. No pointer
            // into a transient binding vector is retained by the batch.
            descriptor_batch->Commit();
            if(descriptor_content_transaction) {
                descriptor_content_transaction->Commit();
                descriptor_contents_.committed_writes+=descriptor_batch->committed_writes();
                descriptor_contents_.update_calls+=descriptor_batch->commits();
            }
            draw_descriptors_.writes+=descriptor_batch->committed_writes();
            draw_descriptors_.update_calls+=descriptor_batch->commits();
        }
        const auto texture_end=time_cpu_stages_?DrawClock::now():DrawClock::time_point{};
        const auto texture_wait=draw_timing_.submit_wait-texture_wait_before;
        const auto shared_bytes=config_.native_fsi?sizeof(fsi_shared):sizeof(shared);
        const auto constant_bytes=kFloatConstantBytes+shared_bytes;
        ConstantDeltaPacker::Writes constant_delta_writes{};
        // Keep original full-input validation before optional shader admission.
        // OFF uploads at the original point. ON waits for both FINAL modules.
        const auto shared_constant_data=std::span(reinterpret_cast<const uint8_t*>(&fsi_shared),shared_bytes);
        if(config_.pinned_null_constant_uploads)(void)PackedDrawConstantSize(raw,shared_constant_data);
        RenderBuffer* constants=nullptr;ShaderPushConstants addresses{};
        bool pinned_null_constants_selected=false;
        const auto upload_constants=[&](bool pinned_null) {
            constants=UploadConstants(draw_slot.constants,raw,shared_constant_data,
                config_.delta_constant_uploads?&constant_delta_writes:nullptr,pinned_null);
            addresses={constants->getDeviceAddress(),constants->getDeviceAddress()+4096,constants->getDeviceAddress()+8192};
        };
        if(!config_.pinned_null_constant_uploads)upload_constants(false);
        RenderTexture* target=nullptr;
        RenderFramebuffer* framebuffer=nullptr;
        if(config_.native_fsi) {
            uint32_t raster_width=extent_width,raster_height=extent_height;
            if(config_.retain_fsi_raster_extent&&fsi_framebuffer_) {
                const auto width=std::max(fsi_raster_width_,extent_width);
                const auto height=std::max(fsi_raster_height_,extent_height);
                // Preserve the original retained area policy for both modes.
                if(uint64_t(width)*height*4<=32u*1024*1024) {
                    raster_width=width;raster_height=height;
                }
            }
            if(!fsi_framebuffer_||fsi_raster_width_!=raster_width||fsi_raster_height_!=raster_height) {
                // Every previously recorded reference completes before either
                // framebuffer/image owner can be replaced. Create replacements
                // locally first: a preaccept refusal never releases the old pair.
                FlushPendingDraws();
                std::unique_ptr<RenderTexture> next_attachment;
                std::unique_ptr<RenderFramebuffer> next;
                bool next_empty=false,next_discard=false;
                if(config_.attachmentless_fsi&&!attachmentless_fsi_refused_) {
                    ++attachmentless_fsi_attempts_;
                    const auto& limits=static_cast<const VulkanDevice&>(*device_).physicalDeviceProperties.limits;
                    try {
                        if(!NativeAttachmentlessFsiEligible(true,raster_width,raster_height,limits))
                            throw std::runtime_error("Attachmentless FSI raster extent is unsupported");
                        if(config_.test_attachmentless_fsi_before_create)config_.test_attachmentless_fsi_before_create();
                        next=device_->createFramebuffer(NativeAttachmentlessFsiFramebuffer(raster_width,raster_height));
                        if(!next)throw std::runtime_error("Attachmentless FSI framebuffer allocation failed");
                        const auto& vk_next=static_cast<const VulkanFramebuffer&>(*next);
                        if(!vk_next.vk||!vk_next.renderPass||!vk_next.colorAttachments.empty()||vk_next.depthAttachment||
                           vk_next.width!=raster_width||vk_next.height!=raster_height)
                            throw std::runtime_error("Attachmentless FSI framebuffer creation failed");
                        next_empty=true;
                    } catch(...) {
                        // This local framebuffer has never been named by a
                        // command. Its destructor destroys any created handles.
                        // Latch ordinary Native mode; do not retry per draw and
                        // never interpret an accepted GPU completion as success.
                        next.reset();attachmentless_fsi_refused_=true;++attachmentless_fsi_refusals_;
                    }
                }
                if(!next_empty) {
                    next_attachment=device_->createTexture(RenderTextureDesc::ColorTarget(raster_width,raster_height,RenderFormat::R8G8B8A8_UNORM));
                    if(!next_attachment)throw std::runtime_error("Native dummy FSI image creation failed");
                    ++fsi_attachment_creations_;
                    const RenderTexture* attachment=next_attachment.get();
                    RenderFramebufferDesc fd;fd.colorAttachments=&attachment;fd.colorAttachmentsCount=1;
                    next=device_->createFramebuffer(fd);
                    if(!next||!static_cast<VulkanFramebuffer&>(*next).vk||!static_cast<VulkanFramebuffer&>(*next).renderPass)
                        throw std::runtime_error("Native dummy FSI framebuffer creation failed");
                    if(config_.discard_dummy_fsi_attachment) {
                        auto& vk_framebuffer=static_cast<VulkanFramebuffer&>(*next);
                        const auto& image=static_cast<const VulkanTexture&>(*next_attachment);
                        const NativeDummyFsiAttachmentShape shape{config_.native_fsi,
                            vk_framebuffer.colorAttachments.size()==1&&vk_framebuffer.colorAttachments.front()==&image,
                            vk_framebuffer.depthAttachment!=nullptr,uint32_t(vk_framebuffer.colorAttachments.size()),
                            vk_framebuffer.width,vk_framebuffer.height,image.desc.mipLevels,image.desc.arraySize,
                            image.desc.format==RenderFormat::R8G8B8A8_UNORM?VK_FORMAT_R8G8B8A8_UNORM:VK_FORMAT_UNDEFINED,
                            VkSampleCountFlagBits(image.desc.multisampling.sampleCount)};
                        ++discard_dummy_fsi_attempts_;
                        discard_dummy_fsi_last_result_=ReplaceFreshNativeDummyFsiRenderPass(
                            static_cast<VulkanDevice&>(*device_).vk,vk_framebuffer.renderPass,
                            shape,vkCreateRenderPass,vkDestroyRenderPass);
                        next_discard=discard_dummy_fsi_last_result_==VK_SUCCESS;
                        if(next_discard)++discard_dummy_fsi_installs_;else ++discard_dummy_fsi_refusals_;
                    }
                }
                fsi_framebuffer_=std::move(next); // Destroy old framebuffer before its image.
                fsi_attachment_=std::move(next_attachment);
                fsi_raster_width_=raster_width;fsi_raster_height_=raster_height;
                attachmentless_fsi_active_=next_empty;discard_dummy_fsi_installed_=next_discard;
                if(next_empty)++attachmentless_fsi_installs_;
            } else if(raster_width!=extent_width||raster_height!=extent_height)++fsi_attachment_extent_reuses_;
            // DecodeDrawState still clips to the exact guest dimensions. The
            // framebuffer extent only bounds rasterization; storage owns all
            // real color/depth/stencil output in either host pass mode.
            target=fsi_attachment_.get();framebuffer=fsi_framebuffer_.get();
        } else if(color)target=&Surface(attachments[0].identity,!state.discard);
        const bool depth_write=!state.discard&&(state.pipeline.depthWriteEnabled||
            (state.pipeline.stencilEnabled&&state.pipeline.stencilWriteMask));
        RenderTexture* depth=!config_.native_fsi&&attachments[4].identity?&Surface(attachments[4].identity,depth_write):nullptr;
        if(!config_.native_fsi) {
            // One framebuffer per attachment pair; EndFrame drops the pair's
            // framebuffer before either image is retired.
            auto& cached=hardware_framebuffers_[{target,depth}];
            if(!cached) {
                const RenderTexture* attachment=target;RenderFramebufferDesc fd;
                fd.colorAttachments=target?&attachment:nullptr;fd.colorAttachmentsCount=target?1:0;fd.depthAttachment=depth;
                cached=device_->createFramebuffer(fd);
            }
            framebuffer=cached.get();
        }
        // Specialized pipelines may change this flag; command emission still
        // follows the original decoded raster state.
        const bool original_dynamic_depth_bias=state.pipeline.dynamicDepthBiasEnabled;
        auto& p=state.pipeline;p.pipelineLayout=layout_.get();
        const PipelineRasterKey raster_key=config_.native_fsi?
            PipelineRasterKey(std::array<uint32_t,6>{uint32_t(p.cullMode),uint32_t(p.frontFace),uint32_t(p.depthClipEnabled),
                uint32_t(p.primitiveTopology),state.specialization,uint32_t(rectangle)}):
            PipelineRasterKey(HardwarePipelineRasterKey(state,rectangle));
        bool audited_vertex=false,null_constant_vertex=false,null_constant_pixel=false;
        NativeShaderVariant vertex_variant=NativeShaderVariant::Unmodified;
        RenderShader* forward_vs=nullptr;
        if(config_.forward_vertex_control_flow&&config_.native_fsi&&forward_vertex_program_ready_&&
           !config_.implicit_fsi_barriers&&!config_.opaque_color_overwrite&&!config_.forward_control_flow&&!config_.builtin_atomic_depth&&
           !o.Shader(false).IsNull()&&ForwardVertexAllowed(o.Shader(false).Digest(),std::get<std::array<uint32_t,6>>(raster_key))) {
            ++forward_vertex_stats_.candidates;
            const PipelineLookup candidate_key{o.Shader(false).OptionalDigest(),o.Shader(true).OptionalDigest(),raster_key,
                target?int32_t(static_cast<VulkanTexture*>(target)->desc.format):-1,depth?int32_t(static_cast<VulkanTexture*>(depth)->desc.format):-1,
                NativeShaderVariant::Unmodified,NativeShaderVariant::ForwardStraightVertexControlFlowV1};
            forward_vs=ResidentForwardVertex(candidate_key);
        }
        if(forward_vs) {p.vertexShader=forward_vs;vertex_variant=NativeShaderVariant::ForwardStraightVertexControlFlowV1;}
        else p.vertexShader=Shader(o.Shader(false),o,false,nullptr,false,nullptr,&audited_vertex,&null_constant_vertex);
        bool atomic_candidate=false;
        if(config_.builtin_atomic_depth) {
            atomic_candidate=BuiltinAtomicDepthAllowed(fsi_shared.raster,extent_width,extent_height,
                BE(raw,0x28dc),pixel_shader_present,queries_->active(),true,extent.Info());
            if(atomic_candidate) {
                ++atomic_depth_stats_.candidates;
                if(!audited_vertex)++atomic_depth_stats_.vertex_refusals;
            }
        }
        NativeShaderVariant pixel_variant=ShaderVariant(config_.native_fsi,o.Shader(true).IsNull());
        RenderShader* opaque_shader=nullptr;
        if(config_.opaque_color_overwrite&&config_.native_fsi&&opaque_program_ready_&&
           !config_.implicit_fsi_barriers&&!o.Shader(false).IsNull()&&OpaqueColorOverwriteAllowed(fsi_shared.raster,extent_width,extent_height,
                extent.Info(),BE(raw,0x28dc),queries_->active(),o.Shader(false).Digest(),
                o.Shader(true).IsNull()?std::string_view{}:o.Shader(true).Digest(),
                std::get<std::array<uint32_t,6>>(raster_key))) {
            ++opaque_overwrite_stats_.candidates;
            const PipelineLookup candidate_key{o.Shader(false).OptionalDigest(),o.Shader(true).OptionalDigest(),
                raster_key,target?int32_t(static_cast<VulkanTexture*>(target)->desc.format):-1,
                depth?int32_t(static_cast<VulkanTexture*>(depth)->desc.format):-1,
                NativeShaderVariant::OpaqueFormat3FullWordOverwriteV1};
            opaque_shader=ResidentOpaqueColorOverwrite(candidate_key);
        }
        RenderShader* forward_shader=nullptr;
        if(config_.forward_control_flow&&config_.native_fsi&&forward_program_ready_&&
           !config_.implicit_fsi_barriers&&!config_.opaque_color_overwrite&&
           !o.Shader(false).IsNull()&&!o.Shader(true).IsNull()&&
           ForwardControlFlowAllowed(o.Shader(false).Digest(),o.Shader(true).Digest(),
                std::get<std::array<uint32_t,6>>(raster_key))) {
            ++forward_control_flow_stats_.candidates;
            const PipelineLookup candidate_key{o.Shader(false).OptionalDigest(),o.Shader(true).OptionalDigest(),
                raster_key,target?int32_t(static_cast<VulkanTexture*>(target)->desc.format):-1,
                depth?int32_t(static_cast<VulkanTexture*>(depth)->desc.format):-1,
                NativeShaderVariant::ForwardOnlyGuestControlFlowV1};
            forward_shader=ResidentForwardControlFlow(candidate_key);
        }
        RenderShader* catalog_shader=nullptr;
        if(config_.forward_pixel_catalog&&config_.native_fsi&&forward_pixel_catalog_ready_&&
           !o.Shader(true).IsNull()&&ForwardPixelCatalogAllowed(o.Shader(true).Digest(),std::get<std::array<uint32_t,6>>(raster_key))&&
           forward_pixel_catalog_source_stats_.contains(o.Shader(true).Digest())) {
            ++forward_pixel_catalog_stats_.candidates;++forward_pixel_catalog_source_stats_.find(o.Shader(true).Digest())->second.candidates;
            const PipelineLookup candidate_key{o.Shader(false).OptionalDigest(),o.Shader(true).OptionalDigest(),raster_key,
                target?int32_t(static_cast<VulkanTexture*>(target)->desc.format):-1,depth?int32_t(static_cast<VulkanTexture*>(depth)->desc.format):-1,
                NativeShaderVariant::ForwardColorPixelCatalogV1,NativeShaderVariant::Unmodified};
            catalog_shader=ResidentForwardPixelCatalog(candidate_key,o.Shader(true).Digest());
        }
        // Decide the authoritative phase path after texture/geometry preparation,
        // before ordinary state/early-depth selection can read stale packed data.
        std::shared_ptr<NativeRetainedOpaqueGeneration> retained_opaque;
        std::optional<NativeRetainedOpaqueAuthority::Ticket> retained_opaque_ticket;
        if(config_.retained_opaque_phase) {
            if(RetainedOpaquePhaseDrawAllowed(o,nullptr,false,phase_facts_ptr)) {
                ++retained_opaque_stats_.candidates;
                if(RetainedOpaquePhaseDrawAllowed(o,nullptr,true,phase_facts_ptr)&&vertex_variant==NativeShaderVariant::Unmodified) {
                    retained_opaque=GetRetainedOpaque(o.Event());
                    if(retained_opaque) {
                        InvalidateEarlyDepth();
                        const auto ps=shaders_.find(ShaderCacheLookup{o.Shader(true).Digest(),true,NativeShaderVariant::RetainedOpaquePhaseConservativeV4});
                        const auto vs=shaders_.find(ShaderCacheLookup{o.Shader(false).Digest(),false,NativeShaderVariant::Unmodified});
                        if(vs==shaders_.end()||vs->second.shader.get()!=p.vertexShader||ps==shaders_.end())
                            throw NativeBatchCompletionError("Opaque phase module owner changed during preparation");
                        cache_epoch_.Mark(vs->second);cache_epoch_.Mark(ps->second);
                        retained_opaque_ticket=retained_opaque->authority.ticket();draw_slot.retained_opaque=retained_opaque;
                        target=retained_opaque->color.get();depth=retained_opaque->depth.get();framebuffer=retained_opaque->Framebuffer();
                        p.pixelShader=ps->second.shader.get();pixel_variant=NativeShaderVariant::RetainedOpaquePhaseConservativeV4;
                        p.depthEnabled=true;p.depthWriteEnabled=true;p.depthFunction=RenderComparisonFunction::GREATER_EQUAL;
                        p.stencilEnabled=false;p.dynamicDepthBiasEnabled=false;
                        p.renderTargetBlend[0]=RenderBlendDesc::Copy();p.renderTargetBlend[0].renderTargetWriteMask=1;
                    }
                } else ++retained_opaque_stats_.residency_refusals;
            }
            if(!retained_opaque&&retained_opaque_current_)MaterializeRetainedOpaque();
        }
        std::optional<NativeFsiStateSpecialization> fsi_state;
        std::optional<PipelineLookup> fsi_candidate;
        bool fsi_state_selected=false;
        ResidentFsiStateSelection resident_fsi;
        bool material_state_allowed=!retained_opaque;
        if(config_.fsi_state_material_composition&&!retained_opaque) {
            ++fsi_state_material_stats_.examined;
            const auto admission=ClassifyFsiStateMaterialComposition(
                o.Shader(false).OptionalDigest().value_or(std::string_view{}),
                o.Shader(true).OptionalDigest().value_or(std::string_view{}),
                {queries_->active(),state.discard,rectangle,vertex_variant==NativeShaderVariant::Unmodified,
                 pixel_variant==NativeShaderVariant::Unmodified,
                 target&&target==fsi_attachment_.get()&&framebuffer==fsi_framebuffer_.get()&&!depth&&!attachmentless_fsi_active_},
                config_.fsi_state_inactive_early_catalog,config_.native_early_depth_catalog);
            material_state_allowed=admission==FsiStateMaterialCompositionAdmission::Admitted;
            if(config_.fsi_state_inactive_early_catalog&&material_state_allowed&&FsiStateInactiveEarlyCatalogPair(
                    o.Shader(false).Digest(),o.Shader(true).Digest()))
                ++fsi_state_material_stats_.inactive_catalog_admitted;
            switch(admission) {
                case FsiStateMaterialCompositionAdmission::Source:++fsi_state_material_stats_.source_refusals;break;
                case FsiStateMaterialCompositionAdmission::Query:++fsi_state_material_stats_.query_refusals;break;
                case FsiStateMaterialCompositionAdmission::State:++fsi_state_material_stats_.state_refusals;break;
                case FsiStateMaterialCompositionAdmission::Admitted:++fsi_state_material_stats_.admitted;break;
            }
        }
        if(material_state_allowed&&(config_.specialize_fsi_pipeline_state||config_.observe_fsi_state)&&!config_.builtin_readonly_depth_queries&&
           (!config_.native_readonly_depth_attachment||config_.fsi_state_material_composition)&&config_.native_fsi&&vertex_variant==NativeShaderVariant::Unmodified&&
           (!config_.fsi_state_forward_cfg||(!o.Shader(false).IsNull()&&!o.Shader(true).IsNull()&&
            FsiStateForwardCfgAllowed(o.Shader(false).Digest(),o.Shader(true).Digest(),std::get<std::array<uint32_t,6>>(raster_key))))) {
            ++fsi_state_stats_.candidates;
            fsi_candidate.emplace(PipelineLookup{o.Shader(false).OptionalDigest(),o.Shader(true).OptionalDigest(),raster_key,
                target?int32_t(static_cast<VulkanTexture*>(target)->desc.format):-1,
                depth?int32_t(static_cast<VulkanTexture*>(depth)->desc.format):-1,
                FsiStatePixelVariant(),NativeShaderVariant::Unmodified,
                NativeFsiStateSpecialization::From(fsi_shared.raster)});
            if(config_.specialize_fsi_pipeline_state)resident_fsi=ResidentFsiState(*fsi_candidate);
            if(resident_fsi) {
                p.pixelShader=resident_fsi.shader;pixel_variant=FsiStatePixelVariant();
                fsi_state=fsi_candidate->fsi_state;fsi_state_selected=true;
                if(config_.fsi_state_inactive_early_catalog&&FsiStateInactiveEarlyCatalogPair(
                        o.Shader(false).Digest(),o.Shader(true).Digest()))
                    ++fsi_state_material_stats_.inactive_catalog_selected;
            }
        }
        RenderShader* readonly_shader=nullptr;
        if(config_.builtin_readonly_depth_queries&&!config_.specialize_fsi_pipeline_state&&config_.native_fsi&&!state.discard&&
           !config_.attachmentless_fsi&&!config_.implicit_fsi_barriers&&!config_.builtin_atomic_depth&&!config_.opaque_color_overwrite&&
           !config_.forward_control_flow&&!config_.forward_vertex_control_flow&&!config_.forward_pixel_catalog&&
           !o.Shader(false).IsNull()&&o.Shader(false).Digest()==kReadonlyQueryVertexSource&&
           BuiltinReadonlyQueryAllowed(fsi_shared.raster,extent_width,extent_height,BE(raw,0x28dc),
               pixel_shader_present,queries_->active(),true,extent.Info())&&
           std::get<std::array<uint32_t,6>>(raster_key)[3]==uint32_t(RenderPrimitiveTopology::TRIANGLE_LIST)&&!rectangle) {
            ++readonly_query_stats_.candidates;
            const PipelineLookup key{o.Shader(false).OptionalDigest(),std::nullopt,raster_key,
                target?int32_t(static_cast<VulkanTexture*>(target)->desc.format):-1,
                depth?int32_t(static_cast<VulkanTexture*>(depth)->desc.format):-1,NativeShaderVariant::BuiltinReadonlyDepthQueryV2};
            readonly_shader=ResidentReadonlyDepthQuery(key,p.vertexShader);
        }
        if(retained_opaque) {
            // Certified phase modules and attachments already own this draw.
        } else if(fsi_state_selected) {
            // The specialization-capable module is already resident.
        } else if(readonly_shader) {
            p.pixelShader=readonly_shader;pixel_variant=NativeShaderVariant::BuiltinReadonlyDepthQueryV2;
        } else if(catalog_shader) {
            p.pixelShader=catalog_shader;pixel_variant=NativeShaderVariant::ForwardColorPixelCatalogV1;
        } else if(forward_shader) {
            p.pixelShader=forward_shader;pixel_variant=NativeShaderVariant::ForwardOnlyGuestControlFlowV1;
        } else if(opaque_shader) {
            p.pixelShader=opaque_shader;pixel_variant=NativeShaderVariant::OpaqueFormat3FullWordOverwriteV1;
        } else p.pixelShader=Shader(o.Shader(true),o,config_.native_fsi,p.vertexShader,
            atomic_candidate&&audited_vertex,&pixel_variant,nullptr,&null_constant_pixel);
        std::shared_ptr<NativeReadonlyDepthGeneration> readonly_depth;
        std::optional<NativeReadonlyDepthVersion::Ticket> readonly_depth_ticket;
        bool readonly_depth_queried=false,readonly_no_write_prepared=false;
        uint64_t readonly_no_write_serial=0;
        const std::array<uint32_t,6> readonly_raster{uint32_t(RenderCullMode::BACK),uint32_t(RenderFrontFace::COUNTER_CLOCKWISE),1,
            uint32_t(RenderPrimitiveTopology::TRIANGLE_LIST),0,0};
        if(config_.native_readonly_depth_attachment&&!state.discard&&vertex_variant==NativeShaderVariant::Unmodified&&
           !o.Shader(false).IsNull()&&o.Shader(false).Digest()==kReadonlyQueryVertexSource&&
           std::get<std::array<uint32_t,6>>(raster_key)==readonly_raster&&
           NativeReadonlyDepthAttachmentEligible(fsi_shared.raster,extent_width,extent_height,BE(raw,0x28dc),
               pixel_shader_present,queries_->active(),true,state.viewport.minDepth,state.viewport.maxDepth,extent.Info())) {
            ++readonly_depth_stats_.candidates;
            const auto vs=shaders_.find(ShaderCacheLookup{kReadonlyQueryVertexSource,false,NativeShaderVariant::Unmodified});
            const auto ps=shaders_.find(ShaderCacheLookup{"depth-only",true,ReadonlyDepthVariant()});
            const PipelineLookup key{o.Shader(false).OptionalDigest(),std::nullopt,raster_key,-1,int32_t(RenderFormat::D32_FLOAT),ReadonlyDepthVariant()};
            if(vs!=shaders_.end()&&vs->second.audited_readonly_vertex&&vs->second.shader.get()==p.vertexShader&&
               ps!=shaders_.end()&&pipelines_.find(key)!=pipelines_.end()&&pipelines_.find(key)->second.pipeline) {
                cache_epoch_.Mark(vs->second);cache_epoch_.Mark(ps->second);cache_epoch_.Mark(pipelines_.find(key)->second);
                readonly_depth=GetNativeReadonlyDepth(storage(4),o.Event());
                if(readonly_depth) {
                    readonly_depth_ticket=readonly_depth->version.Capture(readonly_depth->version.layout(),edram_memory_->SurfaceVersion(storage(4)));
                    if(!readonly_depth_ticket)throw NativeBatchCompletionError("Readonly depth source token changed after FIFO conversion");
                    if(config_.native_readonly_depth_no_write_metadata) {
                        readonly_no_write_prepared=PrepareReadonlyNoWriteMetadata(storage(4));
                        readonly_no_write_serial=edram_memory_->physical_serial();
                    }
                    draw_slot.readonly_depth=readonly_depth;readonly_depth_queried=queries_->active();
                    target=nullptr;depth=readonly_depth->image.get();framebuffer=readonly_depth->framebuffer.get();
                    p.pixelShader=ps->second.shader.get();pixel_variant=ReadonlyDepthVariant();
                    if(config_.native_readonly_depth_conservative) {
                        auto conservative_key=key;conservative_key.pixel_variant=NativeShaderVariant::NativeReadonlyDepthLessV1;
                        const auto conservative_ps=shaders_.find(ShaderCacheLookup{"depth-only",true,NativeShaderVariant::NativeReadonlyDepthLessV1});
                        const auto conservative_pipeline=pipelines_.find(conservative_key);
                        if(conservative_ps!=shaders_.end()&&conservative_pipeline!=pipelines_.end()&&conservative_pipeline->second.pipeline) {
                            cache_epoch_.Mark(conservative_ps->second);cache_epoch_.Mark(conservative_pipeline->second);
                            p.pixelShader=conservative_ps->second.shader.get();pixel_variant=NativeShaderVariant::NativeReadonlyDepthLessV1;
                        }else ++readonly_depth_conservative_stats_.residency_refusals;
                    }
                    p.depthEnabled=true;p.depthWriteEnabled=false;p.depthFunction=RenderComparisonFunction::GREATER_EQUAL;
                    p.stencilEnabled=false;p.dynamicDepthBiasEnabled=false;
                    resident_fsi={};fsi_state.reset();fsi_state_selected=false;
                }
            } else ++readonly_depth_stats_.residency_refusals;
        }
        std::shared_ptr<NativeEarlyDepthGeneration> early_depth;
        std::optional<NativeEarlyDepthLowerBoundVersion::Ticket> early_depth_ticket;
        std::shared_ptr<NativeEarlyDepthGeneration> material_depth;
        std::optional<NativeEarlyDepthMaterialTicket> material_depth_ticket;
        bool material_pipeline_certified=false;
        const auto material_depth_proof=[&] {
            NativeEarlyDepthMaterialProof proof;
            proof.enabled=config_.native_early_depth_material_retention;
            proof.native_fsi=config_.native_fsi;proof.certified_pipeline=material_pipeline_certified;
            proof.private_framebuffer=target&&target==fsi_attachment_.get()&&
                framebuffer==fsi_framebuffer_.get()&&!depth&&!attachmentless_fsi_active_;
            proof.active_query=queries_->active();proof.discarded=state.discard;proof.rectangle=rectangle;
            proof.indexed_triangles=function=="DrawIndexedPrimitive"&&primitive==4&&
                state.pipeline.primitiveTopology==RenderPrimitiveTopology::TRIANGLE_LIST;
            proof.gpu_geometry=!packed_pass_cpu_geometry;
            proof.bounded_scissor=state.scissor.left>=0&&state.scissor.top>=0&&
                state.scissor.right<=1280&&state.scissor.bottom<=720;
            proof.single_sample=p.multisampling.sampleCount==1&&!p.multisampling.sampleLocationsEnabled&&
                !p.alphaToCoverageEnabled&&target&&static_cast<VulkanTexture*>(target)->desc.arraySize==1&&
                static_cast<VulkanTexture*>(target)->desc.multisampling.sampleCount==1;
            proof.draw_identity=draw_slot.token;
            proof.vertex_module=p.vertexShader;proof.pixel_module=p.pixelShader;
            proof.vertex_variant=vertex_variant;proof.pixel_variant=pixel_variant;
            proof.raster=fsi_shared.raster;proof.selected_specialization=fsi_state;
            if(!o.Shader(false).IsNull()) {
                const auto vs=shaders_.find(ShaderCacheLookup{o.Shader(false).Digest(),false,vertex_variant});
                if(vs!=shaders_.end()&&vs->second.audited_monotonic_depth)
                    proof.certified_vertex_module=vs->second.shader.get();
            }
            if(!o.Shader(true).IsNull()) {
                const auto ps=shaders_.find(ShaderCacheLookup{o.Shader(true).Digest(),true,pixel_variant});
                if(ps!=shaders_.end()&&ps->second.audited_monotonic_depth)
                    proof.certified_pixel_module=ps->second.shader.get();
            }
            if(attachments[0].identity&&attachments[4].identity&&
               !attachments[1].identity&&!attachments[2].identity&&!attachments[3].identity&&
               !(attachments[0].identity.Info()&0x30000)&&!(attachments[4].identity.Info()&0x30000)&&
               extent_width==1280&&extent_height==720&&BE(raw,0x28dc)==15) {
                const auto& c=storage(0);const auto& d=storage(4);
                proof.color_layout={c.width,c.height,c.pitch,c.base,c.format,uint32_t(c.depth)};
                proof.depth_layout={d.width,d.height,d.pitch,d.base,d.format,uint32_t(d.depth)};
            }
            return proof;
        };
        if(config_.native_early_depth&&!retained_opaque) {
            if(EarlyDepthDrawAllowed(o,fsi_shared.raster,state,extent_width,extent_height)&&vertex_variant==NativeShaderVariant::Unmodified) {
                const bool catalogue=config_.native_early_depth_catalog&&NativeEarlyDepthCatalogPair(o.Shader(false).Digest(),o.Shader(true).Digest());
                auto& stats=catalogue?early_depth_catalog_stats_:early_depth_stats_;++stats.candidates;
                const auto selected_variant=catalogue?NativeShaderVariant::ConservativeEarlyDepthCatalogV2:NativeShaderVariant::ConservativeEarlyDepthV1;
                const auto vs=shaders_.find(ShaderCacheLookup{o.Shader(false).Digest(),false,NativeShaderVariant::Unmodified});
                const auto ps=shaders_.find(ShaderCacheLookup{o.Shader(true).Digest(),true,selected_variant});
                const PipelineLookup key{o.Shader(false).OptionalDigest(),o.Shader(true).OptionalDigest(),raster_key,-1,
                    int32_t(RenderFormat::D32_FLOAT),selected_variant};
                const auto resident=pipelines_.find(key);
                auto original_ps=shaders_.end();auto original_pipeline=pipelines_.end();
                bool source_resident=bool(early_depth_program_)&&vs!=shaders_.end()&&vs->second.audited_early_depth_vertex;
                if(catalogue) {
                    const auto program=early_depth_catalog_programs_.find(o.Shader(true).Digest());
                    original_ps=shaders_.find(ShaderCacheLookup{o.Shader(true).Digest(),true,NativeShaderVariant::Unmodified});
                    // The live descriptor receives attachment formats below.
                    // Match the prepared generic key to the actual attachments.
                    const PipelineLookup original_key{o.Shader(false).OptionalDigest(),o.Shader(true).OptionalDigest(),raster_key,
                        target?int32_t(static_cast<VulkanTexture*>(target)->desc.format):-1,
                        depth?int32_t(static_cast<VulkanTexture*>(depth)->desc.format):-1,NativeShaderVariant::Unmodified};
                    original_pipeline=pipelines_.find(original_key);
                    source_resident=program!=early_depth_catalog_programs_.end()&&vs!=shaders_.end()&&vs->second.audited_early_depth_catalog_vertex&&
                        original_ps!=shaders_.end()&&original_ps->second.shader.get()==p.pixelShader&&
                        original_pipeline!=pipelines_.end()&&bool(original_pipeline->second.pipeline);
                }
                if(source_resident&&vs->second.shader.get()==p.vertexShader&&
                   ps!=shaders_.end()&&resident!=pipelines_.end()&&resident->second.pipeline) {
                    early_depth=GetNativeEarlyDepth(o.Event(),catalogue);
                    if(early_depth) {
                        early_depth_ticket=early_depth->version.Capture(early_depth->version.layout(),edram_memory_->SurfaceVersion(RetainedDepth()));
                        if(!early_depth_ticket)throw NativeBatchCompletionError("Early depth lost its accepted source certificate");
                        cache_epoch_.Mark(vs->second);cache_epoch_.Mark(ps->second);cache_epoch_.Mark(resident->second);
                        if(catalogue) {cache_epoch_.Mark(original_ps->second);cache_epoch_.Mark(original_pipeline->second);}
                        draw_slot.early_depth=early_depth;target=nullptr;depth=early_depth->resources->image.get();
                        framebuffer=early_depth->resources->framebuffer.get();p.pixelShader=ps->second.shader.get();
                        pixel_variant=selected_variant;
                        p.depthEnabled=true;p.depthWriteEnabled=false;p.depthFunction=RenderComparisonFunction::GREATER_EQUAL;
                        p.stencilEnabled=false;p.dynamicDepthBiasEnabled=false;resident_fsi={};fsi_state.reset();fsi_state_selected=false;
                    }
                }else ++stats.residency_refusals;
            }
            if(!early_depth&&early_depth_current_&&!config_.native_early_depth_material_retention)
                InvalidateEarlyDepth();
        }
        // Early certification may precede pressure/module eviction or a D32
        // conversion refusal. Retain only the actual audited selected path.
        bool disjoint_retained_query=false;
        if(config_.retain_primary_across_disjoint_queries&&
           (retained_opaque_current_||retained_fp24_depth_current_)) {
            disjoint_retained_query=readonly_depth&&IsNativeReadonlyDepthVariant(pixel_variant)&&
                RetainedDisjointQueryDrawAllowed(o);
            if(!disjoint_retained_query&&!RetainedOpaqueDrawAllowed(o)&&!RetainedFp24DepthDrawAllowed(o)) {
                ++disjoint_query_stats_.late_fallbacks;MaterializeRetainedOpaque();
            }
        }
        std::shared_ptr<NativeRetainedOpaqueGeneration> shared_retained_depth;
        std::optional<NativeRetainedOpaqueAuthority::Ticket> shared_retained_ticket;
        if(config_.retained_shared_depth_queries&&RetainedSharedDepthDrawAllowed(o,false)) {
            ++shared_depth_query_stats_.candidates;
            if(!RetainedSharedDepthDrawAllowed(o)) {
                ++shared_depth_query_stats_.residency_refusals;MaterializeRetainedOpaque();
            } else {
                bool prepared=false;
                try {
                    if(!retained_opaque_current_->readonly_framebuffer&&config_.test_retained_shared_depth_before_framebuffer)
                        config_.test_retained_shared_depth_before_framebuffer();
                    prepared=retained_opaque_writer_->PrepareReadonlyFramebuffer(retained_opaque_current_);
                }catch(...){prepared=false;}
                if(!prepared) {
                    ++shared_depth_query_stats_.framebuffer_refusals;MaterializeRetainedOpaque();
                }else {
                    shared_retained_depth=retained_opaque_current_;shared_retained_ticket=shared_retained_depth->authority.ticket();
                    draw_slot.retained_opaque=shared_retained_depth;
                    target=nullptr;depth=shared_retained_depth->depth.get();framebuffer=shared_retained_depth->readonly_framebuffer.get();
                    const auto vs=shaders_.find(ShaderCacheLookup{kReadonlyQueryVertexSource,false,NativeShaderVariant::Unmodified});
                    const auto ps=shaders_.find(ShaderCacheLookup{"depth-only",true,NativeShaderVariant::NativeReadonlyDepthD32LoadV2});
                    if(vs==shaders_.end()||!vs->second.audited_readonly_vertex||vs->second.shader.get()!=p.vertexShader||ps==shaders_.end())
                        throw NativeBatchCompletionError("Shared readonly shader vanished after admission");
                    cache_epoch_.Mark(vs->second);cache_epoch_.Mark(ps->second);
                    p.pixelShader=ps->second.shader.get();pixel_variant=NativeShaderVariant::NativeReadonlyDepthD32LoadV2;
                    p.depthEnabled=true;p.depthWriteEnabled=false;p.depthFunction=RenderComparisonFunction::GREATER_EQUAL;
                    p.stencilEnabled=false;p.dynamicDepthBiasEnabled=false;
                }
            }
        }
        std::shared_ptr<NativeRetainedFp24DepthGeneration> retained_fp24_depth;
        if(RetainedOpaqueEnabled()) {
            if(RetainedOpaqueDrawAllowed(o,nullptr,false)) {
                ++retained_opaque_stats_.candidates;
                if(config_.retained_opaque_catalog) {
                    const auto* pair=RetainedOpaqueCatalogPair(o.Shader(false).Digest(),o.Shader(true).Digest());
                    ++retained_opaque_pair_stats_[size_t(pair-kRetainedOpaqueCatalogPairs.data())].candidates;
                }
                if(!RetainedOpaqueDrawAllowed(o)) {
                    ++retained_opaque_stats_.residency_refusals;MaterializeRetainedOpaque();
                }else retained_opaque=GetRetainedOpaque(o.Event());
                if(retained_opaque) {
                    retained_opaque_ticket=retained_opaque->authority.ticket();draw_slot.retained_opaque=retained_opaque;
                    target=retained_opaque->color.get();depth=retained_opaque->depth.get();framebuffer=retained_opaque->Framebuffer();
                    const auto ps=shaders_.find(ShaderCacheLookup{o.Shader(true).Digest(),true,RetainedOpaqueVariant()});
                    if(ps==shaders_.end())throw NativeBatchCompletionError("Retained opaque module vanished during admission");
                    cache_epoch_.Mark(ps->second);p.pixelShader=ps->second.shader.get();pixel_variant=RetainedOpaqueVariant();
                    p.depthEnabled=true;p.depthWriteEnabled=true;p.depthFunction=RenderComparisonFunction::GREATER_EQUAL;p.stencilEnabled=false;p.dynamicDepthBiasEnabled=false;
                    p.renderTargetBlend[0]=RenderBlendDesc::Copy();p.renderTargetBlend[0].renderTargetWriteMask=1;
                }
            } else if(retained_opaque_current_&&!shared_retained_depth&&!disjoint_retained_query) MaterializeRetainedOpaque();
        }
        std::optional<NativeRetainedFp24DepthAuthority::Ticket> retained_fp24_depth_ticket;
        if(config_.retained_fp24_depth_writer&&RetainedFp24DepthDrawAllowed(o,nullptr,false)) {
            ++retained_fp24_depth_stats_.candidates;
            if(!RetainedFp24DepthDrawAllowed(o)) {
                ++retained_fp24_depth_stats_.residency_refusals;MaterializeRetainedFp24Depth();FlushPendingDraws();
            }else {
                retained_fp24_depth=GetRetainedFp24Depth(o.Event());
                if(retained_fp24_depth) {
                    retained_fp24_depth_ticket=retained_fp24_depth->authority.ticket();draw_slot.retained_fp24_depth=retained_fp24_depth;
                    const auto vs=shaders_.find(ShaderCacheLookup{o.Shader(false).Digest(),false,NativeShaderVariant::Unmodified});
                    const auto ps=shaders_.find(ShaderCacheLookup{"depth-only",true,NativeShaderVariant::RetainedFp24DepthWriterV1});
                    if(vs==shaders_.end()||!vs->second.audited_retained_fp24_vertex||vs->second.shader.get()!=p.vertexShader||ps==shaders_.end())
                        throw NativeBatchCompletionError("Retained FP24 depth shader disappeared during admission");
                    cache_epoch_.Mark(vs->second);cache_epoch_.Mark(ps->second);
                    target=nullptr;depth=retained_fp24_depth->depth.get();framebuffer=retained_fp24_depth->framebuffer.get();
                    p.pixelShader=ps->second.shader.get();pixel_variant=NativeShaderVariant::RetainedFp24DepthWriterV1;
                    p.depthEnabled=true;p.depthWriteEnabled=true;p.depthFunction=RenderComparisonFunction::GREATER_EQUAL;
                    p.stencilEnabled=false;p.dynamicDepthBiasEnabled=false;
                }
            }
        }
        const RenderInputSlot input_slot(0,16);
        const RenderInputElement input_element("POSITION",0,0,RenderFormat::R32G32B32A32_FLOAT,0,0);
        if(rectangle){p.inputSlots=&input_slot;p.inputSlotsCount=1;p.inputElements=&input_element;p.inputElementsCount=1;}
        p.renderTargetCount=target?1:0;if(target)p.renderTargetFormat[0]=static_cast<VulkanTexture*>(target)->desc.format;
        if(depth)p.depthTargetFormat=static_cast<VulkanTexture*>(depth)->desc.format;
        const RenderSpecConstant specialization(0,state.specialization);p.specConstants=&specialization;p.specConstantsCount=1;
        // FSI depth, stencil, blend and bias live in shader constants. Viewport,
        // scissor, alpha threshold and index offset are also dynamic. Key only
        // the immutable host raster state, or streaming these values needlessly
        // recompiles otherwise identical pipelines every frame.
        bool stencil_material_selected=false;
        if(StencilMaterialEnabled()&&!o.Shader(false).IsNull()&&!o.Shader(true).IsNull()&&
           o.Shader(false).Digest()==kStencilMaterialVertex&&o.Shader(true).Digest()==kStencilMaterialPixel) {
            auto& stats=stencil_material_stats_;++stats.candidates;
            const auto vs=shaders_.find(ShaderCacheLookup{kStencilMaterialVertex,false,NativeShaderVariant::Unmodified});
            const auto ps=shaders_.find(ShaderCacheLookup{kStencilMaterialPixel,true,NativeShaderVariant::Unmodified});
            NativeStencilMaterialProof proof;
            proof.original_modules=vs!=shaders_.end()&&ps!=shaders_.end()&&vs->second.audited_stencil_material&&
                ps->second.audited_stencil_material&&vs->second.shader.get()==p.vertexShader&&ps->second.shader.get()==p.pixelShader;
            proof.unmodified_variants=vertex_variant==NativeShaderVariant::Unmodified&&pixel_variant==NativeShaderVariant::Unmodified;
            proof.owned_up_cube=StencilMaterialOwnedCube(o);proof.active_query=queries_->active();
            proof.discarded=state.discard;proof.rectangle=rectangle;proof.width=extent_width;proof.height=extent_height;
            proof.index_count=count;proof.raw_color_mask=BE(raw,0x28dc);proof.raster=std::get<std::array<uint32_t,6>>(raster_key);
            proof.private_framebuffer=target&&target==fsi_attachment_.get()&&framebuffer==fsi_framebuffer_.get()&&!depth&&!attachmentless_fsi_active_;
            proof.bounded_scissor=state.scissor.left>=0&&state.scissor.top>=0&&state.scissor.right<=1280&&state.scissor.bottom<=720;
            proof.single_sample_single_view=p.multisampling.sampleCount==1&&!p.multisampling.sampleLocationsEnabled&&
                !p.alphaToCoverageEnabled&&p.renderTargetCount==1&&target&&
                static_cast<VulkanTexture*>(target)->desc.arraySize==1&&static_cast<VulkanTexture*>(target)->desc.multisampling.sampleCount==1;
            // Pinned Plume pipeline/render-pass producer supplies one viewport,
            // no sample shading and no multiview. This command uses 1 instance.
            proof.primary_surfaces=attachments[0].identity&&attachments[4].identity&&
                !attachments[1].identity&&!attachments[2].identity&&!attachments[3].identity;
            if(proof.primary_surfaces) {
                const auto& c=storage(0);const auto& d=storage(4);
                proof.primary_surfaces=c.width==1280&&c.height==720&&c.pitch==1280&&c.base==720&&c.format==0&&!c.depth&&
                    d.width==1280&&d.height==720&&d.pitch==1280&&d.base==0&&d.format==1&&d.depth&&
                    !(attachments[0].identity.Info()&0x30000)&&!(attachments[4].identity.Info()&0x30000);
                proof.packed_current=edram_memory_->SurfaceFullyDefined(d);
                // Normal Draw has imported current dirty host images already.
                // A leftover dirty image fails closed instead of asserting that
                // physical version metadata alone publishes shader writes.
                for(const auto& [identity,image]:surfaces_)if(image.dirty)proof.packed_current=false;
            }
            const bool budget=config_.native_stencil_material_cutoff||stats.selected<kStencilMaterialMaximumSamples;
            const PipelineLookup candidate_key{o.Shader(false).OptionalDigest(),o.Shader(true).OptionalDigest(),
                raster_key,target?int32_t(p.renderTargetFormat[0]):-1,-1,StencilMaterialVariant()};
            const auto derived=shaders_.find(ShaderCacheLookup{kStencilMaterialPixel,true,StencilMaterialVariant()});
            const auto resident=pipelines_.find(candidate_key);
            if(budget&&NativeStencilMaterialAllowed(fsi_shared.raster,proof)&&derived!=shaders_.end()&&
               resident!=pipelines_.end()&&resident->second.pipeline) {
#ifdef GOW3_NATIVE_STENCIL_MATERIAL_REPLAY
                if(config_.replay_stencil_material_mode) {
                    if(!draw_slot.stencil_material_counter)draw_slot.stencil_material_counter=std::make_unique<NativeStencilMaterialCounterSlot>(*device_);
                    draw_slot.stencil_material_counter->Prepare(draw_slot.token,o.Event(),o.Frame());
                    descriptors[4]->setBuffer(1,&draw_slot.stencil_material_counter->buffer());
                }
#endif
                p.pixelShader=derived->second.shader.get();pixel_variant=StencilMaterialVariant();
                cache_epoch_.Mark(vs->second);cache_epoch_.Mark(ps->second);cache_epoch_.Mark(derived->second);cache_epoch_.Mark(resident->second);
                stencil_material_selected=true;
            } else {
                ++stats.refusals;stats.query_refusals+=proof.active_query;stats.budget_refusals+=!budget;
                const auto reason=[&](const char* name,bool passed){if(!passed)++stats.refusal_reasons[name];};
                reason("original_modules",proof.original_modules);reason("unmodified_variants",proof.unmodified_variants);
                reason("owned_up_cube",proof.owned_up_cube);reason("no_active_query",!proof.active_query);
                reason("not_discarded",!proof.discarded);reason("not_rectangle",!proof.rectangle);
                reason("primary_surfaces",proof.primary_surfaces);reason("private_framebuffer",proof.private_framebuffer);
                reason("bounded_scissor",proof.bounded_scissor);reason("packed_current",proof.packed_current);
                reason("single_sample_single_view",proof.single_sample_single_view);reason("budget",budget);
                reason("resident_module",derived!=shaders_.end());reason("resident_pipeline",resident!=pipelines_.end()&&resident->second.pipeline);
                auto state_only=proof;state_only.original_modules=state_only.unmodified_variants=state_only.owned_up_cube=true;
                state_only.active_query=state_only.discarded=state_only.rectangle=false;
                state_only.primary_surfaces=state_only.private_framebuffer=state_only.bounded_scissor=state_only.packed_current=state_only.single_sample_single_view=true;
                reason("exact_state",NativeStencilMaterialAllowed(fsi_shared.raster,state_only));
            }
        }
        const PipelineLookup pipeline_key{o.Shader(false).OptionalDigest(),o.Shader(true).OptionalDigest(),
            raster_key,target?int32_t(p.renderTargetFormat[0]):-1,depth?int32_t(p.depthTargetFormat):-1,pixel_variant,vertex_variant,fsi_state};
        bool pinned_null_constants_candidate=false;
        if(config_.pinned_null_constant_uploads) {
            pinned_null_constants_candidate=o.Shader(true).IsNull()&&!o.Shader(false).IsNull()&&
                (o.Shader(false).Digest()==kAtomicDepthVertexSources[0]||o.Shader(false).Digest()==kAtomicDepthVertexSources[1]);
            pinned_null_constants_selected=pinned_null_constants_candidate&&null_constant_vertex&&null_constant_pixel&&
                vertex_variant==NativeShaderVariant::Unmodified&&pixel_variant==NativeShaderVariant::Unmodified;
            upload_constants(pinned_null_constants_selected);
        }
        // Async pipelines: count missing pipelines that stay synchronous.
        if(config_.async_pipelines) {
            const auto resident=pipelines_.find(pipeline_key);
            if(resident==pipelines_.end()||!resident->second.pipeline) {
                if(!async_pipelines_.jobs.contains(pipeline_key)) {
                    if(resident_fsi&&!config_.fsi_state_material_composition)++async_pipelines_.sync_resident_fsi;
                    else if(commands&&draw_timing_scope&&draw_timing_scope->active())++async_pipelines_.sync_recorded;
                    else if(pipeline_key.fsi_state)++async_pipelines_.sync_fsi_state;
                    else if(pipeline_key.pixel_variant!=NativeShaderVariant::Unmodified||pipeline_key.vertex_variant!=NativeShaderVariant::Unmodified)
                        ++async_pipelines_.sync_variant;
                    else if(!config_.native_fsi||!p.vertexShader||!p.pixelShader||!DrawGroupsEnabled())++async_pipelines_.sync_other;
                }
            }
        }
        // Skip the draw while its pipeline builds. Draw groups record nothing
        // before admission; a standalone list holds only this draw's uploads
        // and transitions, which stay valid without the draw call, so it is
        // submitted exactly like a finished draw.
        if(config_.async_pipelines&&DrawGroupsEnabled()&&gpu_work_queue_&&
           !(resident_fsi&&!config_.fsi_state_material_composition)&&
           !(draw_timing_scope&&draw_timing_scope->active())&&!AsyncPipelineReady(pipeline_key,p)) {
            ++async_pipelines_.skipped_draws;
            if(commands) {
                commands->end();
                draw_slot.accepted_upload_bytes=index_upload_bytes+constant_bytes+(rectangle?64:0);
                // Preparation may have staged transfers since the draw's first
                // reservation; reserve again exactly as admission would.
                gpu_work_queue_->Reserve();
                pending_draws_.push_back(&draw_slot);++draw_slot.pending_occurrences;pending_upload_bytes_+=draw_slot.accepted_upload_bytes;
                gpu_work_queue_->EnqueuePinnedDraw(*commands,o.Event(),draw_slot.token,nullptr);
                ++async_pipelines_.skipped_recorded;
                // Same batch target and pressure handling as a finished draw.
                pending_last_event_=o.Event();
                if(gpu_work_queue_->staged_draws()>=config_.max_draw_batch_size)
                    gpu_work_queue_->SubmitPending(NativeGpuWorkQueue::SubmissionReason::DrawBatchTarget);
                if(pending_draws_.size()>=submission_capacity_.lists||pending_upload_bytes_>=16u*1024*1024)RelieveDrawPressure();
            } else {
                // Slots are a ring whose allocated entries must all become
                // pending; return this unused slot to keep that order.
                next_draw_slot_=uint32_t(&draw_slot-draw_slots_.data());
            }
            return;
        }
        bool pipeline_created=false;
        uint32_t diagnostic_pair=0,timing_pair=0;
        auto* diagnostic_pair_out=(config_.gpu_batch_attribution||config_.gpu_physical_timestamps)?&diagnostic_pair:nullptr;
        auto* timing_pair_out=((draw_timing_scope&&draw_timing_scope->active())||
             (config_.multi_draw_command_buffers&&draw_timestamps_&&draw_timestamps_->SamplesFrame(timing_frame)))?&timing_pair:nullptr;
        auto* pipeline=resident_fsi&&!config_.fsi_state_material_composition?
            UseResidentFsiPipeline(pipeline_key,resident_fsi,&pipeline_created,diagnostic_pair_out,timing_pair_out):
            Pipeline(pipeline_key,p,&o,&pipeline_created,diagnostic_pair_out,timing_pair_out);
        bool retained_phase_pipeline_certified=false;
        if(config_.retained_opaque_phase&&retained_opaque) {
            const auto selected=pipelines_.find(pipeline_key);
            if(selected==pipelines_.end()||selected->second.pipeline.get()!=pipeline||!selected->second.audited_opaque_phase||
               selected->second.opaque_phase_vertex!=p.vertexShader||selected->second.opaque_phase_pixel!=p.pixelShader||
               pixel_variant!=NativeShaderVariant::RetainedOpaquePhaseConservativeV4||fsi_state||resident_fsi||early_depth||readonly_depth||
               !RetainedOpaquePhaseDrawAllowed(o,nullptr,true,phase_facts_ptr))
                throw NativeBatchCompletionError("Opaque phase selected pipeline lost its creation-time certificate");
            retained_phase_pipeline_certified=true;
        }
        if(config_.native_early_depth_material_retention&&!early_depth&&early_depth_current_) {
            const auto selected=pipelines_.find(pipeline_key);
            material_pipeline_certified=selected!=pipelines_.end()&&
                selected->second.pipeline.get()==pipeline&&selected->second.audited_monotonic_depth;
            bool retain=false;
            auto& stats=early_depth_material_stats_;++stats.candidates;
            const auto proof=material_depth_proof();
            switch(ClassifyNativeEarlyDepthMaterialRetention(proof)) {
                case NativeEarlyDepthMaterialAdmission::Admitted:++stats.admitted;retain=true;break;
                case NativeEarlyDepthMaterialAdmission::Query:++stats.query_refusals;break;
                case NativeEarlyDepthMaterialAdmission::Module:
                    ++stats.module_refusals;
                    if(detailed_reports_)try {
                        const auto vs=o.Shader(false),ps=o.Shader(true);
                        const std::string_view vertex=vs.IsNull()?std::string_view{}:vs.Digest();
                        const std::string_view pixel=ps.IsNull()?std::string_view{}:ps.Digest();
                        const std::array<bool,9> certificates{
                            bool(proof.vertex_module),bool(proof.pixel_module),
                            bool(proof.certified_vertex_module),bool(proof.certified_pixel_module),
                            proof.vertex_module&&proof.vertex_module==proof.certified_vertex_module,
                            proof.pixel_module&&proof.pixel_module==proof.certified_pixel_module,
                            selected!=pipelines_.end()&&bool(selected->second.pipeline),
                            selected!=pipelines_.end()&&selected->second.pipeline.get()==pipeline,
                            proof.certified_pipeline};
                        const auto state=NativeFsiStateSpecialization::From(proof.raster).words;
                        auto row=std::find_if(early_depth_material_refusals_.begin(),early_depth_material_refusals_.end(),
                            [&](const auto& r){return r.vertex==vertex&&r.pixel==pixel&&r.certificates==certificates&&
                                r.vertex_variant==proof.vertex_variant&&r.pixel_variant==proof.pixel_variant&&r.state==state;});
                        const auto frame=o.Frame();const auto event=o.Event();
                        if(row!=early_depth_material_refusals_.end()) {
                            ++row->count;row->last_frame=frame;row->last_event=event;
                        } else if(early_depth_material_refusals_.size()<64) {
                            early_depth_material_refusals_.push_back({std::string(vertex),std::string(pixel),
                                proof.vertex_variant,proof.pixel_variant,certificates,state,event,event,1,frame,frame});
                        } else ++early_depth_material_refusal_overflow_;
                    } catch(...) {++early_depth_material_refusal_errors_;}
                    break;
                case NativeEarlyDepthMaterialAdmission::Layout:++stats.layout_refusals;break;
                case NativeEarlyDepthMaterialAdmission::State:++stats.state_refusals;break;
                default:++stats.draw_refusals;break;
            }
            if(!retain)InvalidateEarlyDepth();
        }
        // Experimental modules have a separate in-memory key; never write
        // their descriptors under a legacy on-disk generic recipe identity.
        if(pipeline_created&&pixel_variant!=NativeShaderVariant::ConservativeEarlyDepthV1&&pixel_variant!=NativeShaderVariant::ConservativeEarlyDepthCatalogV2&&pixel_variant!=NativeShaderVariant::RetainedFp24DepthWriterV1&&!IsNativeReadonlyDepthVariant(pixel_variant)&&pixel_variant!=NativeShaderVariant::RetainedOpaquePackedAttachmentsV1&&pixel_variant!=NativeShaderVariant::RetainedOpaquePackedCatalogV2&&pixel_variant!=NativeShaderVariant::RetainedOpaquePhaseConservativeV4&&config_.native_fsi&&config_.observe_pipeline&&vertex_variant==NativeShaderVariant::Unmodified&&
           pixel_variant!=NativeShaderVariant::BuiltinReadonlyDepthQueryV2&&
           pixel_variant!=NativeShaderVariant::BuiltinAtomicDepthMaxV1&&
           pixel_variant!=NativeShaderVariant::OpaqueFormat3FullWordOverwriteV1&&
           pixel_variant!=NativeShaderVariant::ForwardOnlyGuestControlFlowV1&&
           pixel_variant!=NativeShaderVariant::ForwardColorPixelCatalogV1&&
           pixel_variant!=NativeShaderVariant::PipelineStateEpilogueV1&&
           pixel_variant!=NativeShaderVariant::PipelineStateForwardCfgV1&&
            pixel_variant!=NativeShaderVariant::ReplayStencilMaterialObserveV1&&
            pixel_variant!=NativeShaderVariant::ReplayStencilMaterialCutoffV1&&
            pixel_variant!=NativeShaderVariant::StencilMaterialCutoffV1&&
            pixel_variant!=NativeShaderVariant::ExactIdentityFsiStoresV1&&
            pixel_variant!=NativeShaderVariant::ImmutableFsiStateHoistV1&&
            pixel_variant!=NativeShaderVariant::ImmutableFsiStateAndIncomingDepthHoistV2)try {
            const auto vertex=o.Shader(false),pixel=o.Shader(true);
            if(vertex.typed&&vertex.typed->owner&&(pixel.IsNull()||(pixel.typed&&pixel.typed->owner))) {
                NativeFsiPipelineRecipe recipe{std::string(vertex.Digest()),pixel.IsNull()?std::nullopt:
                    std::optional<std::string>(pixel.Digest()),std::get<std::array<uint32_t,6>>(raster_key),
                    // The host attachment policy is chosen on startup. Keep
                    // disk records canonical; candidate/fallback PSOs are
                    // separately keyed in memory and share exact guest shaders.
                    int32_t(RenderFormat::R8G8B8A8_UNORM),pipeline_key.depth_format};
                config_.observe_pipeline({std::move(recipe),vertex.typed->owner,pixel.IsNull()?nullptr:pixel.typed->owner});
            }
        }catch(...) {/* Optional disk-cache diagnostics never alter draw authority. */}
        bool color_pass_eligible=false,color_pass_variant8=false,color_pass_blend=false,color_pass_action=false;
        if(config_.packed_color_render_pass_groups&&pixel_shader_present&&!o.Shader(false).IsNull()&&
           RetainedOpaqueCatalogPair(o.Shader(false).Digest(),o.Shader(true).Digest())) {
            ++packed_color_pass_stats_.candidates;
            const auto vs=shaders_.find(ShaderCacheLookup{o.Shader(false).Digest(),false,NativeShaderVariant::Unmodified});
            const auto ps=shaders_.find(ShaderCacheLookup{o.Shader(true).Digest(),true,NativeShaderVariant::Unmodified});
            NativePackedColorPassProof proof;
            proof.audited_pair=vs!=shaders_.end()&&ps!=shaders_.end()&&
                vs->second.shader.get()==p.vertexShader&&ps->second.shader.get()==p.pixelShader&&
                vs->second.audited_packed_color_module&&ps->second.audited_packed_color_module;
            proof.unmodified=vertex_variant==NativeShaderVariant::Unmodified&&pixel_variant==NativeShaderVariant::Unmodified;
            proof.cpu_geometry=packed_pass_cpu_geometry;proof.active_query=queries_->active();
            proof.discarded=state.discard;proof.rectangle=rectangle;
            proof.private_framebuffer=target&&framebuffer==fsi_framebuffer_.get()&&target==fsi_attachment_.get()&&!depth&&!attachmentless_fsi_active_;
            proof.bounded_scissor=state.scissor.left>=0&&state.scissor.top>=0&&state.scissor.right<=1280&&state.scissor.bottom<=720;
            proof.primary_surfaces=attachments[0].identity&&attachments[4].identity&&
                !attachments[1].identity&&!attachments[2].identity&&!attachments[3].identity;
            if(proof.primary_surfaces) {
                const auto& c=storage(0);const auto& d=storage(4);
                proof.primary_surfaces=c.width==1280&&c.height==720&&c.pitch==1280&&c.base==720&&c.format==3&&!c.depth&&
                    d.width==1280&&d.height==720&&d.pitch==1280&&d.base==0&&d.format==1&&d.depth&&
                    !(attachments[0].identity.Info()&0x30000)&&!(attachments[4].identity.Info()&0x30000)&&BE(raw,0x28dc)==15;
            }
            color_pass_eligible=NativePackedColorPassAllowed(fsi_shared.raster,proof);
            if(color_pass_eligible&&draw_timestamps_&&draw_timestamps_->SamplesFrame(timing_frame)) {
                color_pass_eligible=false;++packed_color_pass_stats_.timestamp_refusals;
            }
            if(color_pass_eligible)++packed_color_pass_stats_.eligible;
            else {++packed_color_pass_stats_.refusals;packed_color_pass_stats_.query_refusals+=proof.active_query;}
        } else if(config_.packed_color_render_pass_groups&&pixel_shader_present&&!o.Shader(false).IsNull()&&
           vertex_variant==NativeShaderVariant::Unmodified&&pixel_variant==NativeShaderVariant::PipelineStateEpilogueV1&&
           fsi_state&&std::holds_alternative<std::array<uint32_t,6>>(pipeline_key.raster)&&
           NativePackedColorVariant8Tuple(o.Shader(false).Digest(),o.Shader(true).Digest(),
               std::get<std::array<uint32_t,6>>(pipeline_key.raster),*fsi_state,pipeline_key.color_format,pipeline_key.depth_format)) {
            ++packed_color_pass_stats_.candidates;++packed_color_pass_stats_.variant8_candidates;
            color_pass_variant8=true;
            NativePackedColorVariant8PassProof proof;proof.exact_tuple=true;
            proof.vertex_module=p.vertexShader;proof.pixel_module=p.pixelShader;proof.pipeline=pipeline;
            proof.vertex_variant=vertex_variant;proof.pixel_variant=pixel_variant;
            const auto vs=shaders_.find(ShaderCacheLookup{o.Shader(false).Digest(),false,vertex_variant});
            const auto ps=shaders_.find(ShaderCacheLookup{o.Shader(true).Digest(),true,pixel_variant});
            const auto selected=pipelines_.find(pipeline_key);
            if(vs!=shaders_.end()&&vs->second.audited_monotonic_depth)proof.certified_vertex_module=vs->second.shader.get();
            if(ps!=shaders_.end()&&ps->second.audited_monotonic_depth)proof.certified_pixel_module=ps->second.shader.get();
            if(selected!=pipelines_.end()&&selected->second.audited_monotonic_depth)proof.certified_pipeline=selected->second.pipeline.get();
            packed_color_pass_stats_.variant8_module_refusals+=!proof.vertex_module||!proof.pixel_module||
                proof.vertex_module!=proof.certified_vertex_module||proof.pixel_module!=proof.certified_pixel_module;
            packed_color_pass_stats_.variant8_pipeline_refusals+=!proof.pipeline||proof.pipeline!=proof.certified_pipeline;
            proof.cpu_geometry=packed_pass_cpu_geometry;
            proof.indexed_triangles=function=="DrawIndexedPrimitive"&&primitive==4&&
                state.pipeline.primitiveTopology==RenderPrimitiveTopology::TRIANGLE_LIST;
            proof.active_query=queries_->active();proof.discarded=state.discard;proof.rectangle=rectangle;
            proof.private_framebuffer=target&&framebuffer==fsi_framebuffer_.get()&&target==fsi_attachment_.get()&&!depth&&!attachmentless_fsi_active_;
            proof.bounded_scissor=state.scissor.left>=0&&state.scissor.top>=0&&state.scissor.right<=1280&&state.scissor.bottom<=720&&
                state.scissor.right>state.scissor.left&&state.scissor.bottom>state.scissor.top;
            proof.single_sample_layer=p.multisampling.sampleCount==1&&!p.multisampling.sampleLocationsEnabled&&!p.alphaToCoverageEnabled&&
                target&&static_cast<VulkanTexture*>(target)->desc.arraySize==1&&static_cast<VulkanTexture*>(target)->desc.multisampling.sampleCount==1;
            proof.primary_surfaces=attachments[0].identity&&attachments[4].identity&&
                !attachments[1].identity&&!attachments[2].identity&&!attachments[3].identity&&extent_width==1280&&extent_height==720;
            if(proof.primary_surfaces) {
                const auto& c=storage(0);const auto& d=storage(4);
                proof.primary_surfaces=c.width==1280&&c.height==720&&c.pitch==1280&&c.base==720&&c.format==3&&!c.depth&&
                    d.width==1280&&d.height==720&&d.pitch==1280&&d.base==0&&d.format==1&&d.depth&&
                    !(attachments[0].identity.Info()&0x30000)&&!(attachments[4].identity.Info()&0x30000)&&BE(raw,0x28dc)==15;
            }
            color_pass_eligible=NativePackedColorVariant8PassAllowed(fsi_shared.raster,proof);
            if(color_pass_eligible&&draw_timestamps_&&draw_timestamps_->SamplesFrame(timing_frame)) {
                color_pass_eligible=false;++packed_color_pass_stats_.timestamp_refusals;
            }
            if(color_pass_eligible) {++packed_color_pass_stats_.eligible;++packed_color_pass_stats_.variant8_eligible;}
            else {++packed_color_pass_stats_.refusals;packed_color_pass_stats_.query_refusals+=proof.active_query;}
        } else if(config_.packed_color_action_pass_groups&&pixel_shader_present&&!o.Shader(false).IsNull()&&
           vertex_variant==NativeShaderVariant::Unmodified&&pixel_variant==NativeShaderVariant::PipelineStateEpilogueV1&&
           fsi_state&&std::holds_alternative<std::array<uint32_t,6>>(pipeline_key.raster)&&
           NativePackedColorActionVariant8Tuple(o.Shader(false).Digest(),o.Shader(true).Digest(),
               std::get<std::array<uint32_t,6>>(pipeline_key.raster),*fsi_state,pipeline_key.color_format,pipeline_key.depth_format)) {
            ++packed_color_pass_stats_.candidates;++packed_color_action_pass_stats_.candidates;
            color_pass_action=true;
            NativePackedColorVariant8PassProof proof;proof.exact_tuple=true;
            proof.vertex_module=p.vertexShader;proof.pixel_module=p.pixelShader;proof.pipeline=pipeline;
            proof.vertex_variant=vertex_variant;proof.pixel_variant=pixel_variant;
            const auto vs=shaders_.find(ShaderCacheLookup{o.Shader(false).Digest(),false,vertex_variant});
            const auto ps=shaders_.find(ShaderCacheLookup{o.Shader(true).Digest(),true,pixel_variant});
            const auto selected=pipelines_.find(pipeline_key);
            if(vs!=shaders_.end()&&vs->second.audited_packed_color_action)proof.certified_vertex_module=vs->second.shader.get();
            if(ps!=shaders_.end()&&ps->second.audited_packed_color_action)proof.certified_pixel_module=ps->second.shader.get();
            if(selected!=pipelines_.end()&&selected->second.audited_packed_color_action)proof.certified_pipeline=selected->second.pipeline.get();
            packed_color_action_pass_stats_.module_refusals+=!proof.vertex_module||!proof.pixel_module||
                proof.vertex_module!=proof.certified_vertex_module||proof.pixel_module!=proof.certified_pixel_module;
            packed_color_action_pass_stats_.pipeline_refusals+=!proof.pipeline||proof.pipeline!=proof.certified_pipeline;
            proof.cpu_geometry=packed_pass_cpu_geometry;
            proof.indexed_triangles=function=="DrawIndexedPrimitive"&&primitive==4&&
                state.pipeline.primitiveTopology==RenderPrimitiveTopology::TRIANGLE_LIST;
            proof.active_query=queries_->active();proof.discarded=state.discard;proof.rectangle=rectangle;
            proof.private_framebuffer=target&&framebuffer==fsi_framebuffer_.get()&&target==fsi_attachment_.get()&&!depth&&!attachmentless_fsi_active_;
            proof.bounded_scissor=state.scissor.left>=0&&state.scissor.top>=0&&state.scissor.right<=1280&&state.scissor.bottom<=720&&
                state.scissor.right>state.scissor.left&&state.scissor.bottom>state.scissor.top;
            proof.single_sample_layer=p.multisampling.sampleCount==1&&!p.multisampling.sampleLocationsEnabled&&!p.alphaToCoverageEnabled&&
                target&&static_cast<VulkanTexture*>(target)->desc.arraySize==1&&static_cast<VulkanTexture*>(target)->desc.multisampling.sampleCount==1;
            proof.primary_surfaces=attachments[0].identity&&attachments[4].identity&&
                !attachments[1].identity&&!attachments[2].identity&&!attachments[3].identity&&extent_width==1280&&extent_height==720;
            if(proof.primary_surfaces) {
                const auto& c=storage(0);const auto& d=storage(4);
                proof.primary_surfaces=c.width==1280&&c.height==720&&c.pitch==1280&&c.base==720&&c.format==3&&!c.depth&&
                    d.width==1280&&d.height==720&&d.pitch==1280&&d.base==0&&d.format==1&&d.depth&&
                    !(attachments[0].identity.Info()&0x30000)&&!(attachments[4].identity.Info()&0x30000)&&BE(raw,0x28dc)==15;
            }
            color_pass_eligible=NativePackedColorActionVariant8PassAllowed(config_.packed_color_action_pass_groups,fsi_shared.raster,proof);
            if(color_pass_eligible&&draw_timestamps_&&draw_timestamps_->SamplesFrame(timing_frame)) {
                color_pass_eligible=false;++packed_color_pass_stats_.timestamp_refusals;++packed_color_action_pass_stats_.timestamp_refusals;
            }
            if(color_pass_eligible) {++packed_color_pass_stats_.eligible;++packed_color_action_pass_stats_.eligible;}
            else {++packed_color_pass_stats_.refusals;packed_color_pass_stats_.query_refusals+=proof.active_query;
                ++packed_color_action_pass_stats_.refusals;packed_color_action_pass_stats_.query_refusals+=proof.active_query;}
        } else if(const auto blend_recipe=config_.packed_color_blend_pass_groups&&pixel_shader_present&&!o.Shader(false).IsNull()&&
           vertex_variant==NativeShaderVariant::Unmodified&&pixel_variant==NativeShaderVariant::PipelineStateEpilogueV1&&
           fsi_state&&std::holds_alternative<std::array<uint32_t,6>>(pipeline_key.raster)?
           NativePackedColorBlendVariant8RecipeIndex(o.Shader(false).Digest(),o.Shader(true).Digest(),
               std::get<std::array<uint32_t,6>>(pipeline_key.raster),*fsi_state,pipeline_key.color_format,pipeline_key.depth_format):
           std::optional<size_t>{};blend_recipe) {
            ++packed_color_pass_stats_.candidates;++packed_color_blend_pass_stats_.candidates;
            color_pass_blend=true;
            NativePackedColorVariant8PassProof proof;proof.exact_tuple=true;
            proof.vertex_module=p.vertexShader;proof.pixel_module=p.pixelShader;proof.pipeline=pipeline;
            proof.vertex_variant=vertex_variant;proof.pixel_variant=pixel_variant;
            const auto vs=shaders_.find(ShaderCacheLookup{o.Shader(false).Digest(),false,vertex_variant});
            const auto ps=shaders_.find(ShaderCacheLookup{o.Shader(true).Digest(),true,pixel_variant});
            const auto selected=pipelines_.find(pipeline_key);
            if(vs!=shaders_.end()&&vs->second.audited_packed_color_blend)proof.certified_vertex_module=vs->second.shader.get();
            if(ps!=shaders_.end()&&ps->second.audited_packed_color_blend)proof.certified_pixel_module=ps->second.shader.get();
            if(selected!=pipelines_.end()&&selected->second.audited_packed_color_blend)proof.certified_pipeline=selected->second.pipeline.get();
            packed_color_blend_pass_stats_.module_refusals+=!proof.vertex_module||!proof.pixel_module||
                proof.vertex_module!=proof.certified_vertex_module||proof.pixel_module!=proof.certified_pixel_module;
            packed_color_blend_pass_stats_.pipeline_refusals+=!proof.pipeline||proof.pipeline!=proof.certified_pipeline;
            proof.cpu_geometry=packed_pass_cpu_geometry;
            proof.indexed_triangles=function=="DrawIndexedPrimitive"&&primitive==4&&
                state.pipeline.primitiveTopology==RenderPrimitiveTopology::TRIANGLE_LIST;
            proof.active_query=queries_->active();proof.discarded=state.discard;proof.rectangle=rectangle;
            proof.private_framebuffer=target&&framebuffer==fsi_framebuffer_.get()&&target==fsi_attachment_.get()&&!depth&&!attachmentless_fsi_active_;
            proof.bounded_scissor=state.scissor.left>=0&&state.scissor.top>=0&&state.scissor.right<=1280&&state.scissor.bottom<=720&&
                state.scissor.right>state.scissor.left&&state.scissor.bottom>state.scissor.top;
            proof.single_sample_layer=p.multisampling.sampleCount==1&&!p.multisampling.sampleLocationsEnabled&&!p.alphaToCoverageEnabled&&
                target&&static_cast<VulkanTexture*>(target)->desc.arraySize==1&&static_cast<VulkanTexture*>(target)->desc.multisampling.sampleCount==1;
            proof.primary_surfaces=attachments[0].identity&&attachments[4].identity&&
                !attachments[1].identity&&!attachments[2].identity&&!attachments[3].identity&&extent_width==1280&&extent_height==720;
            if(proof.primary_surfaces) {
                const auto& c=storage(0);const auto& d=storage(4);
                proof.primary_surfaces=c.width==1280&&c.height==720&&c.pitch==1280&&c.base==720&&c.format==3&&!c.depth&&
                    d.width==1280&&d.height==720&&d.pitch==1280&&d.base==0&&d.format==1&&d.depth&&
                    !(attachments[0].identity.Info()&0x30000)&&!(attachments[4].identity.Info()&0x30000)&&BE(raw,0x28dc)==15;
            }
            color_pass_eligible=NativePackedColorBlendVariant8PassAllowed(fsi_shared.raster,proof,*blend_recipe);
            if(color_pass_eligible&&draw_timestamps_&&draw_timestamps_->SamplesFrame(timing_frame)) {
                color_pass_eligible=false;++packed_color_pass_stats_.timestamp_refusals;++packed_color_blend_pass_stats_.timestamp_refusals;
            }
            if(color_pass_eligible) {++packed_color_pass_stats_.eligible;++packed_color_blend_pass_stats_.eligible;}
            else {++packed_color_pass_stats_.refusals;packed_color_pass_stats_.query_refusals+=proof.active_query;
                ++packed_color_blend_pass_stats_.refusals;packed_color_blend_pass_stats_.query_refusals+=proof.active_query;}
        }
        bool packed_pass_eligible=false;
        const bool packed_pass_atomic=pixel_variant==NativeShaderVariant::BuiltinAtomicDepthMaxV1;
        if(config_.packed_depth_render_pass_groups&&!pixel_shader_present&&!o.Shader(false).IsNull()&&
           (o.Shader(false).Digest()==kAtomicDepthVertexSources[0]||o.Shader(false).Digest()==kAtomicDepthVertexSources[1])) {
            ++packed_depth_pass_stats_.candidates;
            const auto vs=shaders_.find(ShaderCacheLookup{o.Shader(false).Digest(),false,NativeShaderVariant::Unmodified});
            const auto ps=shaders_.find(ShaderCacheLookup{"depth-only",true,packed_pass_atomic?
                NativeShaderVariant::BuiltinAtomicDepthMaxV1:NativeShaderVariant::Unmodified});
            NativePackedDepthPassProof proof;
            proof.width=extent_width;proof.height=extent_height;proof.surface_info=extent.Info();proof.raw_color_mask=BE(raw,0x28dc);
            proof.pixel_shader=pixel_shader_present;proof.active_query=queries_->active();proof.discarded=state.discard;proof.rectangle=rectangle;
            proof.audited_vertex=vs!=shaders_.end()&&vs->second.shader.get()==p.vertexShader&&vs->second.audited_atomic_vertex;
            proof.audited_pixel=ps!=shaders_.end()&&ps->second.shader.get()==p.pixelShader&&ps->second.audited_packed_depth_pixel;
            proof.cpu_geometry=packed_pass_cpu_geometry;proof.texture_free=packed_pass_texture_free;
            proof.unmodified_variants=vertex_variant==NativeShaderVariant::Unmodified&&pixel_variant==NativeShaderVariant::Unmodified;
            proof.atomic_depth_variant=config_.builtin_atomic_depth&&vertex_variant==NativeShaderVariant::Unmodified&&packed_pass_atomic;
            proof.private_color_only_framebuffer=target&&framebuffer==fsi_framebuffer_.get()&&target==fsi_attachment_.get()&&!depth&&!attachmentless_fsi_active_;
            proof.bounded_scissor=state.scissor.left>=0&&state.scissor.top>=0&&state.scissor.right<=1280&&state.scissor.bottom<=720;
            packed_pass_eligible=NativePackedDepthPassAllowed(fsi_shared.raster,proof);
            const bool timestamp=draw_timestamps_&&draw_timestamps_->SamplesFrame(timing_frame);
            if(packed_pass_eligible&&timestamp) {packed_pass_eligible=false;++packed_depth_pass_stats_.timestamp_refusals;}
            if(packed_pass_eligible)++packed_depth_pass_stats_.eligible;
            else {++packed_depth_pass_stats_.refusals;packed_depth_pass_stats_.query_refusals+=proof.active_query;}
        }
        // The resident D32 classifier already proves the exact vertex/fragment
        // pair. Neither module accesses packed memory, and this framebuffer's
        // LOAD+STORE_NONE attachment is immutable. Retain each query's own slot.
        const bool readonly_pass_eligible=config_.readonly_query_render_pass_groups&&
            queries_->host_reset_enabled()&&queries_->active()&&!state.discard&&!rectangle&&
            readonly_depth&&readonly_depth_ticket&&readonly_depth_queried&&readonly_depth->read_only_pass&&
            framebuffer==readonly_depth->framebuffer.get()&&depth==readonly_depth->image.get()&&!target&&
            vertex_variant==NativeShaderVariant::Unmodified&&pixel_variant==NativeShaderVariant::NativeReadonlyDepthD32LoadV2&&
            packed_pass_cpu_geometry&&packed_pass_texture_free&&
            !(draw_timestamps_&&draw_timestamps_->SamplesFrame(timing_frame));
        if(readonly_pass_eligible)++readonly_query_pass_stats_.eligible;
        // Phase admission above already authenticated actual modules, PSO,
        // raster and authority. A pass additionally belongs to one logical
        // generation, even when successive generations reuse physical images.
        const NativeRetainedOpaquePhasePassIdentity phase_identity{
            retained_opaque.get(),retained_opaque_ticket?retained_opaque_ticket->generation:0,
            framebuffer,target,depth};
        bool phase_pass_eligible=config_.retained_opaque_phase_render_pass_groups&&
            NativeRetainedOpaquePhasePassAllowed({phase_identity,retained_phase_pipeline_certified,
                bool(retained_opaque)&&framebuffer==retained_opaque->Framebuffer()&&
                    target==retained_opaque->color.get()&&depth==retained_opaque->depth.get(),
                vertex_variant==NativeShaderVariant::Unmodified&&pixel_variant==NativeShaderVariant::RetainedOpaquePhaseConservativeV4,
                queries_->active(),state.discard,rectangle});
        if(phase_pass_eligible&&draw_timestamps_&&draw_timestamps_->SamplesFrame(timing_frame)) {
            phase_pass_eligible=false;++retained_phase_pass_stats_.timestamp_refusals;
        }
        if(phase_pass_eligible)++retained_phase_pass_stats_.eligible;
        const bool phase_generation_changed=open_retained_phase_pass_&&phase_pass_eligible&&
            (open_retained_phase_owner_.lock()!=retained_opaque||
             !NativeRetainedOpaquePhasePassContinues(open_retained_phase_identity_,phase_identity));
        if(phase_generation_changed)++retained_phase_pass_stats_.generation_boundaries;
        bool phase_attachment_state_changed=false;
        if(open_retained_phase_pass_&&phase_pass_eligible) {
            const auto& c=static_cast<const VulkanTexture&>(*target);
            const auto& d=static_cast<const VulkanTexture&>(*depth);
            phase_attachment_state_changed=c.textureLayout!=RenderTextureLayout::COLOR_WRITE||
                d.textureLayout!=RenderTextureLayout::DEPTH_WRITE||
                c.barrierStages!=RenderBarrierStage::GRAPHICS||d.barrierStages!=RenderBarrierStage::GRAPHICS;
            if(phase_attachment_state_changed)++retained_phase_pass_stats_.attachment_state_boundaries;
        }
        bool fsi_pass_eligible=false,fsi_pass_boundary=false;
        NativeFsiPassPlanes fsi_planes,fsi_merged_planes;
        if(config_.fsi_draw_pass_groups&&!packed_pass_eligible&&!color_pass_eligible&&!readonly_pass_eligible&&!phase_pass_eligible) {
            ++fsi_draw_pass_stats_.candidates;
            bool msaa=false;
            for(const auto& attachment:attachments)msaa|=attachment.identity&&(attachment.identity.Info()&0x30000u);
            const bool shape=target&&framebuffer==fsi_framebuffer_.get()&&target==fsi_attachment_.get()&&!depth&&
                !attachmentless_fsi_active_&&!retained_opaque&&!stencil_material_selected&&!msaa&&
                pixel_variant!=NativeShaderVariant::BuiltinAtomicDepthMaxV1&&packed_pass_cpu_geometry&&
                // Host reset records no command, so a query slot can begin and
                // end inside the shared subpass like any other draw.
                (!queries_->active()||(config_.fsi_draw_pass_queries&&queries_->host_reset_enabled()))&&
                !state.discard&&!rectangle&&
                !(draw_timestamps_&&draw_timestamps_->SamplesFrame(timing_frame));
            if(!shape)++fsi_draw_pass_stats_.shape_refusals;
            else if(!NativeFsiPassPlanesFor(fsi_shared.raster,extent_height,fsi_planes))++fsi_draw_pass_stats_.plane_refusals;
            else {
                fsi_pass_eligible=true;++fsi_draw_pass_stats_.eligible;
                if(open_fsi_draw_pass_&&!NativeFsiPassJoin(open_fsi_draw_planes_,fsi_planes,fsi_merged_planes)) {
                    fsi_pass_boundary=true;++fsi_draw_pass_stats_.boundaries;
                }
            }
        }
        // Different cohorts or immutable image owners can never share a pass.
        const bool packed_access_changed=open_packed_depth_pass_&&packed_pass_eligible&&
            open_packed_depth_atomic_!=packed_pass_atomic;
        if(packed_access_changed)++packed_depth_pass_stats_.access_model_boundaries;
        if((open_packed_color_pass_&&!color_pass_eligible)||
           (open_packed_depth_pass_&&(!packed_pass_eligible||packed_access_changed))||
           (open_readonly_query_pass_&&(!readonly_pass_eligible||open_readonly_query_owner_.lock()!=readonly_depth))||
           (open_retained_phase_pass_&&(!phase_pass_eligible||phase_generation_changed||phase_attachment_state_changed))||
           (open_fsi_draw_pass_&&(!fsi_pass_eligible||fsi_pass_boundary)))
            SealDrawCommandGroup();
        // A nonmember must close an accepted pass before any new commands are
        // recorded. Resource preparation above may already have closed it.
        if((config_.packed_depth_render_pass_groups||config_.packed_color_render_pass_groups||config_.readonly_query_render_pass_groups||
            config_.retained_opaque_phase_render_pass_groups||config_.fsi_draw_pass_groups)&&
           !packed_pass_eligible&&!readonly_pass_eligible&&!color_pass_eligible&&!phase_pass_eligible&&!fsi_pass_eligible)BeginStandalone();
        if(stencil_material_selected)BeginStandalone();
        // Every resource conversion, resize/cache eviction, CPU readback and
        // capacity wait finishes before an unaccepted child touches a leader.
        if(DrawGroupsEnabled())gpu_work_queue_->Reserve();
        bool grouped_draw=false,packed_pass_continued=false,readonly_pass_continued=false,color_pass_continued=false,phase_pass_continued=false;
        // Hardware render targets: a grouped draw into the same framebuffer
        // continues the active render pass. Rasterization order already orders
        // attachment writes within one render pass instance.
        bool hardware_pass_continued=false,fsi_pass_continued=false;
        if(phase_pass_eligible&&commands)++retained_phase_pass_stats_.preparation_refusals;
        if(color_pass_eligible&&commands)++packed_color_pass_stats_.preparation_refusals;
        if(color_pass_blend&&color_pass_eligible&&commands)++packed_color_blend_pass_stats_.preparation_refusals;
        if(color_pass_action&&color_pass_eligible&&commands)++packed_color_action_pass_stats_.preparation_refusals;
        if(config_.packed_depth_render_pass_groups&&packed_pass_eligible&&commands)
            ++packed_depth_pass_stats_.preparation_refusals;
        if(readonly_pass_eligible&&commands)++readonly_query_pass_stats_.preparation_refusals;
        struct GroupRecordingGuard {
            State& owner;bool recording=false,accepted=false;
            ~GroupRecordingGuard() noexcept {
                if(recording&&!accepted) {
                    // An accepted leader now contains an unaccepted tail (and
                    // possibly an unmatched query). Never seal/submit/reset it.
                    // Latch before earlier preparing owners begin unwinding.
                    owner.batch_failed_=owner.draw_group_recording_failed_=true;
                    owner.gpu_work_queue_->FailDrawCommandGroupRecording();
                }
            }
        } group_recording_guard{*this};
        if(DrawGroupsEnabled()&&!commands) {
            bool unchanged=true;
            for(uint32_t i=0;i<deferred_readonly_count;++i) {
                const auto& image=static_cast<const VulkanTexture&>(*deferred_readonly_barriers[i]);
                unchanged=unchanged&&!NativeSampleBarrierRequired(image.textureLayout,image.barrierStages);
            }
            if(!unchanged) {
                ++group_readonly_changed_;
                if(color_pass_blend&&color_pass_eligible)++packed_color_blend_pass_stats_.preparation_refusals;
                if(color_pass_action&&color_pass_eligible)++packed_color_action_pass_stats_.preparation_refusals;
                if(phase_pass_eligible)++retained_phase_pass_stats_.preparation_refusals;
                BeginStandalone();
            }
            else {
                if(!open_draw_command_group_) {
                    if(!draw_slot.commands)draw_slot.commands=queue_->createCommandList();
                    gpu_work_queue_->ValidateDrawCommandGroupAdmission(*draw_slot.commands,draw_slot.token);
                    BeginRecording(*draw_slot.commands);open_draw_command_group_=draw_slot.commands.get();
                } else gpu_work_queue_->ValidateDrawCommandGroupAdmission(*open_draw_command_group_,draw_slot.token);
                commands=open_draw_command_group_;
                if(config_.packed_depth_render_pass_groups||config_.packed_color_render_pass_groups||config_.readonly_query_render_pass_groups||
                   config_.retained_opaque_phase_render_pass_groups||config_.fsi_draw_pass_groups) {
                    auto& vk_commands=static_cast<VulkanCommandList&>(*commands);
                    fsi_pass_continued=open_fsi_draw_pass_;
                    color_pass_continued=open_packed_color_pass_;
                    packed_pass_continued=open_packed_depth_pass_;
                    readonly_pass_continued=open_readonly_query_pass_;
                    phase_pass_continued=open_retained_phase_pass_;
                    if((packed_pass_continued||readonly_pass_continued||color_pass_continued||phase_pass_continued||fsi_pass_continued)&&(vk_commands.activeRenderPass==VK_NULL_HANDLE||
                       vk_commands.targetFramebuffer!=framebuffer))
                        throw NativeBatchCompletionError("Packed depth group lost its exact active framebuffer");
                    if(!packed_pass_eligible&&!readonly_pass_eligible&&!color_pass_eligible&&!phase_pass_eligible&&!fsi_pass_eligible)throw NativeBatchCompletionError("Unaudited draw entered Native pass group");
                }
                if(!config_.native_fsi) {
                    const auto& vk_commands=static_cast<const VulkanCommandList&>(*commands);
                    // The open instance began with these exact attachments in
                    // attachment layouts; any tracked transition since then
                    // (even from another list) must take the full path.
                    const auto attachment_layout=[](const RenderTexture* image,RenderTextureLayout layout) {
                        if(!image)return true;
                        const auto& vk_image=static_cast<const VulkanTexture&>(*image);
                        return vk_image.textureLayout==layout&&vk_image.barrierStages==RenderBarrierStage::GRAPHICS;
                    };
                    hardware_pass_continued=vk_commands.activeRenderPass!=VK_NULL_HANDLE&&
                        vk_commands.targetFramebuffer==static_cast<const VulkanFramebuffer*>(framebuffer)&&
                        attachment_layout(target,RenderTextureLayout::COLOR_WRITE)&&
                        attachment_layout(depth,RenderTextureLayout::DEPTH_WRITE);
                    if(hardware_pass_continued)++hardware_pass_stats_.continued;
                    else ++hardware_pass_stats_.started;
                }
                grouped_draw=true;draw_group_recording_=true;group_recording_guard.recording=true;
                BeginTiming();
                if(color_pass_continued||phase_pass_continued||hardware_pass_continued||fsi_pass_continued) {
                    // All sampled images are separately owned immutable generations.
                    // Rechecked after all conversions and capacity waits above:
                    // exact GRAPHICS/SHADER_READ introduces no write or transition.
                    if(fsi_pass_continued)fsi_draw_pass_stats_.sampled_barriers_avoided+=deferred_readonly_count;
                    else if(phase_pass_continued)retained_phase_pass_stats_.sampled_barriers_avoided+=deferred_readonly_count;
                    else packed_color_pass_stats_.sampled_barriers_avoided+=deferred_readonly_count;
                    draw_barriers_.sampled_emitted-=deferred_readonly_count;
                    draw_barriers_.sampled_skipped+=deferred_readonly_count;
                    deferred_readonly_count=0;
                } else EmitDeferredReadonly();
            }
        }
        // The selected exact VS reads only owned constants/vertex uploads;
        // the D32 PS reads only owned raster constants. Fixed-function depth
        // tests use the separately retained depth image.
        // Both immutable modules have no packed-EDRAM descriptor/access. Keep
        // every depth-image LOAD+STORE dependency and conversion-source barrier.
        // A skipped buffer has no GRAPHICS access, so never advance its tracked
        // stage here: the next real packed-buffer consumer needs that producer.
        const bool skip_readonly_depth_edram_barrier=config_.native_readonly_depth_skip_edram_barrier&&
            pixel_variant!=NativeShaderVariant::NativeReadonlyDepthLowerBoundQueryV1&&
            !state.discard&&readonly_depth&&readonly_depth_ticket&&readonly_depth_queried&&
            vertex_variant==NativeShaderVariant::Unmodified&&
            IsNativeReadonlyDepthVariant(pixel_variant);
        bool precise_barrier=false;
        if(!packed_pass_continued&&!color_pass_continued&&!fsi_pass_continued&&config_.precise_fsi_barriers&&config_.native_fsi&&target&&!depth&&!retained_opaque) {
            auto& vk_commands=static_cast<VulkanCommandList&>(*commands);
            auto& vk_edram=static_cast<VulkanBuffer&>(edram_memory_->storage_buffer());
            auto& vk_target=static_cast<VulkanTexture&>(*target);
            // Scene's only GRAPHICS user of this buffer is the set4 FSI pixel
            // helper. Its dummy attachment is only loaded/stored by color output.
            // Every compute/copy use changes tracking and takes the full fallback.
            if(NativePreciseFsiBarrierAllowed({true,vk_commands.activeRenderPass!=VK_NULL_HANDLE,
                    vk_edram.barrierStages,vk_target.barrierStages,vk_target.textureLayout})) {
                const NativeFsiBarrierPlan plan(vk_edram.vk,vk_edram.desc.size,vk_target.vk,
                    vk_target.desc.mipLevels,vk_target.desc.arraySize);
                vkCmdPipelineBarrier(vk_commands.vk,plan.stages,plan.stages,plan.dependencies,
                    0,nullptr,1,&plan.edram,1,&plan.target);
                // Metadata already has these exact values. Preserve the normal
                // Plume tracking contract for every subsequent transfer/barrier.
                vk_edram.barrierStages=vk_target.barrierStages=RenderBarrierStage::GRAPHICS;
                vk_target.textureLayout=RenderTextureLayout::COLOR_WRITE;
                precise_barrier=true;++draw_barriers_.precise_fsi;
            }
        }
        if(config_.precise_fsi_barriers&&!precise_barrier)++draw_barriers_.precise_fallback;
        if(hardware_pass_continued) {
            // Same framebuffer, same render pass instance: attachments are
            // already in their attachment layouts with no intervening command.
        } else if(phase_pass_continued) {
            // Exact same R32 color and D32 depth attachments remain writable
            // in the same subpass. No EDRAM descriptor or alias is introduced;
            // all per-draw shader, viewport/scissor and binding updates remain.
            // First members and every sealed boundary keep original barriers.
        } else if(readonly_pass_continued) {
            // The same immutable attachment remains in the same read-only
            // subpass. No packed buffer or sampled image is accessed by this
            // audited pair. Host reset left this draw's query slot unavailable.
            ++readonly_depth_read_barriers_avoided_;
        } else if(color_pass_continued) {
            // Both packed planes have the same injective pixel mapping for
            // every member, with no overlap between color and depth. The
            // unchanged ordered interlocks and shader barriers protect all
            // accesses. Immutable sampled images cannot alias packed storage.
        } else if(packed_pass_continued) {
            // Same subpass + same bijective pixel/word mapping and access model.
            // Original interlock retains shader barriers. Atomic-only members
            // commute as unsigned maxima with an invariant stencil byte. No
            // non-atomic storage access can join that group; its boundaries
            // retain the original full-buffer dependencies.
        } else if(fsi_pass_continued) {
            // Same subpass, same pitch and an injective pixel/word mapping
            // across all pass planes: ordered interlocks and the unchanged
            // shader barriers order every EDRAM access. Sampled images were
            // verified to need no transition at admission.
        } else if(precise_barrier) {
            // The exact fragment/attachment dependency above is complete.
        } else if(config_.coalesce_draw_barriers&&config_.native_fsi&&target&&!retained_opaque) {
            // These two barriers were adjacent with no intervening commands.
            // Both resources are distinct, destination stages are identical,
            // and the array overload unions their original source stages and
            // preserves each access mask/layout transition. Do not move the
            // earlier sampled-image barriers across texture preparation: a
            // later cache miss may submit conversion or readback work.
            commands->barriers(RenderBarrierStage::GRAPHICS,
                RenderBufferBarrier(&edram_memory_->storage_buffer(),RenderBufferAccess::READ|RenderBufferAccess::WRITE),
                RenderTextureBarrier(target,RenderTextureLayout::COLOR_WRITE));
            ++draw_barriers_.merged;
        } else {
            ++draw_barriers_.separate;
            if(target)commands->barriers(RenderBarrierStage::GRAPHICS,RenderTextureBarrier(target,RenderTextureLayout::COLOR_WRITE));
            if(depth) {
                const bool immutable_read=readonly_depth&&readonly_depth->read_only_pass;
                const auto& image=static_cast<const VulkanTexture&>(*depth);
                // Conversion already publishes COPY writes to GRAPHICS. This
                // generation has no attachment writes (including stores), so
                // consecutive reads need no read-to-read dependency. Any other
                // tracked use retains the complete ordinary transition.
                if(immutable_read&&image.textureLayout==RenderTextureLayout::DEPTH_READ&&
                   image.barrierStages==RenderBarrierStage::GRAPHICS)++readonly_depth_read_barriers_avoided_;
                else commands->barriers(RenderBarrierStage::GRAPHICS,RenderTextureBarrier(depth,
                    immutable_read?RenderTextureLayout::DEPTH_READ:RenderTextureLayout::DEPTH_WRITE));
            }
            if(config_.native_fsi&&!skip_readonly_depth_edram_barrier&&!retained_opaque) {
                if(pixel_variant==NativeShaderVariant::NativeReadonlyDepthLowerBoundQueryV1&&readonly_depth&&readonly_depth_ticket) {
                    auto& buffer=static_cast<VulkanBuffer&>(edram_memory_->storage_buffer());
                    const bool same_read=readonly_query_read_owner_.lock()==readonly_depth&&
                        readonly_query_read_mutation_==mutation_serial()&&
                        readonly_query_read_physical_==edram_memory_->physical_serial()&&
                        buffer.barrierStages==RenderBarrierStage::GRAPHICS;
                    if(same_read)++readonly_query_read_reuses_;
                    else {
                        // First read after any mutation, generation change or
                        // transfer publishes every earlier writer. The pinned
                        // fragment has no external stores or FragDepth output.
                        // Later unchanged reads share this availability; any
                        // ordinary Native consumer retains its full barrier.
                        auto& vk=static_cast<VulkanCommandList&>(*commands);vk.endActiveRenderPass();
                        VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
                        barrier.srcAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
                        barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
                        barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
                        barrier.buffer=buffer.vk;barrier.size=VK_WHOLE_SIZE;
                        vkCmdPipelineBarrier(vk.vk,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                            0,0,nullptr,1,&barrier,0,nullptr);
                        buffer.barrierStages=RenderBarrierStage::GRAPHICS;++readonly_query_read_barriers_;
                    }
                } else {
                    readonly_query_read_owner_.reset();
                    commands->barriers(RenderBarrierStage::GRAPHICS,RenderBufferBarrier(&edram_memory_->storage_buffer(),RenderBufferAccess::READ|RenderBufferAccess::WRITE));
                }
                if(attachmentless_fsi_active_)++attachmentless_fsi_barriers_;
            }
        }
        if(stencil_material_selected) {
            if(grouped_draw||packed_pass_continued||readonly_pass_continued||color_pass_continued||fsi_pass_continued||skip_readonly_depth_edram_barrier||retained_opaque)
                throw NativeBatchCompletionError("Stencil material prefix lost standalone EDRAM producer dependency");
#ifdef GOW3_NATIVE_STENCIL_MATERIAL_REPLAY
            if(config_.replay_stencil_material_mode)draw_slot.stencil_material_counter->Begin(*commands);
#endif
        }
        if(!packed_pass_continued&&!readonly_pass_continued&&!color_pass_continued&&!phase_pass_continued&&!hardware_pass_continued&&!fsi_pass_continued)
            commands->setFramebuffer(framebuffer);
        commands->setViewports(state.viewport);
        commands->setScissors(state.discard?RenderRect(0,0,1,1):state.scissor);
        commands->setGraphicsPipelineLayout(layout_.get());commands->setGraphicsPushConstants(0,&addresses);
        const uint32_t all_descriptor_sets=(1u<<descriptor_count)-1;
        uint32_t required_descriptor_sets=all_descriptor_sets;
        if(config_.skip_unused_descriptor_sets) {
            // Inspect the actual selected modules, including specialized/query
            // replacements. Metadata dies with its module, not a recycled pointer.
            const uint32_t declared=p.vertexShader&&p.pixelShader?
                (static_cast<const VulkanShader*>(p.vertexShader)->declaredDescriptorSets|
                 static_cast<const VulkanShader*>(p.pixelShader)->declaredDescriptorSets):UINT32_MAX;
            required_descriptor_sets=declared&all_descriptor_sets;
            ++unused_descriptors_.draws;
            unused_descriptors_.unknown_draws+=declared==UINT32_MAX;
            unused_descriptors_.selected_draws+=required_descriptor_sets!=all_descriptor_sets;
            unused_descriptors_.required_set_bindings+=std::popcount(required_descriptor_sets);
            unused_descriptors_.omitted_set_bindings+=descriptor_count-std::popcount(required_descriptor_sets);
        }
        if(required_descriptor_sets!=all_descriptor_sets) {
            for(unsigned i=0;i<descriptor_count;++i)if(required_descriptor_sets&(1u<<i)) {
                commands->setGraphicsDescriptorSet(descriptors[i].get(),i);
                if(descriptor_batch)++draw_descriptors_.bind_calls;
            }
            if(descriptor_batch)++draw_descriptors_.draws;
        } else if(descriptor_batch) {
            std::array<VkDescriptorSet,5> sets;
            for(unsigned i=0;i<descriptor_count;++i)
                sets[i]=static_cast<const VulkanDescriptorSet&>(*descriptors[i]).vk;
            const auto& vk_commands=static_cast<const VulkanCommandList&>(*commands);
            BindNativeDrawDescriptorSets(vk_commands.vk,vk_commands.activeGraphicsPipelineLayout->vk,
                {sets.data(),descriptor_count},vkCmdBindDescriptorSets);
            ++draw_descriptors_.bind_calls;++draw_descriptors_.draws;
        } else for(unsigned i=0;i<descriptor_count;++i)
            commands->setGraphicsDescriptorSet(descriptors[i].get(),i);
        commands->setPipeline(pipeline);const RenderIndexBufferView view(index_buffer,index_bytes,RenderFormat::R32_UINT);commands->setIndexBuffer(&view);
        if(rectangle){const RenderVertexBufferView vertices(rectangle_vertices,64);commands->setVertexBuffers(0,&vertices,1,&input_slot);}
        if(original_dynamic_depth_bias)commands->setDepthBias(state.depth_bias,0,state.slope_depth_bias);
        // Texture preparation can enqueue conversion lists after Draw's entry
        // reservation. Reserve again before recording a query slot so capacity
        // retirement cannot accidentally collect this still-unsubmitted draw.
        if(gpu_work_queue_&&!DrawGroupsEnabled())gpu_work_queue_->Reserve();
        // All preparation and final PSO selection precedes this ticket. These
        // draws never attach or write the immutable lower-bound image. Only the
        // accepted original packed-depth write may advance its validity token.
        if(config_.native_early_depth_material_retention&&!early_depth&&early_depth_current_) {
            material_depth=early_depth_current_;
            material_depth_ticket=CaptureNativeEarlyDepthMaterialRetention(material_depth->version,
                material_depth_proof(),edram_memory_->SurfaceVersion(RetainedDepth()));
            if(!material_depth_ticket) {
                ++early_depth_material_stats_.late_refusals;InvalidateEarlyDepth();material_depth.reset();
            }
        }
#ifdef GOW3_NATIVE_FRAGMENT_CENSUS_REPLAY
        std::optional<NativeFragmentInvocationScope> fragment_scope;
#endif
        if(!state.discard) {
            draw_slot.queried=queries_->active();
            queries_->ResetDraw(*commands);queries_->BeginDraw(*commands);
#ifdef GOW3_NATIVE_FRAGMENT_CENSUS_REPLAY
            if(fragment_census_)try {
                NativeFragmentInvocationCensus::Key key;
                key.vertex=o.Shader(false).IsNull()?std::string{}:std::string(o.Shader(false).Digest());
                key.pixel=o.Shader(true).IsNull()?std::string{}:std::string(o.Shader(true).Digest());
                key.raster=std::get<std::array<uint32_t,6>>(pipeline_key.raster);
                key.vertex_variant=uint32_t(vertex_variant);key.pixel_variant=uint32_t(pixel_variant);
                key.depth_control=BE(raw,0x2934);key.color_mask=BE(raw,0x28dc);key.blend_mask=fsi_shared.raster.blend_enabled;
                key.pitch=fsi_shared.raster.pitch;key.guest_depth_base=fsi_shared.raster.depth_base;key.guest_depth_format=fsi_shared.raster.depth_format;
                for(unsigned i=0;i<4;++i) {const auto& rt=fsi_shared.raster.color[i];key.guest_color_bases[i]=rt.base;
                    key.guest_color_formats[i]=rt.format;key.guest_color_masks[i]=rt.mask;key.guest_blend_controls[i]=rt.blend;}
                key.width=extent_width;key.height=extent_height;key.queried=draw_slot.queried;
                key.color_format=pipeline_key.color_format;key.depth_format=pipeline_key.depth_format;
                key.hardware_depth=p.depthEnabled;key.hardware_depth_write=p.depthWriteEnabled;
                key.hardware_depth_compare=uint32_t(p.depthFunction);
                NativeFragmentInvocationCensus::Tag tag;
                tag.event=o.Event();tag.frame=o.Frame();tag.key=fragment_census_->Intern(std::move(key));tag.indices=count;
                if(early_depth) {tag.early_depth_anchor=early_depth->version.anchor();tag.early_depth_current=early_depth->version.current();}
                fragment_scope.emplace(*fragment_census_,NativeFragmentInvocationCensus::Token{
                    uint32_t(&draw_slot-draw_slots_.data()),draw_slot.token},tag,*commands);
            }catch(...) {fragment_census_->RejectMetadata();}
#endif
            commands->drawIndexedInstanced(count,1,0,state.index_offset,0);
#ifdef GOW3_NATIVE_FRAGMENT_CENSUS_REPLAY
            if(fragment_scope)fragment_scope->End();
#endif
            queries_->EndDraw(*commands);
        }
#ifdef GOW3_NATIVE_FRAGMENT_CENSUS_REPLAY
        else if(fragment_census_)fragment_census_->Discarded();
#endif
#ifdef GOW3_NATIVE_STENCIL_MATERIAL_REPLAY
        if(stencil_material_selected&&config_.replay_stencil_material_mode)draw_slot.stencil_material_counter->End(*commands);
#endif
        if(!state.discard)++mutation_serial_;
        const auto submit_start=time_cpu_stages_?DrawClock::now():DrawClock::time_point{};
        const double setup_wait=draw_timing_.submit_wait-wait_before;
        const double enqueue_wait_before=draw_timing_.submit_wait;
        if(draw_timing_scope&&draw_timing_scope->active())try {
            NativeGpuWorkTag tag;
            tag.event=o.Event();tag.frame=o.Frame();tag.pair=timing_pair;
            tag.kind=NativeGpuWorkKind::Draw;tag.discarded=state.discard;tag.queried=draw_slot.queried;
            tag.depth_control=BE(raw,0x2934);tag.color_mask=BE(raw,0x28dc);
            tag.blend_mask=config_.native_fsi?fsi_shared.raster.blend_enabled:0;
            tag.width=extent_width;tag.height=extent_height;tag.indices=count;
            draw_timing_scope->End(tag,*commands);
        }catch(...){draw_timestamps_->RejectMetadata();}
        if(grouped_draw) {
            // Only certified pass cohorts keep their subpass open. Generic
            // command grouping preserves each ordinary draw's pass boundary.
            // Hardware render targets keep it open only while the framebuffer
            // and attachment layouts are unchanged (see hardware_pass_continued).
            // The 2026-10-18 RX 9070 XT hangs predate the framebuffer cache fix.
            const bool keep_hardware_pass=!config_.native_fsi&&config_.hardware_render_pass_continuation&&!retained_opaque;
            if(!keep_hardware_pass&&!packed_pass_eligible&&!readonly_pass_eligible&&!color_pass_eligible&&!phase_pass_eligible&&!fsi_pass_eligible)static_cast<VulkanCommandList&>(*commands).endActiveRenderPass();
        } else commands->end();
        const auto end_complete=config_.time_draw_recording?DrawClock::now():DrawClock::time_point{};
        if(config_.time_draw_recording)draw_recording_.end_ms+=DrawMilliseconds(submit_start,end_complete);
        if(!grouped_draw&&(config_.time_draw_recording||config_.reset_draw_command_pool))++draw_recording_.ends;
        if(grouped_draw&&config_.test_grouped_draw_before_enqueue)
            config_.test_grouped_draw_before_enqueue(gpu_work_queue_->open_group_draws());
        draw_slot.account_pinned_null_constants=config_.pinned_null_constant_uploads;
        draw_slot.pinned_null_constants_selected=pinned_null_constants_selected;
        draw_slot.pinned_null_constants_candidate=pinned_null_constants_candidate;
        draw_slot.constant_delta_writes=constant_delta_writes;
        draw_slot.account_constant_delta=config_.delta_constant_uploads;
        draw_slot.accepted_upload_bytes=index_upload_bytes+constant_bytes+(rectangle?64:0);
        pending_draws_.push_back(&draw_slot);++draw_slot.pending_occurrences;pending_upload_bytes_+=draw_slot.accepted_upload_bytes;
        if(gpu_work_queue_) {
            NativeGpuWorkTag tag;
            if(config_.gpu_batch_attribution||config_.gpu_physical_timestamps) {
                try{tag.event=o.Event();tag.frame=o.Frame();}catch(...){}
                tag.pair=diagnostic_pair;
                tag.kind=NativeGpuWorkKind::Draw;tag.discarded=state.discard;tag.queried=draw_slot.queried;
                tag.depth_control=BE(raw,0x2934);tag.color_mask=BE(raw,0x28dc);
                tag.blend_mask=config_.native_fsi?fsi_shared.raster.blend_enabled:0;
                tag.width=extent_width;tag.height=extent_height;tag.indices=count;
                if(config_.gpu_physical_timestamps) {
                    tag.vertex_variant=uint32_t(vertex_variant);tag.pixel_variant=uint32_t(pixel_variant);
                    // 1 standalone,2 packed depth,3 readonly query,4 packed color,5 general group,6 retained phase.
                    tag.cohort=!grouped_draw?1:packed_pass_eligible?2:readonly_pass_eligible?3:color_pass_eligible?4:phase_pass_eligible?6:5;
                    tag.cohort_known=true;tag.fsi_state_specialized=fsi_state_selected;
                }
            }
            if(grouped_draw)gpu_work_queue_->EnqueuePinnedDrawInGroup(*commands,o.Event(),draw_slot.token,(config_.gpu_batch_attribution||config_.gpu_physical_timestamps)?&tag:nullptr);
            else gpu_work_queue_->EnqueuePinnedDraw(*commands,o.Event(),draw_slot.token,(config_.gpu_batch_attribution||config_.gpu_physical_timestamps)?&tag:nullptr);
            if(stencil_material_selected) {
                ++stencil_material_stats_.selected;
#ifdef GOW3_NATIVE_STENCIL_MATERIAL_REPLAY
                if(config_.replay_stencil_material_mode) {
                    draw_slot.stencil_material_counter->Accept(draw_slot.token);draw_slot.stencil_material_selected=true;
                    ++stencil_material_stats_.accepted;
                }
#endif
            }
            group_recording_guard.accepted=true;draw_group_recording_=false;
            if(grouped_draw&&phase_pass_eligible) {
                open_retained_phase_pass_=true;open_retained_phase_owner_=retained_opaque;
                open_retained_phase_identity_=phase_identity;++retained_phase_pass_draws_;
                ++retained_phase_pass_stats_.selected;retained_phase_pass_stats_.continued+=phase_pass_continued;
            }
            if(grouped_draw&&fsi_pass_eligible) {
                open_fsi_draw_planes_=fsi_pass_continued?fsi_merged_planes:fsi_planes;
                open_fsi_draw_pass_=true;++fsi_draw_pass_draws_;
                ++fsi_draw_pass_stats_.selected;fsi_draw_pass_stats_.continued+=fsi_pass_continued;
                fsi_draw_pass_stats_.queried_selected+=draw_slot.queried;
            }
            if(grouped_draw&&color_pass_eligible) {
                open_packed_color_pass_=true;++packed_color_pass_draws_;
                ++packed_color_pass_stats_.selected;packed_color_pass_stats_.continued+=color_pass_continued;
                packed_color_pass_stats_.variant8_selected+=color_pass_variant8;
                packed_color_pass_stats_.variant8_continued+=color_pass_variant8&&color_pass_continued;
                if(color_pass_action) {
                    ++packed_color_action_pass_stats_.selected;
                    packed_color_action_pass_stats_.continued+=color_pass_continued;
                }
                if(config_.packed_color_blend_pass_groups) {
                    if(color_pass_blend) {
                        ++packed_color_blend_pass_stats_.selected;
                        packed_color_blend_pass_stats_.continued+=color_pass_continued;
                        packed_color_blend_pass_stats_.alpha_selected+=fsi_shared.raster.color[0].blend==0x01000706u;
                        packed_color_blend_pass_stats_.additive_selected+=fsi_shared.raster.color[0].blend==0x01000101u;
                        packed_color_blend_pass_stats_.old_to_blend_continued+=color_pass_continued&&last_packed_color_child_variant8_;
                    } else if(color_pass_variant8)
                        packed_color_blend_pass_stats_.blend_to_old_continued+=color_pass_continued&&last_packed_color_child_blend_;
                    last_packed_color_child_blend_=color_pass_blend;
                    last_packed_color_child_variant8_=color_pass_variant8;
                }
            }
            if(grouped_draw&&packed_pass_eligible) {
                open_packed_depth_pass_=true;++packed_depth_pass_draws_;
                ++packed_depth_pass_stats_.selected;packed_depth_pass_stats_.continued+=packed_pass_continued;
                open_packed_depth_atomic_=packed_pass_atomic;
                packed_depth_pass_stats_.atomic_selected+=packed_pass_atomic;
                packed_depth_pass_stats_.atomic_continued+=packed_pass_atomic&&packed_pass_continued;
            }
            if(grouped_draw&&readonly_pass_eligible) {
                open_readonly_query_pass_=true;open_readonly_query_owner_=readonly_depth;++readonly_query_pass_draws_;
                ++readonly_query_pass_stats_.selected;readonly_query_pass_stats_.continued+=readonly_pass_continued;
            }
            if(draw_timing_scope)draw_timing_scope->Accepted();
#ifdef GOW3_NATIVE_FRAGMENT_CENSUS_REPLAY
            if(fragment_scope)fragment_scope->Accepted();
#endif
        }
        pending_last_event_=o.Event();
        if(grouped_draw&&gpu_work_queue_->open_group_draws()>=
           std::min(config_.max_draw_batch_size,NativeDrawCommandGroups::kMaximumDraws))SealDrawCommandGroup();
        if(config_.async_gpu_submissions) {
            if(gpu_work_queue_->staged_draws()>=config_.max_draw_batch_size)gpu_work_queue_->SubmitPending(NativeGpuWorkQueue::SubmissionReason::DrawBatchTarget);
            if(pending_draws_.size()>=submission_capacity_.lists||pending_upload_bytes_>=16u*1024*1024)RelieveDrawPressure();
        } else if(pending_draws_.size()>=config_.max_draw_batch_size||pending_upload_bytes_>=16u*1024*1024)
            FlushPendingDraws();
        const auto submit_end=time_cpu_stages_?DrawClock::now():DrawClock::time_point{};
        if(config_.time_draw_recording) {
            // This interval was outside both setup_ms and finish_ms. Keep its
            // existing fence/retirement wait subset explicit to avoid counting
            // that time twice when estimating host-only command recording.
            draw_recording_.enqueue_submit_ms+=DrawMilliseconds(end_complete,submit_end);
            draw_recording_.enqueue_wait_ms+=draw_timing_.submit_wait-enqueue_wait_before;
        }
        if(!state.discard&&fsi_candidate) {
            ObserveFsiState(*fsi_candidate,fsi_state_selected,queries_->active(),config_.fsi_state_material_composition?nullptr:resident_fsi.pipeline);
            if(fsi_state_selected) {
                ++fsi_state_stats_.selected;
                if(queries_->active())++fsi_state_stats_.queried_selected;
            }
        }
        if(!state.discard&&(pixel_variant==NativeShaderVariant::ImmutableFsiStateHoistV1||
            pixel_variant==NativeShaderVariant::ImmutableFsiStateAndIncomingDepthHoistV2)) {
            ++fsi_hoist_stats_.selected;fsi_hoist_stats_.queried+=queries_->active();
            if(pixel_variant==NativeShaderVariant::ImmutableFsiStateAndIncomingDepthHoistV2)
                fsi_hoist_stats_.incoming_depth_state+=(fsi_shared.raster.depth_control&3u)!=0u;
        }
        if(!state.discard&&pixel_variant==NativeShaderVariant::ExactIdentityFsiStoresV1) {
            ++exact_fsi_stores_stats_.selected;exact_fsi_stores_stats_.queried+=queries_->active();
            const auto& raster=fsi_shared.raster;
            if(NativeFsiExactDepthIdentityControl(raster.depth_control)) {
                ++exact_fsi_stores_stats_.depth_identity_candidates;
                if(raster.reserved&kNativeFsiExactDepthIdentityFlag)++exact_fsi_stores_stats_.depth_identity_certified;
                else ++exact_fsi_stores_stats_.depth_identity_uncertified;
            }
            uint64_t targets=0;
            for(unsigned slot=0;slot<4;++slot)targets+=NativeFsiExactColorOverwrite(raster.color[slot].format,
                raster.color[slot].mask,(raster.blend_enabled>>slot)&1u);
            exact_fsi_stores_stats_.color_overwrite_targets+=targets;
            exact_fsi_stores_stats_.color_overwrite_draws+=targets!=0;
        }
        if(!state.discard&&pixel_variant==NativeShaderVariant::BuiltinAtomicDepthMaxV1)
            ++atomic_depth_stats_.selected;
        if(!state.discard&&pixel_variant==NativeShaderVariant::OpaqueFormat3FullWordOverwriteV1)
            ++opaque_overwrite_stats_.selected;
        if(!state.discard&&pixel_variant==NativeShaderVariant::ForwardColorPixelCatalogV1) {
            auto& source=forward_pixel_catalog_source_stats_.find(o.Shader(true).Digest())->second;
            ++forward_pixel_catalog_stats_.selected;++source.selected;
            if(queries_->active()){++forward_pixel_catalog_stats_.queried_selected;++source.queried_selected;}
        }
        if(!state.discard&&vertex_variant==NativeShaderVariant::ForwardStraightVertexControlFlowV1) {
            ++forward_vertex_stats_.selected;if(queries_->active())++forward_vertex_stats_.queried_selected;
        }
        if(!state.discard&&pixel_variant==NativeShaderVariant::ForwardOnlyGuestControlFlowV1) {
            ++forward_control_flow_stats_.selected;
            if(queries_->active())++forward_control_flow_stats_.queried_selected;
        }
        transfer_report_dirty_=true;++diagnostic_counters_.draws;
        if(config_.native_fsi&&config_.attachmentless_fsi) {
            if(state.discard)++attachmentless_fsi_discarded_;
            else if(attachmentless_fsi_active_) {
                ++attachmentless_fsi_draws_;
                attachmentless_fsi_queried_+=queries_->active();
                attachmentless_fsi_no_pixel_+=o.Shader(true).IsNull();
            }
        }
        if(config_.native_fsi&&config_.discard_dummy_fsi_attachment&&!early_depth&&!readonly_depth&&!retained_opaque&&!shared_retained_depth&&!retained_fp24_depth) {
            if(state.discard)++discard_dummy_fsi_discarded_;
            else if(discard_dummy_fsi_installed_) {
                ++discard_dummy_fsi_draws_;
                discard_dummy_fsi_queried_+=queries_->active();
                discard_dummy_fsi_no_pixel_+=o.Shader(true).IsNull();
            }
        }
        if(config_.native_fsi&&!state.discard) {
            const bool time_phase_bookkeeping=time_cpu_stages_&&retained_opaque&&config_.retained_opaque_phase;
            struct PhaseBookkeepingTimingScope {
                RetainedPhasePredicateTiming* sample;DrawClock::time_point start;
                explicit PhaseBookkeepingTimingScope(RetainedPhasePredicateTiming* value):sample(value),
                    start(sample?DrawClock::now():DrawClock::time_point{}) {}
                ~PhaseBookkeepingTimingScope() {
                    if(!sample)return;
                    ++sample->calls;sample->elapsed_ms+=DrawMilliseconds(start,DrawClock::now());
                }
            } bookkeeping_timing{time_phase_bookkeeping?&retained_phase_bookkeeping_timing_.inclusive:nullptr};
            const auto phase_surface_version=[&](size_t index,auto&& original) {
                PhaseBookkeepingTimingScope timing{time_phase_bookkeeping?&retained_phase_bookkeeping_timing_.surface_versions[index]:nullptr};
                return original();
            };
            std::optional<std::array<ResolveMemory::PrimarySurfaceWriteReceipt,2>> phase_write_receipts;
            if(retained_opaque&&config_.retained_opaque_phase)phase_write_receipts.emplace();
            const auto early_depth_pre=early_depth?edram_memory_->SurfaceVersion(RetainedDepth()):0;
            const auto retained_pre_color=retained_opaque?phase_surface_version(0,[&]{return edram_memory_->SurfaceVersion(storage(0));}):0;
            const auto retained_pre_depth=retained_opaque?phase_surface_version(1,[&]{return edram_memory_->SurfaceVersion(storage(4));}):0;
            for(unsigned slot=0;slot<4;++slot)if(fsi_shared.raster.color[slot].mask)
                edram_memory_->RecordSurfaceWrite(storage(slot),phase_write_receipts&&slot==0?&(*phase_write_receipts)[0]:nullptr);
            if(attachments[4].identity&&(fsi_shared.raster.depth_control&7)) {
                const bool unchanged_readonly=readonly_no_write_prepared&&readonly_depth&&readonly_depth_ticket&&
                    !early_depth_current_&&!early_depth&&
                    readonly_depth_queried&&readonly_no_write_layout_&&
                    *readonly_no_write_layout_==readonly_depth->version.layout()&&ReadonlyNoWriteHostsPrepared()&&
                    readonly_depth->version.AcceptUnchangedReadonly(readonly_depth->version.layout(),*readonly_depth_ticket,
                        readonly_no_write_serial,edram_memory_->physical_serial(),true,
                        IsNativeReadonlyDepthVariant(pixel_variant),false);
                if(config_.native_readonly_depth_no_write_metadata&&readonly_depth&&!unchanged_readonly)
                    ++readonly_no_write_stats_.refusals;
                const uint64_t pre=(readonly_depth||shared_retained_depth)&&!unchanged_readonly?edram_memory_->SurfaceVersion(storage(4)):0;
                const uint64_t shared_color_pre=shared_retained_depth?edram_memory_->SurfaceVersion(RetainedColor()):0;
                const uint64_t fp24_pre=retained_fp24_depth?edram_memory_->SurfaceVersion(storage(4)):0;
                const uint64_t material_pre=material_depth?edram_memory_->SurfaceVersion(RetainedDepth()):0;
                const uint64_t material_serial=material_depth?edram_memory_->physical_serial():0;
                if(unchanged_readonly) {
                    InvalidateReadonlyNoWriteHosts();++readonly_no_write_stats_.selected;
                } else edram_memory_->RecordSurfaceWrite(storage(4),phase_write_receipts?&(*phase_write_receipts)[1]:nullptr);
                if(material_depth&&material_depth_ticket) {
                    const bool promoted=material_depth==early_depth_current_&&
                        CommitNativeEarlyDepthMaterialRetention(material_depth->version,*material_depth_ticket,
                            material_depth_proof(),material_pre,edram_memory_->SurfaceVersion(RetainedDepth()),
                            material_serial,edram_memory_->physical_serial(),true);
                    if(promoted)++early_depth_material_stats_.selected;
                    else {++early_depth_material_stats_.token_refusals;InvalidateEarlyDepth();}
                }
                if(early_depth&&early_depth_ticket) {
                    if(!early_depth->version.PromoteAcceptedMonotonic(early_depth->version.layout(),*early_depth_ticket,
                       early_depth_pre,edram_memory_->SurfaceVersion(RetainedDepth()),true,
                       (pixel_variant==NativeShaderVariant::ConservativeEarlyDepthV1||pixel_variant==NativeShaderVariant::ConservativeEarlyDepthCatalogV2),false))
                        throw NativeBatchCompletionError("Accepted early depth draw lost its monotonic certificate");
                    const bool catalogue=pixel_variant==NativeShaderVariant::ConservativeEarlyDepthCatalogV2;
                    auto& stats=catalogue?early_depth_catalog_stats_:early_depth_stats_;
                    ++stats.promotions;++stats.selected;stats.queried_selected+=queries_->active();
                    uint32_t source=1;
                    if(catalogue)for(uint32_t i=0;i<kNativeEarlyDepthCatalogPixelSources.size();++i)
                        if(o.Shader(true).Digest()==kNativeEarlyDepthCatalogPixelSources[i]) {++early_depth_catalog_source_selected_[i];source=i+2;}
                    if(config_.native_early_depth_catalog&&early_depth_last_source_&&early_depth_last_source_!=source)
                        ++early_depth_catalog_shader_switches_;
                    early_depth_last_source_=source;
                    ++early_depth_run_;stats.maximum_run=std::max(stats.maximum_run,early_depth_run_);
                    if(config_.native_early_depth_catalog)early_depth_catalog_stats_.maximum_run=std::max(early_depth_catalog_stats_.maximum_run,early_depth_run_);
                }
                if(retained_fp24_depth&&retained_fp24_depth_ticket) {
                    retained_fp24_depth->authority.AcceptedDraw(*retained_fp24_depth_ticket,fp24_pre,
                        edram_memory_->SurfaceVersion(storage(4)),pixel_variant==NativeShaderVariant::RetainedFp24DepthWriterV1,false);
                    ++retained_fp24_depth_stats_.selected;retained_fp24_depth_stats_.queried_selected+=queries_->active();++retained_fp24_depth_run_;
                    const auto source=uint32_t(*NativeRetainedFp24DepthSourceIndex(o.Shader(false).Digest()));
                    ++retained_fp24_depth_source_selected_[source];
                    if(retained_fp24_depth_last_source_!=UINT32_MAX&&retained_fp24_depth_last_source_!=source)++retained_fp24_depth_stats_.shader_switches;
                    retained_fp24_depth_last_source_=source;
                }
                if(shared_retained_depth&&shared_retained_ticket) {
                    if(!shared_retained_depth->authority.PromoteAcceptedReadonly(kNativeRetainedOpaqueLayout,*shared_retained_ticket,
                       shared_color_pre,pre,edram_memory_->SurfaceVersion(RetainedColor()),edram_memory_->SurfaceVersion(storage(4)),
                       true,IsNativeReadonlyDepthVariant(pixel_variant),false))
                        throw NativeBatchCompletionError("Accepted shared readonly query was not promoted");
                    ++shared_depth_query_stats_.promotions;++shared_depth_query_stats_.selected;
                    ++shared_depth_query_stats_.queried_selected;
                }
                if(readonly_depth&&readonly_depth_ticket) {
                    if(!unchanged_readonly) {
                        const uint64_t post=edram_memory_->SurfaceVersion(storage(4));
                        if(readonly_depth->version.PromoteAcceptedReadonly(readonly_depth->version.layout(),*readonly_depth_ticket,
                           pre,post,true,IsNativeReadonlyDepthVariant(pixel_variant),false))++readonly_depth_stats_.promotions;
                        else ++readonly_depth_stats_.promotion_refusals;
                    }
                    ++readonly_depth_stats_.selected;readonly_depth_stats_.queried_selected+=readonly_depth_queried;
                    readonly_depth_conservative_stats_.selected+=pixel_variant==NativeShaderVariant::NativeReadonlyDepthLessV1;
                    // Count only the same accepted selected draws as above;
                    // recording failure cannot become positive coverage.
                    if(skip_readonly_depth_edram_barrier)++readonly_depth_stats_.skipped_edram_barriers;
                    else ++readonly_depth_stats_.retained_edram_barriers;
                }
            }
            if(retained_opaque) {
                const auto written_versions=phase_write_receipts?
                    edram_memory_->PrimarySurfaceWriteVersions((*phase_write_receipts)[0],(*phase_write_receipts)[1]):std::nullopt;
                retained_opaque->authority.AcceptedDraw(*retained_opaque_ticket,retained_pre_color,retained_pre_depth,
                    phase_surface_version(2,[&]{return written_versions?(*written_versions)[0]:edram_memory_->SurfaceVersion(storage(0));}),
                    phase_surface_version(3,[&]{return written_versions?(*written_versions)[1]:edram_memory_->SurfaceVersion(storage(4));}));
                if(phase_write_receipts) {
                    if(written_versions)++phase_primary_write_receipts_;
                    else ++phase_primary_write_receipt_fallbacks_;
                }
                ++retained_opaque_stats_.selected;retained_opaque_stats_.queried_selected+=queries_->active();++retained_opaque_run_;
                if(config_.retained_opaque_phase) {
                    const auto* recipe=NativeRetainedOpaquePhaseRecipe(o.Shader(false).Digest(),o.Shader(true).Digest(),std::get<std::array<uint32_t,6>>(raster_key));
                    if(!recipe)throw NativeBatchCompletionError("Accepted opaque phase recipe changed");
                    const auto index=uint32_t(recipe-kRetainedOpaquePhaseRecipes.data());++retained_opaque_phase_selected_[index];
                    if(retained_opaque_last_pair_!=UINT32_MAX&&retained_opaque_last_pair_!=index)++retained_opaque_stats_.shader_switches;
                    retained_opaque_last_pair_=index;
                }
                if(config_.retained_opaque_catalog) {
                    const auto* pair=RetainedOpaqueCatalogPair(o.Shader(false).Digest(),o.Shader(true).Digest());
                    const auto index=uint32_t(pair-kRetainedOpaqueCatalogPairs.data());
                    auto& count=retained_opaque_pair_stats_[index];++count.selected;count.queried_selected+=queries_->active();
                    if(retained_opaque_last_pair_!=UINT32_MAX&&retained_opaque_last_pair_!=index)++retained_opaque_stats_.shader_switches;
                    retained_opaque_last_pair_=index;
                }
            }
        }
        if(disjoint_retained_query&&!state.discard) {
            // Original depth metadata advanced only disjoint tiles. No primary
            // token promotion, color version change or dirty reset is permitted.
            if(!RetainedPrimaryAuthorityForDisjointQuery())
                throw NativeBatchCompletionError("Accepted disjoint query lost retained primary authority");
            ++disjoint_query_stats_.accepted_draws;
            disjoint_query_stats_.opaque_preserved_draws+=bool(retained_opaque_current_);
            disjoint_query_stats_.depth_preserved_draws+=bool(retained_fp24_depth_current_);
        }
        if(!state.discard&&pixel_variant==NativeShaderVariant::BuiltinReadonlyDepthQueryV2) {
            ++readonly_query_stats_.selected;readonly_query_stats_.queried_selected+=queries_->active();
        }
        if(config_.native_fsi&&attachments[0].identity)for(const auto& pixel:config_.debug_pixels) {
            const auto& layout=storage(0);
            if(pixel[0]>=layout.width||pixel[1]>=layout.height)continue;
            const uint32_t address=SurfaceByteOffset(layout,pixel[0],pixel[1]);
            const bool wide=layout.format==5||layout.format==7||layout.format==15;
            FlushPendingDraws();
            auto value=edram_memory_->ReadBytes(address,wide?8:4);
            const auto key=Json::array({pixel,layout.base,layout.format}).dump();
            if(!debug_pixel_values_.contains(key)||debug_pixel_values_.at(key)!=value) {
                report["diagnostic_pixel_changes"].push_back({{"event",o.Event()},{"frame",o.Frame()},{"pixel",pixel},
                    {"format",layout.format},{"edram_address",address},{"packed_bytes",value}});
                debug_pixel_values_[key]=std::move(value);
            }
        }
        diagnostic_counters_.gpu_texture_reads+=gpu_reads;
        if(detailed_reports_)for(const auto& read:alias_reads)report["texture_alias_reads"].push_back(read);
        if(rectangle&&depth&&!output_.empty())Preview(*depth,"depth-restore-"+std::to_string(uint32_t(o.Event())),true);
        ++draw_timing_.count;
        if(time_cpu_stages_) {
            draw_timing_.setup+=std::max(0.0,DrawMilliseconds(draw_start,submit_start)-setup_wait);
            draw_timing_.textures+=std::max(0.0,DrawMilliseconds(texture_start,texture_end)-texture_wait);
            draw_timing_.finish+=DrawMilliseconds(submit_end,DrawClock::now());
        }
        // Capture only an accepted draw, after its conservative alias metadata
        // updates and diagnostic reads. This is an immutable owner plus global
        // mutation/physical epochs, never merely a surface-version cache hit.
        if(!state.discard&&pixel_variant==NativeShaderVariant::NativeReadonlyDepthLowerBoundQueryV1&&readonly_depth) {
            readonly_query_read_owner_=readonly_depth;
            readonly_query_read_mutation_=mutation_serial();
            readonly_query_read_physical_=edram_memory_->physical_serial();
        } else readonly_query_read_owner_.reset();
    }
    void EndFrame(const Json& o) {
        if(config_.diagnose_texture_eviction_reloads)try {SetTextureCensusFrame(o.at("frame").get<uint32_t>());}
        catch(...) {texture_census_frame_valid_=false;texture_eviction_reload_census_->MetadataFailure();}
        FlushCommittedState();FlushPendingDraws();
        const auto& identity=o.at("frontbuffer");
        const auto fetch=o.at("frontbuffer_fetch").get<std::array<uint32_t,6>>();
        const auto layout=DescribeTexture(fetch);const uint32_t address=layout.base_address;
        if(!resolve_memory_->FullyDefined(layout,address))
            throw std::runtime_error("Frontbuffer has uncaptured or unwritten GPU contents");
        // Snapshot the completed GPU image before subsequent frames reuse its
        // physical bytes. Resource/history ownership is deliberately retained.
        const NativeSynchronousGpuTimingContext presentation_timing{presentation_gpu_timings_.get(),o.at("frame").get<uint32_t>()};
        std::shared_ptr<RenderTexture> output=resolve_memory_->Load(layout,address,&presentation_timing);
        const auto reference_path=capture_/("gpu-reference-"+std::to_string(o.at("frame").get<unsigned>())+".json");
        if(!capture_.empty()&&fs::exists(reference_path)) {
            std::ifstream metadata(reference_path);Json reference;metadata>>reference;
            if(reference.at("version")!=1||reference.at("provenance")!="reference_gpu_frame_boundary"||
               reference.at("reference_frame")!=o.at("frame")||
               reference.at("frame").get<uint64_t>()!=o.at("frame").get<uint64_t>()+1)
                throw std::runtime_error("Reference snapshot does not match the completed frame");
            ResolveMemory memory(*device_,*queue_,memory_store_spv,memory_load_spv,512u*1024*1024);
            for(const auto& entry:reference.at("ranges")) {
                const auto digest=Digest(entry.at("sha256"));auto bytes=Read(capture_/(digest+".gpu.bin"),512u*1024*1024);
                if(bytes.size()!=entry.at("bytes").get<uint64_t>()||picosha2::hash256_hex_string(bytes)!=digest)
                    throw std::runtime_error("Reference GPU payload integrity failure");
                const auto addr=entry.at("address").get<uint64_t>();
                if(addr>UINT32_MAX)throw std::runtime_error("Invalid reference memory address");
                memory.Initialize(uint32_t(addr),bytes);
            }
            if(!memory.FullyDefined(layout,address))throw std::runtime_error("Reference frontbuffer is missing GPU bytes");
            auto expected=memory.Load(layout,address);
            if(!output_.empty())Preview(*expected,"reference-frame-"+std::to_string(o.at("frame").get<unsigned>()));
            auto actual_bytes=validation::ReadImage(*device_,*queue_,*output,16);
            auto expected_bytes=validation::ReadImage(*device_,*queue_,*expected,16);
            if(actual_bytes.size()!=expected_bytes.size())throw std::runtime_error("Reference image extent differs");
            double absolute=0,squared=0;float largest=0;uint64_t changed=0;
            for(size_t i=0;i<actual_bytes.size();i+=16) {
                std::array<float,4> a,b;std::memcpy(a.data(),actual_bytes.data()+i,16);std::memcpy(b.data(),expected_bytes.data()+i,16);
                bool different=false;
                for(unsigned c=0;c<3;++c) {
                    if(!std::isfinite(a[c])||!std::isfinite(b[c]))throw std::runtime_error("Non-finite comparison pixel");
                    const auto delta=std::abs(a[c]-b[c]);absolute+=delta;squared+=double(delta)*delta;
                    largest=std::max(largest,delta);different|=delta>0;
                }
                changed+=different;
            }
            const double components=double(actual_bytes.size()/16)*3;
            report["frame_comparisons"].push_back({{"frame",o.at("frame")},{"same_frame",true},
                {"mean_absolute_rgb",absolute/components},{"rms_rgb",std::sqrt(squared/components)},
                {"max_rgb",largest},{"changed_pixels",changed},{"pixels",actual_bytes.size()/16}});
        }
        // Physical frontbuffer bytes retain their packed channel order. The
        // guest swap fetch selects display RGB (commonly B,G,R,1), just as an
        // ordinary texture fetch does. The host native-source hook consumes
        // this already-swizzled image and only applies the display gamma ramp.
        const uint32_t presentation_swizzle=(fetch[3]>>1)&0xfff;
        std::shared_ptr<RenderTexture> presented=output;
        if(presentation_swizzle!=0x688) {
            const auto& desc=static_cast<VulkanTexture&>(*output).desc;
            presented=device_->createTexture(RenderTextureDesc::ColorTarget(desc.width,desc.height,RenderFormat::R32G32B32A32_FLOAT));
            const RenderRect rectangle(0,0,desc.width,desc.height);
            transfers_->Resolve(*output,*presented,rectangle,rectangle,0,PassTransfer::Conversion::Color,presentation_swizzle,&presentation_timing);
        }
        if(!output_.empty())Preview(*presented,"frame-"+std::to_string(o.at("frame").get<unsigned>()));
        if(config_.debug_raw_images&&!output_.empty())Preview(*output,"frame-raw-"+std::to_string(o.at("frame").get<unsigned>()));
        if(config_.present) {
            commands_->begin();
            const auto& desc=static_cast<const VulkanTexture&>(*presented).desc;
            NativeSynchronousGpuTimingScope presentation_scope(&presentation_timing,*commands_,
                NativeSynchronousGpuPhase::PresentTransition,desc.width,desc.height);
            commands_->barriers(RenderBarrierStage::GRAPHICS,RenderTextureBarrier(presented.get(),RenderTextureLayout::SHADER_READ));
            Submit(&presentation_scope);config_.present(o.at("frame"),presented,fetch);
        }
        transfer_report_dirty_=true;
        diagnostic_counters_.completed_frames.push_back(o.at("frame"));
        if(detailed_reports_)report["frame_outputs"].push_back({{"frame",o.at("frame")},{"identity",identity},
            {"presentation_fetch_swizzle",presentation_swizzle},
            {"provenance",o.at("provenance")},{"presentation_submitted",bool(config_.present)}});
        // Each submission has completed. Retire views/uploads not used by this
        // frame so animated vertices, bones and streamed assets do not grow
        // without bound over a campaign. GPU history lives in physical memory.
        for(auto& [key,image]:surfaces_)if(!frame_surfaces_.contains(key)&&image.dirty) {
            edram_memory_->StoreSurface(*image.texture,SurfaceStorage(image.identity));
            image.dirty=false;
        }
        auto retire=[](auto& cache,auto& used) {
            std::erase_if(cache,[&](const auto& entry){return !used.contains(entry.first);});used.clear();
        };
        // Hardware render targets keep working images as the real storage:
        // retiring one used every few frames would recreate and fully restore
        // it. Retire unused ones only past a bound. Framebuffers name images
        // by address, so drop every framebuffer of a retired image with it.
        if(config_.native_fsi||surfaces_.size()>kMaxRetainedHardwareSurfaces) {
            std::set<const RenderTexture*> retired;
            for(const auto& [key,image]:surfaces_)
                if(!frame_surfaces_.contains(key)&&image.texture)retired.insert(image.texture.get());
            std::erase_if(hardware_framebuffers_,[&](const auto& entry) {
                return retired.contains(entry.first.first)||retired.contains(entry.first.second);
            });
            retire(surfaces_,frame_surfaces_);
        } else frame_surfaces_.clear();
        retire(resolved_,frame_resolved_);
        // The frame fence has retired every producer. Sampled generations keep
        // their own owners; unconsumed speculative outputs need no residency.
        canonical_resolve_stats_.discarded+=canonical_resolve_receipts_.size();
        canonical_resolve_receipts_.clear();canonical_resolve_stats_.receipt_bytes=0;
        sampled_alias_index_.Retain([&](const std::string& key){return frame_sampled_.contains(key);});
        retire(sampled_resolves_,frame_sampled_);
        // The existing frame fence covers every cached CPU image and vertex
        // address. Keep the current working set and only a bounded extra set
        // of inactive immutable resources; returning visibility can reuse them.
        if(config_.diagnose_texture_eviction_reloads)
            texture_cache_.EndFrame(textures_,[&](const auto& entry){RetireCpuImageBindings(entry);},TextureCensusObserver());
        else texture_cache_.EndFrame(textures_,[&](const auto& entry){RetireCpuImageBindings(entry);});
        vertex_cache_.EndFrame(vertex_buffers_,[&](auto& entry) {
            if(!vertex_buffer_pool_||!entry.buffer)return;
            const auto& buffer=static_cast<const VulkanBuffer&>(*entry.buffer);
            const RetiredBufferKey key{buffer.desc.size,buffer.desc.flags,uint32_t(buffer.desc.heapType),
                buffer.desc.committed,buffer.device};
            const auto actual_bytes=buffer.allocationInfo.size;
            vertex_buffer_pool_->ReturnAfterFence(std::move(entry.buffer),key,actual_bytes);
        });
        // EndFrame fenced every accepted draw above; cached index buffers
        // have remained immutable and resident through all partial drains.
        index_cache_.RetireAfterFence();
        index_snapshot_=IndexSnapshot{index_cache_.statistics(),index_cache_.size(),
            index_cache_.retained_bytes(),index_cache_.index_bytes()};
        cached_texture_bindings_=0;
        for(const auto& [key,image]:textures_)cached_texture_bindings_+=image.bindings.size();
        for(const auto& [key,image]:sampled_resolves_)if(image.current)cached_texture_bindings_+=image.current->bindings.size();
        binding_snapshot_=BindingSnapshot{texture_binding_hits_,texture_binding_misses_,
            texture_binding_transient_,cached_texture_bindings_};
        batch_snapshot_=BatchSnapshot{batch_submissions_,batch_draws_,batch_peak_};
        texture_snapshot_=TextureSnapshot{o.at("frame"),texture_timing_,
            std::max(0.0,draw_timing_.textures-texture_timing_.version-texture_timing_.payload-
                texture_timing_.overlay-texture_timing_.decode-texture_timing_.upload-
                texture_timing_.alias_load-texture_timing_.binding)};
        texture_timing_={};
        ResourceSnapshot resources{draw_descriptor_allocations_,draw_upload_allocations_,draw_upload_reuses_};
        for(const auto& slot:std::span(draw_slots_).first(submission_capacity_.lists)) {
            resources.index_capacity+=slot.indices.capacity;
            resources.constant_capacity+=slot.constants.capacity;
            resources.rectangle_capacity+=slot.rectangle.capacity;
        }
        resource_snapshot_=resources;
        draw_snapshot_=DrawSnapshot{o.at("frame"),draw_timing_};
        draw_timing_={};
        // Keep recently used programs across frames. A variant absent for just
        // one frame must not force vkCreateGraphicsPipelines on its return.
        // This boundary is already fenced; preserve this frame's working set
        // even when it exceeds the soft target (hard admission remains bounded).
        ReconcileRetainedOpaquePhaseHeadroom();
        uint64_t pipeline_soft=config_.specialize_fsi_pipeline_state?
            std::min<uint64_t>(config_.max_pipeline_cache_entries,config_.max_pipeline_cache_entries*3/4+fsi_state_stats_.prepared):
            config_.retained_opaque_writer?
            std::min<uint64_t>(config_.max_pipeline_cache_entries,config_.max_pipeline_cache_entries*3/4+std::min<uint64_t>(retained_opaque_stats_.prepared,2)):
            config_.max_pipeline_cache_entries*3/4;
        if(config_.native_early_depth)pipeline_soft+=std::min<uint64_t>(early_depth_stats_.prepared,config_.max_pipeline_cache_entries-pipeline_soft);
        if(config_.native_early_depth_catalog)pipeline_soft+=std::min<uint64_t>(early_depth_catalog_stats_.prepared,config_.max_pipeline_cache_entries-pipeline_soft);
        if(config_.retained_opaque_catalog)pipeline_soft=retained_opaque_catalog_headroom_.SoftTargets(true,
            config_.max_shader_cache_entries,config_.max_shader_cache_bytes,config_.max_pipeline_cache_entries).pipelines;
        if(config_.retained_shared_depth_queries)
            pipeline_soft+=std::min<uint64_t>(shared_depth_query_stats_.prepared,config_.max_pipeline_cache_entries-pipeline_soft);
        if(config_.retained_fp24_depth_writer)pipeline_soft=std::min<uint64_t>(config_.max_pipeline_cache_entries,
            config_.max_pipeline_cache_entries*3/4+std::min<uint64_t>(retained_fp24_depth_stats_.prepared,4));
        if(config_.retained_opaque_phase)pipeline_soft=retained_opaque_phase_headroom_.AddToSoftTargets(true,
            {0,0,pipeline_soft},{config_.max_shader_cache_entries,config_.max_shader_cache_bytes,config_.max_pipeline_cache_entries}).pipelines;
        TrimPersistentCache(pipelines_,pipeline_soft,UINT64_MAX,pipeline_cache_stats_,
            [](const auto&){return uint64_t(0);},[&](const auto& entry){return !cache_epoch_.Used(entry);});
        uint64_t shader_soft_entries=config_.max_shader_cache_entries*3/4,shader_soft_bytes=config_.max_shader_cache_bytes*3/4;
        if(config_.forward_pixel_catalog&&forward_pixel_catalog_ready_) {
            // Actual startup module admissions reserve soft-trim headroom, so
            // adding the catalog does not itself evict the original working set.
            // Hard admission/eviction limits and resident-only selection remain.
            const auto soft=forward_pixel_catalog_headroom_.SoftTargets(true,config_.max_shader_cache_entries,config_.max_shader_cache_bytes);
            shader_soft_entries=soft.entries;shader_soft_bytes=soft.bytes;
        }
        if(config_.specialize_fsi_pipeline_state) {
            shader_soft_entries=std::min<uint64_t>(config_.max_shader_cache_entries,shader_soft_entries+fsi_state_stats_.modules_created);
            shader_soft_bytes=std::min<uint64_t>(config_.max_shader_cache_bytes,shader_soft_bytes+fsi_state_stats_.module_bytes);
        }
        if(config_.builtin_readonly_depth_queries&&readonly_query_headroom_.modules()) {
            const auto soft=readonly_query_headroom_.SoftTargets(true,config_.max_shader_cache_entries,config_.max_shader_cache_bytes);
            shader_soft_entries=std::max(shader_soft_entries,soft.entries);shader_soft_bytes=std::max(shader_soft_bytes,soft.bytes);
        }
        if(config_.retained_opaque_writer&&retained_opaque_headroom_.modules()) {
            const auto soft=retained_opaque_headroom_.SoftTargets(true,config_.max_shader_cache_entries,config_.max_shader_cache_bytes);
            shader_soft_entries=std::max(shader_soft_entries,soft.entries);shader_soft_bytes=std::max(shader_soft_bytes,soft.bytes);
        }
        if(config_.retained_opaque_catalog) {
            const auto soft=retained_opaque_catalog_headroom_.SoftTargets(true,config_.max_shader_cache_entries,
                config_.max_shader_cache_bytes,config_.max_pipeline_cache_entries);
            shader_soft_entries=soft.modules;shader_soft_bytes=soft.bytes;
        }
        if(config_.retained_shared_depth_queries&&shared_depth_query_stats_.module_bytes) {
            shader_soft_entries+=std::min<uint64_t>(1,config_.max_shader_cache_entries-shader_soft_entries);
            shader_soft_bytes+=std::min<uint64_t>(shared_depth_query_stats_.module_bytes,config_.max_shader_cache_bytes-shader_soft_bytes);
        }
        if(config_.native_early_depth) {
            shader_soft_entries+=std::min<uint64_t>(early_depth_stats_.modules,config_.max_shader_cache_entries-shader_soft_entries);
            shader_soft_bytes+=std::min<uint64_t>(early_depth_stats_.module_bytes,config_.max_shader_cache_bytes-shader_soft_bytes);
        }
        if(config_.native_early_depth_catalog) {
            shader_soft_entries+=std::min<uint64_t>(early_depth_catalog_stats_.modules,config_.max_shader_cache_entries-shader_soft_entries);
            shader_soft_bytes+=std::min<uint64_t>(early_depth_catalog_stats_.module_bytes,config_.max_shader_cache_bytes-shader_soft_bytes);
        }
        if(config_.retained_fp24_depth_writer&&retained_fp24_depth_headroom_.modules()) {
            const auto soft=retained_fp24_depth_headroom_.SoftTargets(true,config_.max_shader_cache_entries,config_.max_shader_cache_bytes);
            shader_soft_entries=soft.entries;shader_soft_bytes=soft.bytes;
        }
        if(config_.retained_opaque_phase) {
            const auto targets=retained_opaque_phase_headroom_.AddToSoftTargets(true,{shader_soft_entries,shader_soft_bytes,0},
                {config_.max_shader_cache_entries,config_.max_shader_cache_bytes,config_.max_pipeline_cache_entries});
            shader_soft_entries=targets.modules;shader_soft_bytes=targets.bytes;
        }
        TrimPersistentCache(shaders_,shader_soft_entries,shader_soft_bytes,
            shader_cache_stats_,[](const auto& entry){return entry.spirv_bytes;},
            [&](const auto& entry){return !cache_epoch_.Used(entry)&&!AsyncPinned(entry.shader.get());});
        ReconcileRetainedOpaquePhaseHeadroom();
        cache_epoch_.Advance(pipelines_,shaders_);
        if(config_.diagnose_texture_eviction_reloads)texture_eviction_reload_census_->EndFrame();
        if(output_.empty()) {
            const auto before=query_history_.size();
            std::erase_if(query_history_,[&](const auto& query){return o.at("frame")!=query.frame;});
            if(query_history_.size()!=before)query_report_dirty_=true;
        }
        if(output_.empty()&&diagnostic_counters_.completed_frames.size()>64)
            diagnostic_counters_.completed_frames.erase(diagnostic_counters_.completed_frames.begin(),
                diagnostic_counters_.completed_frames.end()-64);
        if(output_.empty())for(auto& [key,value]:report.items()) {
            if(key=="native_occlusion_results"||key=="completed_frames")continue; // Materialized on observation only.
            if(value.is_array()&&key!="limitations"&&value.size()>64)
                value.erase(value.begin(),value.end()-64);
        }
        // Preserve the completed-frame POD snapshot above. JSON construction
        // and diagnostic-only occupancy scans happen only when observed.
        transfer_report_dirty_=true;
    }
    void Execute(const TypedLiveDraw& o) {
        transfer_report_dirty_=true;
        Draw(DrawInput(o));
        transfer_report_dirty_=true;diagnostic_counters_.last_accepted_event=o.event;
        if(!HasPendingGpuWork())diagnostic_counters_.last_completed_event=o.event;
    }
    void Execute(const Json& o) {
        std::optional<RetainedPhaseTimingContext> timing_context;
        if(RetainedPhaseGpuTimingEnabled())timing_context.emplace(*this,o);
        transfer_report_dirty_=true;
        const auto type=o.at("type").get<std::string>();
        if(type!="draw")cpu_image_slots_.Reset(CpuImageSlots::ResetReason::NonDraw);
        // Offline/test JSON queries share the typed live query path.
        if(type=="query_begin"||type=="query_end") {ExecuteQuery(QueryControlFrom(o,type=="query_begin"));return;}
        ExecuteJsonOperation(o,type,false);
    }
    // Fields consumed by query_begin/query_end, from typed or JSON envelopes.
    // JSON reads exactly the keys the former inline branches read.
    struct QueryControl {bool begin=false;uint32_t handle=0,address=0,frame=0;uint64_t generation=0,event=0;};
    static QueryControl QueryControlFrom(const Json& o,bool begin) {
        QueryControl c;c.begin=begin;c.handle=o.at("query_handle");c.address=o.at("report_address");c.event=o.at("event");
        if(!begin){c.frame=o.at("frame");c.generation=o.value("query_generation",uint64_t(0));}
        return c;
    }
    void Execute(const TypedLiveQuery& q) {
        std::optional<RetainedPhaseTimingContext> timing_context;
        if(RetainedPhaseGpuTimingEnabled())timing_context.emplace(*this,q.event,q.frame);
        transfer_report_dirty_=true;
        cpu_image_slots_.Reset(CpuImageSlots::ResetReason::NonDraw);
        ExecuteQuery({q.begin,q.handle,q.report_address,q.frame,q.generation,q.event});
    }
    void ExecuteQuery(const QueryControl& c) {
        if(config_.native_early_depth) {
            AtEarlyDepthReadonlyBoundary([&]{ExecuteQueryOperation(c,true);});return;
        }
        ExecuteQueryOperation(c,false);
    }
    // The former ExecuteJsonOperation prologue/epilogue for type in
    // {query_begin,query_end}; ExecuteJsonOperation no longer sees queries.
    void ExecuteQueryOperation(const QueryControl& c,bool readonly_boundary) {
        if(config_.native_early_depth&&!readonly_boundary)InvalidateEarlyDepth();
        const bool deferred_query=config_.defer_native_queries;
        bool retained_query_control=false;
        if(deferred_query&&(config_.retained_shared_depth_queries||config_.retain_primary_across_disjoint_queries))
            retained_query_control=PrepareCommittedStateForDeferredQuery(c.begin?NativeDeferredQueryControl::Begin:NativeDeferredQueryControl::End);
        else {
            FlushCommittedState(true);
            if(deferred_query&&open_readonly_query_pass_)++readonly_query_pass_stats_.query_controls_preserved;
            else if(deferred_query&&open_fsi_draw_pass_&&config_.fsi_draw_pass_queries&&queries_->host_reset_enabled())
                ++fsi_draw_pass_stats_.query_controls_preserved;
            else SealDrawCommandGroup();
        }
        if(!deferred_query)FlushPendingDraws();
        ExecuteQueryControl(c,retained_query_control);
        transfer_report_dirty_=true;diagnostic_counters_.last_accepted_event=c.event;
        if(!HasPendingGpuWork())diagnostic_counters_.last_completed_event=c.event;
    }
    void ExecuteQueryControl(const QueryControl& c,bool retained_query_control) {
        if(c.begin) {
            queries_->Begin(c.handle,c.address);
            if(retained_query_control&&config_.retained_shared_depth_queries)++shared_depth_query_stats_.accepted_begins;
            if(retained_query_control&&config_.retain_primary_across_disjoint_queries)++disjoint_query_stats_.accepted_begins;
            return;
        }
        const uint32_t handle=c.handle,address=c.address;
        CompletedNativeQuery query{c.frame,c.generation,c.event,{handle,address+32,address,0,0}};
        if(config_.defer_native_queries) {
            if(deferred_queries_.size()>=config_.max_deferred_queries)
                throw std::runtime_error("Native deferred query metadata is full; consume before another end");
            // Reserve metadata before sealing the tracker record. A failed
            // end leaves the active query and all previous results intact.
            // Seal prior transfer recording before taking the all-work
            // watermark, including an empty query behind unqueried work.
            if(config_.retire_completed_query_prefixes||config_.retained_shared_depth_queries||config_.retain_primary_across_disjoint_queries)transfer_batch_->Seal();
            const auto required=(config_.retire_completed_query_prefixes||config_.retained_shared_depth_queries||config_.retain_primary_across_disjoint_queries)?gpu_work_queue_->accepted_work():0;
            const auto diagnostic_frontier=config_.diagnose_marker_query_cohorts?
                ObserveDiagnosticQueryFrontier():std::optional<uint64_t>{};
            deferred_queries_.push_back({query,required,diagnostic_frontier.value_or(0),diagnostic_frontier.has_value()});
            try {queries_->EndDeferred(handle,address);}
            catch(...) {deferred_queries_.pop_back();throw;}
            if(config_.retained_shared_depth_queries) {
                if(retained_query_control)++shared_depth_query_stats_.accepted_ends;
                // Observe EVERY successful deferred End, including a strong
                // fallback after a queued export has cleared current authority.
                shared_depth_query_stats_.last_end_required_work=required;
                shared_depth_query_stats_.last_end_completed_work=gpu_work_queue_->completed_work();
            }
            if(config_.retain_primary_across_disjoint_queries) {
                if(retained_query_control)++disjoint_query_stats_.accepted_ends;
                disjoint_query_stats_.last_end_required_work=required;
                disjoint_query_stats_.last_end_completed_work=gpu_work_queue_->completed_work();
            }
            // Even an empty query cannot pass unrelated pending draw work.
            if(config_.retire_completed_query_prefixes||!HasPendingGpuWork())CollectCompletedQueries();
        } else {
            query.result=queries_->End(handle,address);ReportQuery(query);
        }
    }
    void ExecuteJsonOperation(const Json& o,const std::string& type,bool readonly_boundary) {
        if(type!="draw"&&type!="query_begin"&&type!="query_end")readonly_query_read_owner_.reset();
        if(config_.native_early_depth&&type!="draw"&&!readonly_boundary)InvalidateEarlyDepth();
        const bool deferred_query=config_.defer_native_queries&&(type=="query_begin"||type=="query_end");
        bool retained_query_control=false;
        if(deferred_query&&(config_.retained_shared_depth_queries||config_.retain_primary_across_disjoint_queries))
            retained_query_control=PrepareCommittedStateForDeferredQuery(type=="query_begin"?NativeDeferredQueryControl::Begin:NativeDeferredQueryControl::End);
        else if(type!="draw") {
            FlushCommittedState(true);
            // Logical query controls do not touch the immutable depth image or
            // GPU slots. Each BeginDraw/EndDraw still brackets its own query.
            // All non-query boundaries retain the ordinary seal and ordering.
            if(deferred_query&&open_readonly_query_pass_)++readonly_query_pass_stats_.query_controls_preserved;
            else if(deferred_query&&open_fsi_draw_pass_&&config_.fsi_draw_pass_queries&&queries_->host_reset_enabled())
                ++fsi_draw_pass_stats_.query_controls_preserved;
            else SealDrawCommandGroup();
        }
        const bool queue_cpu_write=type=="cpu_memory_write"&&QueueCpuWrite(o);
        const bool queued_transfer=config_.unified_gpu_queue&&(type=="clearrect"||type=="resolve"||queue_cpu_write);
        if(type!="draw"&&!deferred_query&&!queued_transfer)FlushPendingDraws();
        if(type=="draw")Draw(DrawInput(o));
        else if(type=="clearrect")Clear(o);
        else if(type=="resolve")Resolve(o);
        else if(type=="present")EndFrame(o);
        else if(type=="cpu_memory_write")CpuMemoryWrite(o,queue_cpu_write);
        else if(type=="memory_copy") {
            const uint32_t source=o.at("source"),destination=o.at("destination"),count=o.at("bytes");
            if(o.at("source_kind")=="cpu_snapshot"||o.at("source_kind")=="mixed_memory") {
                const auto bytes=Payload(o.at("payload"));
                if(bytes.size()!=count)throw std::runtime_error("Native memcpy payload extent mismatch");
                if(o.at("source_kind")=="mixed_memory") {
                    const auto ranges=o.at("gpu_ranges").get<std::vector<std::pair<uint32_t,uint32_t>>>();
                    resolve_memory_->CopyFromMixed(source,destination,bytes,ranges);
                } else resolve_memory_->CopyFromCpu(source,destination,bytes);
            } else if(o.at("source_kind")=="gpu_memory")resolve_memory_->Copy(source,destination,count);
            else throw std::runtime_error("Unknown native memcpy provenance");
            if(detailed_reports_)report["memory_copies"].push_back({{"frame",o.at("frame")},{"event",o.at("event")},
                {"source",source},{"destination",destination},{"bytes",count},{"published_to_guest",false}});
        }
        else throw std::runtime_error("Unknown native operation: "+type);
        transfer_report_dirty_=true;diagnostic_counters_.last_accepted_event=o.at("event");
        if(!HasPendingGpuWork())diagnostic_counters_.last_completed_event=o.at("event");
    }
    // Hardware render targets keep host images authoritative: every packed
    // EDRAM reader commits its overlapping dirty images itself (Surface alias
    // commits, fixed-source resolves, export). An operation boundary alone
    // (query control, clear, resolve, memexport, CPU write) does not read
    // packed EDRAM, so it skips the commit and its GPU drain.
    void FlushCommittedState(bool operation_boundary=false) {
        const bool materializing=(retained_opaque_current_&&retained_opaque_current_->authority.dirty())||
            (retained_fp24_depth_current_&&retained_fp24_depth_current_->authority.dirty());
        MaterializeRetainedOpaque();
        if(materializing)FlushPendingDraws();
        // Runtime calls this before every operation. A clean surface cache has
        // no commands or visibility dependency to commit, so keep accepted
        // consecutive FSI draws in their existing physical command group.
        // Even a no-op flush must refuse a poisoned preparing tail.
        if(DrawGroupsEnabled()&&(draw_group_recording_||
           draw_group_recording_failed_||batch_failed_||
           (gpu_work_queue_&&gpu_work_queue_->failed())))
            throw NativeBatchCompletionError("Native draw command group contains a poisoned preparing tail");
        transfer_report_dirty_=true;
        if(operation_boundary&&!config_.native_fsi)return;
        if(surfaces_maybe_dirty_)for(auto& [key,image]:surfaces_)if(image.dirty) {
            // A real image commit retains the original close before any store
            // or completion. Surface alias and non-FSI behavior are unchanged.
            SealDrawCommandGroup();
            if(!gpu_work_queue_||!transfer_batch_->active())FlushPendingDraws();
            const auto layout=SurfaceStorage(image.identity);
            edram_memory_->StoreSurface(*image.texture,layout);
            image.version=edram_memory_->SurfaceVersion(layout);image.dirty=false;
        }
        surfaces_maybe_dirty_=false;
    }
    NativeRendererState ExportState() {
        InvalidateEarlyDepth();FlushCommittedState();FlushPendingDraws();
        return {resolve_memory_->ExportState(),edram_memory_->ExportState()};
    }
    std::vector<uint8_t> ReadFullEdram() {
        if(!config_.native_fsi||queries_->active())
            throw std::runtime_error("Full EDRAM diagnostic requires packed Native with no active query");
        InvalidateEarlyDepth();FlushPendingDraws();FlushCommittedState();
        return edram_memory_->ReadBytes(0,2048*5120);
    }
    void InitializeEdramFromBuffer(VkBuffer source) {
        InvalidateEarlyDepth();transfer_report_dirty_=true;
        if(mutation_serial())throw std::runtime_error("Reference EDRAM must be imported before native rendering");
        const auto destination=edram_memory_->ExportState();
        NativeStateTransfer transfer(*device_,*queue_);
        ++mutation_serial_;
        transfer.Copy({source,destination.capacity,{{0,destination.capacity}}},destination.buffer);
        edram_memory_->RecordImportedBytes(0,destination.capacity);
    }
    uint64_t mutation_serial() const {return mutation_serial_+resolve_memory_->mutation_serial()+edram_memory_->mutation_serial();}
    std::vector<uint8_t> ReadPhysical(uint32_t address,uint32_t bytes) {InvalidateEarlyDepth();FlushPendingDraws();return resolve_memory_->ReadBytes(address,bytes);}
    void InitializeMemory(uint32_t address,std::span<const uint8_t> bytes) {InvalidateEarlyDepth();FlushPendingDraws();resolve_memory_->Initialize(address,bytes);}
    void InitializeEdram(std::span<const uint8_t> bytes) {
        InvalidateEarlyDepth();MaterializeRetainedOpaque();FlushPendingDraws();
        if(bytes.size()!=2048*5120)throw std::runtime_error("Incorrect initial EDRAM extent");
        edram_memory_->Initialize(0,bytes);
    }
    void CheckNativeCapturedFrame(uint32_t frame) {
        const auto expected=ReadNativeFrameCaptureState(capture_,
            ("native-state-"+std::to_string(frame)+".json"),config_.native_frame_capture_sha256);
        const auto actual=ExportState();
        if(actual.physical.ranges.size()!=expected.snapshot.physical.size())
            throw std::runtime_error("Native capture replay physical ownership range count differs");
        uint64_t bytes=0;
        // Report mode (other GPUs): byte mismatches are logged, then the query
        // oracle still runs. Ownership/extent mismatches always stop.
        std::string byte_mismatch;
        for(size_t i=0;i<actual.physical.ranges.size();++i) {
            const auto [address,count]=actual.physical.ranges[i];
            const auto& range=expected.snapshot.physical[i];
            if(address!=range.address||count!=range.bytes.size())
                throw std::runtime_error("Native capture replay physical ownership extent differs");
            if(byte_mismatch.empty()&&resolve_memory_->ReadBytes(address,count)!=range.bytes)
                byte_mismatch="Native capture replay physical bytes differ at "+std::to_string(address);
            bytes+=count;
        }
        if(byte_mismatch.empty()&&ReadFullEdram()!=expected.snapshot.edram)
            byte_mismatch="Native capture replay packed EDRAM differs";
        if(!byte_mismatch.empty()) {
            if(!config_.report_frame_capture_mismatch)throw std::runtime_error(byte_mismatch);
            std::cout<<"Frame "<<frame<<" byte mismatch (reported): "<<byte_mismatch<<"\n";
        }
        ReportQueries();
        auto actual_queries=Json::array();
        for(const auto& query:report.at("native_occlusion_results"))
            if(query.at("frame")==frame)actual_queries.push_back(query);
        auto wanted_queries=expected.query_results;
        // This is rendering replay, not guest execution. Publication bookkeeping
        // is not an occlusion result; every identity, subtotal and draw count is.
        for(auto* values:{&actual_queries,&wanted_queries})
            for(auto& query:*values)query.erase("published_to_guest");
        if(actual_queries!=wanted_queries)
            throw std::runtime_error("Native capture replay query identities or sample counts differ");
        report["native_frame_capture_oracles"].push_back({{"frame",frame},{"physical_bytes",bytes},
            {"physical_ranges",actual.physical.ranges.size()},{"edram_bytes",expected.snapshot.edram.size()},
            {"queries",actual_queries.size()},{"exact",byte_mismatch.empty()}});
    }
    void Run(unsigned through,const std::span<const LiveOperation* const>* supplied=nullptr,bool json_adapter=false,
             bool wrap_frame_queries=false,bool partial_owned_fixture=false) {
        transfer_report_dirty_=true;
        std::ifstream input(capture_/"scene.json");Json scene;input>>scene;
        if(scene.at("version")!=3)throw std::runtime_error("Regenerate scene plan version 3");
        if(config_.native_completed_frame_capture&&(!supplied||through!=UINT32_MAX||
            wrap_frame_queries||partial_owned_fixture||config_.diagnostic_readonly_query_groups||diagnostic_zero_history_||
            config_.native_frame_capture_sha256.size()!=64))
            throw std::runtime_error("Native captured frames require complete supplied operations and actual history");
        unsigned event=0;
        try {
            if(partial_owned_fixture) {
                if(!supplied||wrap_frame_queries||through==UINT32_MAX||
                   (!RetainedOpaqueEnabled()&&!(config_.retained_opaque_phase&&config_.test_retained_resource_pool_diagnostics)))
                    throw std::runtime_error("Partial retained fixture requires a bounded typed candidate prefix");
                for(const auto& operation:scene.at("operations"))
                    if(operation.at("type")=="query_begin"||operation.at("type")=="query_end")
                        throw std::runtime_error("Partial retained ownership fixture does not wrap unfinished captured queries");
            }
            const bool wrap_readonly_queries=config_.diagnostic_readonly_query_groups;
            if(wrap_readonly_queries&&wrap_frame_queries)
                throw std::runtime_error("Select either frame or readonly-group diagnostic queries");
            if(wrap_frame_queries||wrap_readonly_queries) {
                if(!supplied)throw std::runtime_error("Diagnostic frame queries require a supplied live capture");
                for(const auto& operation:scene.at("operations"))
                    if(operation.at("type")=="query_begin"||operation.at("type")=="query_end")
                        throw std::runtime_error("Diagnostic frame queries cannot wrap captured queries");
                report[wrap_readonly_queries?"diagnostic_readonly_group_queries":"diagnostic_frame_queries"]=Json::array();
                if(wrap_readonly_queries)report["diagnostic_readonly_group_scope"]=
                    "Synthetic query groups around captured null-PS readonly vertex draws; group completion drains GPU work. Original draw bytes unchanged; not original live query boundaries or a performance test.";
            }
            if(supplied) {
                const auto& expected=scene.at("operations");
                if(supplied->size()!=expected.size())throw std::runtime_error("Live replay operation count differs from original scene");
                for(size_t i=0;i<expected.size();++i) {
                    const auto* operation=(*supplied)[i];const auto& source=expected[i];
                    if(!operation||operation->Event()!=source.at("event").get<uint64_t>()||
                       operation->Frame()!=source.at("frame").get<uint32_t>()||
                       operation->Kind()!=source.at("type").get_ref<const std::string&>()||
                       (operation->Kind()=="draw"&&!operation->Draw()))
                        throw std::runtime_error("Live replay event/order/frame/type differs at operation "+std::to_string(i));
                }
                report["operation_dispatch"]=json_adapter?"live_plan_materialized_json":"live_plan_typed";
                report["supplied_live_operations"]=supplied->size();
                report["supplied_live_draws_executed"]=0;
                report["live_provenance_unmodified"]=true;
            }
            if(config_.native_completed_frame_capture&&!fs::exists(capture_/"gpu-history.json"))
                throw std::runtime_error("Native captured frames require actual initial GPU history");
            if(fs::exists(capture_/"gpu-history.json")) {
                if(diagnostic_zero_history_)throw std::runtime_error("Cannot combine reference history with synthetic history");
                std::ifstream metadata(capture_/"gpu-history.json");Json history;metadata>>history;
                const std::string provenance=config_.native_completed_frame_capture?
                    "native_gpu_frame_boundary":"reference_gpu_frame_boundary";
                if(history.at("version")!=1||history.at("provenance")!=provenance||
                   history.at("frame")!=scene.at("frames").at(0))
                    throw std::runtime_error("GPU history does not match the first captured frame");
                auto load=[&](const Json& entry) {
                    const auto digest=Digest(entry.at("sha256"));
                    auto bytes=Read(capture_/(digest+".gpu.bin"),512u*1024*1024);
                    if(bytes.size()!=entry.at("bytes").get<uint64_t>()||picosha2::hash256_hex_string(bytes)!=digest)
                        throw std::runtime_error("GPU history payload integrity failure");
                    return bytes;
                };
                std::vector<std::pair<uint32_t,uint32_t>> ranges;
                uint64_t total=0;
                for(const auto& entry:history.at("ranges")) {
                    const auto address=entry.at("address").get<uint64_t>(),size=entry.at("bytes").get<uint64_t>();
                    if(!size||size>512u*1024*1024||address>512u*1024*1024-size||address%4||size%4)
                        throw std::runtime_error("Invalid initial GPU range");
                    ranges.emplace_back(uint32_t(address),uint32_t(address+size));total+=size;
                }
                std::sort(ranges.begin(),ranges.end());
                for(size_t i=1;i<ranges.size();++i)if(ranges[i].first<ranges[i-1].second)
                    throw std::runtime_error("Overlapping initial GPU ranges");
                for(const auto& entry:history.at("ranges"))resolve_memory_->Initialize(entry.at("address"),load(entry));
                if(!history.at("edram").is_null()) {
                    auto bytes=load(history.at("edram"));
                    if(bytes.size()!=2048*5120)throw std::runtime_error("Incorrect initial EDRAM extent");
                    edram_memory_->Initialize(0,bytes);
                }
                if(!history.at("gamma").is_null()) {
                    const auto gamma=load(history.at("gamma"));
                    if(gamma.size()!=2560)throw std::runtime_error("Incorrect gamma ramp extent");
                    report["initial_gamma"]=history.at("gamma");
                }
                report["initial_gpu_history"]={{"frame",history.at("frame")},{"bytes",total},
                    {"ranges",ranges.size()}};
                if(config_.native_completed_frame_capture) {
                    report["initial_gpu_history"]["provenance"]=history.at("provenance");
                    const auto manifest=Read(capture_/"native-frame-capture.json",64u*1024*1024);
                    report["initial_gpu_history"]["native_capture_sha256"]=ContentHash(manifest);
                } else report["initial_gpu_history"]["trace_sha256"]=Digest(history.at("trace_sha256"));
                // The pre-frame frontbuffer is a useful reference image, but
                // it belongs to the preceding frame, not an equivalence pair.
                for(const auto& op:scene.at("operations"))if(op.at("type")=="present") {
                    const auto fetch=op.at("frontbuffer_fetch").get<std::array<uint32_t,6>>();
                    const auto layout=DescribeTexture(fetch);
                    if(resolve_memory_->FullyDefined(layout,layout.base_address)) {
                        auto prior=resolve_memory_->Load(layout,layout.base_address);
                        Preview(*prior,"reference-previous-frontbuffer");
                    }
                    break;
                }
            }
            size_t supplied_index=0;
            std::optional<uint32_t> queried_frame;
            uint64_t query_generation=0;
            const auto finish_query=[&] {
                if(!queried_frame)return;
                Execute(Json{{"type","query_end"},{"frame",*queried_frame},{"event",event},
                    {"query_generation",query_generation},{"query_handle",0x1000},
                    {"report_address",0x1fffffc0}});
                FlushPendingDraws();
                const auto completed=TakeCompletedQueries();
                if(completed.size()!=1||completed[0].generation!=query_generation||
                   completed[0].frame!=*queried_frame)
                    throw std::runtime_error("Diagnostic frame query lost exact generation completion");
                report[wrap_readonly_queries?"diagnostic_readonly_group_queries":"diagnostic_frame_queries"].push_back({{"frame",*queried_frame},
                    {"generation",query_generation},{"draws",completed[0].result.draws},
                    {"samples",completed[0].result.samples}});
                queried_frame.reset();
            };
            for(const auto& o:scene.at("operations")) {
                event=o.at("event");if(event>through)break;
                for(const auto& issue:scene.at("issues"))if(issue.at("event")==event)throw std::runtime_error(issue.at("reason"));
                if(wrap_frame_queries||wrap_readonly_queries) {
                    const auto frame=o.at("frame").get<uint32_t>();
                    bool query_operation=o.at("type")!="present";
                    if(wrap_readonly_queries) {
                        const auto* draw=(*supplied)[supplied_index]->Draw();
                        query_operation=draw&&draw->vertex_shader&&!draw->pixel_shader&&
                            draw->vertex_shader->hash.View()==kReadonlyQueryVertexSource;
                    }
                    if(queried_frame&&*queried_frame!=frame)finish_query();
                    if(!query_operation)finish_query();
                    else if(!queried_frame) {
                        ++query_generation;queried_frame=frame;
                        Execute(Json{{"type","query_begin"},{"frame",frame},{"event",event},
                            {"query_generation",query_generation},{"query_handle",0x1000},
                            {"report_address",0x1fffffe0}});
                    }
                }
                if(supplied) {
                    const auto& operation=*(*supplied)[supplied_index];
                    if(json_adapter)Execute(operation.ToJson());
                    else if(const auto* draw=operation.Draw())Execute(*draw);
                    else if(const auto* query=operation.Query())Execute(*query);
                    else Execute(operation.Control());
                    if(operation.Draw())report["supplied_live_draws_executed"]=
                        report["supplied_live_draws_executed"].get<uint64_t>()+1;
                    ++supplied_index;
                } else Execute(o);
                if(config_.native_completed_frame_capture) {
                    // The live consumer retires query results throughout execution.
                    // Offline replay needs no guest publication or live scheduling;
                    // retire each captured end synchronously to preserve capacity.
                    if(o.at("type")=="query_end") {
                        // Lazy mode keeps open draw groups across query ends like
                        // live execution; it drains only near metadata capacity.
                        if(!config_.replay_lazy_query_retirement||deferred_queries_.size()*2>=config_.max_deferred_queries)FlushPendingDraws();
                        else PollCompletedQueries();
                        TakeCompletedQueries();
                    }
                    if(o.at("type")=="present") {
                        FlushPendingDraws();TakeCompletedQueries();
                        if(!config_.report_frame_capture_mismatch)CheckNativeCapturedFrame(o.at("frame").get<uint32_t>());
                        else try {CheckNativeCapturedFrame(o.at("frame").get<uint32_t>());}
                        catch(const std::runtime_error& error) {
                            std::cout<<"Frame "<<o.at("frame").get<uint32_t>()<<" capture mismatch (reported): "<<error.what()<<"\n";
                        }
                    }
                }
                if(diagnostic_counters_.draws%100==0)std::cout<<"Completed event "<<event<<", "<<diagnostic_counters_.draws<<" native draws\n"<<std::flush;
            }
            finish_query();
            if(partial_owned_fixture) {
                report["partial_owned_retained_fixture"]={{"pending_gpu_work",HasPendingGpuWork()},
                    {"dirty_retained_authority",retained_opaque_current_&&retained_opaque_current_->authority.dirty()},
                    {"drain_deferred_to_destructor",true}};
                report["requested_range_complete"]=true;SaveReport();return;
            }
            FlushPendingDraws();report["requested_range_complete"]=true;SaveReport();
        } catch(const std::exception& e) {
            report["stopped_event"]=event;report["error"]=e.what();SaveReport();
            throw std::runtime_error("Event "+std::to_string(event)+": "+e.what());
        }
    }
    // Async pipelines. Declared last so it is destroyed first: workers join
    // and unconsumed pipelines are released while the device still exists.
    struct AsyncPipelineJob {
        // claimed: a worker or the command thread owns the build.
        std::mutex mutex;std::condition_variable done_cv;bool done=false,claimed=false;
        std::unique_ptr<RenderPipeline> pipeline;
        std::function<void()> build;
        std::chrono::steady_clock::time_point started;
        const RenderShader* vs=nullptr;const RenderShader* ps=nullptr;
    };
    struct AsyncPipelineBuilder {
        std::mutex mutex;std::condition_variable work_cv;
        std::deque<std::function<void()>> work;bool stopping=false;
        std::vector<std::thread> workers;
        // Command thread only.
        std::map<PipelineIdentity,std::shared_ptr<AsyncPipelineJob>,PipelineLess> jobs;
        std::unordered_map<const RenderShader*,uint32_t> pins;
        uint64_t started=0,skipped_draws=0,budget_waits=0,budget_builds=0,adopted=0,failures=0;
        // Missing pipelines that stayed synchronous, by reason.
        uint64_t sync_fsi_state=0,sync_variant=0,sync_recorded=0,sync_resident_fsi=0,sync_other=0,skipped_recorded=0;
        void Start(unsigned count) {
            for(unsigned i=0;i<count;++i)workers.emplace_back([this] {
                for(;;) {
                    std::function<void()> task;
                    {
                        std::unique_lock lock(mutex);
                        work_cv.wait(lock,[&]{return stopping||!work.empty();});
                        if(work.empty())return;
                        task=std::move(work.front());work.pop_front();
                    }
                    task();
                }
            });
        }
        void Post(std::function<void()> task) {
            {std::lock_guard lock(mutex);work.push_back(std::move(task));}
            work_cv.notify_one();
        }
        ~AsyncPipelineBuilder() {
            {std::lock_guard lock(mutex);stopping=true;work.clear();}
            work_cv.notify_all();
            for(auto& worker:workers)if(worker.joinable())worker.join();
            jobs.clear();
        }
    } async_pipelines_;
    bool AsyncPinned(const RenderShader* shader) const {return shader&&async_pipelines_.pins.contains(shader);}
    void UnpinAsync(const RenderShader* shader) {
        if(!shader)return;
        if(auto it=async_pipelines_.pins.find(shader);it!=async_pipelines_.pins.end()&&!--it->second)
            async_pipelines_.pins.erase(it);
    }
    // True: the draw may proceed (resident, finished, not eligible, or over
    // budget after waiting). False: its pipeline is building; skip the draw.
    bool AsyncPipelineReady(const PipelineLookup& key,const RenderGraphicsPipelineDesc& p) {
        if(const auto resident=pipelines_.find(key);resident!=pipelines_.end()&&resident->second.pipeline)return true;
        // Ordinary FSI PSOs are fully described by key, layout and modules.
        // Hardware render target PSOs carry an owned copy of the full
        // descriptor, whose decoded state the key identifies exactly.
        const bool ordinary=!key.fsi_state&&key.pixel_variant==NativeShaderVariant::Unmodified&&
            key.vertex_variant==NativeShaderVariant::Unmodified&&p.vertexShader;
        const bool fsi_job=ordinary&&config_.native_fsi&&p.pixelShader&&
            std::holds_alternative<std::array<uint32_t,6>>(key.raster);
        const bool hardware_job=ordinary&&!config_.native_fsi&&
            std::holds_alternative<std::array<uint32_t,kDrawStateWords>>(key.raster);
        if(!fsi_job&&!hardware_job)return true;
        auto found=async_pipelines_.jobs.find(key);
        if(found==async_pipelines_.jobs.end()) {
            auto job=std::make_shared<AsyncPipelineJob>();
            job->started=std::chrono::steady_clock::now();job->vs=p.vertexShader;job->ps=p.pixelShader;
            ++async_pipelines_.pins[p.vertexShader];if(p.pixelShader)++async_pipelines_.pins[p.pixelShader];
            async_pipelines_.jobs.emplace(PipelineIdentity{OwnDigest(key.vertex),OwnDigest(key.pixel),key.raster,
                key.color_format,key.depth_format,key.pixel_variant,key.vertex_variant,key.fsi_state},job);
            ++async_pipelines_.started;
            if(hardware_job) {
                struct OwnedDescriptor {
                    RenderGraphicsPipelineDesc desc;std::vector<RenderSpecConstant> spec;
                    std::vector<RenderInputSlot> slots;std::vector<RenderInputElement> elements;
                };
                auto owned=std::make_shared<OwnedDescriptor>();owned->desc=p;
                owned->spec.assign(p.specConstants,p.specConstants+p.specConstantsCount);
                owned->slots.assign(p.inputSlots,p.inputSlots+p.inputSlotsCount);
                owned->elements.assign(p.inputElements,p.inputElements+p.inputElementsCount);
                owned->desc.specConstants=owned->spec.empty()?nullptr:owned->spec.data();
                owned->desc.inputSlots=owned->slots.empty()?nullptr:owned->slots.data();
                owned->desc.inputElements=owned->elements.empty()?nullptr:owned->elements.data();
                job->build=[job=job.get(),device=device_,owned] {
                    std::unique_ptr<RenderPipeline> built;
                    try {
                        built=device->createGraphicsPipeline(owned->desc);
                        if(built&&!static_cast<VulkanGraphicsPipeline*>(built.get())->vk)built.reset();
                    } catch(...) {built.reset();}  // The adopting draw retries synchronously.
                    {std::lock_guard lock(job->mutex);job->pipeline=std::move(built);job->done=true;}
                    job->done_cv.notify_all();
                };
            } else
            job->build=[job=job.get(),device=device_,raster=std::get<std::array<uint32_t,6>>(key.raster),
                        color=key.color_format,depth=key.depth_format,layout=p.pipelineLayout] {
                std::unique_ptr<RenderPipeline> built;
                try {
                    NativeFsiPipelineDescriptor owned(raster,color,depth,layout,job->vs,job->ps,nullptr);
                    built=device->createGraphicsPipeline(owned.descriptor);
                    if(built&&!static_cast<VulkanGraphicsPipeline*>(built.get())->vk)built.reset();
                } catch(...) {built.reset();}  // The adopting draw retries synchronously.
                {std::lock_guard lock(job->mutex);job->pipeline=std::move(built);job->done=true;}
                job->done_cv.notify_all();
            };
            async_pipelines_.Post([job] {
                {std::lock_guard lock(job->mutex);if(job->claimed)return;job->claimed=true;}
                job->build();
            });
            return false;
        }
        auto& job=*found->second;
        std::unique_lock lock(job.mutex);
        if(job.done)return true;
        if(std::chrono::steady_clock::now()-job.started>=config_.async_pipeline_budget) {
            if(!job.claimed) {
                // Still queued behind other builds: build it here now rather
                // than wait for the queue to reach it.
                job.claimed=true;lock.unlock();
                ++async_pipelines_.budget_builds;job.build();
                return true;
            }
            ++async_pipelines_.budget_waits;
            job.done_cv.wait(lock,[&]{return job.done;});
            return true;
        }
        return false;
    }
    // Pipeline() calls this on a miss: a finished background build replaces
    // the synchronous creation (null if none, or if the build failed).
    std::unique_ptr<RenderPipeline> TakeAsyncPipeline(const PipelineLookup& key) {
        auto found=async_pipelines_.jobs.find(key);
        if(found==async_pipelines_.jobs.end())return nullptr;
        auto job=found->second;
        std::unique_ptr<RenderPipeline> pipeline;
        {
            std::unique_lock lock(job->mutex);
            job->done_cv.wait(lock,[&]{return job->done;});
            pipeline=std::move(job->pipeline);
        }
        UnpinAsync(job->vs);UnpinAsync(job->ps);
        async_pipelines_.jobs.erase(found);
        if(pipeline)++async_pipelines_.adopted;else ++async_pipelines_.failures;
        return pipeline;
    }
};
SceneExecutor::SceneExecutor(SceneExecutorConfig config,RenderDevice* device,RenderCommandQueue* queue)
    :diagnostic_generation_(AllocateDiagnosticSceneGeneration()),
     state_(std::make_unique<State>(std::move(config),device,queue)) {}
SceneExecutor::~SceneExecutor() {
    // Runtime shutdown drains explicitly so errors remain fatal and visible.
    // Standalone callers may destroy an executor after a partial draw range.
    try {state_->FlushCommittedState();state_->FlushPendingDraws();}
    catch(const std::exception& error) {
        std::cerr<<"Native renderer shutdown completion failed: "<<error.what()<<'\n';
        std::terminate();
    } catch(...) {std::terminate();}
}
void SceneExecutor::Execute(const Json& op) {
    try {state_->Execute(op);}
    catch(const NativeGpuWorkError& error){throw NativeBatchCompletionError(error.what());}
    catch(const NativeTransferBatchError& error){throw NativeBatchCompletionError(error.what());}
    catch(const std::exception& error) {
        if(state_->draw_group_recording_failed_)throw NativeBatchCompletionError("Native grouped draw recording failed: "+std::string(error.what()));
        throw;
    } catch(...) {
        if(state_->draw_group_recording_failed_)throw NativeBatchCompletionError("Native grouped draw recording failed");
        throw;
    }
}
void SceneExecutor::Execute(const TypedLiveQuery& op) {
    try {state_->Execute(op);}
    catch(const NativeGpuWorkError& error){throw NativeBatchCompletionError(error.what());}
    catch(const NativeTransferBatchError& error){throw NativeBatchCompletionError(error.what());}
    catch(const std::exception& error) {
        if(state_->draw_group_recording_failed_)throw NativeBatchCompletionError("Native grouped draw recording failed: "+std::string(error.what()));
        throw;
    } catch(...) {
        if(state_->draw_group_recording_failed_)throw NativeBatchCompletionError("Native grouped draw recording failed");
        throw;
    }
}
void SceneExecutor::Execute(const TypedLiveDraw& op) {
    try {state_->Execute(op);}
    catch(const NativeGpuWorkError& error){throw NativeBatchCompletionError(error.what());}
    catch(const NativeTransferBatchError& error){throw NativeBatchCompletionError(error.what());}
    catch(const std::exception& error) {
        if(state_->draw_group_recording_failed_)throw NativeBatchCompletionError("Native grouped draw recording failed: "+std::string(error.what()));
        throw;
    } catch(...) {
        if(state_->draw_group_recording_failed_)throw NativeBatchCompletionError("Native grouped draw recording failed");
        throw;
    }
}
void SceneExecutor::InitializeMemory(uint32_t address,std::span<const uint8_t> bytes) {state_->FlushCommittedState();state_->InitializeMemory(address,bytes);}
void SceneExecutor::InitializeEdram(std::span<const uint8_t> bytes) {state_->InitializeEdram(bytes);}
bool SceneExecutor::PrewarmRetainedSharedDepthQueryPipeline(const NativeFsiPipelineRecipe& recipe,std::span<const uint32_t> vertex,std::span<const uint32_t> original_builtin) {return state_->PrewarmRetainedSharedDepthQueryPipeline(recipe,vertex,original_builtin);}
bool SceneExecutor::PrewarmRetainedOpaquePhasePipeline(const NativeFsiPipelineRecipe& recipe,std::span<const uint32_t> vertex,std::span<const uint32_t> pixel) {
    return state_->PrewarmRetainedOpaquePhasePipeline(recipe,vertex,pixel);
}
bool SceneExecutor::PrewarmRetainedFp24DepthPipeline(const NativeFsiPipelineRecipe& recipe,std::span<const uint32_t> vertex,std::span<const uint32_t> builtin) {
    return state_->PrewarmRetainedFp24DepthPipeline(recipe,vertex,builtin);
}
bool SceneExecutor::PrewarmNativeEarlyDepthPipeline(const NativeFsiPipelineRecipe& recipe,std::span<const uint32_t> vertex,std::span<const uint32_t> pixel,
    std::shared_ptr<const NativeEarlyDepthProgram> program) {return state_->PrewarmNativeEarlyDepthPipeline(recipe,vertex,pixel,std::move(program));}
bool SceneExecutor::PrewarmNativeEarlyDepthCatalogPipeline(const NativeFsiPipelineRecipe& recipe,std::span<const uint32_t> vertex,std::span<const uint32_t> pixel,
    std::shared_ptr<const NativeEarlyDepthProgram> program) {return state_->PrewarmNativeEarlyDepthCatalogPipeline(recipe,vertex,pixel,std::move(program));}
bool SceneExecutor::PrewarmNativeReadonlyDepthPipeline(const NativeFsiPipelineRecipe& recipe,std::span<const uint32_t> vertex,std::span<const uint32_t> pixel) {return state_->PrewarmNativeReadonlyDepthPipeline(recipe,vertex,pixel);}
bool SceneExecutor::PrewarmReadonlyDepthQueryPipeline(const NativeFsiPipelineRecipe& recipe,
    std::span<const uint32_t> vertex,std::span<const uint32_t> pixel) {
    return state_->PrewarmReadonlyDepthQueryPipeline(recipe,vertex,pixel);
}
bool SceneExecutor::PrewarmOpaqueColorOverwritePipeline(const NativeFsiPipelineRecipe& recipe,
    std::span<const uint32_t> vertex) {return state_->PrewarmOpaqueColorOverwritePipeline(recipe,vertex);}
bool SceneExecutor::PrewarmForwardControlFlowPipeline(const NativeFsiPipelineRecipe& recipe,
    std::span<const uint32_t> vertex) {return state_->PrewarmForwardControlFlowPipeline(recipe,vertex);}
bool SceneExecutor::PrewarmForwardVertexPipeline(const NativeFsiPipelineRecipe& recipe,
    std::span<const uint32_t> vertex) {return state_->PrewarmForwardVertexPipeline(recipe,vertex);}
bool SceneExecutor::PrewarmFsiStatePipeline(const NativeFsiStateRecipe& recipe,std::span<const uint32_t> vertex) {
    return state_->PrewarmFsiStatePipeline(recipe,vertex);
}
bool SceneExecutor::PrewarmForwardPixelCatalogPipeline(const NativeFsiPipelineRecipe& recipe,
    std::span<const uint32_t> vertex) {return state_->PrewarmForwardPixelCatalogPipeline(recipe,vertex);}
bool SceneExecutor::PrewarmStencilMaterialPipeline(const NativeFsiPipelineRecipe& recipe,std::span<const uint32_t> vertex,
    std::span<const uint32_t> pixel) {return state_->PrewarmStencilMaterialPipeline(recipe,vertex,pixel);}
void SceneExecutor::PrewarmFsiPipeline(const NativeFsiPipelineRecipe& recipe,std::span<const uint32_t> vertex,
    std::span<const uint32_t> pixel) {state_->PrewarmFsiPipeline(recipe,vertex,pixel);}
NativeOverflowPrewarmResult SceneExecutor::PrewarmFsiOverflowPipeline(const NativeFsiPipelineRecipe& recipe,std::span<const uint32_t> vertex,std::span<const uint32_t> pixel) {return state_->PrewarmFsiOverflowPipeline(recipe,vertex,pixel);}
void SceneExecutor::InitializeEdramFromBuffer(VkBuffer source) {state_->InitializeEdramFromBuffer(source);}
void SceneExecutor::FlushCommittedState() {state_->AtEarlyDepthReadonlyBoundary([&]{state_->FlushCommittedState();});}
void SceneExecutor::FlushCommittedStateAtOperation() {state_->AtEarlyDepthReadonlyBoundary([&]{state_->FlushCommittedState(true);});}
void SceneExecutor::PrepareCommittedStateForDraw(const TypedLiveDraw& draw) {state_->PrepareCommittedStateForDraw(DrawInput(draw));}
bool SceneExecutor::PrepareCommittedStateForDeferredQuery(NativeDeferredQueryControl control) {return state_->PrepareCommittedStateForDeferredQuery(control);}
bool SceneExecutor::PrewarmRetainedOpaquePipeline(const NativeFsiPipelineRecipe& recipe,std::span<const uint32_t> vertex,std::span<const uint32_t> pixel) {return state_->PrewarmRetainedOpaquePipeline(recipe,vertex,pixel);}
NativeRetainedOpaqueResourceWitness SceneExecutor::RetainedResourcePoolWitnessForFixture()const {
    if(!state_->config_.test_retained_resource_pool_diagnostics||!state_->retained_opaque_writer_)
        throw std::invalid_argument("Retained resource observations require the explicit standalone fixture");
    auto out=state_->retained_opaque_writer_->InspectForFixture(state_->retained_opaque_current_);
    if(state_->gpu_work_queue_) {
        out.accepted_work=state_->gpu_work_queue_->accepted_work();out.completed_work=state_->gpu_work_queue_->completed_work();
    }
    return out;
}
SceneExecutor::RetainedOpaquePhaseOwnerWitness SceneExecutor::MutateRetainedOpaquePhaseOwnerForFixture(
        const NativeFsiPipelineRecipe& recipe,bool remove_pipeline) {
    auto& s=*state_;
    const auto completed=ObserveAcceptedWork();
    if(!s.config_.test_retained_resource_pool_diagnostics||!s.config_.retained_opaque_phase||
       !s.retained_opaque_writer_||!s.retained_opaque_current_||
       !s.retained_opaque_current_->authority.dirty()||s.retained_opaque_run_!=1||
       s.retained_opaque_stats_.selected!=1||s.queries_->active()||!s.deferred_queries_.empty()||
       !completed.available||!completed.accepted||completed.accepted!=completed.completed||
       s.HasPendingGpuWork()||s.test_retained_phase_old_shader_||s.test_retained_phase_old_pipeline_)
        throw std::invalid_argument("Opaque phase owner fixture requires one completed dirty draw and explicit standalone diagnostics");
    const auto* entry=recipe.pixel?NativeRetainedOpaquePhaseRecipe(recipe.vertex,*recipe.pixel,recipe.raster):nullptr;
    if(!entry||recipe.color_format!=int32_t(RenderFormat::R8G8B8A8_UNORM)||recipe.depth_format!=-1)
        throw std::invalid_argument("Opaque phase owner fixture recipe differs");
    const auto variant=NativeShaderVariant::RetainedOpaquePhaseConservativeV4;
    const auto vs=s.shaders_.find(ShaderCacheLookup{recipe.vertex,false,NativeShaderVariant::Unmodified});
    const auto ps=s.shaders_.find(ShaderCacheLookup{*recipe.pixel,true,variant});
    const PipelineLookup key{std::string_view(recipe.vertex),std::string_view(*recipe.pixel),PipelineRasterKey(recipe.raster),
        int32_t(RenderFormat::R32_UINT),int32_t(RenderFormat::D32_FLOAT),variant};
    const auto pipeline=s.pipelines_.find(key);
    const auto recipe_index=size_t(entry-kRetainedOpaquePhaseRecipes.data());
    if(vs==s.shaders_.end()||ps==s.shaders_.end()||!vs->second.shader||!ps->second.shader||
       !vs->second.audited_opaque_phase||!ps->second.audited_opaque_phase||
       pipeline==s.pipelines_.end()||!pipeline->second.pipeline||!pipeline->second.audited_opaque_phase||
       pipeline->second.opaque_phase_vertex!=vs->second.shader.get()||
       pipeline->second.opaque_phase_pixel!=ps->second.shader.get()||
       !s.retained_opaque_phase_headroom_.HasPipeline(recipe_index))
        throw std::invalid_argument("Opaque phase owner fixture needs the originally certified resident owners");
    const auto* program=s.RetainedOpaquePhaseProgramFor(*recipe.pixel);
    const auto bytes=program?std::span(reinterpret_cast<const uint8_t*>(program->spirv.data()),program->spirv.size()*4):std::span<const uint8_t>{};
    if(!program||!NativeRetainedOpaquePhaseModuleCertified(*recipe.pixel,true,variant,bytes)||
       ContentHash(bytes)!=program->spirv_sha256||bytes.size()!=ps->second.spirv_bytes)
        throw std::invalid_argument("Opaque phase owner fixture lost exact immutable module bytes");
    const auto ticket=s.retained_opaque_current_->authority.ticket();
    RetainedOpaquePhaseOwnerWitness out;
    out.removed_pipeline=remove_pipeline;out.certified_bytes=true;out.dirty=true;
    out.old_shader=reinterpret_cast<uintptr_t>(ps->second.shader.get());out.new_shader=out.old_shader;
    out.old_pipeline=reinterpret_cast<uintptr_t>(pipeline->second.pipeline.get());
    out.certified_vertex=reinterpret_cast<uintptr_t>(pipeline->second.opaque_phase_vertex);
    out.certified_pixel=reinterpret_cast<uintptr_t>(pipeline->second.opaque_phase_pixel);
    out.accepted_work=completed.accepted;out.completed_work=completed.completed;
    out.generation=ticket.generation;out.color_version=ticket.color;out.depth_version=ticket.depth;
    // Prepare everything that may throw before displacing the owner. Neither
    // branch rewrites a creation-time certificate or performs a queue action.
    if(remove_pipeline) {
        s.test_retained_phase_old_pipeline_=std::move(pipeline->second.pipeline);
        s.pipelines_.erase(pipeline);
        if(!s.retained_opaque_phase_headroom_.RetirePipeline(recipe_index))
            throw NativeBatchCompletionError("Opaque phase fixture PSO credit unexpectedly missing");
        // The actual old pipeline remains alive, but there is now no cached certificate to select.
    } else {
        auto replacement=s.device_->createShader(bytes.data(),bytes.size(),"main",RenderShaderFormat::SPIRV);
        if(!replacement||!static_cast<VulkanShader&>(*replacement).vk||
           !static_cast<VulkanShader&>(*ps->second.shader).vk||
           static_cast<VulkanShader&>(*replacement).vk==static_cast<VulkanShader&>(*ps->second.shader).vk||
           replacement.get()==ps->second.shader.get())
            throw std::runtime_error("Opaque phase fixture requires two simultaneously live distinct Vulkan modules");
        out.new_shader=reinterpret_cast<uintptr_t>(replacement.get());out.distinct_vulkan_modules=true;
        s.test_retained_phase_old_shader_=std::move(ps->second.shader);
        ps->second.shader=std::move(replacement);
        // These exact bytes remain certified. The existing PSO intentionally
        // keeps the OLD creation pointer and must therefore fail residency.
        out.pipeline_certificate_unchanged=pipeline->second.audited_opaque_phase&&
            pipeline->second.opaque_phase_pixel==s.test_retained_phase_old_shader_.get()&&
            pipeline->second.pipeline.get()==reinterpret_cast<RenderPipeline*>(out.old_pipeline);
    }
    s.transfer_report_dirty_=true;
    return out;
}
SceneExecutor::PackedColorVariant8OwnerWitness SceneExecutor::ReplacePackedColorVariant8ModuleForFixture(
        const NativeFsiPipelineRecipe& recipe,const NativeFsiStateSpecialization& specialization,
        std::span<const uint32_t> replacement) {
    auto& s=*state_;const auto before=ObserveAcceptedWork();
    if(!s.config_.test_retained_resource_pool_diagnostics||!s.config_.packed_color_render_pass_groups||
       !s.config_.native_fsi||!s.config_.specialize_fsi_pipeline_state||!before.available||
       before.accepted!=before.completed||s.HasPendingGpuWork()||s.open_draw_command_group_||
       s.draw_group_recording_||s.draw_group_recording_failed_||s.batch_failed_||
       (s.gpu_work_queue_&&s.gpu_work_queue_->failed())||s.queries_->active()||!s.deferred_queries_.empty()||
       s.test_packed_color_old_shader_||!recipe.pixel||
       !NativePackedColorVariant8Tuple(recipe.vertex,*recipe.pixel,recipe.raster,specialization,recipe.color_format,recipe.depth_format))
        throw std::invalid_argument("Packed-color owner fixture requires an exact tuple, completed frontier and standalone diagnostics");
    const auto variant=NativeShaderVariant::PipelineStateEpilogueV1;
    const auto vs=s.shaders_.find(ShaderCacheLookup{recipe.vertex,false,NativeShaderVariant::Unmodified});
    const auto ps=s.shaders_.find(ShaderCacheLookup{*recipe.pixel,true,variant});
    const PipelineLookup key{std::string_view(recipe.vertex),std::string_view(*recipe.pixel),PipelineRasterKey(recipe.raster),
        recipe.color_format,recipe.depth_format,variant,NativeShaderVariant::Unmodified,specialization};
    const auto pipeline=s.pipelines_.find(key);
    if(vs==s.shaders_.end()||!vs->second.shader||!vs->second.audited_monotonic_depth||
       ps==s.shaders_.end()||!ps->second.shader||pipeline==s.pipelines_.end()||!pipeline->second.pipeline||
       replacement.size()<5||replacement[0]!=0x07230203||replacement.size_bytes()!=ps->second.spirv_bytes)
        throw std::invalid_argument("Packed-color owner fixture requires resident exact owners and equal-size module bytes");
    const auto bytes=std::span(reinterpret_cast<const uint8_t*>(replacement.data()),replacement.size_bytes());
    const bool certified=NativeMonotonicDepthModuleCertified(*recipe.pixel,true,variant,bytes);
    auto shader=s.device_->createShader(bytes.data(),bytes.size(),"main",RenderShaderFormat::SPIRV);
    if(!shader||!static_cast<VulkanShader&>(*shader).vk||!static_cast<VulkanShader&>(*ps->second.shader).vk||
       shader.get()==ps->second.shader.get()||static_cast<VulkanShader&>(*shader).vk==static_cast<VulkanShader&>(*ps->second.shader).vk)
        throw std::runtime_error("Packed-color owner fixture requires two live distinct Vulkan shader modules");
    PackedColorVariant8OwnerWitness out;
    out.old_shader=reinterpret_cast<uintptr_t>(ps->second.shader.get());out.new_shader=reinterpret_cast<uintptr_t>(shader.get());
    out.pipeline=reinterpret_cast<uintptr_t>(pipeline->second.pipeline.get());out.distinct_vulkan_modules=true;
    out.old_module_certified=ps->second.audited_monotonic_depth;out.new_module_certified=certified;
    out.pipeline_certified_before=pipeline->second.audited_monotonic_depth;
    out.accepted_work=before.accepted;out.completed_work=before.completed;out.mutation_serial=s.mutation_serial();
    out.displaced_bytes=bytes.size();
    // All throwing preparation is complete. No cache eviction, queue action,
    // pipeline creation or certificate rewrite occurs at this replacement seam.
    s.test_packed_color_old_shader_=std::move(ps->second.shader);
    ps->second.shader=std::move(shader);ps->second.audited_monotonic_depth=certified;
    out.pipeline_certified_after=pipeline->second.audited_monotonic_depth;
    out.pipeline_unchanged=reinterpret_cast<uintptr_t>(pipeline->second.pipeline.get())==out.pipeline&&
        out.pipeline_certified_before==out.pipeline_certified_after;
    const auto after=ObserveAcceptedWork();
    out.frontier_unchanged=after.available&&after.accepted==out.accepted_work&&after.completed==out.completed_work;
    out.mutation_unchanged=s.mutation_serial()==out.mutation_serial;
    if(!out.pipeline_unchanged||!out.frontier_unchanged||!out.mutation_unchanged)
        throw NativeBatchCompletionError("Packed-color diagnostic module replacement changed execution authority");
    s.transfer_report_dirty_=true;return out;
}
SceneExecutor::PackedColorVariant8OwnerWitness SceneExecutor::ReplacePackedColorBlendVariant8ModuleForFixture(
        const NativeFsiPipelineRecipe& recipe,const NativeFsiStateSpecialization& specialization,
        std::span<const uint32_t> replacement) {
    auto& s=*state_;const auto before=ObserveAcceptedWork();
    if(!s.config_.test_retained_resource_pool_diagnostics||!s.config_.packed_color_render_pass_groups||!s.config_.packed_color_blend_pass_groups||
       !s.config_.native_fsi||!s.config_.specialize_fsi_pipeline_state||!before.available||
       before.accepted!=before.completed||s.HasPendingGpuWork()||s.open_draw_command_group_||
       s.draw_group_recording_||s.draw_group_recording_failed_||s.batch_failed_||
       (s.gpu_work_queue_&&s.gpu_work_queue_->failed())||s.queries_->active()||!s.deferred_queries_.empty()||
       s.test_packed_color_old_shader_||!recipe.pixel||
       !NativePackedColorBlendVariant8Tuple(recipe.vertex,*recipe.pixel,recipe.raster,specialization,recipe.color_format,recipe.depth_format))
        throw std::invalid_argument("Packed-blend owner fixture requires an exact tuple, completed frontier and standalone diagnostics");
    const auto variant=NativeShaderVariant::PipelineStateEpilogueV1;
    const auto vs=s.shaders_.find(ShaderCacheLookup{recipe.vertex,false,NativeShaderVariant::Unmodified});
    const auto ps=s.shaders_.find(ShaderCacheLookup{*recipe.pixel,true,variant});
    const PipelineLookup key{std::string_view(recipe.vertex),std::string_view(*recipe.pixel),PipelineRasterKey(recipe.raster),
        recipe.color_format,recipe.depth_format,variant,NativeShaderVariant::Unmodified,specialization};
    const auto pipeline=s.pipelines_.find(key);
    if(vs==s.shaders_.end()||!vs->second.shader||!vs->second.audited_packed_color_blend||
       ps==s.shaders_.end()||!ps->second.shader||pipeline==s.pipelines_.end()||!pipeline->second.pipeline||
       replacement.size()<5||replacement[0]!=0x07230203||replacement.size_bytes()!=ps->second.spirv_bytes)
        throw std::invalid_argument("Packed-blend owner fixture requires resident exact owners and equal-size module bytes");
    const auto bytes=std::span(reinterpret_cast<const uint8_t*>(replacement.data()),replacement.size_bytes());
    const bool certified=NativePackedColorBlendVariant8ModuleCertified(*recipe.pixel,true,variant,bytes);
    const bool monotonic=NativeMonotonicDepthModuleCertified(*recipe.pixel,true,variant,bytes);
    auto shader=s.device_->createShader(bytes.data(),bytes.size(),"main",RenderShaderFormat::SPIRV);
    if(!shader||!static_cast<VulkanShader&>(*shader).vk||!static_cast<VulkanShader&>(*ps->second.shader).vk||
       shader.get()==ps->second.shader.get()||static_cast<VulkanShader&>(*shader).vk==static_cast<VulkanShader&>(*ps->second.shader).vk)
        throw std::runtime_error("Packed-blend owner fixture requires two live distinct Vulkan shader modules");
    PackedColorVariant8OwnerWitness out;
    out.old_shader=reinterpret_cast<uintptr_t>(ps->second.shader.get());out.new_shader=reinterpret_cast<uintptr_t>(shader.get());
    out.pipeline=reinterpret_cast<uintptr_t>(pipeline->second.pipeline.get());out.distinct_vulkan_modules=true;
    out.old_module_certified=ps->second.audited_packed_color_blend;out.new_module_certified=certified;
    out.pipeline_certified_before=pipeline->second.audited_packed_color_blend;
    out.accepted_work=before.accepted;out.completed_work=before.completed;out.mutation_serial=s.mutation_serial();
    out.displaced_bytes=bytes.size();
    // All throwing preparation is complete. No cache eviction, queue action,
    // pipeline creation or certificate rewrite occurs at this replacement seam.
    s.test_packed_color_old_shader_=std::move(ps->second.shader);
    ps->second.shader=std::move(shader);ps->second.audited_packed_color_blend=certified;
    ps->second.audited_monotonic_depth=monotonic;
    out.pipeline_certified_after=pipeline->second.audited_packed_color_blend;
    out.pipeline_unchanged=reinterpret_cast<uintptr_t>(pipeline->second.pipeline.get())==out.pipeline&&
        out.pipeline_certified_before==out.pipeline_certified_after;
    const auto after=ObserveAcceptedWork();
    out.frontier_unchanged=after.available&&after.accepted==out.accepted_work&&after.completed==out.completed_work;
    out.mutation_unchanged=s.mutation_serial()==out.mutation_serial;
    if(!out.pipeline_unchanged||!out.frontier_unchanged||!out.mutation_unchanged)
        throw NativeBatchCompletionError("Packed-blend diagnostic module replacement changed execution authority");
    s.transfer_report_dirty_=true;return out;
}
void SceneExecutor::FlushPendingDraws() {state_->FlushPendingDraws();}
bool SceneExecutor::HasPendingDraws() const {return !state_->pending_draws_.empty();}
bool SceneExecutor::HasPendingGpuWork() const {return state_->HasPendingGpuWork();}
void SceneExecutor::FlushGpuWork() {state_->AtEarlyDepthReadonlyBoundary([&]{state_->FlushCommittedState();state_->FlushPendingDraws();});}
SceneExecutor::QueueCompletionSnapshot SceneExecutor::ObserveQueueCompletion() const noexcept {
    if(!state_->gpu_work_queue_)return {};
    const auto& stats=state_->gpu_work_queue_->statistics();
    return {true,stats.timings_enabled,stats.waits,stats.fence_wait_ms};
}
SceneExecutor::CompletedFrameSnapshot SceneExecutor::ObserveCompletedFrame(uint32_t expected_frame) const noexcept {
    if(!state_||state_->batch_failed_||state_->draw_group_recording_||state_->draw_group_recording_failed_||
       !state_->draw_snapshot_||!state_->texture_snapshot_||!state_->pending_draws_.empty()||
       state_->draw_timing_.count||(state_->gpu_work_queue_&&(state_->gpu_work_queue_->pending()||state_->gpu_work_queue_->failed()))||
       (state_->transfer_batch_&&(state_->transfer_batch_->in_scope()||state_->transfer_batch_->has_recording())))return {};
    const auto frame_matches=[expected_frame](const Json& frame) noexcept {
        if(const auto* value=frame.get_ptr<const Json::number_unsigned_t*>())return *value==expected_frame;
        if(const auto* value=frame.get_ptr<const Json::number_integer_t*>())return *value>=0&&uint64_t(*value)==expected_frame;
        return false;
    };
    const auto& draw=*state_->draw_snapshot_;const auto& texture=*state_->texture_snapshot_;
    if(!frame_matches(draw.frame)||!frame_matches(texture.frame)||
       !std::isfinite(draw.timing.submit_wait)||draw.timing.submit_wait<0)return {};
    CompletedFrameSnapshot out;
    out.available=true;out.frame=expected_frame;out.draws=draw.timing.count;
    out.completion_ms=draw.timing.submit_wait;
    out.cpu_uploads=texture.timing.uploads;out.cpu_payload_bytes=texture.timing.upload_bytes;
    out.gpu_overlay_bytes=texture.timing.overlay_bytes;out.overlay_copy_regions=texture.timing.overlay_regions;
    out.overlay_readback_bytes=texture.timing.overlay_readback_bytes;out.gpu_alias_loads=texture.timing.alias_loads;
    if(state_->gpu_work_queue_&&!state_->gpu_work_queue_->failed()) {
        const auto& queue=state_->gpu_work_queue_->statistics();out.queue_available=true;
        out.queue_submissions=queue.submissions;out.queue_waits=queue.waits;
        out.queue_polls=queue.fence_polls;out.queue_poll_retirements=queue.poll_retirements;
    }
    return out;
}
SceneExecutor::AcceptedWorkSnapshot SceneExecutor::ObserveAcceptedWork() const noexcept {
    if(!diagnostic_generation_||!state_->gpu_work_queue_||state_->batch_failed_||
        state_->draw_group_recording_||state_->draw_group_recording_failed_||
        (state_->transfer_batch_&&(state_->transfer_batch_->in_scope()||state_->transfer_batch_->has_recording())))return {};
    const auto observed=state_->gpu_work_queue_->ObserveAcceptedWork();
    return {diagnostic_generation_,observed.draws,observed.transfers,
        observed.accepted,observed.completed,observed.available};
}
SceneExecutor::MarkerQueryReadiness SceneExecutor::ObserveMarkerQueryReadiness() const noexcept {
    const auto work=ObserveAcceptedWork();
    if(!work.available||state_->deferred_queries_.size()>128||
       state_->ready_query_count_>state_->deferred_queries_.size())return {};
    size_t ready=state_->ready_query_count_;uint32_t unknown=0;
    for(size_t i=ready;i<state_->deferred_queries_.size();++i) {
        const auto& query=state_->deferred_queries_[i];
        const bool known=state_->config_.retire_completed_query_prefixes||query.diagnostic_frontier_known;
        const uint64_t required=state_->config_.retire_completed_query_prefixes?
            query.required_work:query.diagnostic_required_work;
        if(!known)++unknown;
        if(i==ready&&known&&required<=work.completed)++ready;
    }
    return {uint32_t(state_->deferred_queries_.size()),uint32_t(ready),unknown,
        state_->queries_->MetadataSnapshot().has_value(),true};
}
void SceneExecutor::CompleteGpuWorkForQueryCompletion() {state_->CompleteGpuWorkForQueryCompletion();}
SceneExecutor::QueryWorkObservation SceneExecutor::ObserveQueryWork(
    uint32_t frame,uint64_t generation,uint32_t address) const noexcept {
    if(!generation||!state_->config_.retire_completed_query_prefixes||state_->batch_failed_||
       state_->draw_group_recording_||state_->draw_group_recording_failed_||!state_->gpu_work_queue_||
       state_->deferred_queries_.size()>128)return {};
    const auto work=state_->gpu_work_queue_->ObserveSubmission();
    if(!work.available)return {};
    for(const auto& item:state_->deferred_queries_) {
        const auto& query=item.query;
        if(query.generation!=generation||query.frame!=frame||query.result.end_address!=address)continue;
        if(item.required_work>work.accepted)return {};
        const auto stage=item.required_work<=work.completed?QueryWorkStage::Completed:
            item.required_work<=work.submitted?QueryWorkStage::Submitted:QueryWorkStage::Staged;
        return {stage,item.required_work,work.accepted,work.submitted,work.completed};
    }
    return {};
}
void SceneExecutor::SubmitGpuWork() {state_->SubmitGpuWork();}
uint32_t SceneExecutor::DeferredQueryCount() const {return uint32_t(state_->deferred_queries_.size());}
std::vector<CompletedNativeQuery> SceneExecutor::TakeCompletedQueries() {return state_->TakeCompletedQueries();}
void SceneExecutor::PollCompletedQueries() {state_->PollCompletedQueries();}
void SceneExecutor::FlushOldestQuery() {state_->FlushOldestQuery();}
NativeRendererState SceneExecutor::ExportState() {return state_->ExportState();}
std::vector<uint8_t> SceneExecutor::ReadFullEdram() {return state_->ReadFullEdram();}
uint64_t SceneExecutor::mutation_serial() const {return state_->mutation_serial();}
std::vector<uint8_t> SceneExecutor::ReadPhysical(uint32_t address,uint32_t bytes) {state_->FlushCommittedState();return state_->ReadPhysical(address,bytes);}
std::optional<NativeQueryTracker::Result> SceneExecutor::ActiveQuery() const {state_->FlushPendingDraws();return state_->queries_->Snapshot();}
std::optional<NativeQueryTracker::Metadata> SceneExecutor::ActiveQueryMetadata() const {return state_->queries_->MetadataSnapshot();}
const Json& SceneExecutor::report() const {state_->SealDrawCommandGroup();state_->ReportQueries();state_->ReportTransfers();return state_->report;}
Json SceneExecutor::GpuBatchAttributionReport(bool details) const {state_->SealDrawCommandGroup();return state_->GpuBatchAttributionReport(details);}
Json SceneExecutor::GpuPhysicalTimingReport(bool details) const {return state_->gpu_work_queue_?state_->gpu_work_queue_->PhysicalTimingReport(details):Json{{"enabled",false}};}
Json SceneExecutor::GpuDrawTimingReport(bool details) const {state_->SealDrawCommandGroup();return state_->GpuDrawTimingReport(details);}
Json SceneExecutor::FragmentInvocationReport() const {return state_->FragmentInvocationReport();}
void SceneExecutor::RunCapture(uint32_t last_event) {state_->Run(last_event);}
void SceneExecutor::RunLiveCapturePartialForRetainedFixture(std::span<const LiveOperation* const> operations,uint32_t last_event) {
    state_->Run(last_event,&operations,false,false,true);
}
void SceneExecutor::RunLiveCapture(std::span<const LiveOperation* const> operations,uint32_t last_event,bool json_adapter,bool wrap_frame_queries) {
    state_->Run(last_event,&operations,json_adapter,wrap_frame_queries);
}
}
