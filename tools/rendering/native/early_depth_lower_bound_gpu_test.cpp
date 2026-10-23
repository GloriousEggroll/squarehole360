#include "renderer/native/early_depth_lower_bound.h"
#include "renderer/native/fsi_device.h"
#include "renderer/native/fsi_state.h"
#include "renderer/native/content_hash.h"
#include "renderer/native/query_tracker.h"
#include "native_early_depth_fixture_pair.h"
#include "early_depth_lower_bound_comp_spv.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>
using namespace plume;
using namespace gow3::renderer;
static_assert(std::endian::native==std::endian::little,"Fixture SPIR-V file reader requires little-endian wire order");
namespace {
using Words=std::vector<uint32_t>;
constexpr uint32_t kBytes=2048u*5120u;
uint64_t checks=0;
void Check(bool condition,const char* message){++checks;if(!condition)throw std::runtime_error(message);}
Words Read(const char* path){
    std::ifstream f(path,std::ios::binary|std::ios::ate);
    Check(f&&f.tellg()>=20&&f.tellg()<=1048576&&f.tellg()%4==0,"Invalid bounded fixture module");
    Words w(size_t(f.tellg())/4);f.seekg(0);
    Check(bool(f.read(reinterpret_cast<char*>(w.data()),w.size()*4)),"Truncated fixture module");return w;
}
std::string Hash(std::span<const uint32_t> w){return ContentHash({reinterpret_cast<const uint8_t*>(w.data()),w.size_bytes()});}
void Upload(RenderBuffer& b,const void* data,size_t bytes){
    auto* mapped=b.map();Check(mapped,"Fixture upload mapping failed");std::memcpy(mapped,data,bytes);
    auto& v=static_cast<VulkanBuffer&>(b);
    Check(vmaFlushAllocation(v.device->allocator,v.allocation,0,VK_WHOLE_SIZE)==VK_SUCCESS,"Fixture upload flush failed");b.unmap();
}
void BufferDependency(RenderCommandList& command,RenderBuffer& buffer,VkPipelineStageFlags stage,VkAccessFlags access){
    VkBufferMemoryBarrier b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    b.srcAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;b.dstAccessMask=access;
    b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    b.buffer=static_cast<VulkanBuffer&>(buffer).vk;b.size=VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(static_cast<VulkanCommandList&>(command).vk,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,stage,0,0,nullptr,1,&b,0,nullptr);
}
// Declare this after every unfenced owner. Submission/wait errors terminate
// before resources can unwind on an unproved fence, matching the existing D32
// isolated fixture. Successful completion permits normal scoped destruction.
struct Accepted {
    uint32_t pending=0;
    ~Accepted(){if(pending)std::terminate();}
    void Submit(RenderCommandQueue& q,std::span<const VkCommandBuffer> commands,RenderCommandFence& f){
        auto& queue=static_cast<VulkanCommandQueue&>(q);auto& fence=static_cast<VulkanCommandFence&>(f);
        VkSubmitInfo s{VK_STRUCTURE_TYPE_SUBMIT_INFO};s.commandBufferCount=uint32_t(commands.size());s.pCommandBuffers=commands.data();
        ++pending;std::scoped_lock lock(*queue.queue->mutex);
        if(vkQueueSubmit(queue.queue->vk,1,&s,fence.vk)!=VK_SUCCESS)std::terminate();
    }
    void Wait(RenderDevice& d,RenderCommandFence& f){
        if(vkWaitForFences(static_cast<VulkanDevice&>(d).vk,1,&static_cast<VulkanCommandFence&>(f).vk,VK_TRUE,UINT64_MAX)!=VK_SUCCESS)std::terminate();
        Check(pending!=0,"Fixture accepted-work conservation failed");--pending;
    }
};
Words Mapped(RenderBuffer& buffer,size_t bytes){
    auto& v=static_cast<VulkanBuffer&>(buffer);auto* mapped=buffer.map();Check(mapped,"Fixture readback map failed");
    Check(vmaInvalidateAllocation(v.device->allocator,v.allocation,0,VK_WHOLE_SIZE)==VK_SUCCESS,"Fixture readback invalidate failed");
    Words words(bytes/4);std::memcpy(words.data(),mapped,bytes);buffer.unmap();return words;
}
Words ReadBuffer(RenderDevice& device,RenderCommandQueue& queue,RenderBuffer& source,size_t bytes){
    auto output=device.createBuffer(RenderBufferDesc::ReadbackBuffer(bytes));auto commands=queue.createCommandList();auto fence=device.createCommandFence();
    commands->begin();BufferDependency(*commands,source,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_READ_BIT);
    commands->copyBuffer(output.get(),&source);BufferDependency(*commands,*output,VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_HOST_READ_BIT);commands->end();
    Accepted accepted;const auto command=static_cast<VulkanCommandList&>(*commands).vk;accepted.Submit(queue,{&command,1},*fence);accepted.Wait(device,*fence);
    return Mapped(*output,bytes);
}
// Numerical oracle uses arithmetic scaling, not the candidate's exponent+112
// bit construction or NativeEarlyDepthLowerBound. Values are exactly float32.
double Decode(uint32_t code){const uint32_t e=code/1048576u,m=code%1048576u;return std::ldexp(double(e?m+1048576u:m),e?int(e)-35:-34);}
uint32_t LowerBits(uint32_t anchor){return std::bit_cast<uint32_t>(float(std::min(1.0,Decode(anchor?anchor-1u:0u))));}
uint32_t Quantize(float z){
    double value=std::clamp(double(z),0.,1.);
    if(value<0x1p-14)value=std::floor(value*0x1p37)*0x1p-37; // Original subnormal guard truncation.
    uint32_t first=0,last=0xffffffu;
    while(first<last){const auto mid=first+(last-first)/2;if(Decode(mid)<value)first=mid+1;else last=mid;}
    if(!first)return 0;
    const double hi=Decode(first)-value,lo=value-Decode(first-1);
    return hi<lo?first:lo<hi?first-1:(first&1)?first-1:first;
}
// Independent tile enumeration makes the exact depth half-column permutation
// explicit; the converter uses pixel division/modulo instead.
uint32_t Word(uint32_t x,uint32_t y,uint32_t pitch,uint32_t base,bool depth){
    const uint32_t tile_y=y/16,tile_x=x/80,row=y-16*tile_y,column=x-80*tile_x;
    const uint32_t physical_column=depth?(column+40)%80:column;
    return (((base+tile_y*((pitch+79)/80)+tile_x)%2048)*1280+row*80+physical_column);
}
Words Seed(){Words result(kBytes/4);uint32_t r=0x94ad5913u;for(auto& w:result){r^=r<<13;r^=r>>17;r^=r<<5;w=r;}return result;}
struct Converted {
    std::unique_ptr<RenderTexture> image;
    std::unique_ptr<RenderFramebuffer> framebuffer;
    std::unique_ptr<RenderBuffer> scratch;
    std::unique_ptr<RenderDescriptorSet> descriptor;
};
class Converter {
    RenderDevice& device_;
    const std::array<RenderDescriptorRange,2> ranges_{{{RenderDescriptorRangeType::READ_WRITE_BYTE_ADDRESS_BUFFER,0,1},
        {RenderDescriptorRangeType::READ_WRITE_BYTE_ADDRESS_BUFFER,1,1}}};
    std::unique_ptr<RenderPipelineLayout> layout_;
    std::unique_ptr<RenderShader> shader_;
    std::unique_ptr<RenderPipeline> pipeline_;
public:
    Converter(RenderDevice& device,std::span<const uint32_t> module):device_(device){
        const RenderDescriptorSetDesc set(ranges_.data(),ranges_.size());const RenderPushConstantRange push(0,0,0,16,RenderShaderStageFlag::COMPUTE);
        layout_=device.createPipelineLayout(RenderPipelineLayoutDesc(&push,1,&set,1));
        shader_=device.createShader(module.data(),module.size_bytes(),"main",RenderShaderFormat::SPIRV);
        pipeline_=device.createComputePipeline(RenderComputePipelineDesc(layout_.get(),shader_.get(),8,8,1));
        Check(layout_&&shader_&&pipeline_&&static_cast<VulkanComputePipeline&>(*pipeline_).vk,"Fixture converter allocation failed");
    }
    Converted Prepare(RenderBuffer& packed,uint32_t width,uint32_t height){
        Converted g;g.image=device_.createTexture(RenderTextureDesc::DepthTarget(width,height,RenderFormat::D32_FLOAT));
        Check(bool(g.image),"Fixture D32 allocation failed");RenderFramebufferDesc f;f.depthAttachment=g.image.get();f.depthAttachmentReadOnly=false;
        g.framebuffer=device_.createFramebuffer(f);
        g.scratch=device_.createBuffer(RenderBufferDesc::DefaultBuffer(uint64_t(width)*height*4,RenderBufferFlag::STORAGE|RenderBufferFlag::UNORDERED_ACCESS));
        g.descriptor=device_.createDescriptorSet(RenderDescriptorSetDesc(ranges_.data(),ranges_.size()));
        Check(g.framebuffer&&g.scratch&&g.descriptor,"Fixture converted owner allocation failed");
        g.descriptor->setBuffer(0,&packed);g.descriptor->setBuffer(1,g.scratch.get());return g;
    }
    void Record(RenderCommandList& command,RenderBuffer& packed,Converted& g,uint32_t width,uint32_t height,uint32_t pitch,uint32_t base){
        BufferDependency(command,packed,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_READ_BIT);
        BufferDependency(command,*g.scratch,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_WRITE_BIT);
        const std::array<uint32_t,4> pc{width,height,pitch,base};
        command.setComputePipelineLayout(layout_.get());command.setComputeDescriptorSet(g.descriptor.get(),0);command.setComputePushConstants(0,pc.data());
        command.setPipeline(pipeline_.get());command.dispatch((width+7)/8,(height+7)/8,1);
        BufferDependency(command,*g.scratch,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_READ_BIT);
        command.barriers(RenderBarrierStage::COPY,RenderTextureBarrier(g.image.get(),RenderTextureLayout::COPY_DEST));
        VkBufferImageCopy region{};region.imageSubresource={VK_IMAGE_ASPECT_DEPTH_BIT,0,0,1};region.imageExtent={width,height,1};
        vkCmdCopyBufferToImage(static_cast<VulkanCommandList&>(command).vk,static_cast<VulkanBuffer&>(*g.scratch).vk,
            static_cast<VulkanTexture&>(*g.image).vk,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&region);
        command.barriers(RenderBarrierStage::GRAPHICS,RenderTextureBarrier(g.image.get(),RenderTextureLayout::DEPTH_WRITE));
    }
};
Words ReadImage(RenderDevice& device,RenderCommandQueue& queue,Converted& g,uint32_t width,uint32_t height){
    const uint64_t bytes=uint64_t(width)*height*4;auto output=device.createBuffer(RenderBufferDesc::ReadbackBuffer(bytes));
    auto command=queue.createCommandList();auto fence=device.createCommandFence();command->begin();
    command->barriers(RenderBarrierStage::COPY,RenderTextureBarrier(g.image.get(),RenderTextureLayout::COPY_SOURCE));
    VkBufferImageCopy region{};region.imageSubresource={VK_IMAGE_ASPECT_DEPTH_BIT,0,0,1};region.imageExtent={width,height,1};
    vkCmdCopyImageToBuffer(static_cast<VulkanCommandList&>(*command).vk,static_cast<VulkanTexture&>(*g.image).vk,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,static_cast<VulkanBuffer&>(*output).vk,1,&region);
    BufferDependency(*command,*output,VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_HOST_READ_BIT);command->end();
    Accepted accepted;const auto vkcommand=static_cast<VulkanCommandList&>(*command).vk;accepted.Submit(queue,{&vkcommand,1},*fence);accepted.Wait(device,*fence);
    return Mapped(*output,bytes);
}
void Exhaustive(RenderDevice& device,RenderCommandQueue& queue,Converter& converter){
    constexpr uint32_t width=1280,height=720,pixels=width*height,count=1u<<24;
    auto packed=device.createBuffer(RenderBufferDesc::DefaultBuffer(kBytes,RenderBufferFlag::STORAGE|RenderBufferFlag::UNORDERED_ACCESS));
    auto upload=device.createBuffer(RenderBufferDesc::UploadBuffer(kBytes));auto g=converter.Prepare(*packed,width,height);
    uint64_t tested=0;
    for(uint32_t first=0;first<count;first+=pixels){
        auto words=Seed();Words expected(pixels);
        for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<width;++x){
            const uint32_t pixel=y*width+x,anchor=std::min(first+pixel,count-1u);
            words[Word(x,y,width,0,true)]=(anchor<<8)|((pixel*37u+(first/pixels)*91u)&255u);
            expected[pixel]=LowerBits(anchor);if(first+pixel<count)++tested;
        }
        Upload(*upload,words.data(),kBytes);auto command=queue.createCommandList();auto fence=device.createCommandFence();command->begin();
        BufferDependency(*command,*packed,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);command->copyBuffer(packed.get(),upload.get());
        converter.Record(*command,*packed,g,width,height,width,0);command->end();
        Accepted accepted;const auto vkcommand=static_cast<VulkanCommandList&>(*command).vk;accepted.Submit(queue,{&vkcommand,1},*fence);accepted.Wait(device,*fence);
        Check(ReadBuffer(device,queue,*g.scratch,uint64_t(pixels)*4)==expected,"Exhaustive converter scratch differs from independent predecessor oracle");
        Check(ReadImage(device,queue,g,width,height)==expected,"Exhaustive D32 image copy differs from independent predecessor oracle");
        Check(ReadBuffer(device,queue,*packed,kBytes)==words,"Exhaustive conversion changed packed depth/stencil/untouched10MiB source");
    }
    Check(tested==count,"Exhaustive GPU anchor coverage count is not16777216");
    std::cout<<"EARLY_DEPTH_EXHAUSTIVE anchors="<<tested<<" chunks="<<(count+pixels-1)/pixels
        <<" scratch_bits_exact=true d32_bits_exact=true full_edram_unchanged=true varied_stencil=true\n";
}
struct Statistics {
    VkDevice device;VkQueryPool pool=VK_NULL_HANDLE;
    Statistics(RenderDevice& d,uint32_t capacity):device(static_cast<VulkanDevice&>(d).vk){
        VkQueryPoolCreateInfo q{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};q.queryType=VK_QUERY_TYPE_PIPELINE_STATISTICS;
        q.queryCount=capacity;q.pipelineStatistics=VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT;
        Check(vkCreateQueryPool(device,&q,nullptr,&pool)==VK_SUCCESS,"Diagnostic fragment-invocation query allocation failed");
    }
    ~Statistics(){if(pool)vkDestroyQueryPool(device,pool,nullptr);}
};
struct MaterialOutcome {Words edram;std::vector<uint64_t> samples,invocations;};
MaterialOutcome Material(RenderDevice& device,RenderCommandQueue& queue,Converter& converter,
    std::span<const uint32_t> vertex,std::span<const uint32_t> baseline,std::span<const uint32_t> early,
    uint32_t capacity,bool reverse,uint32_t mode){
    // mode0 original canonical/noattachment; mode1 unchanged canonical+late
    // hardware filter; mode2 exact early-only module+hardware filter. Queries
    // are DIAGNOSTIC: production admission explicitly refuses active queries.
    constexpr uint32_t width=160,height=32,pitch=160,color_base=720,pixels=width*height;
    const uint32_t half=capacity/2;
    auto packed=device.createBuffer(RenderBufferDesc::DefaultBuffer(kBytes,RenderBufferFlag::STORAGE|RenderBufferFlag::UNORDERED_ACCESS));
    std::array<Words,2> initial,oracle;
    std::array<Words,2> anchor_bits;
    const std::array<uint32_t,8> anchors{0xe00000,0xe00001,0xdfffff,0xf00000,0xf00001,0xffffff,0xefffff,0xd00000};
    for(uint32_t generation=0;generation<2;++generation){
        initial[generation]=Seed();anchor_bits[generation].resize(pixels);
        for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<width;++x){
            const auto d=anchors[((x/20)+(y/8)+generation)%anchors.size()];
            initial[generation][Word(x,y,pitch,0,true)]=(d<<8)|((x*37+y*11+generation*91)&255);
            anchor_bits[generation][y*width+x]=LowerBits(d);
        }
        oracle[generation]=initial[generation];
    }
    auto vs=device.createShader(vertex.data(),vertex.size_bytes(),"main",RenderShaderFormat::SPIRV);
    const auto pixel=mode==2?early:baseline;auto ps=device.createShader(pixel.data(),pixel.size_bytes(),"main",RenderShaderFormat::SPIRV);
    const std::array<RenderDescriptorRange,2> input_ranges{{{RenderDescriptorRangeType::READ_WRITE_BYTE_ADDRESS_BUFFER,0,1},
        {RenderDescriptorRangeType::READ_WRITE_BYTE_ADDRESS_BUFFER,1,1}}};
    const RenderDescriptorRange edram_range{RenderDescriptorRangeType::READ_WRITE_BYTE_ADDRESS_BUFFER,0,1};
    const std::array<RenderDescriptorSetDesc,5> sets{{{input_ranges.data(),input_ranges.size()},{},{},{},{&edram_range,1}}};
    const RenderPushConstantRange push(0,0,0,sizeof(ShaderPushConstants),RenderShaderStageFlag::PIXEL);
    auto layout=device.createPipelineLayout(RenderPipelineLayoutDesc(&push,1,sets.data(),sets.size()));
    auto edram_set=device.createDescriptorSet(sets[4]);edram_set->setBuffer(0,packed.get());
    RenderGraphicsPipelineDesc pd;pd.pipelineLayout=layout.get();pd.vertexShader=vs.get();pd.pixelShader=ps.get();
    pd.depthClipEnabled=true;pd.cullMode=RenderCullMode::NONE;pd.frontFace=RenderFrontFace::COUNTER_CLOCKWISE;
    pd.renderTargetCount=1;pd.renderTargetFormat[0]=RenderFormat::R8G8B8A8_UNORM;
    pd.renderTargetBlend[0]=RenderBlendDesc::Copy();pd.renderTargetBlend[0].renderTargetWriteMask=0;
    if(mode){pd.depthTargetFormat=RenderFormat::D32_FLOAT;pd.depthEnabled=true;pd.depthWriteEnabled=false;pd.depthFunction=RenderComparisonFunction::GREATER_EQUAL;}
    auto pipeline=device.createGraphicsPipeline(pd);Check(pipeline&&static_cast<VulkanGraphicsPipeline&>(*pipeline).vk,"Controlled material pipeline allocation failed");
    auto dummy=device.createTexture(RenderTextureDesc::ColorTarget(width,height,RenderFormat::R8G8B8A8_UNORM));
    const RenderTexture* color=dummy.get();RenderFramebufferDesc fd;fd.colorAttachments=&color;fd.colorAttachmentsCount=1;
    auto ordinary_fb=device.createFramebuffer(fd);std::array<Converted,2> g;
    if(mode)for(auto& generation:g){generation=converter.Prepare(*packed,width,height);RenderFramebufferDesc f=fd;
        f.depthAttachment=generation.image.get();f.depthAttachmentReadOnly=false;generation.framebuffer=device.createFramebuffer(f);}
    struct Slot {std::unique_ptr<RenderBuffer> input,constants;std::unique_ptr<RenderDescriptorSet> set;std::unique_ptr<RenderCommandList> commands;};
    std::vector<Slot> slots;slots.reserve(capacity);
    std::array<std::unique_ptr<RenderBuffer>,2> uploads;
    std::array<std::unique_ptr<RenderCommandList>,2> mutations;
    std::array<std::unique_ptr<RenderBuffer>,2> snapshots;
    std::array<std::unique_ptr<RenderCommandList>,2> snapshot_commands;
    std::array<std::vector<VkCommandBuffer>,2> batches;
    for(uint32_t i=0;i<2;++i){uploads[i]=device.createBuffer(RenderBufferDesc::UploadBuffer(kBytes));Upload(*uploads[i],initial[i].data(),kBytes);
        mutations[i]=queue.createCommandList();mutations[i]->begin();BufferDependency(*mutations[i],*packed,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);
        mutations[i]->copyBuffer(packed.get(),uploads[i].get());if(mode)converter.Record(*mutations[i],*packed,g[i],width,height,pitch,0);
        mutations[i]->end();batches[i].push_back(static_cast<VulkanCommandList&>(*mutations[i]).vk);}
    NativeQueryTracker queries(device,capacity);Statistics statistics(device,capacity);
    std::vector<uint64_t> expected_samples,manual_samples;
    const std::array<float,8> depths{0,.5f,.25f,.75f,.5f,1,1,0};
    const std::array<std::array<float,4>,3> colors{{{.25f,.5f,1,1},{1,.25f,.5f,1},{.5f,1,.25f,1}}};
    const std::array<uint32_t,3> color_words{128u|(256u<<10)|(384u<<20)|(3u<<30),
        384u|(128u<<10)|(256u<<20)|(3u<<30),256u|(384u<<10)|(128u<<20)|(3u<<30)};
    bool stale_overaccept=false;
    for(uint32_t i=0;i<capacity;++i){
        const uint32_t generation=i/half,phase=i%8;const float z=depths[phase],clip=reverse?1.f-z:z;
        const bool write=phase==1||phase==3||phase==5;const uint32_t q=Quantize(z),palette=i%3;
        uint64_t manual=0,hardware=0;
        for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<width;++x){
            auto& d=oracle[generation][Word(x,y,pitch,0,true)];const bool pass=q>=(d>>8);manual+=pass;
            hardware+=z>=std::bit_cast<float>(anchor_bits[generation][y*width+x]);
            if(pass){if(write)d=(q<<8)|(d&255u);oracle[generation][Word(x,y,pitch,color_base,false)]=color_words[palette];}
        }
        Check(hardware>=manual,"Controlled stale threshold would reject a canonical passing fragment");
        stale_overaccept|=hardware>manual&&phase==2;manual_samples.push_back(manual);expected_samples.push_back(mode==2?hardware:manual);
        Slot slot;std::array<float,8> input{};std::copy(colors[palette].begin(),colors[palette].end(),input.begin());input[4]=clip;
        slot.input=device.createBuffer(RenderBufferDesc::UploadBuffer(sizeof(input),RenderBufferFlag::STORAGE));Upload(*slot.input,input.data(),sizeof(input));
        NativeFsiShaderConstants constants;constants.raster.pitch=pitch;constants.raster.depth_base=0;
        constants.raster.depth_control=write?0x700766:0x700762;constants.raster.depth_format=1;
        constants.raster.color[0]={color_base,3,15,0x10001};
        slot.constants=device.createBuffer(RenderBufferDesc::UploadBuffer(sizeof(constants),RenderBufferFlag::CONSTANT|RenderBufferFlag::DEVICE_ADDRESSABLE));
        Upload(*slot.constants,&constants,sizeof(constants));slot.set=device.createDescriptorSet(sets[0]);slot.set->setBuffer(0,packed.get());slot.set->setBuffer(1,slot.input.get());
        const ShaderPushConstants addresses{0,0,slot.constants->getDeviceAddress()};slot.commands=queue.createCommandList();auto& command=*slot.commands;command.begin();
        const auto vkcommand=static_cast<VulkanCommandList&>(command).vk;vkCmdResetQueryPool(vkcommand,statistics.pool,i,1);
        BufferDependency(command,*packed,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT);
        command.barriers(RenderBarrierStage::GRAPHICS,RenderTextureBarrier(dummy.get(),RenderTextureLayout::COLOR_WRITE));
        if(mode)command.barriers(RenderBarrierStage::GRAPHICS,RenderTextureBarrier(g[generation].image.get(),RenderTextureLayout::DEPTH_WRITE));
        command.setFramebuffer(mode?g[generation].framebuffer.get():ordinary_fb.get());
        command.setViewports(RenderViewport(0,0,width,height,reverse?1.f:0.f,reverse?0.f:1.f));command.setScissors(RenderRect(0,0,width,height));
        command.setGraphicsPipelineLayout(layout.get());command.setGraphicsPushConstants(0,&addresses);command.setGraphicsDescriptorSet(slot.set.get(),0);
        command.setGraphicsDescriptorSet(edram_set.get(),4);command.setPipeline(pipeline.get());
        queries.Begin(i+1,0x2000+64*i+32);queries.ResetDraw(command);queries.BeginDraw(command);
        vkCmdBeginQuery(vkcommand,statistics.pool,i,0);command.drawInstanced(3,1,0,0);vkCmdEndQuery(vkcommand,statistics.pool,i);
        queries.EndDraw(command);command.end();queries.EndDeferred(i+1,0x2000+64*i);batches[generation].push_back(vkcommand);slots.push_back(std::move(slot));
    }
    // Observe both generations before the following full source replacement;
    // otherwise a first-generation color error could be erased by the second
    // upload and escape the final full-memory comparison.
    for(uint32_t generation=0;generation<2;++generation){
        snapshots[generation]=device.createBuffer(RenderBufferDesc::ReadbackBuffer(kBytes));
        snapshot_commands[generation]=queue.createCommandList();auto& command=*snapshot_commands[generation];command.begin();
        BufferDependency(command,*packed,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_READ_BIT);
        command.copyBuffer(snapshots[generation].get(),packed.get());
        BufferDependency(command,*snapshots[generation],VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_HOST_READ_BIT);command.end();
        batches[generation].push_back(static_cast<VulkanCommandList&>(command).vk);
    }
    Check(stale_overaccept&&manual_samples.front()==0&&std::any_of(manual_samples.begin(),manual_samples.end(),[](auto n){return n>0;}),
        "Controlled materials must prove actual occlusion, positive survival and stale-anchor overaccept");
    bool refused=false;try{(void)queries.TakeDeferredResults();}catch(const std::logic_error&){refused=true;}catch(const std::runtime_error&){refused=true;}
    Check(refused,"Unfenced controlled query result escaped");
    auto first=device.createCommandFence(),second=device.createCommandFence();Accepted accepted;
    accepted.Submit(queue,batches[0],*first);accepted.Submit(queue,batches[1],*second);MaterialOutcome out;
    const auto consume=[&](std::vector<NativeQueryTracker::Result> results){for(const auto& result:results){const auto i=uint32_t(out.samples.size());
        Check(result.handle==i+1&&result.begin_address==0x2000+64*i+32&&result.end_address==0x2000+64*i&&result.draws==1,
            "Controlled query identity/order changed");out.samples.push_back(result.samples);}};
    accepted.Wait(device,*first);queries.CompletePrefix(half);consume(queries.TakeDeferredPrefix(half));
    Check(Mapped(*snapshots[0],kBytes)==oracle[0],"First controlled generation full10MiB oracle differs before replacement");
    Check(queries.pending_draws()==half,"Older material fence retired newer query owner");
    accepted.Wait(device,*second);queries.CompletePrefix(half);consume(queries.TakeDeferredResults());
    Check(Mapped(*snapshots[1],kBytes)==oracle[1],"Second controlled generation full10MiB oracle differs");
    Check(out.samples==expected_samples&&queries.pending_draws()==0,"Controlled early/late diagnostic samples differ from their respective exact oracles");
    out.invocations.resize(capacity);
    Check(vkGetQueryPoolResults(static_cast<VulkanDevice&>(device).vk,statistics.pool,0,capacity,
        out.invocations.size()*8,out.invocations.data(),8,VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WAIT_BIT)==VK_SUCCESS,
        "Actual diagnostic fragment-invocation results unavailable after both real fences");
    out.edram=ReadBuffer(device,queue,*packed,kBytes);Check(out.edram==oracle[1],"Controlled materials differ from independent full10MiB/depth/stencil/color oracle");
    if(mode)for(uint32_t i=0;i<2;++i)Check(ReadImage(device,queue,g[i],width,height)==anchor_bits[i],"Controlled stale D32 filter was changed by material passes");
    std::cout<<"EARLY_DEPTH_CONTROLLED mode="<<mode<<" reverse="<<reverse<<" capacity="<<capacity
        <<" queries="<<out.samples.size()<<" first_samples="<<out.samples.front()<<" first_fragment_invocations="<<out.invocations.front()
        <<" stale_overaccept=true edram_oracle_exact=true depth_image_immutable="<<bool(mode)<<'\n';return out;
}
}
int main(int argc,char** argv)try{
    Check(argc==2||argc==6,"Usage: early-depth-lower-bound-gpu-test 64|128 OR baseline.spv certified-early.spv vertex.spv authenticated_baseline_sha256 64|128");
    const std::span<const uint32_t> conversion(early_depth_lower_bound_comp_spv);const auto capacity_arg=std::string_view(argv[argc-1]);
    Check(capacity_arg=="64"||capacity_arg=="128","Unsupported controlled owner/query capacity");const uint32_t capacity=capacity_arg=="64"?64:128;
    Words baseline,early,vertex;if(argc==6){baseline=Read(argv[1]);early=Read(argv[2]);vertex=Read(argv[3]);
        Check(fixture::AuditNativeEarlyDepthFixturePair(baseline,early,argv[4]),"Separately audited controlled EarlyFragmentTests-only fixture pair refused");}
    FsiDevice host;auto& device=host.device();auto& vk=static_cast<VulkanDevice&>(device);
    VkPhysicalDeviceProperties properties{};vkGetPhysicalDeviceProperties(vk.physicalDevice,&properties);VkPhysicalDeviceFeatures features{};vkGetPhysicalDeviceFeatures(vk.physicalDevice,&features);
    VkFormatProperties format{};vkGetPhysicalDeviceFormatProperties(vk.physicalDevice,VK_FORMAT_D32_SFLOAT,&format);
    const auto required=VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT|VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
    Check((format.optimalTilingFeatures&required)==required,"D32 depth/copy capability unavailable");
    if(argc==6)Check(features.pipelineStatisticsQuery,"Actual device lacks diagnostic fragment-invocation query capability");
    std::cout<<"EARLY_DEPTH_DEVICE vendor="<<properties.vendorID<<" device="<<properties.deviceID<<" type="<<properties.deviceType
        <<" hardware_required=true software="<<(properties.deviceType==VK_PHYSICAL_DEVICE_TYPE_CPU)<<" api="<<properties.apiVersion<<" driver="<<properties.driverVersion
        <<" load_store_op_none_supported="<<vk.loadStoreOpNoneSupported<<" framebuffer_readonly=false depth_write_enable=false attachment_policy_source=LOAD_STORE"
        <<" pipeline_statistics_supported="<<features.pipelineStatisticsQuery<<" name="<<properties.deviceName<<'\n';
    Check(properties.deviceType!=VK_PHYSICAL_DEVICE_TYPE_CPU,"Hardware GPU required; refusing CPU/software Vulkan renderer before fixture commands");
    std::cout<<"EARLY_DEPTH_MODULES converter="<<Hash(conversion);if(argc==6)std::cout<<" baseline="<<Hash(baseline)<<" early="<<Hash(early)<<" vertex="<<Hash(vertex);std::cout<<'\n';
    auto queue=device.createCommandQueue(RenderCommandListType::DIRECT);Converter converter(device,conversion);Exhaustive(device,*queue,converter);
    if(argc==6)for(bool reverse:{false,true}){
        const auto original=Material(device,*queue,converter,vertex,baseline,early,capacity,reverse,0);
        const auto late=Material(device,*queue,converter,vertex,baseline,early,capacity,reverse,1);
        const auto filtered=Material(device,*queue,converter,vertex,baseline,early,capacity,reverse,2);
        Check(original.edram==late.edram&&original.edram==filtered.edram,"Original/late-filter/early-filter packed words differ");
        Check(original.samples==late.samples,"Unchanged late filter changed canonical post-discard query counts");
        Check(original.invocations.front()>0&&filtered.invocations.front()<original.invocations.front(),
            "Actual fully occluded controlled first draw did not reduce diagnostic fragment invocations");
    }
    std::cout<<"PASS: exhaustive lower-bound converter16777216anchors; capacity="<<capacity<<" controlled_material_pair="<<(argc==6)
        <<"; synthetic controlled inputs, exact packed oracle and actual queries only; no natural capture, production query eligibility or FPS claim\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
