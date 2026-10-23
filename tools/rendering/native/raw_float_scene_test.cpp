#include "raw_float_scene_test.h"
#include "renderer/native/resolve_memory.h"
#include "renderer/native/texture_upload.h"
#include "renderer/native/raw_float_texture.h"
#include "renderer/native/transfer_batch.h"
#include "renderer/native/gpu_work_queue.h"
#include "renderer/native/gpu_helpers.h"
#include "raw_float_texture_spv.h"
#include "raw_float_sample_spv.h"
#include "raw_float_scene_spv.h"
#include "memory_store_spv.h"
#include "memory_load_spv.h"
#include <array>
#include <bit>
#include <cstring>
#include <iostream>
#include <unordered_map>
namespace gow3::renderer {
namespace {
using namespace plume;using Json=nlohmann::json;
void Require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
template<class F>void Reject(F&& f,const char* message) {
    bool rejected=false;try{f();}catch(const std::runtime_error&){rejected=true;}Require(rejected,message);
}
template<class T,size_t N>std::vector<uint8_t> ShaderBytes(const T(&data)[N]) {
    std::vector<uint8_t> bytes(sizeof(data));std::memcpy(bytes.data(),data,sizeof(data));return bytes;
}
std::array<uint32_t,6> Fetch(uint32_t base) {
    return {2u|(85u<<2)|(128u<<22),base|38u|(2u<<6),4095,0x688u<<1|1u,0,1u<<9};
}
std::vector<uint8_t> PhysicalWords(const std::vector<uint32_t>& words) {
    std::vector<uint8_t> bytes(words.size()*4);
    for(size_t i=0;i<words.size();++i)for(unsigned b=0;b<4;++b)bytes[i*4+b]=uint8_t(words[i]>>(24-8*b));
    return bytes;
}
std::vector<uint8_t> Sample(RenderDevice& device,RenderCommandQueue& queue,NativeTexture& source,const TextureLayout& t) {
    auto shader=device.createShader(raw_float_sample_spv,sizeof(raw_float_sample_spv),"main",RenderShaderFormat::SPIRV);
    const std::array<RenderDescriptorRange,3> ranges{{{RenderDescriptorRangeType::TEXTURE,0,1},
        {RenderDescriptorRangeType::SAMPLER,1,1},{RenderDescriptorRangeType::READ_WRITE_BYTE_ADDRESS_BUFFER,2,1}}};
    const RenderDescriptorSetDesc desc(ranges.data(),ranges.size());
    auto layout=device.createPipelineLayout(RenderPipelineLayoutDesc(nullptr,0,&desc,1));
    auto pipeline=device.createComputePipeline(RenderComputePipelineDesc(layout.get(),shader.get(),64,1,1));
    Require(pipeline&&static_cast<VulkanComputePipeline*>(pipeline.get())->vk,"Raw float sampler pipeline failed");
    auto output=device.createBuffer(RenderBufferDesc::DefaultBuffer(65536,RenderBufferFlag::STORAGE|RenderBufferFlag::UNORDERED_ACCESS));
    auto readback=device.createBuffer(RenderBufferDesc::ReadbackBuffer(65536));
    auto descriptors=device.createDescriptorSet(desc);
    Require(source.binding.properties.signs==85&&source.binding.properties.exponent==0,
        "Raw float binding changed original signed/number-format interpretation");
    descriptors->setTexture(0,source.GetImage(),RenderTextureLayout::SHADER_READ,source.binding.view.get());
    descriptors->setSampler(1,source.binding.sampler.get());descriptors->setBuffer(2,output.get(),65536);
    auto commands=queue.createCommandList();auto fence=device.createCommandFence();commands->begin();
    commands->barriers(RenderBarrierStage::COMPUTE,RenderTextureBarrier(source.GetImage(),RenderTextureLayout::SHADER_READ));
    commands->barriers(RenderBarrierStage::COMPUTE,RenderBufferBarrier(output.get(),RenderBufferAccess::WRITE));
    commands->setComputePipelineLayout(layout.get());commands->setComputeDescriptorSet(descriptors.get(),0);
    commands->setPipeline(pipeline.get());commands->dispatch(64,1,1);
    commands->barriers(RenderBarrierStage::COPY,RenderBufferBarrier(output.get(),RenderBufferAccess::READ));
    commands->copyBufferRegion(RenderBufferReference(readback.get()),RenderBufferReference(output.get()),65536);
    VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(static_cast<VulkanCommandList&>(*commands).vk,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,
        0,1,&host,0,nullptr,0,nullptr);
    commands->end();queue.executeCommandLists(commands.get(),fence.get());queue.waitForCommandFence(fence.get());
    auto& buffer=static_cast<VulkanBuffer&>(*readback);const auto mapped=readback->map();Require(mapped,"Raw float sample mapping failed");
    Require(vmaInvalidateAllocation(buffer.device->allocator,buffer.allocation,0,VK_WHOLE_SIZE)==VK_SUCCESS,
        "Raw float sample invalidation failed");
    std::vector<uint8_t> result(65536);std::memcpy(result.data(),mapped,result.size());readback->unmap();return result;
}
std::vector<uint8_t> ReadPacked(RenderDevice& device,RenderCommandQueue& queue,VkBuffer source) {
    auto readback=device.createBuffer(RenderBufferDesc::ReadbackBuffer(2048*5120));
    auto commands=queue.createCommandList();auto fence=device.createCommandFence();commands->begin();
    VkBufferMemoryBarrier before{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};before.buffer=source;before.size=VK_WHOLE_SIZE;
    before.srcQueueFamilyIndex=before.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    before.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;before.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    const auto vk=static_cast<VulkanCommandList&>(*commands).vk;
    vkCmdPipelineBarrier(vk,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,1,&before,0,nullptr);
    const VkBufferCopy copy{0,0,2048*5120};vkCmdCopyBuffer(vk,source,static_cast<VulkanBuffer&>(*readback).vk,1,&copy);
    VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(vk,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);
    commands->end();queue.executeCommandLists(commands.get(),fence.get());queue.waitForCommandFence(fence.get());
    auto& buffer=static_cast<VulkanBuffer&>(*readback);const auto mapped=readback->map();Require(mapped,"Raw Scene packed mapping failed");
    Require(vmaInvalidateAllocation(buffer.device->allocator,buffer.allocation,0,VK_WHOLE_SIZE)==VK_SUCCESS,"Raw Scene packed invalidate failed");
    std::vector<uint8_t> result(2048*5120);std::memcpy(result.data(),mapped,result.size());readback->unmap();return result;
}
void CheckRawBits(RenderDevice& device,RenderCommandQueue& queue) {
    std::weak_ptr<RenderTexture> watched;bool fence_owner=false;
    NativeGpuWorkQueue work(device,queue,[&](const auto&){if(!watched.expired())fence_owner=true;},true);
    NativeTransferBatch batch(device,queue,&work);
    ResolveMemory memory(device,queue,memory_store_spv,memory_load_spv,512*1024,0,&batch);
    memory.EnableRawFloatCpuTextures(raw_float_texture_spv);Require(memory.raw_float_cpu_statistics().supported,"RGBA32F raw copy unavailable");
    bool reconfiguration_rejected=false;
    try {memory.EnableRawFloatCpuTextures(raw_float_texture_spv);}
    catch(const std::logic_error&){reconfiguration_rejected=true;}
    Require(reconfiguration_rejected,"Raw pipeline allowed reconfiguration");
    const auto fetch=Fetch(0x10000);const auto t=DescribeTexture(fetch);
    std::vector<uint32_t> words(16384);
    // This is a bit oracle, including payloads a float shader must never touch.
    const std::array<uint32_t,16> special{0,0x80000000,1,0x80000001,0x007fffff,0x00800000,
        0x7f800000,0xff800000,0x7fc01234,0xffc05678,0x7f800001,0xff800001,
        0x3f800000,0xc0000000,0x40400000,0xc0800000};
    for(size_t i=0;i<words.size();++i)words[i]=special[i%special.size()];
    const auto physical=PhysicalWords(words);memory.Initialize(t.base_address,physical);
    Require(memory.CanLoadRawFloatCpuTexture(t,physical.size())&&!memory.CanLoadRawFloatCpuTexture(t,65535),
        "Raw GPU admission changed the exact payload/ownership minimum");
    auto expected=DecodeTexture(fetch,physical,{}).levels[0].bytes;
    auto commands=queue.createCommandList();auto fence=device.createCommandFence();commands->begin();
    auto cpu=UploadTexture(device,*commands,DecodeTexture(fetch,physical,{}));commands->end();
    queue.executeCommandLists(commands.get(),fence.get());queue.waitForCommandFence(fence.get());
    NativeTexture raw;raw.raw_gpu_image=device.createTexture(RenderTextureDesc::Texture2D(4096,1,1,RenderFormat::R32G32B32A32_FLOAT));
    raw.binding=BindTexture(device,*raw.raw_gpu_image,t,false);watched=raw.raw_gpu_image;
    const auto before=work.statistics().waits;
    batch.Run([&]{memory.LoadRawFloatCpuTexture(raw.raw_gpu_image,t);});
    Require(work.pending()&&work.statistics().waits==before,"Raw copy inserted an eager host fence");
    work.Drain();Require(fence_owner,"Raw output lost its owner before actual FIFO completion");
    Require(validation::ReadImage(device,queue,*raw.GetImage(),16)==expected&&
        validation::ReadImage(device,queue,*cpu.GetImage(),16)==expected,
        "Integer raw copy changed signed zero/NaN/Inf/denormal/finite image bits");
    const auto cpu_sample=Sample(device,queue,cpu,t),raw_sample=Sample(device,queue,raw,t);
    for(size_t i=0;i<cpu_sample.size();i+=4) {
        uint32_t a,b;std::memcpy(&a,cpu_sample.data()+i,4);std::memcpy(&b,raw_sample.data()+i,4);
        const auto nan=[](uint32_t value){return (value&0x7f800000)==0x7f800000&&(value&0x007fffff);};
        Require(a==b||(nan(a)&&nan(b)),"Original RGBA32F sampling changed its paired finite/nonfinite interpretation");
    }
    // A later history write cannot alter the already decoded immutable image.
    auto replacement=physical;const std::array<float,4> pixel{1,-2,3,-4};
    for(size_t c=0;c<4;++c)for(unsigned b=0;b<4;++b)replacement[c*4+b]=uint8_t(std::bit_cast<uint32_t>(pixel[c])>>(24-8*b));
    NativeTexture next;next.raw_gpu_image=device.createTexture(RenderTextureDesc::Texture2D(4096,1,1,RenderFormat::R32G32B32A32_FLOAT));
    next.binding=BindTexture(device,*next.raw_gpu_image,t,false);
    batch.Run([&]{memory.InitializeCpuWrite(t.base_address,std::span(replacement).first(16),true);
        memory.LoadRawFloatCpuTexture(next.raw_gpu_image,t);});
    work.Drain();Require(validation::ReadImage(device,queue,*raw.GetImage(),16)==expected&&
        validation::ReadImage(device,queue,*next.GetImage(),16)==DecodeTexture(fetch,replacement,{}).levels[0].bytes,
        "Current raw generation changed or the newer ordered version sampled stale history");
    Reject([&]{batch.Run([&]{memory.LoadRawFloatCpuTexture(next.raw_gpu_image,t);});},
        "Raw API allowed overwrite of an existing immutable destination");
    const auto copies=memory.raw_float_cpu_statistics().copies;
    memory.Invalidate(t.base_address+65532,4);
    Require(!memory.CanLoadRawFloatCpuTexture(t,65536),"One missing sampled word admitted the raw GPU path");
    Reject([&]{batch.Run([&]{memory.LoadRawFloatCpuTexture(next.raw_gpu_image,t);});},"Undefined raw source accepted");
    Require(memory.raw_float_cpu_statistics().copies==copies&&!work.pending(),"Refused raw source recorded an unrelated copy");
    memory.Initialize(t.base_address,replacement);
    // Near-bound older image work must be fenced, preserving its owner, then
    // complete this scope synchronously rather than exceed scratch ownership.
    auto prior=std::shared_ptr<RenderTexture>(device.createTexture(RenderTextureDesc::ColorTarget(1,1,RenderFormat::R32G32B32A32_FLOAT)));
    auto fallback_image=std::shared_ptr<RenderTexture>(device.createTexture(
        RenderTextureDesc::Texture2D(4096,1,1,RenderFormat::R32G32B32A32_FLOAT)));
    watched=prior;fence_owner=false;const auto fallback_before=batch.statistics().synchronous_fallbacks;
    batch.Run([&] {
        batch.KeepImage(prior,NativeTransferBatch::kMaxImageBytes-4096);
        auto& c=batch.BeginStage();c.barriers(RenderBarrierStage::COPY,RenderTextureBarrier(prior.get(),RenderTextureLayout::COPY_DEST));
        const VkClearColorValue black{};const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        vkCmdClearColorImage(static_cast<VulkanCommandList&>(c).vk,static_cast<VulkanTexture&>(*prior).vk,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&black,1,&range);
        prior.reset();memory.LoadRawFloatCpuTexture(fallback_image,t);
    });
    Require(fence_owner&&watched.expired()&&!work.pending()&&
        batch.statistics().synchronous_fallbacks==fallback_before+1&&memory.raw_float_cpu_statistics().synchronous==1,
        "Raw scratch pressure released old owners or removed the bounded synchronous completion");
    Require(validation::ReadImage(device,queue,*fallback_image,16)==DecodeTexture(fetch,replacement,{}).levels[0].bytes,
        "Raw scratch fallback changed exact output bits");
    // A later error cannot abandon the accepted transfer's layout or owner.
    auto failed=std::shared_ptr<RenderTexture>(device.createTexture(RenderTextureDesc::Texture2D(4096,1,1,RenderFormat::R32G32B32A32_FLOAT)));
    watched=failed;fence_owner=false;bool fatal=false;
    try{batch.Run([&]{memory.LoadRawFloatCpuTexture(failed,t);failed.reset();throw std::runtime_error("later fixture error");});}
    catch(const NativeTransferBatchError&){fatal=true;}
    Require(fatal&&fence_owner&&watched.expired()&&!work.pending(),"Accepted raw prefix failure lost fatality/completion ownership");
    Require(memory.ReadBytes(t.base_address,65536)==replacement,"Raw GPU read changed authoritative physical history");
}
void CheckCpuTextureDrains(RenderDevice& device,RenderCommandQueue& queue,
    SceneExecutorConfig base,Json draw,const std::vector<uint8_t>& state) {
    Require(!base.defer_cpu_texture_drains,"CPU texture drain experiment changed default");
    const auto fetch=Fetch(0x50000);
    const std::string state_id=draw.at("device_state"),ps_id(64,'8'),write_id(64,'b');
    const auto original_pixel=base.fsi_shader;
    auto run=[&](bool enabled) {
        auto config=base;config.defer_cpu_texture_drains=enabled;
        config.defer_native_queries=config.batch_transfers=config.unified_gpu_queue=true;
        config.queue_cpu_memory_writes=true;config.retire_completed_query_prefixes=false;
        config.max_draw_batch_size=16;config.async_gpu_submissions=true;
        config.payload={};config.shader_snapshot={};config.observe_pipeline={};
        std::unordered_map<std::string,OwnedPayload::Snapshot> bank;
        bank[state_id]=std::make_shared<const std::vector<uint8_t>>(state);
        config.payload_snapshot=[&](const std::string& key){const auto i=bank.find(key);
            Require(i!=bank.end(),"Missing CPU texture drain fixture payload");return i->second;};
        config.fsi_shader=[&](const std::string& key){return key==ps_id?ShaderBytes(raw_float_scene_spv):original_pixel(key);};
        SceneExecutor scene(config,&device,&queue);uint64_t event=0;
        scene.InitializeEdram(std::vector<uint8_t>(2048*5120));
        scene.InitializeMemory(0x1000,std::vector<uint8_t>(16));
        auto sample=[&](char identity) {
            const std::string key(64,identity);
            std::vector<uint32_t> words(16384);const std::array<float,4> red{1,-2,3,-4};
            for(unsigned c=0;c<4;++c)words[c]=std::bit_cast<uint32_t>(red[c]);
            words.back()=uint32_t(identity); // Distinct immutable payload; sampled first texel unchanged.
            bank[key]=std::make_shared<const std::vector<uint8_t>>(PhysicalWords(words));
            auto op=draw;op["event"]=++event;op["pixel_shader"]=ps_id;
            op["textures"]=Json::array({{{"slot",16},{"source","cpu_snapshot"},{"fetch",fetch},
                {"base_payload",key},{"mip_payload",nullptr}}});scene.Execute(op);
            bank.erase(key); // Scene must own both image and upload beyond this scope.
        };
        sample('c');scene.FlushGpuWork(); // Warm shader/attachment before measuring completion.
        const auto waits=scene.report().at("gpu_work_queue").at("waits");
        scene.Execute({{"type","query_begin"},{"frame",7},{"event",++event},{"query_generation",1600},
            {"query_handle",0x1000},{"report_address",0x2020}});
        sample('d');sample('e');sample('f');
        Require(scene.HasPendingGpuWork(),"Pure CPU texture sequence lost accepted tail");
        const auto after=scene.report().at("gpu_work_queue").at("waits");
        Require(enabled?after==waits:after>waits,"CPU-only texture completion selection changed");
        if(enabled) {
            const auto stats=scene.report().at("cpu_texture_drains");
            Require(stats.at("pure_cpu")==4&&stats.at("pending_pure_cpu").get<uint64_t>()>=2&&
                stats.at("overlays")==0,"Pure CPU texture drain coverage missing");
        }
        // A queued physical write claims only the first texel. The later CPU
        // snapshot must wait and overlay those bytes, then discard that sample.
        bank[write_id]=std::make_shared<const std::vector<uint8_t>>(16,0);
        scene.Execute({{"type","cpu_memory_write"},{"frame",7},{"event",++event},{"address",0x50000},
            {"bytes",16},{"payload",write_id}});bank.erase(write_id);
        sample('1');
        Require(scene.report().at("gpu_work_queue").at("waits")>after,
            "Partial GPU-owned texture bypassed its required readback completion");
        if(enabled)Require(scene.report().at("cpu_texture_drains").at("overlays")==1,
            "Partial GPU overlay was not counted");
        scene.Execute({{"type","query_end"},{"frame",7},{"event",++event},{"query_generation",1600},
            {"query_handle",0x1000},{"report_address",0x2000}});
        scene.FlushGpuWork();const auto completed=scene.TakeCompletedQueries();
        if(completed.size()==1)std::cout<<"CPU texture drain fixture enabled="<<enabled
            <<" draws="<<completed[0].result.draws<<" samples="<<completed[0].result.samples<<'\n';
        Require(completed.size()==1&&completed[0].generation==1600&&completed[0].result.draws==4&&
            completed[0].result.samples==768,"Deferred CPU uploads changed visibility or read stale overlay bytes");
        return std::pair{ReadPacked(device,queue,scene.ExportState().edram.buffer),completed[0].result.samples};
    };
    Require(run(false)==run(true),"CPU texture drain change altered full packed output or query samples");
    std::cout<<"PASS: CPU-only uploads retain accepted work and owners; partial GPU overlays wait; exact full EDRAM and real query parity\n";
}
} // namespace
void CheckRawFloatCpuTextures(RenderDevice& device,RenderCommandQueue& queue,
    SceneExecutorConfig base,Json draw,const std::vector<uint8_t>& state) {
    Require(!base.raw_gpu_float_cpu_textures,"Raw float CPU textures changed their default");
    CheckRawBits(device,queue);
    CheckCpuTextureDrains(device,queue,base,draw,state);
    const auto fetch=Fetch(0x10000);const auto t=DescribeTexture(fetch);
    const std::string state_id=draw.at("device_state"),base_id(64,'a'),write_id(64,'b'),ps_id(64,'8');
    const auto original_pixel=base.fsi_shader;
    auto run=[&](bool enabled,bool prefixes) {
        auto config=base;config.raw_gpu_float_cpu_textures=enabled;
        config.defer_native_queries=config.batch_transfers=config.unified_gpu_queue=true;
        config.queue_cpu_memory_writes=true;config.retire_completed_query_prefixes=prefixes;
        config.max_draw_batch_size=16;config.async_gpu_submissions=true;
        config.max_dormant_texture_bytes=config.max_dormant_texture_entries=0;
        config.payload={};config.shader_snapshot={};config.observe_pipeline={};
        std::unordered_map<std::string,OwnedPayload::Snapshot> bank;
        bank[state_id]=std::make_shared<const std::vector<uint8_t>>(state);
        bank[base_id]=std::make_shared<const std::vector<uint8_t>>(65536,0);
        config.payload_snapshot=[&](const std::string& key){const auto i=bank.find(key);
            Require(i!=bank.end(),"Missing raw Scene immutable CPU texture payload");return i->second;};
        config.fsi_shader=[&](const std::string& key){return key==ps_id?ShaderBytes(raw_float_scene_spv):original_pixel(key);};
        SceneExecutor scene(config,&device,&queue);uint64_t event=0;
        scene.InitializeEdram(std::vector<uint8_t>(2048*5120));scene.InitializeMemory(0x1000,std::vector<uint8_t>(16));
        std::vector<uint32_t> words(16384);const std::array<float,4> red{1,-2,3,-4};
        for(unsigned i=0;i<4;++i)words[i]=std::bit_cast<uint32_t>(red[i]);scene.InitializeMemory(t.base_address,PhysicalWords(words));
        draw["event"]=++event;scene.Execute(draw);scene.FlushGpuWork(); // warm only the dummy FSI attachment
        auto sample=[&](const std::string& key) {
            auto op=draw;op["event"]=++event;op["pixel_shader"]=ps_id;
            op["textures"]=Json::array({{{"slot",16},{"source","cpu_snapshot"},{"fetch",fetch},
                {"base_payload",key},{"mip_payload",nullptr}}});scene.Execute(op);
        };
        auto write=[&](bool visible) {
            std::vector<uint32_t> changed(4);for(unsigned c=0;c<4;++c)changed[c]=std::bit_cast<uint32_t>(red[c]);
            if(!visible)changed[0]=0;auto owner=std::make_shared<const std::vector<uint8_t>>(PhysicalWords(changed));
            std::weak_ptr<const std::vector<uint8_t>> weak=owner;bank[write_id]=owner;
            scene.Execute({{"type","cpu_memory_write"},{"frame",7},{"event",++event},{"address",t.base_address},
                {"bytes",16},{"payload",write_id}});bank.erase(write_id);owner.reset();
            Require(weak.expired(),"Raw Scene queued write retained a borrowed CPU owner");
        };
        const auto waits=scene.report().at("gpu_work_queue").at("waits");
        scene.Execute({{"type","query_begin"},{"frame",7},{"event",++event},{"query_generation",1500},
            {"query_handle",0x1000},{"report_address",0x2020}});
        sample(base_id);write(false);sample(base_id);write(true);sample(base_id);sample(base_id);
        Require(scene.HasPendingGpuWork(),"Raw Scene lost its actual accepted tail");
        if(enabled)Require(scene.report().at("gpu_work_queue").at("waits")==waits,
            "Fully owned raw CPU snapshot inserted an eager host fence");
        const auto before=scene.report().at("raw_gpu_float_cpu_textures");
        Reject([&]{sample(std::string(64,'0'));},"Raw Scene silently ignored a missing CPU provenance input");
        // The existing CPU path eagerly fences before asking for its payload;
        // the raw path rejects malformed provenance without draining its tail.
        Require(scene.HasPendingGpuWork()==enabled&&scene.report().at("raw_gpu_float_cpu_textures")==before,
            "Missing later CPU snapshot changed its expected completion boundary");
        scene.Execute({{"type","query_end"},{"frame",7},{"event",++event},{"query_generation",1500},
            {"query_handle",0x1000},{"report_address",0x2000}});
        scene.FlushGpuWork();const auto completed=scene.TakeCompletedQueries();
        Require(completed.size()==1&&completed[0].generation==1500&&completed[0].result.draws==4&&
            completed[0].result.samples==768,"Raw CPU snapshots changed real geometry samples/current history version");
        const auto raw_stats=scene.report().at("raw_gpu_float_cpu_textures");
        Require(raw_stats.at("copies")==uint64_t(enabled?3:0)&&raw_stats.at("queued")==uint64_t(enabled?3:0)&&
            raw_stats.at("logical_bytes")==uint64_t(enabled?3*65536:0)&&
            scene.report().at("texture_cache").at("creations")==3,
            "Raw Scene physical copies diverged from immutable cache generations");
        const auto packed=ReadPacked(device,queue,scene.ExportState().edram.buffer);
        for(size_t at=0;at<packed.size();at+=4) {uint32_t value;std::memcpy(&value,packed.data()+at,4);
            Require(value==(at<256*4?3u:0u),"Raw CPU sampling changed packed EDRAM/untouched words");}
        return std::pair{packed,completed[0].result.samples};
    };
    for(bool prefixes:{false,true})Require(run(false,prefixes)==run(true,prefixes),
        "Paired original CPU/native raw textures changed Scene query/packed output");
    std::cout<<"PASS: original RGBA32F raw bits/sampling, nonfinite/denormal/zero precision, float signed fetch semantics, immutable history versions, bounded scratch/failure completion and paired Scene query/full EDRAM\n";
}
} // namespace gow3::renderer
