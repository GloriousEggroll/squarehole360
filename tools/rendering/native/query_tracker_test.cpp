#include "renderer/native/query_tracker.h"
#include "renderer/native/fsi_device.h"
#include "fullscreen_spv.h"
#include "query_test_spv.h"
#include <iostream>
#include <stdexcept>
#include <vector>
#include <string_view>
namespace plume {std::unique_ptr<RenderInterface> CreateVulkanInterface();}
using namespace plume;
using namespace gow3::renderer;
int main(int argc,char** argv) {
    try {
        uint32_t capacity=NativeSubmissionCapacity::kDefault;
        bool host_reset=false,hide_host_reset=false;
        for(int i=1;i<argc;++i) {
            const std::string_view argument=argv[i];
            if(argument=="--async-submission-capacity=64")capacity=64;
            else if(argument=="--async-submission-capacity=128")capacity=128;
            else if(argument=="--host-query-reset")host_reset=true;
            else if(argument=="--hide-host-query-reset")hide_host_reset=true;
            else throw std::invalid_argument("Unknown query fixture argument");
        }
        const uint32_t half=capacity/2,quarter=capacity/4;
        auto interface=CreateVulkanInterface();
        std::unique_ptr<RenderDevice> ordinary_device;
        std::unique_ptr<FsiDevice> fsi;
        RenderDevice* device=nullptr;
        if(host_reset) {fsi=std::make_unique<FsiDevice>();device=&fsi->device();}
        else {ordinary_device=interface->createDevice();device=ordinary_device.get();}
        if(!device)throw std::runtime_error("Vulkan unavailable");
        auto& vk_device=static_cast<VulkanDevice&>(*device);
        if(host_reset&&!vk_device.hostQueryResetEnabled)throw std::runtime_error("Fixture requires enabled hostQueryReset");
        if(hide_host_reset)vk_device.hostQueryResetEnabled=false;
        auto queue=device->createCommandQueue(RenderCommandListType::DIRECT);
        auto fence=device->createCommandFence();
        auto vs=device->createShader(fullscreen_spv,sizeof(fullscreen_spv),"main",RenderShaderFormat::SPIRV);
        auto ps=device->createShader(query_test_spv,sizeof(query_test_spv),"main",RenderShaderFormat::SPIRV);
        auto layout=device->createPipelineLayout(RenderPipelineLayoutDesc{});
        auto target=device->createTexture(RenderTextureDesc::ColorTarget(16,16,RenderFormat::R8G8B8A8_UNORM));
        auto depth=device->createTexture(RenderTextureDesc::DepthTarget(16,16,RenderFormat::D32_FLOAT_S8_UINT));
        const RenderTexture* color=target.get();RenderFramebufferDesc fd;
        fd.colorAttachments=&color;fd.colorAttachmentsCount=1;fd.depthAttachment=depth.get();
        auto framebuffer=device->createFramebuffer(fd);
        NativeQueryTracker tracker(*device,capacity,host_reset);
        if(tracker.host_reset_enabled()!=(host_reset&&!hide_host_reset))
            throw std::runtime_error("Host reset ignored actual enabled capability or requested mode");
        if(tracker.capacity()!=capacity)throw std::runtime_error("Physical query capacity mismatch");
        if(tracker.Snapshot()||tracker.MetadataSnapshot())throw std::runtime_error("Inactive query returned state");
        auto reject=[&](auto&& action,const char* message) {
            bool rejected=false;
            try { action(); } catch(const std::runtime_error&) { rejected=true; }
            if(!rejected)throw std::runtime_error(message);
        };
        auto reject_snapshot=[&] {reject([&]{(void)tracker.Snapshot();},"Unfinished draw exposed a query subtotal");};
        auto make_pipeline=[&](bool visible) {
            RenderGraphicsPipelineDesc p;p.pipelineLayout=layout.get();p.vertexShader=vs.get();p.pixelShader=ps.get();
            p.renderTargetCount=1;p.renderTargetFormat[0]=RenderFormat::R8G8B8A8_UNORM;
            p.renderTargetBlend[0]=RenderBlendDesc::Copy();p.depthTargetFormat=RenderFormat::D32_FLOAT_S8_UINT;
            p.depthEnabled=true;p.depthWriteEnabled=false;p.depthFunction=visible?RenderComparisonFunction::ALWAYS:RenderComparisonFunction::NEVER;
            return device->createGraphicsPipeline(p);
        };
        auto visible_pipeline=make_pipeline(true),hidden_pipeline=make_pipeline(false);
        std::vector<std::unique_ptr<RenderCommandList>> pending;
        auto record=[&](bool visible) {
            auto commands=queue->createCommandList();
            commands->begin();
            commands->barriers(RenderBarrierStage::GRAPHICS,RenderTextureBarrier(target.get(),RenderTextureLayout::COLOR_WRITE));
            commands->barriers(RenderBarrierStage::GRAPHICS,RenderTextureBarrier(depth.get(),RenderTextureLayout::DEPTH_WRITE));
            commands->setFramebuffer(framebuffer.get());commands->setViewports(RenderViewport(0,0,16,16));
            commands->setScissors(RenderRect(0,0,16,16));commands->setGraphicsPipelineLayout(layout.get());
            commands->setPipeline(visible?visible_pipeline.get():hidden_pipeline.get());
            tracker.ResetDraw(*commands);reject_snapshot();
            reject([&]{tracker.CompletePending();},"Prepared query allowed batch retirement");
            tracker.BeginDraw(*commands);reject_snapshot();
            reject([&]{tracker.CompletePending();},"Recording query allowed batch retirement");
            commands->drawInstanced(3,1,0,0);tracker.EndDraw(*commands);reject_snapshot();
            commands->end();pending.push_back(std::move(commands));
        };
        auto flush=[&] {
            std::vector<const RenderCommandList*> lists;
            for(const auto& command:pending)lists.push_back(command.get());
            queue->executeCommandLists(lists.data(),uint32_t(lists.size()),nullptr,0,nullptr,0,fence.get());
            queue->waitForCommandFence(fence.get());tracker.CompletePending();pending.clear();
            tracker.CompletePending(); // Repeated retirement must not recount results.
        };
        auto draw=[&](bool visible) {record(visible);flush();};
        auto metadata=[&](uint64_t draws,uint32_t count) {
            const auto state=tracker.MetadataSnapshot();
            if(!state||state->handle!=0x1000||state->begin_address!=0x2020||state->end_address!=0x2000||
               state->draws!=draws||state->pending_draws!=count||state->recording||tracker.pending_draws()!=count)
                throw std::runtime_error("Query metadata did not retain accepted draw count");
        };
        tracker.Begin(0x1000,0x2020);
        auto subtotal=tracker.Snapshot();
        if(!subtotal||subtotal->handle!=0x1000||subtotal->begin_address!=0x2020||
           subtotal->end_address!=0x2000||subtotal->samples||subtotal->draws)
            throw std::runtime_error("Empty active query subtotal invalid");
        metadata(0,0);
        record(true);record(false);metadata(2,2);
        reject([&]{(void)tracker.End(0x1000,0x2000);},"Query ended before batch fence");
        flush();metadata(2,0);
        subtotal=tracker.Snapshot();
        if(!subtotal||subtotal->samples!=256||subtotal->draws!=2)
            throw std::runtime_error("Completed query subtotal invalid");
        draw(true);
        const auto visible=tracker.End(0x1000,0x2000);
        if(visible.samples!=512||visible.draws!=3)throw std::runtime_error("Native query failed to accumulate visible/occluded draws");
        if(tracker.Snapshot())throw std::runtime_error("Ended query retained active subtotal");
        tracker.Begin(0x1000,0x2020);draw(false);
        const auto hidden=tracker.End(0x1000,0x2000);
        if(hidden.samples||hidden.draws!=1)throw std::runtime_error("Reused native query retained prior samples");
        tracker.Begin(0x1000,0x2020);
        const auto empty=tracker.End(0x1000,0x2000);
        if(empty.samples||empty.draws)throw std::runtime_error("Empty query was not zero");
        tracker.Begin(0x1000,0x2020);
        for(unsigned round=0;round<2;++round) {
            for(unsigned i=0;i<capacity;++i)record(i%2==0);
            metadata((round+1)*capacity,capacity);
            auto overflow=queue->createCommandList();overflow->begin();
            reject([&]{tracker.ResetDraw(*overflow);},"Query pool reused a pending slot before its fence");
            overflow->end();
            flush();metadata((round+1)*capacity,0);
            subtotal=tracker.Snapshot();
            if(!subtotal||subtotal->samples!=(round+1)*capacity*128u||subtotal->draws!=(round+1)*capacity)
                throw std::runtime_error("Full query batch accumulation failed");
        }
        const auto pooled=tracker.End(0x1000,0x2000);
        if(pooled.samples!=capacity*256u||pooled.draws!=capacity*2u)throw std::runtime_error("Pooled query subtotal changed at End");
        tracker.Begin(0x1000,0x2020);
        for(unsigned i=0;i<capacity;++i)record(false);
        flush();const auto pooled_hidden=tracker.End(0x1000,0x2000);
        if(pooled_hidden.samples||pooled_hidden.draws!=capacity)throw std::runtime_error("Reused full pool retained previous visibility");
        // Multiple ended logical queries share one submission without mixing
        // slot ownership. Reusing an address does not reuse its logical record.
        tracker.Begin(0x1100,0x3020);record(true);record(false);
        tracker.EndDeferred(0x1100,0x3000);
        if(tracker.MetadataSnapshot()||tracker.active())throw std::runtime_error("Closed query remained active");
        tracker.Begin(0x1200,0x4020);tracker.EndDeferred(0x1200,0x4000); // No GPU slot.
        tracker.Begin(0x1100,0x3020);record(false);tracker.EndDeferred(0x1100,0x3000);
        tracker.Begin(0x1300,0x5020);record(true);
        auto active=tracker.MetadataSnapshot();
        if(!active||active->handle!=0x1300||active->draws!=1||active->pending_draws!=1||tracker.pending_draws()!=4)
            throw std::runtime_error("Closed slot owners contaminated active query metadata");
        if(tracker.deferred_queries()!=3)throw std::runtime_error("Zero-draw query escaped logical queue accounting");
        reject([&]{(void)tracker.TakeDeferredResults();},"Deferred results escaped before their fence");
        flush();
        if(tracker.deferred_queries()!=3)throw std::runtime_error("Fence discarded unconsumed query results");
        auto completed=tracker.TakeDeferredResults();
        if(completed.size()!=3||completed[0].handle!=0x1100||completed[0].end_address!=0x3000||
            completed[0].samples!=256||completed[0].draws!=2||completed[1].handle!=0x1200||
            completed[1].end_address!=0x4000||completed[1].samples||completed[1].draws||
            completed[2].handle!=0x1100||completed[2].end_address!=0x3000||
            completed[2].samples||completed[2].draws!=1)
            throw std::runtime_error("Deferred per-query ownership or end order changed");
        subtotal=tracker.Snapshot();
        if(!subtotal||subtotal->samples!=256||subtotal->draws!=1)
            throw std::runtime_error("Active query subtotal lost while closed queries retired");
        record(true);tracker.EndDeferred(0x1300,0x5000);flush();
        completed=tracker.TakeDeferredResults();
        if(completed.size()!=1||completed[0].samples!=512||completed[0].draws!=2)
            throw std::runtime_error("Deferred active query failed accumulation across fences");
        if(!tracker.TakeDeferredResults().empty())throw std::runtime_error("Deferred consumption repeated results");

        // Consume A while B owns a physical slot, then immediately reuse A's
        // logical record for C. The older B slot must still address B's record.
        tracker.Begin(0x1400,0x5420);record(true);tracker.EndDeferred(0x1400,0x5400);
        tracker.Begin(0x1500,0x5820);record(false);tracker.EndDeferred(0x1500,0x5800);
        queue->executeCommandLists(pending[0].get(),fence.get());queue->waitForCommandFence(fence.get());
        tracker.CompletePrefix(1);pending.erase(pending.begin());
        completed=tracker.TakeDeferredPrefix(1);
        if(completed.size()!=1||completed[0].handle!=0x1400||completed[0].samples!=256||tracker.pending_draws()!=1)
            throw std::runtime_error("First prefix lost samples or retired B's slot");
        tracker.Begin(0x1600,0x5c20);record(true);tracker.EndDeferred(0x1600,0x5c00);
        reject([&]{tracker.TakeDeferredPrefix(1);},"Unfenced logical prefix was consumed");
        if(tracker.deferred_queries()!=2)throw std::runtime_error("Rejected prefix altered logical ownership");
        flush();completed=tracker.TakeDeferredPrefix(2);
        if(completed.size()!=2||completed[0].handle!=0x1500||completed[0].samples||completed[0].draws!=1||
            completed[1].handle!=0x1600||completed[1].samples!=256||completed[1].draws!=1)
            throw std::runtime_error("Reused logical record stranded an older pending physical slot");

        // Record pressure is independent of draw pressure: completed results
        // remain bounded until consumed, independently of physical slot capacity.
        for(uint32_t i=0;i<NativeQueryTracker::kMaxDeferredQueries;++i) {
            tracker.Begin(0x2000+i,0x6020+i*64);record(i%2==0);
            tracker.EndDeferred(0x2000+i,0x6000+i*64);
            if((i+1)%capacity==0)flush();
        }
        tracker.Begin(0x3000,0x8020);
        reject([&]{tracker.EndDeferred(0x3000,0x8000);},"Logical query beyond configured bound was accepted");
        if(!tracker.MetadataSnapshot()||tracker.MetadataSnapshot()->handle!=0x3000)
            throw std::runtime_error("Queue pressure consumed the active query");
        if(!pending.empty())flush();
        reject([&]{tracker.EndDeferred(0x3000,0x8000);},"Ready queries escaped bounded queue accounting");
        completed=tracker.TakeDeferredResults();
        if(completed.size()!=NativeQueryTracker::kMaxDeferredQueries)throw std::runtime_error("Full deferred queue lost records");
        for(size_t i=0;i<completed.size();++i)
            if(completed[i].handle!=0x2000+i||completed[i].end_address!=0x6000+i*64||
               completed[i].draws!=1||completed[i].samples!=(i%2==0?256u:0u))
                throw std::runtime_error("Full deferred queue mixed GPU slot owners");
        tracker.EndDeferred(0x3000,0x8000);completed=tracker.TakeDeferredResults();
        if(completed.size()!=1||completed[0].samples||completed[0].draws)
            throw std::runtime_error("Empty query retry after pressure failed");
        for(uint32_t i=0;i<NativeQueryTracker::kMaxDeferredQueries;++i) {
            tracker.Begin(0x4000+i,0x9020);tracker.EndDeferred(0x4000+i,0x9000);
        }
        tracker.Begin(0x5000,0xa020);
        reject([&]{tracker.EndDeferred(0x5000,0xa000);},"Empty queries bypassed logical record cap");
        completed=tracker.TakeDeferredResults();
        if(completed.size()!=NativeQueryTracker::kMaxDeferredQueries)throw std::runtime_error("Empty-query pressure lost records");
        tracker.EndDeferred(0x5000,0xa000);completed=tracker.TakeDeferredResults();
        if(completed.size()!=1||completed[0].samples||completed[0].draws)
            throw std::runtime_error("Query slot reuse after empty pressure failed");
        // Two actual GPU fences overlap CPU recording. Retiring A must free
        // only its physical prefix, retaining B's active/logical ownership.
        auto second_fence=device->createCommandFence();
        auto submit_range=[&](size_t first,size_t count,RenderCommandFence* completed_fence) {
            std::vector<const RenderCommandList*> lists;
            for(size_t i=0;i<count;++i)lists.push_back(pending.at(first+i).get());
            queue->executeCommandLists(lists.data(),uint32_t(lists.size()),nullptr,0,nullptr,0,completed_fence);
        };
        tracker.Begin(0x6100,0xb020);
        for(unsigned i=0;i<quarter;++i)record(true);
        tracker.EndDeferred(0x6100,0xb000);
        tracker.Begin(0x6101,0xb060);tracker.EndDeferred(0x6101,0xb040);
        tracker.Begin(0x6200,0xc020);
        for(unsigned i=0;i<quarter;++i)record(false);
        submit_range(0,half,fence.get());
        for(unsigned i=0;i<half;++i)record(true);
        submit_range(half,half,second_fence.get());
        reject([&]{tracker.CompletePrefix(capacity+1);},"Prefix retirement exceeded owned query slots");
        queue->waitForCommandFence(fence.get());tracker.CompletePrefix(half);
        active=tracker.MetadataSnapshot();
        if(tracker.pending_draws()!=half||!active||active->handle!=0x6200||active->draws!=quarter+half||active->pending_draws!=half)
            throw std::runtime_error("First fence retired later physical slots or active query ownership");
        reject_snapshot();reject([&]{tracker.TakeDeferredResults();},"Completed prefix bypassed later query slots");
        if(!tracker.TakeDeferredPrefix(0).empty()||tracker.deferred_queries()!=2)
            throw std::runtime_error("Empty prefix altered closed record ownership");
        reject([&]{tracker.TakeDeferredPrefix(3);},"Prefix consumed an active or missing query");
        completed=tracker.TakeDeferredPrefix(2);
        if(completed.size()!=2||completed[0].handle!=0x6100||completed[0].samples!=quarter*256u||
            completed[0].draws!=quarter||completed[1].handle!=0x6101||completed[1].samples||
            completed[1].draws||tracker.deferred_queries()||tracker.pending_draws()!=half)
            throw std::runtime_error("Completed query prefix lost exact counts or released later physical slots");
        // Reuse exactly the fenced physical ring prefix while B stays owned.
        // The same logical query accumulates across all three submissions.
        for(unsigned i=0;i<half;++i)record(false);
        tracker.EndDeferred(0x6200,0xc000);submit_range(capacity,half,fence.get());
        queue->waitForCommandFence(second_fence.get());tracker.CompletePrefix(half);
        if(tracker.pending_draws()!=half)throw std::runtime_error("Second fence reused an unfenced ring tail");
        reject([&]{tracker.TakeDeferredResults();},"Ring-prefix retirement exposed a partial closed query");
        queue->waitForCommandFence(fence.get());tracker.CompletePrefix(half);pending.clear();
        completed=tracker.TakeDeferredResults();
        if(completed.size()!=1||completed[0].handle!=0x6200||completed[0].samples!=half*256u||
            completed[0].draws!=quarter+2*half)
            throw std::runtime_error("Partial physical-ring retirement mixed closed/empty/active query records");
        std::cout<<"Native occlusion: "<<capacity<<" physical slots, exact full-pool overflow/reuse, "
                    "logical record pressure, two in-flight "<<half<<"-slot fences and physical-ring prefix reuse passed\n";
        const auto& resets=tracker.reset_statistics();
        if(tracker.host_reset_enabled()) {
            if(!resets.host_calls||!resets.gpu_calls_avoided||resets.gpu_calls||
               resets.host_slots!=capacity+resets.gpu_calls_avoided)
                throw std::runtime_error("Host reset ranges do not conserve retired and initially free slots");
        } else if(resets.host_calls||resets.host_slots||resets.gpu_calls_avoided||!resets.gpu_calls)
            throw std::runtime_error("Disabled/unadvertised host reset did not retain GPU reset commands");
        std::cout<<"Query resets: host="<<tracker.host_reset_enabled()<<" calls="<<resets.host_calls
                 <<" slots="<<resets.host_slots<<" gpu="<<resets.gpu_calls<<" avoided="<<resets.gpu_calls_avoided<<'\n';
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
