#include "retained_opaque_phase_pass_failure_fixture.h"
#include "renderer/native/content_hash.h"
#include "renderer/native/fsi_device.h"
#include "renderer/native/live_operations.h"
#include "renderer/native/retained_opaque_phase_catalog.h"
#include "plume_vulkan.h"
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <set>

namespace gow3::renderer::validation {
namespace {
using Json=nlohmann::json;
using namespace plume;
constexpr uint32_t kBytes=10u*1024*1024,kPrefix=289,kLastPrefix=287,kFirst=290,kLast=487,kFrame=17672;
void Require(bool value,const char* why){if(!value)throw std::runtime_error(why);}
class SubmitObserver final:public RenderCommandQueue {
    RenderCommandQueue& queue_;
public:
    uint64_t submissions=0,lists=0;
    explicit SubmitObserver(RenderCommandQueue& queue):queue_(queue){}
    std::unique_ptr<RenderCommandList> createCommandList()override{return queue_.createCommandList();}
    std::unique_ptr<RenderSwapChain> createSwapChain(const RenderSwapChainDesc& desc)override{return queue_.createSwapChain(desc);}
    void executeCommandLists(const RenderCommandList** commands,uint32_t count,RenderCommandSemaphore** waits,
        uint32_t wait_count,RenderCommandSemaphore** signals,uint32_t signal_count,RenderCommandFence* fence)override {
        ++submissions;lists+=count;queue_.executeCommandLists(commands,count,waits,wait_count,signals,signal_count,fence);
    }
    void waitForCommandFence(RenderCommandFence* fence)override{queue_.waitForCommandFence(fence);}
};
// Observer remains alive when Scene's destructor invokes terminate. The final
// C stdio receipt checks that destructor unwinding never submitted the list.
bool proof_ready=false;
SubmitObserver* terminating_observer=nullptr;
uint64_t fault_submissions=0,fault_lists=0;
std::string termination_path;
void ExpectedTermination()noexcept {
    const bool exact=proof_ready&&terminating_observer&&
        terminating_observer->submissions==fault_submissions&&terminating_observer->lists==fault_lists;
    bool saved=false;
    if(auto* file=std::fopen(termination_path.c_str(),"wb")) {
        const int written=std::fprintf(file,"{\"schema\":1,\"verified_failure_proof\":%s,\"post_fault_submissions\":%llu,\"post_fault_lists\":%llu,\"expected_exit\":%d}\n",
            exact?"true":"false",
            static_cast<unsigned long long>(terminating_observer?terminating_observer->submissions-fault_submissions:0),
            static_cast<unsigned long long>(terminating_observer?terminating_observer->lists-fault_lists:0),exact?85:86);
        saved=std::fclose(file)==0&&written>0;
    }
    std::_Exit(exact&&saved?85:86);
}
}
void RetainedOpaquePhasePassFailureFixture(const SceneExecutorConfig& original,
        std::span<const LiveOperation* const> operations,
        std::span<const RetainedOpaquePhaseOwnerFixtureSource> sources,bool seed_full_edram) {
    Require(seed_full_edram&&!std::filesystem::exists(original.capture/"gpu-history.json"),
        "Phase follower fixture requires explicit zero10MiB seed without captured history");
    Require(original.retained_opaque_phase&&original.retained_opaque_phase_render_pass_groups&&
        original.retained_opaque_phase_programs&&original.fsi_state_material_composition&&original.native_fsi&&
        original.unified_gpu_queue&&original.async_gpu_submissions&&original.batch_transfers&&original.defer_native_queries&&
        !original.multi_draw_command_buffers&&!original.packed_color_render_pass_groups&&!original.submission_worker&&
        !original.gpu_draw_timestamps&&!original.gpu_timestamps&&!original.gpu_batch_attribution&&
        !original.gpu_physical_timestamps&&!original.diagnostic_readonly_query_groups&&
        !original.test_grouped_draw_before_enqueue&&original.max_draw_batch_size>=2&&
        original.max_draw_batch_size<=16&&operations.size()<=65536&&sources.size()==65,
        "Phase follower fixture requires explicit untimed Native phase groups and synchronous driver receipts");
    std::vector<const TypedLiveDraw*> donors;uint32_t last=UINT32_MAX;
    for(const auto* operation:operations) {
        Require(operation,"Phase follower fixture has null operation");
        Require(last==UINT32_MAX||operation->Event()>last,"Phase follower fixture operation order changed");last=operation->Event();
        Require(operation->Kind()!="query_begin"&&operation->Kind()!="query_end","Phase follower fixture requires original unqueried capture");
        if(operation->Event()<kFirst||operation->Event()>kLast)continue;
        const auto* draw=operation->Draw();
        Require(draw&&draw->frame==kFrame&&draw->vertex_shader&&draw->pixel_shader&&
            draw->function==DrawFunction::DrawIndexedPrimitive&&!draw->rectangle_list&&draw->args[1]==4,
            "Phase follower fixture actual first phase donor changed");
        Require(operation->Event()==kFirst+donors.size(),"Phase follower fixture first phase is not contiguous");
        donors.push_back(draw);
    }
    Require(donors.size()==198,"Phase follower fixture requires all198 actual first-phase draws");
    std::set<NativeFsiPipelineRecipe> recipes;
    for(const auto& source:sources)Require(source.recipe.pixel&&
        NativeRetainedOpaquePhaseCatalogRecipe(source.recipe.vertex,*source.recipe.pixel,source.recipe.raster)&&
        source.recipe.color_format==int32_t(RenderFormat::R8G8B8A8_UNORM)&&source.recipe.depth_format==-1&&
        recipes.insert(source.recipe).second&&!source.vertex.empty()&&!source.pixel.empty(),
        "Phase follower fixture exact65 canonical startup recipes changed");
    FsiDevice host(original.restore_depth_stencil_export,false,original.native_readonly_depth_read_only_pass);
    auto& device=host.device();const auto& vk=static_cast<const VulkanDevice&>(device);
    const auto& properties=vk.physicalDeviceProperties;
    Require(properties.deviceType==VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU||
        properties.deviceType==VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU,"Phase follower fixture requires hardware Vulkan GPU");
    auto queue=device.createCommandQueue(RenderCommandListType::DIRECT);Require(bool(queue),"Phase follower queue unavailable");
    SubmitObserver observer(*queue);
    auto config=original;config.present={};config.test_retained_resource_pool_diagnostics=true;
    config.ordered_pixel_interlock_enabled=true;config.shader_stencil_export_enabled=host.stencil_export_enabled();
    VkPhysicalDeviceFeatures features{};vkGetPhysicalDeviceFeatures(vk.physicalDevice,&features);
    config.storage_image_extended_formats_enabled=features.shaderStorageImageExtendedFormats;
    config.max_sampler_anisotropy=features.samplerAnisotropy?uint32_t(properties.limits.maxSamplerAnisotropy):0;
    // Observer delegates creation to the real queue. Its pointer differs, so
    // use normal begin() rather than the optional exact-queue pool-reset check.
    config.reset_draw_command_pool=false;
    std::filesystem::create_directories(config.output);
    termination_path=(config.output/"phase-pass-follower-termination.json").string();
    proof_ready=false;terminating_observer=&observer;
    std::set_terminate(ExpectedTermination);
    uint32_t current_event=UINT32_MAX,leader_event=UINT32_MAX,injected_event=UINT32_MAX,older_accepted=0;
    bool armed=false,injected=false;
    config.test_grouped_draw_before_enqueue=[&](uint32_t older) {
        if(!armed)return;
        Require(current_event>=kFirst&&current_event<=kLast,"Phase follower callback escaped authenticated donor range");
        // Generic/packed-color groups are disabled. Every donor has a pixel
        // shader, excluding packed-depth; there are no active/captured queries,
        // excluding readonly-query groups. The policy-excluded310 may only
        // take the standalone path, never invoke this armed group callback.
        Require(donors[current_event-kFirst]->pixel_shader->hash.View()!=kRetainedOpaquePhaseExcludedPixel,
            "Policy-excluded material entered a retained phase command group");
        if(!older) {leader_event=current_event;return;}
        if(older!=1||injected)return;
        Require(leader_event>=kFirst&&leader_event<current_event,"Phase follower missing its actual accepted phase leader");
        injected=true;injected_event=current_event;older_accepted=older;
        fault_submissions=observer.submissions;fault_lists=observer.lists;
        throw std::runtime_error("Injected fully recorded unaccepted retained-phase follower");
    };
    SceneExecutor scene(config,&device,&observer);
    for(const auto& source:sources)scene.PrewarmFsiPipeline(source.recipe,source.vertex,source.pixel);
    size_t admitted=0;
    for(const auto& source:sources)if(NativeRetainedOpaquePhaseRecipe(source.recipe.vertex,*source.recipe.pixel,source.recipe.raster)) {
        Require(scene.PrewarmRetainedOpaquePhasePipeline(source.recipe,source.vertex,source.pixel),"Phase follower startup residency refused");++admitted;
    }
    Require(admitted==64,"Phase follower eligible64 startup count changed");
    const std::vector<uint8_t> seed(kBytes,0);scene.InitializeEdram(seed);
    scene.RunLiveCapture(operations,kPrefix);
    const auto prefix=scene.report();
    Require(prefix.at("requested_range_complete").get<bool>()&&prefix.at("last_accepted_event")==kLastPrefix&&
        prefix.at("retained_opaque_phase").at("selected")==0,"Phase follower prefix289/actual287 changed");
    bool fatal=false;const TypedLiveDraw* failed_draw=nullptr;
    for(const auto* operation:operations) {
        if(operation->Event()<kFirst||operation->Event()>kLast)continue;
        const auto& draw=*operation->Draw();current_event=operation->Event();armed=true;
        try {scene.PrepareCommittedStateForDraw(draw);scene.Execute(draw);}
        catch(const NativeBatchCompletionError&) {fatal=true;failed_draw=&draw;break;}
    }
    armed=false;
    Require(fatal&&injected&&failed_draw&&older_accepted==1&&scene.HasPendingDraws()&&scene.HasPendingGpuWork(),
        "Phase follower did not poison an actual accepted multi-child leader");
    Json refused=Json::array();
    auto refuse=[&](const char* name,auto&& operation) {
        bool rejected=false;try{operation();}catch(const NativeBatchCompletionError&){rejected=true;}
        Require(rejected,"Poisoned phase leader accepted a post-failure operation");refused.push_back(name);
        Require(observer.submissions==fault_submissions&&observer.lists==fault_lists,
            "Poisoned phase leader was submitted after follower failure");
    };
    refuse("draw",[&]{scene.Execute(*failed_draw);});
    refuse("prepare_draw",[&]{scene.PrepareCommittedStateForDraw(*failed_draw);});
    refuse("flush",[&]{scene.FlushGpuWork();});
    refuse("submit_progress",[&]{scene.SubmitGpuWork();});
    refuse("export_state",[&]{(void)scene.ExportState();});
    refuse("coherence",[&]{scene.FlushCommittedState();});
    refuse("readback",[&]{(void)scene.ReadPhysical(0,16);});
    refuse("query_take",[&]{(void)scene.TakeCompletedQueries();});
    refuse("query_poll",[&]{scene.PollCompletedQueries();});
    refuse("query_pressure",[&]{scene.FlushOldestQuery();});
    refuse("active_query",[&]{(void)scene.ActiveQuery();});
    refuse("report",[&]{(void)scene.report();});
    refuse("batch_report",[&]{(void)scene.GpuBatchAttributionReport();});
    refuse("draw_report",[&]{(void)scene.GpuDrawTimingReport();});
    const Json proof={{"schema","retained-phase-pass-follower-failure-v1"},{"passed_before_destructor",true},
        {"device",{{"name",properties.deviceName},{"type",uint32_t(properties.deviceType)},
            {"vendor_id",properties.vendorID},{"device_id",properties.deviceID}}},
        {"requested_prefix",kPrefix},{"actual_prefix_last",kLastPrefix},{"actual_donor_range",{kFirst,kLast}},
        {"actual_leader_event",leader_event},{"injected_event",injected_event},{"older_accepted_children",older_accepted},
        {"leader_pixel",std::string(donors[leader_event-kFirst]->pixel_shader->hash.View())},
        {"follower_pixel",std::string(failed_draw->pixel_shader->hash.View())},
        {"pixel_bearing_unqueried_donors",donors.size()},{"policy_excluded_callback_refused",true},
        {"has_pending_draw_owners",scene.HasPendingDraws()},{"has_pending_gpu_work",scene.HasPendingGpuWork()},
        {"submissions_at_fault",fault_submissions},{"lists_at_fault",fault_lists},
        {"submissions_since_fault",observer.submissions-fault_submissions},{"lists_since_fault",observer.lists-fault_lists},
        {"refused_paths",refused},{"actual_grouping_is_phase_only",true},{"generic_multidraw",false},
        {"prewarmed_originals",sources.size()},{"prewarmed_phase",admitted},
        {"seed",{{"kind","explicit_zero_without_captured_history"},{"bytes",kBytes},{"sha256",ContentHash(seed)}}},
        {"reset_draw_command_pool_original",original.reset_draw_command_pool},{"reset_draw_command_pool_fixture",false},
        {"maximum_group_draws",16},{"async_submission_capacity",config.async_submission_capacity},
        {"max_draw_batch_size",config.max_draw_batch_size},{"max_inflight_batches",config.max_inflight_batches},
        {"expected_scene_destructor_termination",true},{"expected_exit",85},{"unexpected_termination_exit",86},
        {"scope","Actual first-phase donors and real accepted-prefix recording failure; not an image-parity or FPS test"}};
    {std::ofstream file(config.output/"phase-pass-follower-failure.json");file<<proof.dump(2)<<'\n';file.flush();
        Require(bool(file),"Cannot preserve phase follower failure proof");}
    std::cout<<"PHASE_PASS_FATAL_PROOF "<<proof.dump()<<'\n'<<std::flush;
    proof_ready=true;
    // Reaches Scene's real fail-closed destructor. Handler verifies zero
    // submissions during that final unwind before reporting exit85.
}
}
