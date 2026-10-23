#include "renderer/native/scene_executor.h"
#include "renderer/native/builtin_readonly_query.h"
#include "renderer/native/content_hash.h"
#include "renderer/native/fsi_device.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <fstream>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

using namespace plume;
using namespace gow3::renderer;
namespace {
using Json=nlohmann::json;
using Words=std::vector<uint32_t>;
using QueryIdentity=std::array<uint64_t,8>;
constexpr uint32_t kWidth=640,kHeight=360,kBase=1440,kBytes=2048*5120;
uint64_t checks=0;
void Check(bool condition,const char* message) {++checks;if(!condition)throw std::runtime_error(message);}
Words Read(const char* path) {
    std::ifstream file(path,std::ios::binary|std::ios::ate);
    Check(bool(file)&&file.tellg()>=20&&file.tellg()%4==0,"Invalid pinned Scene fixture shader");
    Words result(size_t(file.tellg())/4);file.seekg(0);
    Check(bool(file.read(reinterpret_cast<char*>(result.data()),result.size()*4)),"Truncated pinned shader");return result;
}
std::vector<uint8_t> Bytes(std::span<const uint32_t> words) {
    std::vector<uint8_t> result(words.size_bytes());std::memcpy(result.data(),words.data(),result.size());return result;
}
void Word(std::vector<uint8_t>& bytes,size_t at,uint32_t value) {
    for(unsigned i=0;i<4;++i)bytes.at(at+i)=uint8_t(value>>(24-8*i));
}
void Number(std::vector<uint8_t>& bytes,size_t at,float value) {Word(bytes,at,std::bit_cast<uint32_t>(value));}
struct AcceptedWork {
    bool pending=false;
    // Declared after Scene: an unexpected accepted failure cannot destroy GPU
    // owners while an unfenced prefix might still refer to them.
    ~AcceptedWork() {if(pending)std::terminate();}
};
struct Outcome {
    Words edram;
    std::vector<QueryIdentity> queries;
    Json stats,queue,shader_cache,pipeline_cache,no_write,groups,bindings;
    uint32_t pressure_publications=0,eligible=0;
    bool ready=false;
};
Words ExpectedPacked(uint32_t final_packed_word) {
    Words expected(kBytes/4,0xe000005a);
    // Independent complete packed-byte oracle. Last writer in both64+8 and
    //128+8 sequences writes incoming depth+.125, preserving stencil5a.
    // The caller supplies independently derived literal packed codes for the
    // admitted fixture planes; no candidate quantizer computes this oracle.
    // Enumerate each pixel, without invoking SurfaceFootprint/SurfaceByteOffset.
    for(uint32_t y=0;y<kHeight;++y)for(uint32_t x=0;x<kWidth;++x) {
        const uint64_t tile=uint64_t(y/16)*((kWidth+79)/80)+x/80;
        const uint32_t column=(x%80+40)%80;
        const uint32_t word=uint32_t(((uint64_t(kBase)+tile)*1280+(y%16)*80+column)%(kBytes/4));
        expected[word]=final_packed_word;
    }
    return expected;
}
Outcome RunScene(RenderDevice& device,RenderCommandQueue& queue,std::span<const uint32_t> vertex,
                 std::span<const uint32_t> builtin,uint32_t capacity,bool candidate,bool startup,
                 uint32_t pipeline_limit=2048,bool own_device=false,bool destructor_drain=false,
                 bool reverse_viewport=false,float clip_depth=.5f,bool skip_edram_barrier=false,
                 bool conservative=false,bool no_write_metadata=false,bool read_only_pass=false,bool lower_bound_query=false,
                 bool query_groups=false,bool empty_queries=false,bool skip_unused_descriptors=false) {
    Check((!reverse_viewport&&clip_depth==.5f)||(reverse_viewport&&(clip_depth==.25f||clip_depth==.75f)),
        "Unsupported independent Scene plane oracle");
    // Reverse viewport maps clip.25/.75 to actual raster.75/.25. The final
    //+.125 writer produces exact FP24.875/.375 (e14,m.75/e13,m.5).
    const uint32_t final_packed_word=!reverse_viewport?0xe400005a:
        clip_depth==.25f?0xec00005a:0xd800005a;
    std::map<std::string,std::vector<uint8_t>> payloads;
    const auto add=[&](std::vector<uint8_t> bytes) {
        auto key=ContentHash(bytes);payloads.emplace(key,std::move(bytes));return key;
    };
    const auto floats=[&](std::initializer_list<float> values) {
        std::vector<uint8_t> bytes(values.size()*4);std::memcpy(bytes.data(),values.begin(),bytes.size());return add(std::move(bytes));
    };
    // Same authenticated observed VS fetch/identity-matrix contract as
    // builtin_readonly_query_gpu_test.cpp's existing real RoomScene fixture.
    // Raster depth comes from this geometry and the original viewport transform.
    // No fragment parameter substitutes for interpolated FragCoord.
    // The pinned VS's identity transform and DXC -fvk-invert-y put this
    // triangle at (0,360),(1280,360),(0,-360), up to one uniform half-pixel
    // translation. Vulkan defines area as minus one-half the shoelace sum,
    // so the resulting positive area is front-facing for BACK/CCW. Clipping
    // preserves the orientation and covers the complete640x360 viewport.
    const std::array<std::array<double,2>,3> framebuffer_vertices{{
        {{0,double(kHeight)}},{{2*double(kWidth),double(kHeight)}},{{0,-double(kHeight)}}}};
    double twice_area=0;
    for(unsigned i=0;i<3;++i) {
        const auto& a=framebuffer_vertices[i];const auto& b=framebuffer_vertices[(i+1)%3];
        twice_area-=a[0]*b[1]-b[0]*a[1];
    }
    Check(twice_area==4*double(kWidth)*kHeight&&twice_area>0,
        "Pinned fullscreen triangle is not front-facing for actual BACK/CCW raster");
    const std::array<std::string,3> geometry_ids{add({0,1,2,0}),floats({0,0,0,1,1,1}),
        floats({-1,-1,clip_depth,3,-1,clip_depth,-1,3,clip_depth})};
    std::vector<uint8_t> state(0x2a80);
    for(unsigned i=0;i<4;++i)Number(state,0x780+16*i+4*i,1.f);
    Word(state,0x2880,kWidth);Word(state,0x28bc,kWidth|(kHeight<<16));Word(state,0x28c4,0x80000000);
    Word(state,0x28c8,kWidth|(kHeight<<16));Word(state,0x28dc,0);Word(state,0x2944,1<<19);Word(state,0x294c,0x43f);
    Word(state,0x2948,(1<<11)|(1<<12)|2u);Word(state,0x29c0,1);
    Number(state,0x2908,320);Number(state,0x290c,320);Number(state,0x2910,-180);Number(state,0x2914,180);Number(state,0x2918,reverse_viewport?-1.f:1.f);
    Number(state,0x291c,reverse_viewport?1.f:0.f);
    const Json depth{{"depth",true},{"edram_base",kBase},{"format",1},{"format_info",0},
        {"surface_info",kWidth},{"width",kWidth},{"height",kHeight}};
    Json geometry=Json::array();
    for(unsigned i=0;i<3;++i)geometry.push_back({{"kind",14},{"fetch_slot",93+i},{"endian",0},
        {"address","00007000"},{"bytes",payloads.at(geometry_ids[i]).size()},{"sha256",geometry_ids[i]}});
    SceneExecutorConfig config;config.native_fsi=true;config.ordered_pixel_interlock_enabled=true;
    config.native_readonly_depth_attachment=candidate;config.max_pipeline_cache_entries=pipeline_limit;
    config.native_readonly_depth_skip_edram_barrier=skip_edram_barrier;
    config.native_readonly_depth_read_only_pass=read_only_pass;
    config.native_readonly_depth_lower_bound_query=lower_bound_query;
    config.readonly_query_render_pass_groups=query_groups;config.host_query_reset=query_groups;
    config.skip_unused_descriptor_sets=skip_unused_descriptors;
    config.native_readonly_depth_conservative=conservative;
    config.native_readonly_depth_no_write_metadata=no_write_metadata;
    config.native_early_depth=!skip_edram_barrier; // Retain shipping aid except the existing incompatible barrier-skip cases.
    config.batch_transfers=config.unified_gpu_queue=config.async_gpu_submissions=config.defer_native_queries=true;
    config.discard_dummy_fsi_attachment=true;config.batch_draw_descriptors=true;
    config.max_draw_batch_size=16;config.max_inflight_batches=2;
    config.async_submission_capacity=capacity;config.max_deferred_queries=capacity;
    config.payload=[&](const std::string& key){return payloads.at(key);};
    config.shader=[&](const std::string& key) {
        Check(key==kReadonlyQueryVertexSource,"Unexpected Scene shader source");return Bytes(vertex);
    };
    config.fsi_depth_shader=[&]{return Bytes(builtin);};
    auto scene_owner=std::make_unique<SceneExecutor>(config,own_device?nullptr:&device,own_device?nullptr:&queue);
    auto& scene=*scene_owner;AcceptedWork accepted;
    const auto actual_device=scene.report().at("device");
    Check(actual_device.at("software")==false&&actual_device.at("vendor_id")==static_cast<VulkanDevice&>(device).physicalDeviceProperties.vendorID&&
        actual_device.at("device_id")==static_cast<VulkanDevice&>(device).physicalDeviceProperties.deviceID,
        "Owned/borrowed Scene did not use the pinned hardware device");
    NativeFsiPipelineRecipe recipe{std::string(kReadonlyQueryVertexSource),std::nullopt,
        {uint32_t(RenderCullMode::BACK),uint32_t(RenderFrontFace::COUNTER_CLOCKWISE),1,
         uint32_t(RenderPrimitiveTopology::TRIANGLE_LIST),0,0},int32_t(RenderFormat::R8G8B8A8_UNORM),-1};
    scene.PrewarmFsiPipeline(recipe,vertex,builtin);
    Outcome result;result.ready=candidate&&startup&&scene.PrewarmNativeReadonlyDepthPipeline(recipe,vertex,builtin);
    Check(result.ready==(candidate&&startup&&pipeline_limit!=2),"Unexpected D32 startup admission result");
    const bool conservative_ready=conservative&&result.ready&&pipeline_limit!=3;
    const auto original_cache=scene.report();
    Check(original_cache.at("native_readonly_depth_attachment").at("conservative_prepared")==unsigned(conservative_ready),
        "Optional conservative depth startup coverage differs from the independent budget case");
    // Optional startup work cannot destroy the canonical Native owners.
    scene.PrewarmFsiPipeline(recipe,vertex,builtin);
    Check(scene.report().at("shader_cache").at("creations")==original_cache.at("shader_cache").at("creations")&&
        scene.report().at("pipeline_cache").at("creations")==original_cache.at("pipeline_cache").at("creations"),
        "Original Native startup owner disappeared after optional prewarm");
    scene.InitializeEdram(Bytes(Words(kBytes/4,0xe000005a)));
    const uint32_t count=destructor_drain?3:capacity+8;uint64_t event=0;
    const auto collect=[&] {
        for(const auto& q:scene.TakeCompletedQueries())result.queries.push_back({q.generation,q.frame,q.end_event,
            q.result.handle,q.result.begin_address,q.result.end_address,q.result.samples,q.result.draws});
    };
    const auto relieve_query_pressure=[&] {
        if(scene.DeferredQueryCount()==capacity) {
            // Real bounded guest-publication pressure: complete the oldest
            // original frontier, rather than force-drain each draw/generation.
            scene.FlushOldestQuery();collect();++result.pressure_publications;
        }
    };
    for(uint32_t i=0;i<count;++i) {
        relieve_query_pressure();
        auto raw=state;const auto phase=i%6;const bool readonly=phase!=0&&phase!=3;
        Word(raw,0x2934,readonly?0x700762:0x700776);
        const float bias=readonly?0.f:phase==0?-.125f:.125f;
        Number(raw,0x2a54,bias);Number(raw,0x2a5c,bias);const auto state_id=add(std::move(raw));
        const uint32_t address=0x8000+64*(i%13),handle=0x1000+32*i;const uint64_t generation=i+1;
        scene.Execute({{"type","query_begin"},{"frame",7},{"event",++event},{"query_generation",generation},
            {"query_handle",handle},{"report_address",address+32}});
        accepted.pending=true;
        scene.Execute({{"type","draw"},{"frame",7},{"event",++event},{"function","DrawPrimitive"},
            {"args",{0,4,0,3,0,0,0,0}},{"device_state",state_id},{"vertex_shader",kReadonlyQueryVertexSource},
            {"pixel_shader",nullptr},{"surfaces",{{"10",depth}}},{"geometry",geometry},
            {"vertex_slots",{93,94,95}},{"textures",Json::array()}});
        scene.Execute({{"type","query_end"},{"frame",7},{"event",++event},{"query_generation",generation},
            {"query_handle",handle},{"report_address",address}});
        result.eligible+=readonly;
        if(empty_queries&&phase==1) {
            relieve_query_pressure();
            scene.Execute({{"type","query_begin"},{"frame",7},{"event",++event},{"query_generation",0x10000u+i},
                {"query_handle",0x60000u+32*i},{"report_address",0x40020u+64*(i%13)}});
            scene.Execute({{"type","query_end"},{"frame",7},{"event",++event},{"query_generation",0x10000u+i},
                {"query_handle",0x60000u+32*i},{"report_address",0x40000u+64*(i%13)}});
        }
    }
    if(destructor_drain) {
        Check(own_device&&result.ready,"Partial teardown requires an owned prepared D32 Scene");
        Check(scene.HasPendingGpuWork()&&scene.DeferredQueryCount()==count,
            "Partial teardown has no accepted work/query owners to drain");
        result.stats=scene.report().at("native_readonly_depth_attachment");
        if(query_groups)Check(scene.report().at("readonly_query_render_pass_groups").at("render_passes_avoided")==1,
            "Owned partial teardown did not retain the open readonly query group");
        Check(result.stats.at("selected")==2&&result.stats.at("cache_hits")==1&&result.stats.at("conversions")==1,
            "Partial owned teardown did not create/reuse the cached D32 generation");
        // Transfer the accepted-work obligation to Scene's terminating drain.
        // Reset must complete before reporting success or destroying the device.
        accepted.pending=false;scene_owner.reset();
        std::cout<<"READONLY_DEPTH_SCENE_OWNED_PARTIAL_SHUTDOWN "<<Json{{"capacity",capacity},
            {"accepted_draws",count},{"actual_device",actual_device},{"stats_before_shutdown",result.stats},
            {"shutdown_returned",true},{"explicit_flush",false}}.dump()<<'\n';
        return result;
    }
    scene.FlushGpuWork();collect();accepted.pending=false;
    Check(!scene.HasPendingGpuWork()&&scene.DeferredQueryCount()==0,"Scene query/owner frontier remains pending");
    Check(result.pressure_publications>0&&result.queries.size()==count+(empty_queries?(count+4)/6:0),"Query pressure/publication coverage was vacuous");
    for(uint32_t i=0;i<count;++i) {
        const uint32_t empty_before=empty_queries?(i+4)/6:0;
        const auto& q=result.queries[i+empty_before];const uint32_t address=0x8000+64*(i%13);
        const uint64_t samples=i%6>=4?0:uint64_t(kWidth)*kHeight;
        const uint64_t end_event=uint64_t(3)*(i+1)+2*empty_before;
        Check(q==QueryIdentity{i+1,7,end_event,0x1000+32*i,address+32,address,samples,1},
            "Actual Scene query identity, zero/positive samples or watermark differs from independent oracle");
        if(empty_queries&&i%6==1)Check(result.queries[i+empty_before+1]==
            QueryIdentity{0x10000u+i,7,end_event+2,0x60000u+32*i,0x40020u+64*(i%13),0x40000u+64*(i%13),0,0},
            "Empty query inside a read-only group changed identity, order, samples or draw count");
    }
    const auto& report=scene.report();result.stats=report.at("native_readonly_depth_attachment");
    result.groups=report.at("readonly_query_render_pass_groups");
    if(skip_unused_descriptors) {
        result.bindings=report.at("unused_descriptor_sets");
        const uint64_t required=count-(result.ready?result.eligible:0);
        Check(result.bindings.at("unknown_draws")==0&&result.bindings.at("draws")==count&&
              result.bindings.at("selected_draws")==count&&
              result.bindings.at("required_set_bindings")==required&&
              result.bindings.at("omitted_set_bindings")==5*count-required,
              "D32 queries must bind zero sets; packed null fragments must retain set4");
    }
    result.no_write=report.at("native_readonly_depth_no_write_metadata");
    if(!skip_edram_barrier)Check(report.at("native_early_depth").at("enabled")==true,
        "Readonly Scene comparison did not keep shipping early-depth enabled");
    Check(result.no_write.at("enabled")==no_write_metadata&&
        result.no_write.at("selected")==uint64_t(no_write_metadata&&result.ready?result.eligible:0)&&
        result.no_write.at("refusals")==0,"Readonly no-write selection/refusal coverage differs");
    result.queue=report.at("gpu_work_queue");result.shader_cache=report.at("shader_cache");result.pipeline_cache=report.at("pipeline_cache");
    Check(report.at("draws")==count,"Actual Scene logical draw count differs");
    Check(report.at("pipelines_created")==unsigned((result.ready?2:1)+conservative_ready),"D32/generic pipeline identity collided or compiled in gameplay");
    Check(result.stats.at("conservative_configured")==conservative,
        "Optional conservative depth configuration does not match this case");
    Check(result.stats.at("conservative_prepared")==unsigned(conservative_ready)&&
        result.stats.at("conservative_selected")==uint64_t(conservative_ready?result.eligible:0),
        "Conservative depth prepared/accepted selection coverage differs");
    Check(result.stats.at("conservative_module_refusals")==unsigned(conservative&&result.ready&&pipeline_limit==3)&&
        result.stats.at("conservative_residency_refusals")==uint64_t(conservative&&result.ready&&!conservative_ready?result.eligible:0),
        "Conservative depth optional startup/residency refusal provenance differs");
    if(candidate) {
        const auto& s=result.stats;
        Check(s.at("skip_edram_barrier_enabled")==skip_edram_barrier,
            "Separate packed EDRAM barrier flag does not match this case");
        Check(s.at("skipped_edram_barriers").get<uint64_t>()+s.at("retained_edram_barriers").get<uint64_t>()==s.at("selected").get<uint64_t>(),
            "Accepted selected barrier counters do not conserve real D32 draws");
        Check(s.at(skip_edram_barrier?"retained_edram_barriers":"skipped_edram_barriers")==0,
            "Packed EDRAM barrier coverage crossed the separate flag");
        Check(s.at("supported_viewport_depth_ranges")==Json::array({Json::array({0,1}),Json::array({1,0})}),
            "Scene fixture does not prove the complete forward/reverse unit viewport scope");
        Check(s.at("supported_raster")==Json::array({3,2,1,4,0,0}),
            "Scene fixture did not select the exact actual captured BACK/CCW raster");
        Check(s.at("lower_bound_query")==lower_bound_query&&s.at("identity")==
            (lower_bound_query?"native-readonly-query-full-fp24-predecessor-exact-packed-discard-v1":"native-readonly-depth-d32-v2-exact-half-zero-bias-load-store"),
            "Scene fixture does not exercise the corrected LOAD+STORE shader/pipeline identity");
        Check(s.at("candidates")==result.eligible,"Dynamic readonly classifier did not match the exact zero-bias queried shape");
        if(result.ready) {
            Check(s.at("selected").get<uint64_t>()>0&&s.at("selected")==s.at("queried_selected")&&
                s.at("promotions").get<uint64_t>()+result.no_write.at("selected").get<uint64_t>()==s.at("selected").get<uint64_t>()&&s.at("promotion_refusals")==0,
                "Scene did not execute/certify accepted audited readonly draws exactly");
            Check(s.at(skip_edram_barrier?"skipped_edram_barriers":"retained_edram_barriers").get<uint64_t>()>0,
                "Packed EDRAM retained/skipped barrier coverage is vacuous");
            Check(s.at("cache_hits").get<uint64_t>()>0&&s.at("conversions").get<uint64_t>()>1,
                "Scene cache reuse or writer reconversion is vacuous");
            Check(s.at("cache_misses").get<uint64_t>()==s.at("conversions").get<uint64_t>()+s.at("resource_refusals").get<uint64_t>()&&
                s.at("cache_hits").get<uint64_t>()+s.at("cache_misses").get<uint64_t>()==result.eligible&&
                s.at("selected").get<uint64_t>()+s.at("resource_refusals").get<uint64_t>()==result.eligible,
                "Conversion/refusal/selection counters do not conserve actual eligible draws");
            Check(s.at("source_refusals")==0&&s.at("residency_refusals")==0&&s.at("allocation_refusals")==0,
                "Known full source/pinned resident Scene unexpectedly refused");
            Check(s.at("maximum_retained_image_bytes")==32u*1024*1024&&
                s.at("peak_retained_image_bytes").get<uint64_t>()>0&&
                s.at("peak_retained_image_bytes").get<uint64_t>()<=s.at("maximum_retained_image_bytes").get<uint64_t>()&&
                s.at("retained_image_bytes").get<uint64_t>()<=s.at("peak_retained_image_bytes").get<uint64_t>(),
                "Retained D32 image owners exceed the exact bounded budget");
            // Descriptor/policy witnesses, not Vulkan introspection. The
            // authenticated Plume source maps readonly=false to LOAD+STORE,
            // independently of the actual NONE extension capability. Hardware
            // depth writes remain disabled; selected passes use DEPTH_WRITE
            // image layout/dependencies to preserve each LOAD+STORE boundary.
            Check(s.at("attachment_load_op")==uint32_t(VK_ATTACHMENT_LOAD_OP_LOAD)&&
                s.at("attachment_store_op")==uint32_t(read_only_pass&&static_cast<VulkanDevice&>(device).storeOpNoneSupported?VK_ATTACHMENT_STORE_OP_NONE:VK_ATTACHMENT_STORE_OP_STORE)&&
                s.at("framebuffer_depth_attachment_readonly")==bool(read_only_pass&&static_cast<VulkanDevice&>(device).storeOpNoneSupported)&&
                s.at("pipeline_depth_write_enable")==false&&
                (own_device||s.at("host_load_store_op_none_supported")==static_cast<VulkanDevice&>(device).loadStoreOpNoneSupported),
                "Corrected D32 LOAD+STORE descriptor/pipeline policy does not preserve prior depth contents");
        } else {
            Check(s.at("selected")==0&&s.at("queried_selected")==0&&s.at("promotions")==0&&
                s.at("conversions")==0&&s.at("residency_refusals")==result.eligible,
                "Missing/refused startup candidate escaped into gameplay");
            Check(s.at("prewarm_refusals")==unsigned(startup?1:0),"Startup refusal provenance differs");
        }
    } else Check(result.stats.at("enabled")==false&&result.stats.at("selected")==0,"OFF selected candidate");
    result.edram=Words(kBytes/4);const auto packed=scene.ReadFullEdram();
    Check(packed.size()==kBytes,"Scene full packed readback is incomplete");std::memcpy(result.edram.data(),packed.data(),kBytes);
    Check(result.edram==ExpectedPacked(final_packed_word),"Full10MiB depth/stencil/untouched-guard oracle differs");
    std::cout<<"READONLY_DEPTH_SCENE_COVERAGE "<<Json{{"capacity",capacity},{"candidate",candidate},{"startup",startup},{"own_device",own_device},{"actual_device",actual_device},
        {"pipeline_limit",pipeline_limit},{"reverse_viewport",reverse_viewport},{"clip_depth",clip_depth},{"skip_edram_barrier",skip_edram_barrier},{"conservative",conservative},
        {"expected_final_packed_word",final_packed_word},{"ready",result.ready},{"logical_draws",count},{"pressure_publications",result.pressure_publications},
        {"eligible",result.eligible},{"stats",result.stats},{"queue",result.queue}}.dump()<<'\n';
    scene_owner.reset();
    std::cout<<"READONLY_DEPTH_SCENE_SHUTDOWN "<<Json{{"own_device",own_device},{"capacity",capacity},
        {"candidate",candidate},{"ready",result.ready},{"reverse_viewport",reverse_viewport},{"clip_depth",clip_depth},{"skip_edram_barrier",skip_edram_barrier},{"conservative",conservative},
        {"explicit_flush",true},{"shutdown_returned",true}}.dump()<<'\n';
    return result;
}

struct PrecisionOutcome {
    std::vector<uint8_t> edram,physical,raw;
    std::vector<QueryIdentity> queries;
};
PrecisionOutcome RunMetadataPrecision(RenderDevice& device,RenderCommandQueue& queue,
    std::span<const uint32_t> vertex,std::span<const uint32_t> builtin,uint32_t capacity,bool enabled,unsigned which) {
    Check(which<4&&std::endian::native==std::endian::little,"Unsupported no-write precision fixture");
    // 0 same-tile color host; 1 neighboring tile sharing only a 4KiB page;
    // 2 pre-encode UNORM24 atlas receipt; 3 completely disjoint color host.
    const bool receipt=which==2,neighbor=which==1,disjoint=which==3;
    const uint32_t query_base=neighbor?1441:1440;
    std::map<std::string,std::vector<uint8_t>> payloads;
    const auto add=[&](std::vector<uint8_t> bytes) {
        auto key=ContentHash(bytes);payloads.emplace(key,std::move(bytes));return key;
    };
    const auto floats=[&](std::initializer_list<float> values) {
        std::vector<uint8_t> bytes(values.size()*4);std::memcpy(bytes.data(),values.begin(),bytes.size());return add(std::move(bytes));
    };
    // Same authenticated observed VS fetch/identity-matrix contract as
    // builtin_readonly_query_gpu_test.cpp's existing real RoomScene fixture.
    // Raster depth comes from this geometry and the original viewport transform.
    // No fragment parameter substitutes for interpolated FragCoord.
    // The pinned VS's identity transform and DXC -fvk-invert-y put this
    // triangle at (0,360),(1280,360),(0,-360), up to one uniform half-pixel
    // translation. Vulkan defines area as minus one-half the shoelace sum,
    // so the resulting positive area is front-facing for BACK/CCW. Clipping
    // preserves the orientation and covers the complete640x360 viewport.
    const std::array<std::array<double,2>,3> framebuffer_vertices{{
        {{0,double(kHeight)}},{{2*double(kWidth),double(kHeight)}},{{0,-double(kHeight)}}}};
    double twice_area=0;
    for(unsigned i=0;i<3;++i) {
        const auto& a=framebuffer_vertices[i];const auto& b=framebuffer_vertices[(i+1)%3];
        twice_area-=a[0]*b[1]-b[0]*a[1];
    }
    Check(twice_area==4*double(kWidth)*kHeight&&twice_area>0,
        "Pinned fullscreen triangle is not front-facing for actual BACK/CCW raster");
    const std::array<std::string,3> geometry_ids{add({0,1,2,0}),floats({0,0,0,1,1,1}),
        floats({-1,-1,0.5f,3,-1,0.5f,-1,3,0.5f})};
    std::vector<uint8_t> state(0x2a80);
    for(unsigned i=0;i<4;++i)Number(state,0x780+16*i+4*i,1.f);
    Word(state,0x2880,kWidth);Word(state,0x28bc,kWidth|(kHeight<<16));Word(state,0x28c4,0x80000000);
    Word(state,0x28c8,kWidth|(kHeight<<16));Word(state,0x28dc,0);Word(state,0x2944,1<<19);Word(state,0x294c,0x43f);
    Word(state,0x2948,(1<<11)|(1<<12)|2u);Word(state,0x29c0,1);
    Number(state,0x2908,320);Number(state,0x290c,320);Number(state,0x2910,-180);Number(state,0x2914,180);Number(state,0x2918,1.f);
    Number(state,0x291c,0.f);
    const Json depth{{"depth",true},{"edram_base",query_base},{"format",1},{"format_info",0},
        {"surface_info",kWidth},{"width",kWidth},{"height",kHeight}};
    Json geometry=Json::array();
    for(unsigned i=0;i<3;++i)geometry.push_back({{"kind",14},{"fetch_slot",93+i},{"endian",0},
        {"address","00007000"},{"bytes",payloads.at(geometry_ids[i]).size()},{"sha256",geometry_ids[i]}});

    SceneExecutorConfig config;config.native_fsi=true;config.ordered_pixel_interlock_enabled=true;
    config.native_readonly_depth_attachment=true;config.native_readonly_depth_no_write_metadata=enabled;
    config.native_early_depth=true; // Precision comparisons use the same shipping classifier in both arms.
    config.batch_transfers=config.unified_gpu_queue=config.async_gpu_submissions=config.defer_native_queries=true;
    config.discard_dummy_fsi_attachment=true;config.batch_draw_descriptors=true;
    config.fused_atlas_depth_clear=receipt;config.direct_atlas_depth_resolves=receipt;
    config.max_draw_batch_size=16;config.max_inflight_batches=2;
    config.async_submission_capacity=capacity;config.max_deferred_queries=capacity;
    config.payload=[&](const std::string& key){return payloads.at(key);};
    config.shader=[&](const std::string& key) {
        Check(key==kReadonlyQueryVertexSource,"Unexpected precision fixture shader");return Bytes(vertex);
    };
    config.fsi_depth_shader=[&]{return Bytes(builtin);};
    const auto output=std::filesystem::temp_directory_path()/
        ("gow3-readonly-no-write-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+
         "-"+std::to_string(capacity)+"-"+std::to_string(which)+"-"+std::to_string(enabled));
    Check(!std::filesystem::exists(output)&&std::filesystem::create_directories(output),"Precision output path collision");
    config.output=output;config.debug_raw_images=true;config.detailed_reports=true;
    SceneExecutor scene(config,&device,&queue);AcceptedWork accepted;
    NativeFsiPipelineRecipe recipe{std::string(kReadonlyQueryVertexSource),std::nullopt,
        {uint32_t(RenderCullMode::BACK),uint32_t(RenderFrontFace::COUNTER_CLOCKWISE),1,
         uint32_t(RenderPrimitiveTopology::TRIANGLE_LIST),0,0},int32_t(RenderFormat::R8G8B8A8_UNORM),-1};
    scene.PrewarmFsiPipeline(recipe,vertex,builtin);
    Check(scene.PrewarmNativeReadonlyDepthPipeline(recipe,vertex,builtin),"Precision D32 pipeline unavailable");
    scene.InitializeEdram(Bytes(Words(kBytes/4,0xe000005a)));
    scene.InitializeMemory(0x4000,std::vector<uint8_t>(4096,0xa5));
    Json source=receipt?Json{{"depth",true},{"edram_base",1440},{"format",0},{"format_info",0},
        {"surface_info",1120},{"width",1120},{"height",688}}:
        Json{{"depth",false},{"edram_base",disjoint?0:1440},{"format",2},{"format_info",2},
        {"surface_info",80},{"width",neighbor?80:1},{"height",neighbor?16:1}};
    const unsigned slot=receipt?4:0;
    const int x=receipt?80:neighbor?79:0,y=receipt?20:neighbor?15:0;
    uint64_t event=0;
    auto clear=[&](std::array<int32_t,4> r,float depth_value) {
        scene.Execute({{"type","clearrect"},{"frame",7},{"event",++event},{"flags",receipt?16:1},
            {"rectangle",r},{"color",{0.5f/1023.f,0.f,0.f,0.5f/3.f}},{"depth",depth_value},{"stencil",0},
            {"surfaces",{{std::to_string(6+slot),source}}}});
    };
    clear({x,y,x+1,y+1},std::bit_cast<float>(0x3e800003u));
    if(receipt) {
        clear({0,0,74,74},1.f);
        const auto& stats=scene.report().at("fused_atlas_depth_clear");
        Check(stats.at("selected")==1&&stats.at("host_sources")==1&&stats.at("deferred_host_materializations")==0,
            "Pre-encode receipt invalidation witness is vacuous");
    }
    const auto before=scene.ReadFullEdram();
    const uint32_t source_at=receipt?((1440u+(20u/16u)*14u+80u/80u)*1280u+(20u%16u)*80u+(80u%80u+40u)%80u)*4u:
        (uint32_t(disjoint?0:1440)*1280u+uint32_t(y)*80u+uint32_t(x))*4u;
    uint32_t packed_source=0;std::memcpy(&packed_source,before.data()+source_at,4);
    Check(packed_source==(receipt?0x4000015au:0x40000001u),
        "Canonical encoded source precision witness differs");
    const uint32_t format=receipt?22:7;
    const std::array<uint32_t,6> fetch{2u|(1u<<22),0x4000u|format,0,(1u<<4)|(2u<<7)|(3u<<10),0,1u<<9};
    const auto resolve=[&] {
        scene.Execute({{"type","resolve"},{"frame",7},{"event",++event},{"level",0},{"flags",0},
            {"source_slot",slot},{"source_rectangle",{x,y,x+1,y+1}},{"destination_rectangle",{0,0,1,1}},
            {"destination",{{"base","00004000"},{"format",format},{"width",1},{"height",1}}},
            {"destination_fetch",fetch},{"exponent_bias",0},{"copy_dest_swap",false},
            {"surfaces",{{std::to_string(6+slot),source}}}});
        return event;
    };
    const auto raw=[&](uint64_t id) {
        const auto file=output/("resolve-"+std::to_string(id)+".rgba32f");
        std::ifstream input(file,std::ios::binary|std::ios::ate);
        Check(bool(input)&&input.tellg()==16,"Precision raw resolve artifact missing");
        std::vector<uint8_t> bytes(16);input.seekg(0);input.read(reinterpret_cast<char*>(bytes.data()),16);
        Check(bool(input),"Truncated precision raw artifact");return bytes;
    };
    if(!receipt) {
        const auto host_raw=raw(resolve());uint32_t r=1,alpha=1;
        std::memcpy(&r,host_raw.data(),4);std::memcpy(&alpha,host_raw.data()+12,4);
        Check(r==0&&alpha==0,"Noncanonical host half-up versus round-even witness is vacuous");
    }
    Word(state,0x2934,0x700762);const auto state_id=add(state);
    scene.Execute({{"type","query_begin"},{"frame",7},{"event",++event},{"query_generation",1},
        {"query_handle",0x4411},{"report_address",0x8020}});
    accepted.pending=true;
    scene.Execute({{"type","draw"},{"frame",7},{"event",++event},{"function","DrawPrimitive"},
        {"args",{0,4,0,3,0,0,0,0}},{"device_state",state_id},{"vertex_shader",kReadonlyQueryVertexSource},
        {"pixel_shader",nullptr},{"surfaces",{{"10",depth}}},{"geometry",geometry},
        {"vertex_slots",{93,94,95}},{"textures",Json::array()}});
    scene.Execute({{"type","query_end"},{"frame",7},{"event",++event},{"query_generation",1},
        {"query_handle",0x4411},{"report_address",0x8000}});
    const auto query_end=event;
    scene.FlushGpuWork();accepted.pending=false;PrecisionOutcome result;
    for(const auto& q:scene.TakeCompletedQueries())result.queries.push_back({q.generation,q.frame,q.end_event,
        q.result.handle,q.result.begin_address,q.result.end_address,q.result.samples,q.result.draws});
    Check(result.queries.size()==1&&result.queries[0][0]==1&&result.queries[0][2]==query_end&&
        result.queries[0][3]==0x4411&&result.queries[0][4]==0x8020&&result.queries[0][5]==0x8000&&
        result.queries[0][6]>0&&result.queries[0][7]==1,"Precision query identity or actual samples missing");
    if(!receipt)Check(result.queries[0][6]==uint64_t(kWidth)*kHeight,"Precision color case query samples differ");
    result.edram=scene.ReadFullEdram();Check(result.edram==before,"Readonly query changed any packed source/guard byte");
    result.raw=raw(resolve());result.physical=scene.ReadPhysical(0x4000,4096);
    uint32_t r=0;std::memcpy(&r,result.raw.data(),4);
    const uint32_t expected=receipt?0x3e800000u:disjoint?0u:std::bit_cast<uint32_t>(1.f/1023.f);
    Check(r==expected,"Future raw resolve did not retain exact legacy precision authority");
    uint32_t physical_word=0;std::memcpy(&physical_word,result.physical.data(),4);
    Check(physical_word==(receipt?0x4000005au:disjoint?0u:0x40000001u),
        "Future physical resolve did not retain independent packed precision");
    for(size_t i=4;i<result.physical.size();++i)Check(result.physical[i]==0xa5,"Resolve changed untouched destination bytes");
    const auto& report=scene.report();const auto& stats=report.at("native_readonly_depth_no_write_metadata");
    Check(report.at("native_early_depth").at("enabled")==true,
        "Precision comparison did not keep shipping early-depth enabled");
    Check(stats.at("selected")==unsigned(enabled)&&stats.at("refusals")==0,"Precision no-write path not selected");
    if(enabled)Check(stats.at("host_invalidations").get<uint64_t>()+stats.at("receipt_invalidations").get<uint64_t>()>0||disjoint,
        "Overlapping precision invalidation was vacuous");
    if(enabled&&receipt)Check(stats.at("receipt_invalidations")==1,"Actual atlas receipt did not invalidate");
    if(enabled&&disjoint)Check(stats.at("host_invalidations")==0&&stats.at("receipt_invalidations")==0,
        "Disjoint host was needlessly normalized");
    Check(scene.ReadFullEdram()==before,"Future resolve changed packed source bytes");
    std::cout<<"READONLY_NO_WRITE_METADATA_PRECISION "<<Json{{"enabled",enabled},{"case",which},{"capacity",capacity},
        {"raw_red_bits",r},{"expected_red_bits",expected},{"full_edram_bytes",kBytes},{"physical_bytes",4096},
        {"raw_bytes",16},{"output",output.string()},{"stats",stats}}.dump()<<'\n';
    return result;
}
}
int main(int argc,char** argv)try {
    if(argc<3||argc>4)throw std::runtime_error("Usage: readonly-depth-scene-test original-builtin.spv pinned-vs.spv [64|128]");
    const auto capacity=argc==4?uint32_t(std::stoul(argv[3])):64u;
    Check(capacity==64||capacity==128,"Unsupported bounded Scene owner capacity");
    const auto builtin=Read(argv[1]),vertex=Read(argv[2]);
    Check(builtin.size()==2423&&ContentHash(Bytes(builtin))==kReadonlyQueryBuiltinSpirv,"Canonical builtin module pin differs");
    Check(AuditedReadonlyQueryVertex(kReadonlyQueryVertexSource,Bytes(vertex)),"Pinned actual VS module differs");
    FsiDevice host(false,false,true);auto& device=host.device();auto& vk=static_cast<VulkanDevice&>(device);
    Check(vk.storeOpNoneSupported,"Preserved readonly Scene fixture requires enabled STORE_OP_NONE");
    Check(vk.physicalDeviceProperties.deviceType!=VK_PHYSICAL_DEVICE_TYPE_CPU,"Hardware Scene fixture cannot use software device");
    std::cout<<"READONLY_DEPTH_SCENE_DEVICE "<<Json{{"name",vk.physicalDeviceProperties.deviceName},
        {"type",uint32_t(vk.physicalDeviceProperties.deviceType)},{"vendor_id",vk.physicalDeviceProperties.vendorID},
        {"device_id",vk.physicalDeviceProperties.deviceID},{"driver_version",vk.physicalDeviceProperties.driverVersion},
        {"load_store_op_none_supported",vk.loadStoreOpNoneSupported}}.dump()<<'\n';
    auto queue=device.createCommandQueue(RenderCommandListType::DIRECT);
    const auto off=RunScene(device,*queue,vertex,builtin,capacity,false,false);
    const auto on=RunScene(device,*queue,vertex,builtin,capacity,true,true);
    const auto missing=RunScene(device,*queue,vertex,builtin,capacity,true,false);
    const auto budget=RunScene(device,*queue,vertex,builtin,capacity,true,true,2);
    const auto owned=RunScene(device,*queue,vertex,builtin,capacity,true,true,2048,true);
    const auto partial=RunScene(device,*queue,vertex,builtin,capacity,true,true,2048,true,true);
    const auto reverse_quarter_off=RunScene(device,*queue,vertex,builtin,capacity,false,false,2048,false,false,true,.25f);
    const auto reverse_quarter_on=RunScene(device,*queue,vertex,builtin,capacity,true,true,2048,false,false,true,.25f);
    const auto reverse_threequarter_off=RunScene(device,*queue,vertex,builtin,capacity,false,false,2048,false,false,true,.75f);
    const auto reverse_threequarter_on=RunScene(device,*queue,vertex,builtin,capacity,true,true,2048,false,false,true,.75f);
    Check(reverse_quarter_off.edram==reverse_quarter_on.edram&&reverse_quarter_off.queries==reverse_quarter_on.queries&&
          reverse_threequarter_off.edram==reverse_threequarter_on.edram&&reverse_threequarter_off.queries==reverse_threequarter_on.queries,
        "Asymmetric reversed viewport changes packed bytes or real query identities/samples");
    const auto skipped=RunScene(device,*queue,vertex,builtin,capacity,true,true,2048,false,false,false,.5f,true);
    const auto skipped_missing=RunScene(device,*queue,vertex,builtin,capacity,true,false,2048,false,false,false,.5f,true);
    const auto skipped_budget=RunScene(device,*queue,vertex,builtin,capacity,true,true,2,false,false,false,.5f,true);
    const auto skipped_reverse_quarter=RunScene(device,*queue,vertex,builtin,capacity,true,true,2048,false,false,true,.25f,true);
    const auto skipped_reverse_threequarter=RunScene(device,*queue,vertex,builtin,capacity,true,true,2048,false,false,true,.75f,true);
    Check(skipped_reverse_quarter.edram==reverse_quarter_off.edram&&skipped_reverse_quarter.queries==reverse_quarter_off.queries&&
          skipped_reverse_threequarter.edram==reverse_threequarter_off.edram&&skipped_reverse_threequarter.queries==reverse_threequarter_off.queries,
        "Skipping packed EDRAM barrier changes asymmetric reversed viewport packed bytes or real query identities/samples");
    for(const auto* result:{&on,&missing,&budget,&owned,&skipped,&skipped_missing,&skipped_budget})Check(result->edram==off.edram&&result->queries==off.queries,
        "Actual D32 Scene candidate/fallback changes full packed bytes or real query identities/samples");
    // Optional DepthLess changes only the additional immutable resident PSO.
    // Existing writers keep the canonical module; exact positive/zero query
    // identities and the independent whole-EDRAM oracle above remain mandatory.
    const auto conservative=RunScene(device,*queue,vertex,builtin,capacity,true,true,2048,false,false,false,.5f,false,true);
    const auto conservative_quarter=RunScene(device,*queue,vertex,builtin,capacity,true,true,2048,false,false,true,.25f,false,true);
    const auto conservative_threequarter=RunScene(device,*queue,vertex,builtin,capacity,true,true,2048,false,false,true,.75f,false,true);
    const auto conservative_missing=RunScene(device,*queue,vertex,builtin,capacity,true,false,2048,false,false,false,.5f,false,true);
    // floor(3*3/4)==2 permits generic+ordinary D32, then refuses the optional
    // third PSO. This must preserve the already-supported D32 path in gameplay.
    const auto conservative_budget=RunScene(device,*queue,vertex,builtin,capacity,true,true,3,false,false,false,.5f,false,true);
    const auto equal=[&](const Outcome& actual,const Outcome& expected) {
        Check(actual.edram==expected.edram&&actual.queries==expected.queries,
            "Conservative depth changes independent packed bytes or actual query results");
        Check(actual.queue==expected.queue&&actual.pressure_publications==expected.pressure_publications&&
              actual.eligible==expected.eligible,
            "Conservative depth changes queue ownership/submission/pressure coverage");
    };
    equal(conservative,on);equal(conservative_quarter,reverse_quarter_on);
    equal(conservative_threequarter,reverse_threequarter_on);
    equal(conservative_missing,missing);equal(conservative_budget,on);
    std::cout<<"READONLY_DEPTH_CONSERVATIVE_SCENE_COVERAGE "<<Json{{"capacity",capacity},{"cases",5},
        {"selected_cases",3},{"missing_base_cases",1},{"optional_budget_fallback_cases",1},
        {"full_edram_bytes_per_case",kBytes},{"logical_draws_per_case",capacity+8},
        {"selected_draws_per_case",conservative.eligible},{"actual_zero_and_positive_queries",true},
        {"exact_query_identity_and_queue_parity",true},{"asymmetric_reversed_viewports",true},
        {"ordinary_D32_preserved_on_optional_refusal",conservative_budget.ready}}.dump()<<'\n';
    const auto no_write=RunScene(device,*queue,vertex,builtin,capacity,true,true,2048,false,false,false,.5f,false,false,true);
    const auto no_write_reverse=RunScene(device,*queue,vertex,builtin,capacity,true,true,2048,false,false,true,.25f,false,false,true);
    const auto no_write_missing=RunScene(device,*queue,vertex,builtin,capacity,true,false,2048,false,false,false,.5f,false,false,true);
    equal(no_write,on);equal(no_write_reverse,reverse_quarter_on);equal(no_write_missing,missing);
    for(unsigned which=0;which<4;++which) {
        const auto before=RunMetadataPrecision(device,*queue,vertex,builtin,capacity,false,which);
        const auto after=RunMetadataPrecision(device,*queue,vertex,builtin,capacity,true,which);
        Check(before.edram==after.edram&&before.physical==after.physical&&before.raw==after.raw&&before.queries==after.queries,
            "No-write metadata changed future packed, physical, raw or query result");
    }
    const auto preserved=RunScene(device,*queue,vertex,builtin,capacity,true,true,2048,false,false,false,.5f,false,false,true,true);
    const auto preserved_reverse=RunScene(device,*queue,vertex,builtin,capacity,true,true,2048,false,false,true,.25f,false,false,true,true);
    const auto preserved_skipped=RunScene(device,*queue,vertex,builtin,capacity,true,true,2048,false,false,false,.5f,true,false,true,true);
    const auto preserved_missing=RunScene(device,*queue,vertex,builtin,capacity,true,false,2048,false,false,false,.5f,false,false,true,true);
    equal(preserved,no_write);equal(preserved_reverse,no_write_reverse);equal(preserved_skipped,no_write);equal(preserved_missing,no_write_missing);
    Check(preserved.stats.at("read_only_pass_active")==true&&
          preserved.stats.at("read_only_depth_barriers_avoided").get<uint64_t>()>0,
          "Preserved readonly pass or immutable read barrier coverage is vacuous");
    const auto lower=RunScene(device,*queue,vertex,builtin,capacity,true,true,2048,false,false,false,.5f,false,false,true,true,true);
    const auto lower_reverse=RunScene(device,*queue,vertex,builtin,capacity,true,true,2048,false,false,true,.25f,false,false,true,true,true);
    const auto lower_missing=RunScene(device,*queue,vertex,builtin,capacity,true,false,2048,false,false,false,.5f,false,false,true,true,true);
    equal(lower,no_write);equal(lower_reverse,no_write_reverse);equal(lower_missing,no_write_missing);
    const auto grouped=RunScene(device,*queue,vertex,builtin,capacity,true,true,2048,false,false,false,.5f,true,false,true,true,false,true);
    const auto grouped_reverse=RunScene(device,*queue,vertex,builtin,capacity,true,true,2048,false,false,true,.25f,true,false,true,true,false,true);
    const auto grouped_missing=RunScene(device,*queue,vertex,builtin,capacity,true,false,2048,false,false,false,.5f,true,false,true,true,false,true);
    const auto empty_base=RunScene(device,*queue,vertex,builtin,capacity,true,true,2048,false,false,false,.5f,true,false,true,true,false,false,true);
    const auto grouped_empty=RunScene(device,*queue,vertex,builtin,capacity,true,true,2048,false,false,false,.5f,true,false,true,true,false,true,true);
    (void)RunScene(device,*queue,vertex,builtin,capacity,true,true,2048,true,true,false,.5f,true,false,true,true,false,true);
    equal(grouped,no_write);equal(grouped_reverse,no_write_reverse);equal(grouped_missing,no_write_missing);
    equal(grouped_empty,empty_base);
    Check(grouped.groups.at("selected").get<uint64_t>()>0&&grouped.groups.at("render_passes_avoided").get<uint64_t>()>0&&
          grouped.groups.at("query_controls_preserved").get<uint64_t>()>0&&grouped.groups.at("maximum_group_draws")==2&&
          grouped.groups.at("selected").get<uint64_t>()==grouped.groups.at("closed_render_pass_groups").get<uint64_t>()+
              grouped.groups.at("render_passes_avoided").get<uint64_t>(),"Query groups lack exact accepted pair/control coverage");
    Check(grouped_missing.groups.at("selected")==0,"Missing pipeline entered readonly query pass");
    const auto unbound=RunScene(device,*queue,vertex,builtin,capacity,true,true,2048,false,false,false,.5f,true,false,true,true,false,true,true,true);
    const auto unbound_reverse=RunScene(device,*queue,vertex,builtin,capacity,true,true,2048,false,false,true,.25f,true,false,true,true,false,true,false,true);
    const auto unbound_missing=RunScene(device,*queue,vertex,builtin,capacity,true,false,2048,false,false,false,.5f,true,false,true,true,false,true,false,true);
    equal(unbound,grouped_empty);equal(unbound_reverse,grouped_reverse);equal(unbound_missing,grouped_missing);
    (void)RunScene(device,*queue,vertex,builtin,capacity,true,true,2048,true,true,false,.5f,true,false,true,true,false,true,false,true);
    std::cout<<"UNUSED_DESCRIPTOR_SCENE_COVERAGE "<<Json{{"capacity",capacity},{"paired_cases",3},
        {"full_edram_bytes",kBytes},{"zero_and_positive_queries_and_empty_queries",true},
        {"reverse_and_missing_pipeline",true},{"stats",unbound.bindings}}.dump()<<'\n';
    vk.hostQueryResetEnabled=false;
    const auto grouped_missing_reset=RunScene(device,*queue,vertex,builtin,capacity,true,true,2048,false,false,false,.5f,true,false,true,true,false,true);
    vk.hostQueryResetEnabled=true;equal(grouped_missing_reset,no_write);
    Check(grouped_missing_reset.groups.at("selected")==0,"Unadvertised host-reset feature entered readonly query pass");
    Check(lower.stats.at("lower_bound_query")==true&&lower.stats.at("selected").get<uint64_t>()>0&&
          lower.stats.at("skipped_edram_barriers")==0&&
          lower.stats.at("lower_bound_read_barriers").get<uint64_t>()>0&&
          lower.stats.at("lower_bound_read_barriers_avoided").get<uint64_t>()>0&&
          lower.stats.at("lower_bound_read_barriers").get<uint64_t>()+
              lower.stats.at("lower_bound_read_barriers_avoided").get<uint64_t>()==lower.stats.at("selected"),
          "Lower-bound query coverage is missing or dropped its packed-memory dependency");
    // Capability refusal preserves ordinary Native D32 instead of depending
    // on a physically supported but unadvertised extension.
    vk.storeOpNoneSupported=false;
    const auto unsupported=RunScene(device,*queue,vertex,builtin,capacity,true,true,2048,false,false,false,.5f,false,false,true,true);
    const auto lower_unsupported=RunScene(device,*queue,vertex,builtin,capacity,true,true,2048,false,false,false,.5f,false,false,true,true,true);
    const auto grouped_unsupported=RunScene(device,*queue,vertex,builtin,capacity,true,true,2048,false,false,false,.5f,true,false,true,true,false,true);
    vk.storeOpNoneSupported=true;equal(unsupported,no_write);
    equal(lower_unsupported,no_write);
    equal(grouped_unsupported,no_write);
    Check(grouped_unsupported.groups.at("selected")==0,"Unadvertised STORE_NONE entered readonly query pass");
    std::cout<<"READONLY_QUERY_GROUP_SCENE_COVERAGE "<<Json{{"capacity",capacity},{"paired_cases",6},
        {"full_edram_bytes_per_case",kBytes},{"actual_query_and_queue_equality",true},{"stats",grouped.groups},
        {"reverse_writer_reconversion_and_missing_pipeline_reset_store_none",true},
        {"empty_queries_between_group_members_and_owned_open_group_teardown",true}}.dump()<<'\n';
    Check(unsupported.stats.at("read_only_pass_active")==false&&
          unsupported.stats.at("read_only_depth_barriers_avoided")==0,
          "Disabled capability selected preserved readonly pass");
    std::cout<<"READONLY_LOWER_BOUND_QUERY_SCENE_COVERAGE "<<Json{{"capacity",capacity},{"paired_cases",4},
        {"full_edram_bytes_per_case",kBytes},{"actual_query_and_queue_equality",true},
        {"selected",lower.stats.at("selected")},{"retained_edram_barriers",lower.stats.at("retained_edram_barriers")},
        {"reverse_viewport_and_missing_pipeline_and_missing_store_none",true}}.dump()<<'\n';
    std::cout<<"READONLY_PRESERVED_SCENE_COVERAGE "<<Json{{"capacity",capacity},{"paired_cases",5},
        {"full_edram_bytes_per_case",kBytes},{"actual_query_and_queue_equality",true},
        {"barriers_avoided",preserved.stats.at("read_only_depth_barriers_avoided")},
        {"missing_pipeline_and_missing_capability",true}}.dump()<<'\n';
    std::cout<<"READONLY_NO_WRITE_METADATA_SCENE_COVERAGE "<<Json{{"capacity",capacity},{"paired_draw_cases",3},
        {"future_precision_cases",4},{"selected_draws",no_write.eligible+no_write_reverse.eligible},
        {"full_edram_bytes_per_case",kBytes},{"exact_future_resolve_bytes",true},{"current_host_and_receipt",true},
        {"adjacent_tile_page_alias",true},{"disjoint_host_preserved",true}}.dump()<<'\n';
    std::cout<<"PASS: actual Scene readonly D32 paired OFF/ON/missing/budget/owned/owned-partial/reverse-asymmetric/skip-packed-barrier/skip-missing/skip-budget/skip-reverse-asymmetric/conservative-forward-reverse-missing-budget;capacity="<<capacity
        <<";checks="<<checks<<";full_edram_bytes="<<kBytes<<";draws_per_case="<<capacity+8
        <<";positive_cache_reuse_writer_reconversion_exact_promotion=true;zero_and_positive_real_queries=true\n";
    return 0;
} catch(const std::exception& e) {std::cerr<<"Readonly depth Scene fixture failed: "<<e.what()<<'\n';return 1;}
