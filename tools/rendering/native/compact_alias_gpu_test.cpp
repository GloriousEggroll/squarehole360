#include "gpu_helpers.h"
#include "renderer/native/resolve_memory.h"
#include "renderer/native/texture_upload.h"
#include "memory_store_spv.h"
#include "memory_load_spv.h"
#include "memory_load_r32_spv.h"
#include "memory_load_rg32_spv.h"
#include "compact_alias_sample_spv.h"
#include <bit>
#include <iostream>
#include <string>
using namespace plume;
using namespace gow3::renderer;
namespace {
void Require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
constexpr uint32_t kSamples=128,kResultBytes=kSamples*16*2;
class SamplerProbe {
    RenderDevice& device_;
    RenderCommandQueue& queue_;
    std::unique_ptr<RenderShader> shader_;
    std::unique_ptr<RenderPipelineLayout> layout_;
    std::unique_ptr<RenderPipeline> pipeline_;
    std::unique_ptr<RenderBuffer> output_,readback_;
    std::unique_ptr<RenderCommandList> commands_;
    std::unique_ptr<RenderCommandFence> fence_;
    std::array<RenderDescriptorRange,3> ranges_{{
        {RenderDescriptorRangeType::TEXTURE,0,1},
        {RenderDescriptorRangeType::SAMPLER,1,1},
        {RenderDescriptorRangeType::READ_WRITE_STRUCTURED_BUFFER,2,1}}};
public:
    SamplerProbe(RenderDevice& device,RenderCommandQueue& queue):device_(device),queue_(queue) {
        shader_=device_.createShader(compact_alias_sample_spv,sizeof(compact_alias_sample_spv),"main",RenderShaderFormat::SPIRV);
        const RenderPushConstantRange push(0,0,0,4,RenderShaderStageFlag::COMPUTE);
        const RenderDescriptorSetDesc descriptor(ranges_.data(),ranges_.size());
        layout_=device_.createPipelineLayout(RenderPipelineLayoutDesc(&push,1,&descriptor,1));
        pipeline_=device_.createComputePipeline(RenderComputePipelineDesc(layout_.get(),shader_.get(),64,1,1));
        Require(pipeline_&&static_cast<VulkanComputePipeline&>(*pipeline_).vk,"Cannot create compact sampling fixture pipeline");
        output_=device_.createBuffer(RenderBufferDesc::DefaultBuffer(kResultBytes,RenderBufferFlag::STORAGE|RenderBufferFlag::UNORDERED_ACCESS));
        readback_=device_.createBuffer(RenderBufferDesc::ReadbackBuffer(kResultBytes));
        commands_=queue_.createCommandList();fence_=device_.createCommandFence();
    }
    void Compare(RenderTexture& original,RenderTexture& compact,const TextureLayout& texture) {
        auto first=BindTexture(device_,original,texture,true),second=BindTexture(device_,compact,texture,true);
        Require(first.properties.signs==second.properties.signs&&first.properties.exponent==second.properties.exponent,
            "Compact binding changed shader signs/exponent");
        std::array<std::unique_ptr<RenderDescriptorSet>,2> descriptors;
        std::array<RenderTexture*,2> images{&original,&compact};
        std::array<TextureBinding*,2> bindings{&first,&second};
        const RenderBufferStructuredView view(16,0);
        for(unsigned i=0;i<2;++i) {
            descriptors[i]=device_.createDescriptorSet(RenderDescriptorSetDesc(ranges_.data(),ranges_.size()));
            descriptors[i]->setTexture(0,images[i],RenderTextureLayout::SHADER_READ,bindings[i]->view.get());
            descriptors[i]->setSampler(1,bindings[i]->sampler.get());
            descriptors[i]->setBuffer(2,output_.get(),kResultBytes,&view);
        }
        commands_->begin();
        commands_->barriers(RenderBarrierStage::COMPUTE,RenderBufferBarrier(output_.get(),RenderBufferAccess::WRITE));
        commands_->setComputePipelineLayout(layout_.get());commands_->setPipeline(pipeline_.get());
        for(uint32_t i=0;i<2;++i) {
            commands_->barriers(RenderBarrierStage::COMPUTE,RenderTextureBarrier(images[i],RenderTextureLayout::SHADER_READ));
            const uint32_t destination=i*kSamples;
            commands_->setComputePushConstants(0,&destination);commands_->setComputeDescriptorSet(descriptors[i].get(),0);
            commands_->dispatch(2,1,1);
        }
        commands_->barriers(RenderBarrierStage::COPY,RenderBufferBarrier(output_.get(),RenderBufferAccess::READ));
        commands_->copyBuffer(readback_.get(),output_.get());
        VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(static_cast<VulkanCommandList&>(*commands_).vk,VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);
        commands_->end();queue_.executeCommandLists(commands_.get(),fence_.get());queue_.waitForCommandFence(fence_.get());
        auto& buffer=static_cast<VulkanBuffer&>(*readback_);const auto* bytes=static_cast<const uint8_t*>(readback_->map());
        if(vmaInvalidateAllocation(buffer.device->allocator,buffer.allocation,0,VK_WHOLE_SIZE)!=VK_SUCCESS)
            throw std::runtime_error("Cannot invalidate compact sample readback");
        const bool equal=std::memcmp(bytes,bytes+kResultBytes/2,kResultBytes/2)==0;
        readback_->unmap();Require(equal,"Compact sampling is not bit-identical to RGBA32F");
    }
};
std::vector<uint8_t> Seed(const TextureLayout& layout) {
    std::vector<uint8_t> result(65536,0xcd);
    constexpr std::array<uint16_t,8> normalized{0,1,32767,32768,65534,65535,123,45678};
    constexpr std::array<uint16_t,8> half{0,0x8000,1,0x03ff,0x0400,0x3c00,0xbc00,0x7bff};
    constexpr std::array<uint32_t,8> real{0,0x80000000,1,0x007fffff,0x00800000,0x3eaaaaab,0xc640e600,0x47800001};
    constexpr std::array<uint32_t,8> depth{0,1,0x0fffff,0x100000,0x7fffff,0x800000,0xefffff,0xffffff};
    constexpr std::array<uint8_t,4> swaps{0,1,3,2};
    for(uint32_t y=0;y<layout.height;++y)for(uint32_t x=0;x<layout.width;++x) {
        const auto offset=uint32_t(TextureBlockOffset(layout,layout.levels.front(),x,y,0))+0x1000;
        std::array<uint8_t,16> texel{};
        if(layout.format==22||layout.format==23) {
            const uint32_t word=(depth[(x+y*3)%8]<<8)|((x+y)&255);
            for(unsigned b=0;b<4;++b)texel[b]=uint8_t(word>>(b*8));
        }else for(unsigned channel=0;channel<layout.block.components;++channel) {
            const auto selected=(x+y*3+channel*5)%8;
            const unsigned width=layout.block.bytes/layout.block.components;
            const uint32_t value=width==1?normalized[selected]&255:width==4?real[selected]:
                layout.format>=27&&layout.format<=32?half[selected]:normalized[selected];
            for(unsigned b=0;b<width;++b)texel[channel*width+b]=uint8_t(value>>(b*8));
        }
        for(unsigned b=0;b<layout.block.bytes;++b)result.at((offset+b)^swaps[layout.endian])=texel[b];
    }
    return result;
}
void CompareDecoded(RenderDevice& device,RenderCommandQueue& queue,RenderTexture& full,RenderTexture& compact) {
    const auto format=static_cast<VulkanTexture&>(compact).desc.format;
    const unsigned bytes=format==RenderFormat::R32_FLOAT?4:format==RenderFormat::R32G32_FLOAT?8:16;
    const auto expected=validation::ReadImage(device,queue,full,16),actual=validation::ReadImage(device,queue,compact,bytes);
    Require(actual.size()/bytes==expected.size()/16,"Compact extent changed");
    for(size_t i=0;i<actual.size()/bytes;++i)
        Require(std::memcmp(actual.data()+i*bytes,expected.data()+i*16,bytes)==0,"Decoded float32 component bits changed");
}
}
int main() try {
    auto interface=CreateVulkanInterface();auto device=interface->createDevice();
    Require(bool(device),"Vulkan unavailable");auto queue=device->createCommandQueue(RenderCommandListType::DIRECT);
    // Plume standalone creation enables all supported core features. Borrowed
    // runtime devices must supply actual enabled feature proof instead.
    const auto& vk=static_cast<VulkanDevice&>(*device);VkPhysicalDeviceFeatures enabled{};
    vkGetPhysicalDeviceFeatures(vk.physicalDevice,&enabled);
    ResolveMemory reference(*device,*queue,memory_store_spv,memory_load_spv,65536,1024);
    ResolveMemory candidate(*device,*queue,memory_store_spv,memory_load_spv,65536,1024);
    candidate.EnableCompactLoadFormats(memory_load_r32_spv,memory_load_rg32_spv,enabled.shaderStorageImageExtendedFormats);
    candidate.SetImagePoolEnabled(true);SamplerProbe probe(*device,*queue);
    const auto support=candidate.compact_load_statistics();unsigned checks=0,serial=0;
    for(uint32_t format:{2u,22u,23u,24u,25u,27u,28u,30u,31u,36u,37u,6u,26u,32u,38u}) {
        const uint32_t index=serial++,endian=index%4,signs=(index%3==0?0u:index%3==1?0x55u:0x11u);
        auto texture=DescribeTexture({2u|(1u<<22)|(index%2?1u<<31:0)|(signs<<2),
            0x1000u|format|(endian<<6),16u|(12u<<13),0u,0u,1u<<9});
        const auto source=Seed(texture);reference.Initialize(0,source);candidate.Initialize(0,source);
        auto full=reference.LoadShared(texture,0x1000),compact=candidate.LoadShared(texture,0x1000);
        const auto actual_format=static_cast<VulkanTexture&>(*compact).desc.format;
        const auto expected_format=texture.block.components==1&&support.r32_supported?RenderFormat::R32_FLOAT:
            texture.block.components==2&&support.rg32_supported?RenderFormat::R32G32_FLOAT:RenderFormat::R32G32B32A32_FLOAT;
        Require(actual_format==expected_format,"Source channel count or capability fallback changed");
        CompareDecoded(*device,*queue,*full,*compact);
        for(unsigned filter=0;filter<3;++filter)for(unsigned swizzle=0;swizzle<6;++swizzle) {
            auto sampled=texture;sampled.fetch[3]=0;
            for(unsigned c=0;c<4;++c)sampled.fetch[3]|=((swizzle+c)%6)<<(1+c*3);
            sampled.fetch[3]|=(filter?1u:0u)<<19|(filter?1u:0u)<<21|filter<<23;
            // Alternate wrap/clamp/border; use both black and white borders.
            const uint32_t address=filter==0?0:filter==1?2:6;
            sampled.fetch[0]|=address<<10|address<<13;sampled.fetch[5]|=swizzle&1;
            try{probe.Compare(*full,*compact,sampled);}
            catch(const std::exception& error){throw std::runtime_error("Format "+std::to_string(format)+
                " filter "+std::to_string(filter)+" swizzle "+std::to_string(swizzle)+": "+error.what());}
            ++checks;
        }
    }
    // Missing enabled-feature proof must disable rg32 even on capable hardware.
    ResolveMemory no_extended(*device,*queue,memory_store_spv,memory_load_spv,65536,1024);
    no_extended.EnableCompactLoadFormats(memory_load_r32_spv,memory_load_rg32_spv,false);
    Require(!no_extended.compact_load_statistics().rg32_supported,"Physical support bypassed enabled feature proof");
    auto two=DescribeTexture({2u|(1u<<22),0x1000u|31u,16u|(12u<<13),0u,0u,1u<<9});
    no_extended.Initialize(0,Seed(two));auto fallback=no_extended.LoadShared(two,0x1000);
    Require(static_cast<VulkanTexture&>(*fallback).desc.format==RenderFormat::R32G32B32A32_FLOAT,
        "Missing extended-format capability did not preserve RGBA32F fallback");
    const auto stats=candidate.compact_load_statistics();
    std::cout<<"PASS: "<<checks<<" exact sampled comparisons; r32="<<stats.r32_loads<<" rg32="<<stats.rg32_loads
        <<" rgba32="<<stats.rgba32_loads<<"; odd/tiled/endian/depth/float edge values, swizzles/constants, filters, capability fallback\n";
}catch(const std::exception& error){std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}
