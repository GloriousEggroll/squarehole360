#include "gpu_helpers.h"
#include "renderer/native/scene_executor.h"
#include "renderer/native/fsi_device.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>
using namespace plume;
using namespace gow3::renderer;
namespace {void Require(bool value,const char* why){if(!value)throw std::runtime_error(why);}}
int main(int argc,char** argv) try {
    const bool asynchronous=argc==2&&std::string_view(argv[1])=="--async-gpu-submissions";
    const bool unified=asynchronous||(argc==2&&std::string_view(argv[1])=="--unified-gpu-queue");
    const bool batched=unified||(argc==2&&std::string_view(argv[1])=="--batch-transfers");
    Require(argc==1||batched,"Usage: gow3-native-state-export-test [--batch-transfers|--unified-gpu-queue|--async-gpu-submissions]");
    FsiDevice fsi_device;auto* device=&fsi_device.device();
    auto queue=device->createCommandQueue(RenderCommandListType::DIRECT);
    constexpr uint32_t edram_bytes=10*1024*1024;
    std::vector<uint8_t> input(edram_bytes);
    for(size_t i=0;i<input.size();++i)input[i]=uint8_t(i*83+(i>>8));
    auto upload=device->createBuffer(RenderBufferDesc::UploadBuffer(edram_bytes,RenderBufferFlag::STORAGE));
    std::memcpy(upload->map(),input.data(),input.size());validation::Flush(*upload);upload->unmap();
    SceneExecutorConfig config;config.native_fsi=true;config.ordered_pixel_interlock_enabled=true;config.batch_transfers=batched;
    config.unified_gpu_queue=unified;
    config.async_gpu_submissions=asynchronous;
    SceneExecutor renderer(config,device,queue.get());
    Require(renderer.mutation_serial()==0,"New native executor is already mutated");
    renderer.InitializeEdramFromBuffer(static_cast<VulkanBuffer&>(*upload).vk);
    const auto serial=renderer.mutation_serial();Require(serial!=0,"EDRAM import did not record mutation");
    auto state=renderer.ExportState();
    Require(state.physical.ranges.empty(),"EDRAM import invented physical ownership");
    Require(state.edram.capacity==edram_bytes&&state.edram.ranges==NativeStateRanges{{0,edram_bytes}},"EDRAM import lost initialized ranges");
    auto readback=device->createBuffer(RenderBufferDesc::ReadbackBuffer(edram_bytes));
    auto commands=queue->createCommandList();auto fence=device->createCommandFence();commands->begin();
    auto command=static_cast<VulkanCommandList&>(*commands).vk;
    VkBufferMemoryBarrier b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    b.buffer=state.edram.buffer;b.size=VK_WHOLE_SIZE;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    b.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;b.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,1,&b,0,nullptr);
    VkBufferCopy region{0,0,edram_bytes};
    vkCmdCopyBuffer(command,state.edram.buffer,static_cast<VulkanBuffer&>(*readback).vk,1,&region);
    VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);
    commands->end();queue->executeCommandLists(commands.get(),fence.get());queue->waitForCommandFence(fence.get());
    auto data=readback->map();auto& rb=static_cast<VulkanBuffer&>(*readback);
    Require(vmaInvalidateAllocation(rb.device->allocator,rb.allocation,0,VK_WHOLE_SIZE)==VK_SUCCESS,"Cannot read imported EDRAM");
    const bool equal=std::memcmp(data,input.data(),input.size())==0;readback->unmap();Require(equal,"EDRAM bytes changed at native handoff");
    bool rejected=false;try{renderer.InitializeEdramFromBuffer(static_cast<VulkanBuffer&>(*upload).vk);}catch(const std::runtime_error&){rejected=true;}
    Require(rejected&&renderer.mutation_serial()==serial,"Late EDRAM reinitialization was not rejected before writes");
    renderer.InitializeMemory(64,std::span(input.data(),128));
    const auto physical_serial=renderer.mutation_serial();Require(physical_serial>serial,"Physical initialization missed mutation");
    renderer.Execute({{"type","cpu_memory_write"},{"event",1},{"frame",0},{"address",68},{"bytes",2},{"payload",nullptr}});
    state=renderer.ExportState();Require(state.physical.ranges==NativeStateRanges{{64,4},{70,122}},"Export covered freed neighboring bytes");
    Require(renderer.ReadPhysical(64,4)==std::vector<uint8_t>(input.begin(),input.begin()+4),"Physical publication bytes changed");
    const auto before=renderer.mutation_serial();rejected=false;
    try{renderer.ReadPhysical(68,4);}catch(const std::runtime_error&){rejected=true;}
    Require(rejected&&renderer.mutation_serial()==before,"Undefined read changed native state");
    rejected=false;try{renderer.Execute({{"type","unsupported_test_operation"},{"event",2},{"frame",0}});}catch(const std::runtime_error&){rejected=true;}
    Require(rejected&&renderer.mutation_serial()==before,"Unsupported operation changed native state");
    // A packet baseline must commit the previous clear even when no ownership
    // snapshot is requested. Subsequent flushes/exports must be idempotent.
    const nlohmann::json surface{{"depth",false},{"edram_base",0},{"format",0},
        {"surface_info",80},{"width",1},{"height",1}};
    renderer.Execute({{"type","clearrect"},{"event",3},{"frame",0},{"flags",1},
        {"rectangle",{0,0,1,1}},{"color",{1.f,0.f,0.f,1.f}},{"depth",1.f},{"stencil",0},
        {"surfaces",{{"6",surface}}}});
    const auto dirty_serial=renderer.mutation_serial();
    renderer.FlushCommittedState();
    const auto flushed_serial=renderer.mutation_serial();
    Require(batched?flushed_serial==dirty_serial:flushed_serial>dirty_serial,
        "Packed clear commit timing did not match the configured synchronous/batched path");
    renderer.FlushCommittedState();state=renderer.ExportState();
    Require(renderer.mutation_serial()==flushed_serial,"Clean flush/export wrote EDRAM again");
    Require(state.physical.ranges==NativeStateRanges{{64,4},{70,122}},"Flush altered physical ownership");
    commands->begin();
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,1,&b,0,nullptr);
    region.size=4;
    vkCmdCopyBuffer(command,state.edram.buffer,static_cast<VulkanBuffer&>(*readback).vk,1,&region);
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);
    commands->end();queue->executeCommandLists(commands.get(),fence.get());queue->waitForCommandFence(fence.get());
    data=readback->map();
    Require(vmaInvalidateAllocation(rb.device->allocator,rb.allocation,0,VK_WHOLE_SIZE)==VK_SUCCESS,"Cannot read flushed EDRAM");
    const std::array<uint8_t,4> red{255,0,0,255};
    const bool red_equal=std::memcmp(data,red.data(),red.size())==0;readback->unmap();
    Require(red_equal,"Flush did not preserve the clear's exact packed EDRAM bytes");
    // Immediate standalone clear->resolve must commit the dirty clear even
    // when no Draw/runtime transaction flush occurs between the operations.
    // Repeat with changed contents so a stale host view cannot satisfy this.
    uint64_t event=4;
    uint32_t destination=0x1000;
    for(uint32_t format:{4u,5u})for(bool swap:{false,true}) {
        const nlohmann::json fixed{{"depth",false},{"edram_base",8},{"format",format},
            {"surface_info",80},{"width",1},{"height",1}};
        std::array<float,4> color=swap?std::array<float,4>{4,-12,20,-28}:
            std::array<float,4>{-8,16,24,32};
        renderer.Execute({{"type","clearrect"},{"event",event++},{"frame",0},{"flags",1},
            {"rectangle",{0,0,1,1}},{"color",color},{"depth",1.f},{"stencil",0},
            {"surfaces",{{"6",fixed}}}});
        const auto clear_serial=renderer.mutation_serial();
        // A one-texel float4 destination makes fixed16 rounding, exponent
        // bias, the missing B/A defaults of format4, and storage swap visible.
        const std::array<uint32_t,6> fetch{2u|(1u<<22),destination|38u,0,swap?4u:0u,0,1u<<9};
        char base[9];std::snprintf(base,sizeof(base),"%08x",destination);
        renderer.Execute({{"type","resolve"},{"event",event++},{"frame",0},{"level",0},{"flags",0},
            {"source_slot",0},{"source_rectangle",{0,0,1,1}},{"destination_rectangle",{0,0,1,1}},
            {"destination",{{"base",base},{"format",38},{"width",1},{"height",1}}},
            {"destination_fetch",fetch},{"exponent_bias",-1},{"copy_dest_swap",swap},
            {"surfaces",{{"6",fixed}}}});
        Require(renderer.mutation_serial()==clear_serial+(batched?1u:2u),
            "Direct resolve did not commit exactly one dirty EDRAM image and one destination write");
        std::array<float,4> expected;
        for(unsigned c=0;c<4;++c)
            expected[c]=std::round(std::clamp(color[c],-32.f,32.f)*(32767.f/32.f))/32767.f*32.f;
        if(format==4){expected[2]=0;expected[3]=1;}
        for(auto& component:expected)component*=.5f;
        if(swap)std::swap(expected[0],expected[2]);
        const auto bytes=renderer.ReadPhysical(destination,16);
        std::array<float,4> actual;std::memcpy(actual.data(),bytes.data(),bytes.size());
        for(unsigned c=0;c<4;++c)Require(std::abs(actual[c]-expected[c])<.00001f,
            "Direct fixed16 resolve lost clear contents, quantization, exponent or channel swap");
        const auto resolve_serial=renderer.mutation_serial();
        renderer.FlushCommittedState();state=renderer.ExportState();
        Require(renderer.mutation_serial()==resolve_serial,"Direct resolve left a dirty source for a later flush");
        Require(state.edram.ranges==NativeStateRanges{{0,edram_bytes}},"Direct resolve changed source EDRAM ownership");
        const auto range=std::find(state.physical.ranges.begin(),state.physical.ranges.end(),
            NativeStateRanges::value_type{destination,16});
        Require(range!=state.physical.ranges.end(),"Direct resolve lost exact destination ownership");
        destination+=0x1000;
    }
    Require(renderer.report().at("direct_fixed_source_resolves")==4,
        "Fixed16 test did not exercise every direct packed-source resolve");
    if(batched) {
        const auto& batches=renderer.report().at("transfer_batches");
        Require(batches.at("stages").get<uint64_t>()>batches.at("submissions").get<uint64_t>()&&
            batches.at("submissions")==batches.at("waits")&&batches.at("maximum_stages").get<uint64_t>()>=3&&
            batches.at("maximum_stages").get<uint64_t>()<=64&&batches.at("synchronous_fallbacks")==0,
            "Batched state fixture did not combine actual GPU stages behind one fence");
        std::cout<<batches.dump()<<'\n';
        if(unified) {
            const auto& work=renderer.report().at("gpu_work_queue");
            Require(work.at("submissions").get<uint64_t>()>0&&work.at("submissions")==work.at("waits")&&
                work.at("transfer_lists").get<uint64_t>()>0&&work.at("maximum_lists").get<uint64_t>()<=64&&
                work.at("maximum_scratch_bytes").get<uint64_t>()<=128u*1024*1024&&
                !renderer.HasPendingGpuWork(),"Unified state fixture left work or exceeded shared queue bounds");
            std::cout<<work.dump()<<'\n';
        }
    }
    std::cout<<"PASS: actual 10 MiB EDRAM handoff, exact physical ownership/publication, dirty flush, four direct fixed16 clear-resolves and mutation tracking\n";
    return 0;
} catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
