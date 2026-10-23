#include "renderer/native/readonly_depth_attachment.h"
#include "renderer/native/readonly_depth_less.h"
#include "renderer/native/early_depth_lower_bound.h"
#include "renderer/native/fsi_device.h"
#include "renderer/native/content_hash.h"
#include "renderer/native/query_tracker.h"
#include "readonly_depth_attachment_comp_spv.h"
#include "readonly_depth_attachment_frag_spv.h"
#include "early_depth_lower_bound_comp_spv.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <span>
#include <stdexcept>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <vector>
using namespace plume;
using namespace gow3::renderer;
namespace {
constexpr uint32_t kBytes=2048*5120;
using Words=std::vector<uint32_t>;
void Check(bool value,const char* text){if(!value)throw std::runtime_error(text);}
Words Read(const char* path) {
    std::ifstream f(path,std::ios::binary|std::ios::ate);
    Check(f&&f.tellg()>=20&&f.tellg()<=1024*1024&&f.tellg()%4==0,"Invalid bounded fixture module");
    Words w(size_t(f.tellg())/4);f.seekg(0);
    Check(bool(f.read(reinterpret_cast<char*>(w.data()),w.size()*4)),"Truncated fixture module");return w;
}
// Buffer tracking reflects recording order, which differs from final FIFO
// order in this isolated owner fixture. Explicit ALL_COMMANDS dependencies
// independently cover every earlier accepted writer/read before a later use.
void BufferDependency(RenderCommandList& commands,RenderBuffer& buffer,
                     VkPipelineStageFlags stage,VkAccessFlags access) {
    VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    barrier.srcAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask=access;barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer=static_cast<VulkanBuffer&>(buffer).vk;barrier.size=VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(static_cast<VulkanCommandList&>(commands).vk,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        stage,0,0,nullptr,1,&barrier,0,nullptr);
}
void CheckLateDepthModule(std::span<const uint32_t> module,bool conservative=false) {
    Check(module.size()>=5&&module.size()<=262144&&module[0]==0x07230203&&module[3]<=1048576,
        "Invalid bounded isolated fragment module");
    std::vector<uint32_t> pointer_storage(module[3],UINT32_MAX),value_storage(module[3],UINT32_MAX);
    uint32_t output=0,depth_builtin=0,depth_mode=0,origin_mode=0,depth_stores=0,precise_ops=0,less_mode=0;
    for(size_t at=5;at<module.size();) {
        const uint32_t count=module[at]>>16,op=module[at]&65535;
        Check(count&&count<=module.size()-at,"Truncated isolated fragment instruction");
        const auto a=module.subspan(at+1,count-1);
        if(op==32&&a.size()==3) {Check(a[0]<module[3],"Out-of-range fragment pointer type");pointer_storage[a[0]]=a[1];}
        if(op==59&&a.size()>=3) {
            Check(a[0]<module[3]&&a[1]<module[3],"Out-of-range fragment variable");value_storage[a[1]]=pointer_storage[a[0]];
            if(a[2]==3) {Check(!output,"Unexpected additional fragment output");output=a[1];}
        }
        if((op==55||op==65||op==66||op==67||op==70||op==83||op==120||op==245)&&a.size()>=2) {
            Check(a[0]<module[3]&&a[1]<module[3],"Out-of-range fragment pointer producer");
            value_storage[a[1]]=pointer_storage[a[0]];
        }
        if(op==71&&a.size()>=2) {
            if(a[1]==11&&a.size()==3&&a[2]==22) {Check(!depth_builtin,"Duplicate FragDepth builtin");depth_builtin=a[0];}
            if(a[1]==42)++precise_ops; // NoContraction.
        }
        if(op==16) {
            Check(a.size()==2&&(a[1]==7||a[1]==12||(conservative&&a[1]==15)),"Unexpected early/conservative/interlock fragment execution mode");
            depth_mode+=a[1]==12;origin_mode+=a[1]==7;less_mode+=a[1]==15;
        }
        if(op==331)Check(false,"Unexpected execution-mode-id in fragment");
        if(op==62) {
            Check(a.size()>=2&&a[0]<module[3],"Invalid fragment store");
            if(a[0]==output)++depth_stores;
            else Check(value_storage[a[0]]==7,"Unexpected nonlocal fragment store");
        }
        Check(op!=99&&!(op>=227&&op<=242)&&op!=5364&&op!=5365&&op!=252&&op!=5370,
            "Unexpected image/atomic/interlock/discard instruction in depth fragment");
        at+=count;
    }
    Check(output&&output==depth_builtin&&depth_stores==1&&depth_mode==1&&origin_mode==1&&precise_ops>=2&&less_mode==unsigned(conservative),
        "Actual fragment does not preserve one late FragDepth output and precise bias operations");
}
void Upload(RenderBuffer& b,const void* data,size_t bytes) {
    void* p=b.map();Check(p,"Readonly D32 upload mapping failed");std::memcpy(p,data,bytes);
    auto& v=static_cast<VulkanBuffer&>(b);
    Check(vmaFlushAllocation(v.device->allocator,v.allocation,0,VK_WHOLE_SIZE)==VK_SUCCESS,"Readonly D32 upload flush failed");b.unmap();
}
uint32_t At(uint32_t x,uint32_t y,uint32_t pitch,uint32_t base) {
    const auto column=x%80<40?x%80+40:x%80-40;
    return ((base+(y/16)*((pitch+79)/80)+x/80)*1280+(y%16)*80+column)%(2048*1280);
}
double Decode(uint32_t code) {
    const uint32_t e=code>>20,m=code&0xfffff;
    return std::ldexp(double(e?m+0x100000:m),e?int(e)-35:-34);
}
uint32_t Quantize(float z) {
    double value=std::clamp(double(z),0.,1.);
    if(value<0x1p-14)value=std::floor(value*0x1p37)*0x1p-37;
    uint32_t first=0,last=0xffffff;
    while(first<last){auto mid=first+(last-first)/2;if(Decode(mid)<value)first=mid+1;else last=mid;}
    if(!first)return 0;
    const double hi=Decode(first)-value,lo=value-Decode(first-1);
    return hi<lo?first:lo<hi?first-1:(first&1)?first-1:first;
}
// All command/descriptor/image/buffer owners are declared before this latch.
// On an accepted-submit or wait failure the fixture terminates before any
// owner can unwind; it never releases resources on an unproven fence.
struct Accepted {
    uint32_t pending=0;
    ~Accepted(){if(pending)std::terminate();}
    void Submit(RenderCommandQueue& q,std::span<const VkCommandBuffer> commands,RenderCommandFence& f) {
        auto& queue=static_cast<VulkanCommandQueue&>(q);auto& fence=static_cast<VulkanCommandFence&>(f);
        VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};info.commandBufferCount=uint32_t(commands.size());info.pCommandBuffers=commands.data();
        ++pending;std::scoped_lock lock(*queue.queue->mutex);
        if(vkQueueSubmit(queue.queue->vk,1,&info,fence.vk)!=VK_SUCCESS)std::terminate();
    }
    void Wait(RenderDevice& d,RenderCommandFence& f) {
        auto& device=static_cast<VulkanDevice&>(d);auto& fence=static_cast<VulkanCommandFence&>(f);
        if(vkWaitForFences(device.vk,1,&fence.vk,VK_TRUE,UINT64_MAX)!=VK_SUCCESS)std::terminate();
        Check(pending!=0,"Readonly D32 fixture completion order failed");--pending;
    }
};
Words ReadBuffer(RenderDevice& device,RenderCommandQueue& queue,RenderBuffer& source) {
    auto out=device.createBuffer(RenderBufferDesc::ReadbackBuffer(kBytes));
    auto commands=queue.createCommandList();auto fence=device.createCommandFence();commands->begin();
    BufferDependency(*commands,source,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_READ_BIT);
    commands->copyBuffer(out.get(),&source);
    BufferDependency(*commands,*out,VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_HOST_READ_BIT);commands->end();Accepted accepted;
    const auto command=static_cast<VulkanCommandList&>(*commands).vk;
    accepted.Submit(queue,{&command,1},*fence);accepted.Wait(device,*fence);
    auto& native=static_cast<VulkanBuffer&>(*out);auto p=out->map();Check(p,"Readonly D32 EDRAM read mapping failed");
    Check(vmaInvalidateAllocation(native.device->allocator,native.allocation,0,VK_WHOLE_SIZE)==VK_SUCCESS,"Readonly D32 read invalidate failed");
    Words result(kBytes/4);std::memcpy(result.data(),p,kBytes);out->unmap();return result;
}
struct Generation {
    std::unique_ptr<RenderTexture> image;
    std::unique_ptr<RenderFramebuffer> framebuffer;
    std::unique_ptr<RenderBuffer> scratch;
    std::unique_ptr<RenderDescriptorSet> descriptor;
    std::unique_ptr<RenderCommandList> conversion;
};
struct TimestampPool {
    VkDevice device=VK_NULL_HANDLE;VkQueryPool pool=VK_NULL_HANDLE;
    ~TimestampPool(){if(pool)vkDestroyQueryPool(device,pool,nullptr);}
};
struct Outcome {Words edram;std::vector<uint64_t> samples;double query_gpu_ms=0;};
Outcome Run(RenderDevice& device,RenderCommandQueue& queue,std::span<const uint32_t> original,
    std::span<const uint32_t> vertex,uint32_t capacity,uint32_t width,uint32_t height,uint32_t base,bool native_depth,
    std::span<const uint32_t> alternate_fragment={},bool boundary_cases=false,bool read_only_pass=false,
    bool lower_bound_query=false,bool time_queries=false,bool coherent_depth=false) {
    const uint32_t pitch=(width+79)/80*80,half=capacity/2;
    TimestampPool timestamps;uint32_t valid_bits=0;double period=0;
    if(time_queries) {
        auto& vk=static_cast<VulkanDevice&>(device);
        uint32_t count=0;vkGetPhysicalDeviceQueueFamilyProperties(vk.physicalDevice,&count,nullptr);
        std::vector<VkQueueFamilyProperties> families(count);vkGetPhysicalDeviceQueueFamilyProperties(vk.physicalDevice,&count,families.data());
        const auto family=static_cast<VulkanCommandQueue&>(queue).familyIndex;
        Check(family<count&&families[family].timestampValidBits,"Query fixture timestamps unavailable");
        valid_bits=families[family].timestampValidBits;period=vk.physicalDeviceProperties.limits.timestampPeriod;
        Check(valid_bits<=64&&period>0&&std::isfinite(period),"Invalid query timestamp properties");
        timestamps.device=vk.vk;VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        info.queryType=VK_QUERY_TYPE_TIMESTAMP;info.queryCount=4;
        Check(vkCreateQueryPool(vk.vk,&info,nullptr,&timestamps.pool)==VK_SUCCESS,"Query timestamp allocation failed");
    }
    NativeFsiConstants gate;gate.pitch=pitch;gate.depth_base=base;gate.depth_format=1;gate.depth_control=0x700762;
    Check(NativeReadonlyDepthAttachmentEligible(gate,width,height,0,false,true,true,0,1,pitch),"Prototype layout is outside readonly admission");
    auto storage=device.createBuffer(RenderBufferDesc::DefaultBuffer(kBytes,RenderBufferFlag::STORAGE|RenderBufferFlag::UNORDERED_ACCESS));
    std::array<Words,2> initial;
    const std::array<uint32_t,20> edges{0,1,2,3,0x0ffffe,0x0fffff,0x100000,0x100001,0xcfffff,0xd00000,
        0xdfffff,0xe00000,0xe00001,0xefffff,0xf00000,0xf00001,0xfffffc,0xfffffd,0xfffffe,0xffffff};
    for(unsigned generation=0;generation<2;++generation) {
        auto& words=initial[generation];words.resize(kBytes/4);
        for(size_t i=0;i<words.size();++i) {
            const uint32_t code=i%3?uint32_t(i*1664525+generation*1013904223)&0xffffff:edges[(i/3+generation)%edges.size()];
            words[i]=(code<<8)|uint32_t((i*37+generation*11)&255);
        }
        if(coherent_depth)for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<width;++x) {
            auto& word=words[At(x,y,pitch,base)];
            const uint32_t code=((x/80+y/16+generation)%4)?0xe80000u:0xd80000u;
            word=(code<<8)|(word&255u);
        }
    }
    // The first real draw has z0 and an inset scissor: guarantee every tested
    // sample there has strictly positive stored depth, hence actual count0.
    // Keep a real zero-depth pixel outside that scissor and every other
    // exponent/edge/stencil/random value unchanged for decode/GE coverage.
    for(auto& words:initial) {
        for(uint32_t y=3;y<height-2;++y)for(uint32_t x=7;x<width-5;++x) {
            auto& word=words[At(x,y,pitch,base)];if(!(word>>8))word|=1u<<8;
        }
        words[At(0,0,pitch,base)]&=255u;
    }
    auto vs=device.createShader(vertex.data(),vertex.size_bytes(),"main",RenderShaderFormat::SPIRV);
    auto baseline=device.createShader(original.data(),original.size_bytes(),"main",RenderShaderFormat::SPIRV);
    const auto fragment=alternate_fragment.empty()?std::span<const uint32_t>(readonly_depth_attachment_frag_spv):alternate_fragment;
    auto native=device.createShader(fragment.data(),fragment.size_bytes(),"main",RenderShaderFormat::SPIRV);
    const RenderDescriptorRange edram{RenderDescriptorRangeType::READ_WRITE_BYTE_ADDRESS_BUFFER,0,1};
    const std::array<RenderDescriptorSetDesc,5> sets{{{}, {}, {}, {}, {&edram,1}}};
    const RenderPushConstantRange push(0,0,0,sizeof(ShaderPushConstants),RenderShaderStageFlag::VERTEX|RenderShaderStageFlag::PIXEL);
    auto layout=device.createPipelineLayout(RenderPipelineLayoutDesc(&push,1,sets.data(),sets.size()));
    auto descriptor=device.createDescriptorSet(sets[4]);descriptor->setBuffer(0,storage.get());
    const RenderSpecConstant specialization(0,0);
    RenderGraphicsPipelineDesc pd;pd.specConstants=&specialization;pd.specConstantsCount=1;pd.pipelineLayout=layout.get();pd.vertexShader=vs.get();pd.pixelShader=baseline.get();
    pd.depthClipEnabled=true;pd.cullMode=RenderCullMode::NONE;pd.frontFace=RenderFrontFace::COUNTER_CLOCKWISE;
    pd.renderTargetCount=1;pd.renderTargetFormat[0]=RenderFormat::R8G8B8A8_UNORM;
    pd.renderTargetBlend[0]=RenderBlendDesc::Copy();pd.renderTargetBlend[0].renderTargetWriteMask=0;
    auto baseline_pipeline=device.createGraphicsPipeline(pd);
    pd.renderTargetCount=0;pd.depthTargetFormat=RenderFormat::D32_FLOAT;pd.pixelShader=native.get();
    pd.depthEnabled=true;pd.depthWriteEnabled=false;pd.depthFunction=RenderComparisonFunction::GREATER_EQUAL;
    auto native_pipeline=device.createGraphicsPipeline(pd);
    Check(baseline_pipeline&&native_pipeline&&static_cast<VulkanGraphicsPipeline&>(*baseline_pipeline).vk&&
        static_cast<VulkanGraphicsPipeline&>(*native_pipeline).vk,"Readonly D32 pipeline allocation failed");
    auto dummy=device.createTexture(RenderTextureDesc::ColorTarget(width,height,RenderFormat::R8G8B8A8_UNORM));
    const RenderTexture* dummy_ptr=dummy.get();RenderFramebufferDesc fd;fd.colorAttachments=&dummy_ptr;fd.colorAttachmentsCount=1;
    auto baseline_framebuffer=device.createFramebuffer(fd);
    Check(baseline_framebuffer&&static_cast<VulkanFramebuffer&>(*baseline_framebuffer).vk,"Readonly baseline framebuffer creation failed");
    const std::array<RenderDescriptorRange,2> ranges{{{RenderDescriptorRangeType::READ_WRITE_BYTE_ADDRESS_BUFFER,0,1},
        {RenderDescriptorRangeType::READ_WRITE_BYTE_ADDRESS_BUFFER,1,1}}};
    const RenderDescriptorSetDesc conversion_set(ranges.data(),ranges.size());
    const RenderPushConstantRange conversion_push(0,0,0,16,RenderShaderStageFlag::COMPUTE);
    auto conversion_layout=device.createPipelineLayout(RenderPipelineLayoutDesc(&conversion_push,1,&conversion_set,1));
    const auto conversion=lower_bound_query?std::span<const uint32_t>(early_depth_lower_bound_comp_spv):
        std::span<const uint32_t>(readonly_depth_attachment_comp_spv);
    auto compute=device.createShader(conversion.data(),conversion.size_bytes(),"main",RenderShaderFormat::SPIRV);
    auto conversion_pipeline=device.createComputePipeline(RenderComputePipelineDesc(conversion_layout.get(),compute.get(),8,8,1));
    Check(conversion_pipeline&&static_cast<VulkanComputePipeline&>(*conversion_pipeline).vk,"Readonly D32 conversion pipeline allocation failed");
    std::array<Generation,2> generations;
    if(native_depth)for(auto& g:generations) {
        g.image=device.createTexture(RenderTextureDesc::DepthTarget(width,height,RenderFormat::D32_FLOAT));
        Check(g.image&&static_cast<VulkanTexture&>(*g.image).vk,"Readonly D32 image allocation failed");
        RenderFramebufferDesc frame;frame.depthAttachment=g.image.get();frame.depthAttachmentReadOnly=read_only_pass;
        frame.depthAttachmentPreserveReadOnly=read_only_pass;
        g.framebuffer=device.createFramebuffer(frame);
        Check(g.framebuffer&&static_cast<VulkanFramebuffer&>(*g.framebuffer).vk,"Readonly D32 framebuffer creation failed");
        g.scratch=device.createBuffer(RenderBufferDesc::DefaultBuffer(uint64_t(width)*height*4,RenderBufferFlag::STORAGE|RenderBufferFlag::UNORDERED_ACCESS));
        g.descriptor=device.createDescriptorSet(conversion_set);g.descriptor->setBuffer(0,storage.get());g.descriptor->setBuffer(1,g.scratch.get());
        g.conversion=queue.createCommandList();auto& command=*g.conversion;command.begin();
        BufferDependency(command,*storage,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_READ_BIT);
        command.barriers(RenderBarrierStage::COMPUTE,RenderBufferBarrier(g.scratch.get(),RenderBufferAccess::WRITE));
        const std::array<uint32_t,4> parameters{width,height,pitch,base};
        command.setComputePipelineLayout(conversion_layout.get());command.setComputeDescriptorSet(g.descriptor.get(),0);
        command.setComputePushConstants(0,parameters.data());command.setPipeline(conversion_pipeline.get());
        command.dispatch((width+7)/8,(height+7)/8,1);
        BufferDependency(command,*g.scratch,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_READ_BIT);
        command.barriers(RenderBarrierStage::COPY,RenderTextureBarrier(g.image.get(),RenderTextureLayout::COPY_DEST));
        VkBufferImageCopy region{};region.imageSubresource={VK_IMAGE_ASPECT_DEPTH_BIT,0,0,1};region.imageExtent={width,height,1};
        vkCmdCopyBufferToImage(static_cast<VulkanCommandList&>(command).vk,static_cast<VulkanBuffer&>(*g.scratch).vk,
            static_cast<VulkanTexture&>(*g.image).vk,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&region);
        command.barriers(RenderBarrierStage::GRAPHICS,RenderTextureBarrier(g.image.get(),read_only_pass?RenderTextureLayout::DEPTH_READ:RenderTextureLayout::DEPTH_WRITE));command.end();
    }
    std::array<std::unique_ptr<RenderBuffer>,2> uploads;
    std::array<std::unique_ptr<RenderCommandList>,2> mutations;
    for(unsigned i=0;i<2;++i) {
        uploads[i]=device.createBuffer(RenderBufferDesc::UploadBuffer(kBytes));Upload(*uploads[i],initial[i].data(),kBytes);
        mutations[i]=queue.createCommandList();mutations[i]->begin();
        BufferDependency(*mutations[i],*storage,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);
        mutations[i]->copyBuffer(storage.get(),uploads[i].get());mutations[i]->end();
    }
    struct Slot {std::unique_ptr<RenderBuffer> input,constants;std::unique_ptr<RenderCommandList> commands;};
    std::vector<Slot> slots;slots.reserve(capacity);NativeQueryTracker queries(device,capacity);
    const std::array<float,8> depths{0.f,0x1p-34f,0x1p-14f,.125f,std::nextafter(.5f,0.f),.5f,std::nextafter(.5f,1.f),1.f};
    // Opt-in candidate cases exercise the first positive code after the guest's
    // subnormal guard truncation, signed zero, FP24 ties, and the normal boundary.
    // These are actual flat triangle depths, not fragment-only substitute input.
    const float first_positive=5.f*0x1p-37f;
    const std::array<float,16> boundaries{0.f,-0.f,std::numeric_limits<float>::denorm_min(),
        std::nextafter(first_positive,0.f),first_positive,std::nextafter(first_positive,1.f),
        0x1p-35f,std::nextafter(0x1p-35f,1.f),0x1p-34f,
        std::nextafter(0x1p-14f,0.f),0x1p-14f,std::nextafter(0x1p-14f,1.f),
        std::nextafter(.5f,0.f),.5f,std::nextafter(.5f,1.f),1.f};
    std::vector<uint64_t> expected;
    std::array<std::vector<VkCommandBuffer>,2> batches;
    for(unsigned i=0;i<capacity;++i) {
        const unsigned generation=i<half?0:1;if(i%half==0) {
            batches[generation].push_back(static_cast<VulkanCommandList&>(*mutations[generation]).vk);
            if(native_depth)batches[generation].push_back(static_cast<VulkanCommandList&>(*generations[generation].conversion).vk);
        }
        const float z=boundary_cases?boundaries[i%boundaries.size()]:depths[i%depths.size()];const auto quantized=Quantize(z);
        const RenderRect scissor=i%3?RenderRect(0,0,width,height):RenderRect(7,3,width-5,height-2);
        uint64_t wanted=0;for(int y=scissor.top;y<scissor.bottom;++y)for(int x=scissor.left;x<scissor.right;++x)
            wanted+=quantized>=(initial[generation][At(x,y,pitch,base)]>>8);
        expected.push_back(wanted);
        std::array<uint8_t,256> inputs{};
        const std::array<uint8_t,4> indices{0,1,2,0};std::memcpy(inputs.data(),indices.data(),indices.size());
        const std::array<float,6> transform{0,0,0,1,1,1};std::memcpy(inputs.data()+16,transform.data(),sizeof(transform));
        const std::array<float,9> positions{-1,-1,z,3,-1,z,-1,3,z};std::memcpy(inputs.data()+64,positions.data(),sizeof(positions));
        Slot slot;slot.input=device.createBuffer(RenderBufferDesc::UploadBuffer(inputs.size(),RenderBufferFlag::STORAGE|RenderBufferFlag::DEVICE_ADDRESSABLE));
        Upload(*slot.input,inputs.data(),inputs.size());NativeFsiShaderConstants constants;constants.raster=gate;
        if(boundary_cases) {
            const float zero=(i&1)?-0.f:0.f;
            constants.raster.front_slope_bias=constants.raster.back_slope_bias=zero;
            constants.raster.front_constant_bias=constants.raster.back_constant_bias=zero;
        }
        const uint64_t input=slot.input->getDeviceAddress();constants.shared.vertex_streams[93]={input,4,0};
        constants.shared.vertex_streams[94]={input+16,sizeof(transform),0};constants.shared.vertex_streams[95]={input+64,sizeof(positions),0};
        std::array<uint8_t,8192+sizeof(constants)> packed{};
        for(unsigned axis=0;axis<4;++axis){const float one=1;std::memcpy(packed.data()+axis*20,&one,4);}
        std::memcpy(packed.data()+8192,&constants,sizeof(constants));
        slot.constants=device.createBuffer(RenderBufferDesc::UploadBuffer(packed.size(),RenderBufferFlag::CONSTANT|RenderBufferFlag::DEVICE_ADDRESSABLE));
        Upload(*slot.constants,packed.data(),packed.size());const uint64_t address=slot.constants->getDeviceAddress();
        const ShaderPushConstants addresses{address,address+4096,address+8192};
        slot.commands=queue.createCommandList();auto& command=*slot.commands;command.begin();
        if(native_depth) {
            // LOAD+STORE pass reuse has depth/stencil attachment access even
            // with shader depth writes disabled. Match live Draw's preserved
            // dependency before every physical pass, including same-layout use.
            command.barriers(RenderBarrierStage::GRAPHICS,
                RenderTextureBarrier(generations[generation].image.get(),read_only_pass?RenderTextureLayout::DEPTH_READ:RenderTextureLayout::DEPTH_WRITE));
            // The lower-bound candidate still reads exact packed depth. Its
            // frozen D32 image is only a conservative rejection aid.
            if(lower_bound_query)BufferDependency(command,*storage,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_ACCESS_SHADER_READ_BIT);
        } else {
            BufferDependency(command,*storage,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT);
            command.barriers(RenderBarrierStage::GRAPHICS,RenderTextureBarrier(dummy.get(),RenderTextureLayout::COLOR_WRITE));
        }
        if(time_queries&&i%half==0) {
            auto vk=static_cast<VulkanCommandList&>(command).vk;
            vkCmdResetQueryPool(vk,timestamps.pool,generation*2,2);
            vkCmdWriteTimestamp(vk,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,timestamps.pool,generation*2);
        }
        command.setFramebuffer(native_depth?generations[generation].framebuffer.get():baseline_framebuffer.get());
        command.setViewports(RenderViewport(0,0,width,height));command.setScissors(scissor);
        command.setGraphicsPipelineLayout(layout.get());command.setGraphicsPushConstants(0,&addresses);
        command.setGraphicsDescriptorSet(descriptor.get(),4);command.setPipeline(native_depth?native_pipeline.get():baseline_pipeline.get());
        const uint32_t report=0x2000+64*i;queries.Begin(i+1,report+32);queries.ResetDraw(command);queries.BeginDraw(command);
        command.drawInstanced(3,1,0,0);queries.EndDraw(command);
        if(time_queries&&i%half==half-1) {
            auto& vk=static_cast<VulkanCommandList&>(command);vk.endActiveRenderPass();
            vkCmdWriteTimestamp(vk.vk,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,timestamps.pool,generation*2+1);
        }
        command.end();queries.EndDeferred(i+1,report);
        batches[generation].push_back(static_cast<VulkanCommandList&>(command).vk);slots.push_back(std::move(slot));
    }
    Check(!expected.empty()&&expected.front()==0&&std::any_of(expected.begin(),expected.end(),[](auto n){return n>0;}),
        "Fixture must contain actual fully-occluded and positive real depth queries");
    bool unavailable=false;
    try{(void)queries.TakeDeferredResults();}catch(const std::logic_error&){unavailable=true;}catch(const std::runtime_error&){unavailable=true;}
    Check(unavailable,"Unfenced readonly query result escaped before actual submission");
    auto first=device.createCommandFence(),second=device.createCommandFence();Accepted accepted;
    accepted.Submit(queue,batches[0],*first);accepted.Submit(queue,batches[1],*second);
    Outcome result;accepted.Wait(device,*first);queries.CompletePrefix(half);
    for(const auto& q:queries.TakeDeferredPrefix(half)) {
        const uint32_t i=uint32_t(result.samples.size());
        Check(q.handle==i+1&&q.begin_address==0x2000+64*i+32&&q.end_address==0x2000+64*i&&q.draws==1,
            "D32 first prefix changed logical query identity");result.samples.push_back(q.samples);
    }
    Check(queries.pending_draws()==half,"D32 first fence retired a newer query token");
    accepted.Wait(device,*second);queries.CompletePrefix(half);
    for(const auto& q:queries.TakeDeferredResults()) {
        const uint32_t i=uint32_t(result.samples.size());
        Check(q.handle==i+1&&q.begin_address==0x2000+64*i+32&&q.end_address==0x2000+64*i&&q.draws==1,
            "D32 second prefix changed logical query identity");result.samples.push_back(q.samples);
    }
    Check(result.samples.size()==capacity&&queries.pending_draws()==0,"D32 query prefix lost actual records/tokens");
    Check(result.samples.front()==0,"Fully occluded actual query did not return0");
    Check(result.samples==expected,"Actual D32 query counts differ from independent quantized GE/pixel oracle");
    if(time_queries) {
        std::array<uint64_t,4> ticks{};
        Check(vkGetQueryPoolResults(timestamps.device,timestamps.pool,0,4,sizeof(ticks),ticks.data(),sizeof(uint64_t),
            VK_QUERY_RESULT_64_BIT)==VK_SUCCESS,"Completed query timestamps unavailable");
        const auto mask=valid_bits==64?UINT64_MAX:(uint64_t(1)<<valid_bits)-1;
        for(unsigned generation=0;generation<2;++generation)
            result.query_gpu_ms+=double((ticks[generation*2+1]-ticks[generation*2])&mask)*period/1e6;
    }
    const auto [min_count,max_count]=std::minmax_element(result.samples.begin(),result.samples.end());
    std::cout<<"READONLY_D32_QUERY_COVERAGE native="<<native_depth<<" capacity="<<capacity
        <<" width="<<width<<" height="<<height<<" base="<<base<<" conversions="<<(native_depth?2:0)
        <<" depth_less="<<(!alternate_fragment.empty()&&!lower_bound_query)<<" lower_bound_query="<<lower_bound_query<<" boundary_cases="<<boundary_cases<<" coherent_depth="<<coherent_depth
        <<" real_queries="<<result.samples.size()<<" first="<<result.samples.front()
        <<" min="<<*min_count<<" max="<<*max_count
        <<" zero="<<std::count(result.samples.begin(),result.samples.end(),0)
        <<" positive="<<std::count_if(result.samples.begin(),result.samples.end(),[](auto n){return n>0;})
        <<" timed="<<time_queries<<" query_gpu_ms="<<result.query_gpu_ms<<'\n';
    result.edram=ReadBuffer(device,queue,*storage);Check(result.edram==initial[1],"D32 or original readonly query modified any10MiB EDRAM/stencil word");
    if(native_depth)for(unsigned generation=0;generation<2;++generation) {
        auto readback=device.createBuffer(RenderBufferDesc::ReadbackBuffer(uint64_t(width)*height*4));
        auto cmd=queue.createCommandList();auto fence=device.createCommandFence();cmd->begin();
        cmd->barriers(RenderBarrierStage::COPY,RenderTextureBarrier(generations[generation].image.get(),RenderTextureLayout::COPY_SOURCE));
        VkBufferImageCopy region{};region.imageSubresource={VK_IMAGE_ASPECT_DEPTH_BIT,0,0,1};region.imageExtent={width,height,1};
        vkCmdCopyImageToBuffer(static_cast<VulkanCommandList&>(*cmd).vk,static_cast<VulkanTexture&>(*generations[generation].image).vk,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,static_cast<VulkanBuffer&>(*readback).vk,1,&region);BufferDependency(*cmd,*readback,VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_HOST_READ_BIT);cmd->end();
        Accepted copy;const auto command=static_cast<VulkanCommandList&>(*cmd).vk;copy.Submit(queue,{&command,1},*fence);copy.Wait(device,*fence);
        auto& rb=static_cast<VulkanBuffer&>(*readback);auto mapped=readback->map();Check(mapped,"D32 image readback mapping failed");
        Check(vmaInvalidateAllocation(rb.device->allocator,rb.allocation,0,VK_WHOLE_SIZE)==VK_SUCCESS,"D32 image readback invalidation failed");
        for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x) {
            uint32_t bits;std::memcpy(&bits,static_cast<const uint8_t*>(mapped)+(uint64_t(y)*width+x)*4,4);
            const uint32_t packed_word=initial[generation][At(x,y,pitch,base)];
            const float expected_depth=lower_bound_query?NativeEarlyDepthLowerBound(packed_word):float(Decode(packed_word>>8)*.5);
            Check(bits==std::bit_cast<uint32_t>(expected_depth),"D32 conversion/repeated depth queries changed exact decoded image bits");
        }readback->unmap();
    }
    return result;
}
}
int main(int argc,char** argv)try {
    Check(argc==4||(argc==5&&(std::string_view(argv[4])=="--depth-less"||std::string_view(argv[4])=="--read-only-pass"||std::string_view(argv[4])=="--lower-bound-query")),
        "Usage: readonly-depth-attachment-gpu-test original-builtin.spv pinned-vs.spv 64|128 [--depth-less|--read-only-pass|--lower-bound-query]");
    const bool depth_less=argc==5&&std::string_view(argv[4])=="--depth-less";
    const bool lower_bound_query=argc==5&&std::string_view(argv[4])=="--lower-bound-query";
    const bool read_only_pass=lower_bound_query||(argc==5&&std::string_view(argv[4])=="--read-only-pass");
    const auto original=Read(argv[1]),vertex=Read(argv[2]);Check(bool(TryBuiltinReadonlyQuery(original)),"Canonical builtin identity refused");
    Check(AuditedReadonlyQueryVertex(kReadonlyQueryVertexSource,{reinterpret_cast<const uint8_t*>(vertex.data()),vertex.size()*4}),"Actual readonly vertex identity refused");
    Check(std::string_view(argv[3])=="64"||std::string_view(argv[3])=="128","Unsupported query/owner capacity");
    const uint32_t capacity=std::string_view(argv[3])=="64"?64:128;
    CheckLateDepthModule(readonly_depth_attachment_frag_spv);
    const auto hash=[](std::span<const uint32_t> words){return ContentHash({reinterpret_cast<const uint8_t*>(words.data()),words.size_bytes()});};
    std::cout<<"READONLY_D32_MODULE_IDENTITY original="<<hash(original)<<" vertex="<<hash(vertex)
        <<" fragment="<<hash(readonly_depth_attachment_frag_spv)<<" conversion="<<hash(readonly_depth_attachment_comp_spv)<<'\n';
    FsiDevice host(false,false,read_only_pass);auto& device=host.device();auto& vk=static_cast<VulkanDevice&>(device);VkFormatProperties format{};
    Check(!read_only_pass||vk.storeOpNoneSupported,"Requested STORE_OP_NONE was not actually enabled");
    VkPhysicalDeviceProperties properties{};vkGetPhysicalDeviceProperties(vk.physicalDevice,&properties);
    std::cout<<"READONLY_D32_DEVICE vendor_id="<<properties.vendorID<<" device_id="<<properties.deviceID
        <<" type="<<properties.deviceType<<" software="<<(properties.deviceType==VK_PHYSICAL_DEVICE_TYPE_CPU)
        <<" api_version="<<properties.apiVersion<<" driver_version="<<properties.driverVersion
        <<" load_store_op_none_supported="<<vk.loadStoreOpNoneSupported<<" attachment_load_op="<<VK_ATTACHMENT_LOAD_OP_LOAD
        <<" attachment_store_op="<<VK_ATTACHMENT_STORE_OP_STORE<<" framebuffer_readonly=false pipeline_depth_write_enable="<<false
        <<" policy=native-readonly-depth-d32-v2-load-store name="<<properties.deviceName<<'\n';
    vkGetPhysicalDeviceFormatProperties(vk.physicalDevice,VK_FORMAT_D32_SFLOAT,&format);
    const auto required=VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT|VK_FORMAT_FEATURE_TRANSFER_SRC_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    Check((format.optimalTilingFeatures&required)==required,"D32 native depth/copy capability unsupported; production would refuse to ordinary Native FSI");
    auto queue=device.createCommandQueue(RenderCommandListType::DIRECT);
    const std::array<std::array<uint32_t,3>,2> extents{{{64,48,2047},{640,360,1440}}};
    for(const auto& extent:extents) {
        const auto ordinary=Run(device,*queue,original,vertex,capacity,extent[0],extent[1],extent[2],false);
        const auto native=Run(device,*queue,original,vertex,capacity,extent[0],extent[1],extent[2],true);
        Check(ordinary.edram==native.edram&&ordinary.samples==native.samples,"Native readonly D32 differs from original actual query/storage output");
    }
    if(read_only_pass) {
        uint64_t compared_queries=0;
        for(const auto& extent:extents) {
            const auto packed=Run(device,*queue,original,vertex,capacity,extent[0],extent[1],extent[2],false,{},true);
            const auto baseline=Run(device,*queue,original,vertex,capacity,extent[0],extent[1],extent[2],true,{},true);
            const auto candidate=Run(device,*queue,original,vertex,capacity,extent[0],extent[1],extent[2],true,{},true,true);
            Check(packed.edram==baseline.edram&&baseline.edram==candidate.edram&&
                  packed.samples==baseline.samples&&baseline.samples==candidate.samples,
                  "LOAD+STORE_NONE changed exact boundary visibility or full packed memory");
            compared_queries+=candidate.samples.size();
        }
        std::cout<<"READONLY_D32_PRESERVED_PASS capacity="<<capacity<<" real_queries_compared="<<compared_queries
            <<" full_edram_bytes_compared="<<uint64_t(kBytes)*extents.size()
            <<" exact_d32_bits_checked=true load=LOAD store=NONE layout=DEPTH_READ\n";
    }
    if(depth_less) {
        const auto code=TryNativeReadonlyDepthLess(readonly_depth_attachment_frag_spv);
        Check(bool(code),"Pinned conservative readonly fragment refused");CheckLateDepthModule(*code,true);
        uint64_t compared_queries=0,edram_bytes=0,d32_bytes=0;
        for(const auto& extent:extents) {
            const auto packed=Run(device,*queue,original,vertex,capacity,extent[0],extent[1],extent[2],false,{},true);
            const auto baseline=Run(device,*queue,original,vertex,capacity,extent[0],extent[1],extent[2],true,{},true);
            const auto candidate=Run(device,*queue,original,vertex,capacity,extent[0],extent[1],extent[2],true,*code,true);
            Check(packed.edram==baseline.edram&&baseline.edram==candidate.edram&&
                  packed.samples==baseline.samples&&baseline.samples==candidate.samples,
                  "Conservative D32 changes actual boundary query counts or full packed memory");
            compared_queries+=candidate.samples.size();edram_bytes+=kBytes;
            d32_bytes+=uint64_t(extent[0])*extent[1]*4*2;
        }
        std::cout<<"READONLY_D32_DEPTH_LESS_PASS capacity="<<capacity<<" fragment="<<hash(*code)
            <<" original_instructions_preserved=true depth_less_mode=true early_fragment_tests=false"
            <<" real_queries_compared="<<compared_queries<<" full_edram_bytes_compared="<<edram_bytes
            <<" candidate_d32_bytes_checked="<<d32_bytes
            <<" actual_packed_baseline_and_plain_D32=true signed_zero_bias=true pending_prefix_ownership=true\n";
    }
    if(lower_bound_query) {
        // Keep the exact packed comparison and discard, with no FragDepth
        // output, memory writes, interlock or EarlyFragmentTests. Default late
        // query counting must include shader discards. The unchanged original
        // VS and viewport feed a hardware GE test against a full-FP24
        // predecessor. Never substitute the half-range D32 embedding here.
        const auto code=TryBuiltinReadonlyQuery(original);
        Check(bool(code),"Pinned readonly packed query fragment refused");
        uint64_t compared_queries=0;
        for(const auto& extent:extents) {
            const auto packed=Run(device,*queue,original,vertex,capacity,extent[0],extent[1],extent[2],false,{},true,false,false,true);
            const auto baseline=Run(device,*queue,original,vertex,capacity,extent[0],extent[1],extent[2],true,{},true,true,false,true);
            const auto candidate=Run(device,*queue,original,vertex,capacity,extent[0],extent[1],extent[2],true,*code,true,true,true,true);
            Check(packed.edram==baseline.edram&&baseline.edram==candidate.edram&&
                  packed.samples==baseline.samples&&baseline.samples==candidate.samples,
                  "Lower-bound query pretest changed exact late visibility or full packed memory");
            compared_queries+=candidate.samples.size();
            std::cout<<"READONLY_LOWER_BOUND_QUERY_MICROTIMING capacity="<<capacity<<" width="<<extent[0]<<" height="<<extent[1]
                <<" packed_ms="<<packed.query_gpu_ms<<" late_depth_ms="<<baseline.query_gpu_ms
                <<" lower_bound_ms="<<candidate.query_gpu_ms
                <<" excludes_initial_conversion_and_cpu_setup=true synthetic_fullscreen_triangles=true not_live_frame_time=true\n";
        }
        // Spatially coherent occluders exercise hierarchical rejection, unlike
        // the deliberately adversarial random-depth correctness surfaces above.
        // This is still a synthetic stress case, not captured gameplay timing.
        for(unsigned repeat=0;repeat<3;++repeat) {
            const auto baseline=Run(device,*queue,original,vertex,capacity,1280,720,1440,true,{},true,true,false,true,true);
            const auto candidate=Run(device,*queue,original,vertex,capacity,1280,720,1440,true,*code,true,true,true,true,true);
            Check(baseline.edram==candidate.edram&&baseline.samples==candidate.samples,
                "Coherent query pretest changed exact visibility or packed memory");
            compared_queries+=candidate.samples.size();
            std::cout<<"READONLY_LOWER_BOUND_QUERY_COHERENT_MICROTIMING capacity="<<capacity<<" repeat="<<repeat
                <<" width=1280 height=720 late_depth_ms="<<baseline.query_gpu_ms<<" lower_bound_ms="<<candidate.query_gpu_ms
                <<" synthetic=true not_live_frame_time=true\n";
        }
        std::cout<<"READONLY_LOWER_BOUND_QUERY_PASS capacity="<<capacity<<" real_queries_compared="<<compared_queries
            <<" fragment="<<hash(*code)<<" conversion="<<hash(early_depth_lower_bound_comp_spv)
            <<" early_fragment_tests=false frag_depth_output=false exact_packed_discard=true"
            <<" full_edram_bytes_compared="<<uint64_t(kBytes)*extents.size()
            <<" exact_d32_bits_checked=true no_live_integration=true no_performance_claim=true\n";
    }
    std::cout<<"PASS: isolated actual native D32 readonly depth queries; capacity="<<capacity
        <<",pinned original VS/canonicalPS,2converted generations reused across"<<capacity
        <<" real queries, late exact float24 rounding, wrapped/room footprints,scissor/pixel/10MiB oracle,old/new alias FIFO,two real fence prefixes and exact unchanged D32 bits; no live integration or FPS proof\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
