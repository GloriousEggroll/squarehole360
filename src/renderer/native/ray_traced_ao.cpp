#include "ray_traced_ao.h"
#include "render_scale.h"
#include "plume_vulkan.h"
#include "rt_ao_composite_rgb10_a2_spv.h"
#include "rt_ao_composite_rgba16f_spv.h"
#include "rt_ao_composite_rgba8_spv.h"
#include "rt_ao_convert_spv.h"
#include "rt_ao_temporal_spv.h"
#include "rt_ao_trace_spv.h"
#include "rt_shadow_rgb10_a2_spv.h"
#include "rt_shadow_rgba16f_spv.h"
#include "rt_shadow_rgba8_spv.h"
#include "rt_shadow_trace_spv.h"
#include <rex/logging.h>
#include <algorithm>
#include <cstddef>
#include <bit>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace gow3::renderer {
using namespace plume;
namespace {
constexpr uint32_t kOpCapability=17,kOpEntryPoint=15,kOpExecutionMode=16,kOpDecorate=71,kOpVariable=59;
constexpr uint32_t kCapabilityTransformFeedback=53,kExecutionModeXfb=11;
constexpr uint32_t kDecorationBuiltIn=11,kDecorationOffset=35,kDecorationXfbBuffer=36,kDecorationXfbStride=37;
constexpr uint32_t kBuiltInPosition=0,kStorageClassOutput=3;
constexpr uint32_t kNaN=0x7fc00000u;

using Matrix=std::array<std::array<double,4>,4>;
bool Invert(const Matrix& m,Matrix& out) {
    std::array<std::array<double,8>,4> a{};
    for(int i=0;i<4;++i)for(int j=0;j<4;++j) {a[i][j]=m[i][j];a[i][j+4]=i==j;}
    for(int c=0;c<4;++c) {
        int pivot=c;
        for(int r=c+1;r<4;++r)if(std::abs(a[r][c])>std::abs(a[pivot][c]))pivot=r;
        if(std::abs(a[pivot][c])<1e-12)return false;
        std::swap(a[c],a[pivot]);
        const double scale=1/a[c][c];
        for(auto& v:a[c])v*=scale;
        for(int r=0;r<4;++r)if(r!=c) {
            const double f=a[r][c];
            if(f!=0)for(int k=0;k<8;++k)a[r][k]-=f*a[c][k];
        }
    }
    for(int i=0;i<4;++i)for(int j=0;j<4;++j)out[i][j]=a[i][j+4];
    return true;
}
// The guest matrices are row-vector (clip = position * M). Shaders take one
// row per output component, that is the columns of M.
void Columns(const Matrix& m,float (&out)[4][4]) {
    for(int j=0;j<4;++j)for(int i=0;i<4;++i)out[j][i]=float(m[i][j]);
}

struct ConvertPush {float inverse[4][4];uint64_t clip,world;uint32_t count,padding[3];};
struct TracePush {float inverse[4][4];uint32_t size[2],frame,rays;float radius,sky_depth;uint32_t step;float padding;};
struct TemporalPush {float inverse[4][4],reprojection[4][4];uint32_t size[2],history_valid;float sky_depth,max_frames;
    uint32_t flags;float velocity_scale[2];};
// std430 aligns the shader's vec2 to 8 bytes.
static_assert(offsetof(TemporalPush,velocity_scale)==152);
struct CompositePush {uint32_t size[2];float strength,sky_depth;uint32_t debug_view,step,padding[2];};
struct ShadowPush {float inverse[4][4],light[4];uint32_t size[2],frame,rays;float softness,sky_depth;uint32_t step;float max_distance;};
struct ShadowMergePush {uint32_t size[2];int32_t blur_radius;uint32_t debug_view;uint32_t step,padding[3];};
static_assert(sizeof(ConvertPush)==96&&sizeof(TracePush)==96&&sizeof(TemporalPush)==160&&sizeof(CompositePush)==32&&
              sizeof(ShadowPush)==112&&sizeof(ShadowMergePush)==32);

float GuestFloat(std::span<const uint8_t> state,size_t offset) {
    uint32_t bits;
    std::memcpy(&bits,state.data()+offset,4);
    return std::bit_cast<float>((bits>>24)|((bits>>8)&0xff00u)|((bits<<8)&0xff0000u)|(bits<<24));
}
constexpr size_t kPixelConstants=0x1780;  // pixel float constants in a captured device state

void FullBarrier(VkCommandBuffer commands) {
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(commands,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        0,1,&barrier,0,nullptr,0,nullptr);
}
// Returns a borrowed executor image to the layout it was found in.
struct BorrowedLayout {
    RenderTexture* texture;RenderTextureLayout layout;
    explicit BorrowedLayout(RenderTexture* t):texture(t),layout(static_cast<VulkanTexture&>(*t).textureLayout) {}
    void Restore(RenderCommandList& commands) const {
        if(layout==RenderTextureLayout::UNKNOWN||static_cast<VulkanTexture&>(*texture).textureLayout==layout)return;
        commands.barriers(RenderBarrierStage::GRAPHICS|RenderBarrierStage::COMPUTE,RenderTextureBarrier(texture,layout));
    }
};
VkPrimitiveTopology Topology(RenderPrimitiveTopology topology) {
    switch(topology) {
    case RenderPrimitiveTopology::TRIANGLE_LIST:return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case RenderPrimitiveTopology::TRIANGLE_STRIP:return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case RenderPrimitiveTopology::TRIANGLE_FAN:return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    default:return VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;
    }
}
}  // namespace

std::vector<uint32_t> NativeAddPositionTransformFeedback(std::span<const uint32_t> code) {
    if(code.size()<5||code[0]!=0x07230203)return {};
    size_t capabilities_end=0,entry_points_end=0,position_decoration_end=0;
    uint32_t entry=0,position=0;
    bool output=false;
    for(size_t at=5;at<code.size();) {
        const uint32_t count=code[at]>>16,opcode=code[at]&0xffff;
        if(!count||count>code.size()-at)return {};
        if(opcode==kOpCapability)capabilities_end=at+count;
        if(opcode==kOpEntryPoint&&count>=3) {
            if(code[at+1]!=0||entry)return {};  // one vertex entry point
            entry=code[at+2];entry_points_end=at+count;
        }
        if(opcode==kOpDecorate&&count==4&&code[at+2]==kDecorationBuiltIn&&code[at+3]==kBuiltInPosition) {
            position=code[at+1];position_decoration_end=at+count;
        }
        if(opcode==kOpVariable&&count>=4&&position&&code[at+2]==position)output=code[at+3]==kStorageClassOutput;
        at+=count;
    }
    if(!entry||!position||!output||!capabilities_end||!entry_points_end||!position_decoration_end)return {};
    std::vector<uint32_t> result;
    result.reserve(code.size()+16);
    for(size_t at=0;at<code.size();++at) {
        if(at==capabilities_end)result.insert(result.end(),{2u<<16|kOpCapability,kCapabilityTransformFeedback});
        if(at==entry_points_end)result.insert(result.end(),{3u<<16|kOpExecutionMode,entry,kExecutionModeXfb});
        if(at==position_decoration_end)result.insert(result.end(),{
            4u<<16|kOpDecorate,position,kDecorationXfbBuffer,0u,
            4u<<16|kOpDecorate,position,kDecorationXfbStride,16u,
            4u<<16|kOpDecorate,position,kDecorationOffset,0u});
        result.push_back(code[at]);
    }
    return result;
}

struct NativeRayTracedAo::CapturePipeline {
    std::atomic<VkPipeline> pipeline{VK_NULL_HANDLE};
    std::atomic<bool> failed{false};
};
struct NativeRayTracedAo::Job {
    std::shared_ptr<const std::vector<uint32_t>> spirv;
    VkPrimitiveTopology topology;
    VkRenderPass render_pass;
    uint32_t color_attachments;
    VkSampleCountFlagBits samples;
    bool depth;
    std::shared_ptr<CapturePipeline> pipeline;
};
// Everything one frame's recorded passes refer to that may be replaced later.
struct NativeRayTracedAo::FrameLease {
    std::vector<std::unique_ptr<RenderDescriptorSet>> sets;
    std::vector<std::shared_ptr<RenderTexture>> images;
};

NativeRayTracedAo::NativeRayTracedAo(RenderDevice& device,const RenderPipelineLayout& draw_layout):device_(device) {
    auto& vk_device=static_cast<VulkanDevice&>(device);
    vk_=vk_device.vk;
    draw_layout_=static_cast<const VulkanPipelineLayout&>(draw_layout).vk;
    bind_feedback_=reinterpret_cast<PFN_vkCmdBindTransformFeedbackBuffersEXT>(
        vkGetDeviceProcAddr(vk_,"vkCmdBindTransformFeedbackBuffersEXT"));
    begin_feedback_=reinterpret_cast<PFN_vkCmdBeginTransformFeedbackEXT>(
        vkGetDeviceProcAddr(vk_,"vkCmdBeginTransformFeedbackEXT"));
    end_feedback_=reinterpret_cast<PFN_vkCmdEndTransformFeedbackEXT>(
        vkGetDeviceProcAddr(vk_,"vkCmdEndTransformFeedbackEXT"));
    if(!bind_feedback_||!begin_feedback_||!end_feedback_||!vkCmdBuildAccelerationStructuresKHR||
       !vkGetAccelerationStructureBuildSizesKHR||!vkCreateAccelerationStructureKHR||!draw_layout_) {
        REXLOG_WARN("Ray-traced AO unavailable: Vulkan entry points missing");
        return;
    }
    if(!CreatePasses()) {
        REXLOG_WARN("Ray-traced AO unavailable: compute pass creation failed");
        return;
    }
    worker_=std::thread([this]{Worker();});
    supported_=true;
}

// Buffers and acceleration structures, allocated when first enabled.
bool NativeRayTracedAo::EnsureResources() {
    if(resources_ready_)return true;
    if(resources_failed_)return false;
    auto& device=device_;
    auto& vk_device=static_cast<VulkanDevice&>(device_);
    try {
        for(auto& capture:capture_) {
            VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            info.size=uint64_t(kMaxVertices)*16;
            info.usage=VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            VmaAllocationCreateInfo allocation{};
            allocation.usage=VMA_MEMORY_USAGE_GPU_ONLY;
            if(vmaCreateBuffer(vk_device.allocator,&info,&allocation,&capture.buffer,&capture.allocation,nullptr)!=VK_SUCCESS)
                throw std::runtime_error("capture buffer");
            VkBufferDeviceAddressInfo address{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
            address.buffer=capture.buffer;
            capture.address=vkGetBufferDeviceAddress(vk_,&address);
            capture.dirty_vertices=kMovingBase;capture.dirty_moving=kMaxMovingVertices;
        }
        positions_=device.createBuffer(RenderBufferDesc::DefaultBuffer(uint64_t(kMaxVertices)*12,
            RenderBufferFlag::ACCELERATION_STRUCTURE_INPUT|RenderBufferFlag::STORAGE|RenderBufferFlag::UNORDERED_ACCESS));
        if(!positions_)throw std::runtime_error("positions");
        const RenderBottomLevelASMesh mesh(RenderBufferReference(),RenderBufferReference(positions_.get()),
            RenderFormat::UNKNOWN,RenderFormat::R32G32B32_FLOAT,0,kMaxVertices,12,true);
        RenderBottomLevelASBuildInfo blas_info;
        device.setBottomLevelASBuildInfo(blas_info,&mesh,1,false,true);
        blas_capacity_=blas_info.accelerationStructureSize;
        const RenderBottomLevelASMesh moving_mesh(RenderBufferReference(),RenderBufferReference(positions_.get(),uint64_t(kMovingBase)*12),
            RenderFormat::UNKNOWN,RenderFormat::R32G32B32_FLOAT,0,kMaxMovingVertices,12,true);
        RenderBottomLevelASBuildInfo moving_info;
        device.setBottomLevelASBuildInfo(moving_info,&moving_mesh,1,false,true);
        moving_blas_capacity_=moving_info.accelerationStructureSize;
        // Parts 0..kMaxParts-1, then the moving meshes.
        std::array<RenderTopLevelASInstance,kMaxInstances> instances;
        for(uint32_t p=0;p<kMaxInstances;++p) {
            auto& part=p<kMaxParts?scene_parts_[p]:moving_part_;
            const uint64_t capacity=p<kMaxParts?blas_capacity_:moving_blas_capacity_;
            part.buffer=device.createBuffer(RenderBufferDesc::AccelerationStructureBuffer(capacity));
            if(!part.buffer)throw std::runtime_error("bottom-level buffer");
            part.blas=device.createAccelerationStructure(RenderAccelerationStructureDesc(
                RenderAccelerationStructureType::BOTTOM_LEVEL,RenderBufferReference(part.buffer.get()),capacity));
            if(!part.blas)throw std::runtime_error("bottom-level structure");
            instances[p]=RenderTopLevelASInstance(RenderBufferReference(part.buffer.get()),0,0xff,0,true,RenderAffineTransform());
        }
        for(uint32_t n=1;n<=kMaxInstances;++n)device.setTopLevelASBuildInfo(tlas_infos_[n-1],instances.data(),n,true,false);
        const auto& all=tlas_infos_[kMaxInstances-1];
        if(all.instancesBufferData.size()!=kMaxInstances*sizeof(VkAccelerationStructureInstanceKHR))
            throw std::runtime_error("instance layout");
        for(uint32_t p=0;p<kMaxInstances;++p)
            std::memcpy((p<kMaxParts?scene_parts_[p]:moving_part_).instance.data(),
                        all.instancesBufferData.data()+p*sizeof(VkAccelerationStructureInstanceKHR),
                        sizeof(VkAccelerationStructureInstanceKHR));
        tlas_buffer_=device.createBuffer(RenderBufferDesc::AccelerationStructureBuffer(all.accelerationStructureSize));
        if(!tlas_buffer_)throw std::runtime_error("top-level buffer");
        tlas_=device.createAccelerationStructure(RenderAccelerationStructureDesc(
            RenderAccelerationStructureType::TOP_LEVEL,RenderBufferReference(tlas_buffer_.get()),all.accelerationStructureSize));
        if(!tlas_)throw std::runtime_error("acceleration structures");
        for(auto& ring:instances_) {
            ring=device.createBuffer(RenderBufferDesc::UploadBuffer(all.instancesBufferData.size(),
                RenderBufferFlag::ACCELERATION_STRUCTURE_INPUT));
            if(!ring)throw std::runtime_error("instance upload");
        }
        scratch_capacity_=std::max(blas_info.scratchSize,moving_info.scratchSize);
        for(const auto& info:tlas_infos_)scratch_capacity_=std::max(scratch_capacity_,info.scratchSize);
        scratch_=device.createBuffer(RenderBufferDesc::DefaultBuffer(scratch_capacity_,RenderBufferFlag::ACCELERATION_STRUCTURE_SCRATCH));
        if(!scratch_)throw std::runtime_error("scratch");
    } catch(const std::exception& error) {
        REXLOG_WARN("Ray-traced AO unavailable: {} creation failed",error.what());
        resources_failed_=true;
        return false;
    }
    resources_ready_=true;
    REXLOG_INFO("Ray-traced AO ready: {} vertices per frame, acceleration structure {} MB, scratch {} MB",
                kMaxVertices,blas_capacity_>>20,scratch_capacity_>>20);
    return true;
}

NativeRayTracedAo::~NativeRayTracedAo() {
    {
        std::lock_guard lock(jobs_mutex_);
        stopping_=true;jobs_.clear();
    }
    jobs_changed_.notify_all();
    if(worker_.joinable())worker_.join();
    for(auto& [key,pipeline]:pipelines_)
        if(const VkPipeline vk=pipeline->pipeline.load())vkDestroyPipeline(vk_,vk,nullptr);
    for(auto& [key,pass]:render_passes_)vkDestroyRenderPass(vk_,pass,nullptr);
    if(timing_pool_)vkDestroyQueryPool(vk_,timing_pool_,nullptr);
    auto& vk_device=static_cast<VulkanDevice&>(device_);
    for(auto& capture:capture_)
        if(capture.buffer)vmaDestroyBuffer(vk_device.allocator,capture.buffer,capture.allocation);
}

bool NativeRayTracedAo::CreatePasses() {
    auto shader=[&](std::span<const uint32_t> code)->RenderShader* {
        auto created=device_.createShader(code.data(),code.size_bytes(),"main",RenderShaderFormat::SPIRV);
        if(!created||!static_cast<VulkanShader&>(*created).vk)return nullptr;
        shaders_.push_back(std::move(created));
        return shaders_.back().get();
    };
    auto layout=[&](uint32_t push_bytes,std::span<const RenderDescriptorRange> ranges) {
        const RenderPushConstantRange push(0,0,0,push_bytes,RenderShaderStageFlag::COMPUTE);
        if(ranges.empty())return device_.createPipelineLayout(RenderPipelineLayoutDesc(&push,1,nullptr,0));
        const RenderDescriptorSetDesc set(ranges.data(),uint32_t(ranges.size()));
        return device_.createPipelineLayout(RenderPipelineLayoutDesc(&push,1,&set,1));
    };
    using T=RenderDescriptorRangeType;
    const std::array<RenderDescriptorRange,3> trace{{{T::ACCELERATION_STRUCTURE,0,1},{T::TEXTURE,1,1},{T::READ_WRITE_TEXTURE,2,1}}};
    const std::array<RenderDescriptorRange,7> temporal{{{T::TEXTURE,0,1},{T::TEXTURE,1,1},{T::TEXTURE,2,1},{T::TEXTURE,3,1},
        {T::READ_WRITE_TEXTURE,4,1},{T::READ_WRITE_TEXTURE,5,1},{T::TEXTURE,6,1}}};
    const std::array<RenderDescriptorRange,5> composite{{{T::TEXTURE,0,1},{T::TEXTURE,1,1},{T::TEXTURE,2,1},{T::READ_WRITE_TEXTURE,3,1},
        {T::TEXTURE,4,1}}};
    convert_layout_=layout(sizeof(ConvertPush),{});
    trace_layout_=layout(sizeof(TracePush),trace);
    temporal_layout_=layout(sizeof(TemporalPush),temporal);
    composite_layout_=layout(sizeof(CompositePush),composite);
    const std::array<RenderDescriptorRange,4> shadow{{{T::ACCELERATION_STRUCTURE,0,1},{T::TEXTURE,1,1},{T::TEXTURE,2,1},
        {T::READ_WRITE_TEXTURE,3,1}}};
    shadow_layout_=layout(sizeof(ShadowPush),shadow);
    const std::array<RenderDescriptorRange,4> shadow_merge{{{T::TEXTURE,0,1},{T::TEXTURE,1,1},{T::TEXTURE,2,1},
        {T::READ_WRITE_TEXTURE,3,1}}};
    shadow_merge_layout_=layout(sizeof(ShadowMergePush),shadow_merge);
    if(!convert_layout_||!trace_layout_||!temporal_layout_||!composite_layout_||!shadow_layout_||!shadow_merge_layout_)return false;
    auto* convert=shader(rt_ao_convert_spv);auto* trace_shader=shader(rt_ao_trace_spv);auto* temporal_shader=shader(rt_ao_temporal_spv);
    if(!convert||!trace_shader||!temporal_shader)return false;
    convert_=device_.createComputePipeline(RenderComputePipelineDesc(convert_layout_.get(),convert,256,1,1));
    trace_=device_.createComputePipeline(RenderComputePipelineDesc(trace_layout_.get(),trace_shader,8,8,1));
    temporal_=device_.createComputePipeline(RenderComputePipelineDesc(temporal_layout_.get(),temporal_shader,8,8,1));
    auto* shadow_trace=shader(rt_shadow_trace_spv);
    if(!shadow_trace)return false;
    shadow_trace_=device_.createComputePipeline(RenderComputePipelineDesc(shadow_layout_.get(),shadow_trace,8,8,1));
    auto valid=[](const std::unique_ptr<RenderPipeline>& p){return p&&static_cast<VulkanComputePipeline&>(*p).vk;};
    return valid(convert_)&&valid(trace_)&&valid(temporal_)&&valid(shadow_trace_);
}

RenderPipeline* NativeRayTracedAo::Composite(VkFormat format) {
    if(const auto found=composites_.find(format);found!=composites_.end())return found->second.get();
    std::span<const uint32_t> code;
    if(format==VK_FORMAT_R8G8B8A8_UNORM)code=rt_ao_composite_rgba8_spv;
    else if(format==VK_FORMAT_A2B10G10R10_UNORM_PACK32)code=rt_ao_composite_rgb10_a2_spv;
    else if(format==VK_FORMAT_R16G16B16A16_SFLOAT)code=rt_ao_composite_rgba16f_spv;
    std::unique_ptr<RenderPipeline> pipeline;
    if(!code.empty()) {
        auto shader=device_.createShader(code.data(),code.size_bytes(),"main",RenderShaderFormat::SPIRV);
        if(shader&&static_cast<VulkanShader&>(*shader).vk) {
            pipeline=device_.createComputePipeline(RenderComputePipelineDesc(composite_layout_.get(),shader.get(),8,8,1));
            shaders_.push_back(std::move(shader));
            if(pipeline&&!static_cast<VulkanComputePipeline&>(*pipeline).vk)pipeline.reset();
        }
    }
    if(!pipeline)REXLOG_WARN("Ray-traced AO: no composite for scene format {}",int(format));
    return composites_.emplace(format,std::move(pipeline)).first->second.get();
}

RenderPipeline* NativeRayTracedAo::Shadow(VkFormat format) {
    if(const auto found=shadow_passes_.find(format);found!=shadow_passes_.end())return found->second.get();
    std::span<const uint32_t> code;
    if(format==VK_FORMAT_R8G8B8A8_UNORM)code=rt_shadow_rgba8_spv;
    else if(format==VK_FORMAT_A2B10G10R10_UNORM_PACK32)code=rt_shadow_rgb10_a2_spv;
    else if(format==VK_FORMAT_R16G16B16A16_SFLOAT)code=rt_shadow_rgba16f_spv;
    std::unique_ptr<RenderPipeline> pipeline;
    if(!code.empty()) {
        auto shader=device_.createShader(code.data(),code.size_bytes(),"main",RenderShaderFormat::SPIRV);
        if(shader&&static_cast<VulkanShader&>(*shader).vk) {
            pipeline=device_.createComputePipeline(RenderComputePipelineDesc(shadow_merge_layout_.get(),shader.get(),8,8,1));
            shaders_.push_back(std::move(shader));
            if(pipeline&&!static_cast<VulkanComputePipeline&>(*pipeline).vk)pipeline.reset();
        }
    }
    if(!pipeline)REXLOG_WARN("Ray-traced shadows: no pass for attenuation format {}",int(format));
    return shadow_passes_.emplace(format,std::move(pipeline)).first->second.get();
}

void NativeRayTracedAo::RememberVertexShader(std::string_view digest,std::span<const uint8_t> bytes) {
    if(!supported_||bytes.size()%4||bytes.size()<24)return;
    std::vector<uint32_t> words(bytes.size()/4);
    std::memcpy(words.data(),bytes.data(),bytes.size());
    // Only vertex modules: pixel shaders (over 100,000 with the precompile)
    // would crowd them out.
    bool vertex=false;
    for(size_t at=5;at<words.size();) {
        const uint32_t count=words[at]>>16,opcode=words[at]&0xffff;
        if(!count)return;
        if(opcode==kOpEntryPoint&&count>=2) {vertex=words[at+1]==0;break;}
        at+=count;
    }
    if(!vertex)return;
    std::lock_guard lock(shaders_mutex_);
    const std::string key(digest);
    if(vertex_shaders_.contains(key)||vertex_shaders_.size()>=65536)return;
    auto patched=NativeAddPositionTransformFeedback(words);
    vertex_shaders_.emplace(key,patched.empty()?nullptr:std::make_shared<const std::vector<uint32_t>>(std::move(patched)));
}

void NativeRayTracedAo::BeginFrame(uint32_t width,uint32_t height,const NativeRtAoSettings& settings) {
    settings_=settings;
    width_=width;height_=height;
    applied_=scene_built_=shadows_applied_=prepass_seen_=false;used_vertices_=moving_vertices_=0;camera_.reset();depth_=nullptr;
    moving_index_^=1;moving_meshes_[moving_index_].clear();
    shadow_projection_.reset();
    capture_index_^=1;
    timing_reset_=false;
    if(settings.timing)CollectTiming();
    const bool wanted=settings.enabled||settings.shadows;
    if(supported_&&wanted&&!resources_ready_)EnsureResources();
    capturing_=supported_&&resources_ready_&&wanted&&width&&height&&capture_cleared_;
    // This frame builds one part; the others are reused while they are
    // fresh (built within the last parts frames).
    const uint32_t parts=std::clamp(settings.scene_parts,1u,kMaxParts);
    if(!capturing_||parts!=parts_)for(auto& part:scene_parts_)part.valid=false;
    if(!capturing_)history_valid_=false;
    parts_=parts;part_=uint32_t((frame_+1)%parts_);reused_parts_=0;
    for(uint32_t p=0;p<parts_;++p) {
        auto& part=scene_parts_[p];
        if(part.valid&&frame_+1-part.frame>=parts_)part.valid=false;
        if(p!=part_&&part.valid)++reused_parts_;
    }
    // Until every other part exists, this one takes the whole scene.
    capture_all_=reused_parts_+1<parts_;
    ++frame_;
    if(capturing_)++statistics_.frames;
}

void NativeRayTracedAo::ObserveCamera(std::span<const uint8_t> state) {
    if(capturing_&&!camera_)camera_=NativeTemporalReadCamera(state);
}

bool NativeRayTracedAo::ObserveShadowProjection(std::span<const uint8_t> state) {
    if(!capturing_||!settings_.shadows||state.size()<kPixelConstants+12*16)return false;
    // c7.xy is the shadow buffer size in a projection; a light function or
    // other red-only pass has something else there.
    const float width=GuestFloat(state,kPixelConstants+7*16),height=GuestFloat(state,kPixelConstants+7*16+4);
    if(!(width>=16&&width<=8192&&height>=16&&height<=8192)||width!=std::floor(width)||height!=std::floor(height))return false;
    if(shadow_projection_)return true;
    std::array<std::array<double,4>,4> m;
    for(int r=0;r<4;++r)for(int c=0;c<4;++c) {
        const float value=GuestFloat(state,kPixelConstants+(8+r)*16+c*4);
        if(!std::isfinite(value))return false;
        m[r][c]=value;
    }
    shadow_projection_=m;
    return true;
}

void NativeRayTracedAo::ObserveDepthResolve(RenderTexture* depth) {
    if(!capturing_||!depth)return;
    const auto& texture=static_cast<const VulkanTexture&>(*depth);
    if(texture.desc.width==width_&&texture.desc.height==height_&&texture.imageFormat==VK_FORMAT_R32_SFLOAT)depth_=depth;
}

VkRenderPass NativeRayTracedAo::CompatibleRenderPass(const RenderCommandList& commands,std::string& signature) {
    const auto& vk_commands=static_cast<const VulkanCommandList&>(commands);
    const auto* framebuffer=vk_commands.targetFramebuffer;
    if(!framebuffer||!vk_commands.activeRenderPass)return VK_NULL_HANDLE;
    std::vector<VkAttachmentDescription> attachments;
    auto describe=[&](const VulkanTexture& texture,bool depth) {
        VkAttachmentDescription a{};
        a.format=texture.imageFormat;
        a.samples=VkSampleCountFlagBits(std::max(1u,uint32_t(texture.desc.multisampling.sampleCount)));
        a.loadOp=a.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_LOAD;
        a.storeOp=a.stencilStoreOp=VK_ATTACHMENT_STORE_OP_STORE;
        a.initialLayout=a.finalLayout=depth?VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachments.push_back(a);
        signature+=std::to_string(int(a.format))+"x"+std::to_string(int(a.samples))+(depth?"d;":";");
    };
    for(const auto* color:framebuffer->colorAttachments)describe(*color,false);
    if(framebuffer->depthAttachment)describe(*framebuffer->depthAttachment,true);
    if(const auto found=render_passes_.find(signature);found!=render_passes_.end())return found->second;
    std::vector<VkAttachmentReference> colors;
    for(uint32_t i=0;i<framebuffer->colorAttachments.size();++i)colors.push_back({i,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL});
    const VkAttachmentReference depth{uint32_t(colors.size()),VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount=uint32_t(colors.size());subpass.pColorAttachments=colors.data();
    subpass.pDepthStencilAttachment=framebuffer->depthAttachment?&depth:nullptr;
    VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    info.attachmentCount=uint32_t(attachments.size());info.pAttachments=attachments.data();
    info.subpassCount=1;info.pSubpasses=&subpass;
    VkRenderPass pass=VK_NULL_HANDLE;
    if(vkCreateRenderPass(vk_,&info,nullptr,&pass)!=VK_SUCCESS)return VK_NULL_HANDLE;
    render_passes_.emplace(signature,pass);
    return pass;
}

bool NativeRayTracedAo::CaptureDraw(RenderCommandList& commands,std::string_view vertex_digest,
        RenderPrimitiveTopology topology,uint32_t index_count,int32_t index_offset,const RenderPipeline& restore,
        bool depth_only,uint64_t mesh) {
    if(!capturing_||scene_built_)return false;
    if(depth_only) {
        if(!prepass_seen_)++statistics_.prepass_frames;
        prepass_seen_=true;
    } else if(prepass_seen_) {++statistics_.skipped_after_prepass;return false;}
    const bool moving=mesh&&(moving_meshes_[0].contains(mesh)||moving_meshes_[1].contains(mesh));
    if(!capture_all_&&!moving) {
        // A per-mesh key, stable across frames: which part holds this draw.
        uint64_t key=std::hash<std::string_view>{}(vertex_digest)^(uint64_t(index_count)*0x9e3779b97f4a7c15ull)^
            (uint64_t(uint32_t(index_offset))*0xc2b2ae3d27d4eb4full);
        key^=key>>29;key*=0xbf58476d1ce4e5b9ull;key^=key>>32;
        if(key%parts_!=part_)return false;
    }
    const VkPrimitiveTopology vk_topology=Topology(topology);
    if(vk_topology==VK_PRIMITIVE_TOPOLOGY_MAX_ENUM)return false;
    const uint32_t vertices=vk_topology==VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST?index_count/3*3:
        index_count>=3?(index_count-2)*3:0;
    if(!vertices)return false;
    if(moving?vertices>kMaxMovingVertices-moving_vertices_:vertices>kMovingBase-used_vertices_) {
        ++(moving?statistics_.moving_overflows:statistics_.overflows);return false;
    }
    std::shared_ptr<const std::vector<uint32_t>> spirv;
    {
        std::lock_guard lock(shaders_mutex_);
        const auto found=vertex_shaders_.find(std::string(vertex_digest));
        if(found==vertex_shaders_.end()) {++statistics_.missing_shaders;return false;}
        if(!found->second) {++statistics_.unpatchable_shaders;return false;}
        spirv=found->second;
    }
    std::string key(vertex_digest);
    key+='|';key+=std::to_string(int(vk_topology));key+='|';
    const VkRenderPass pass=CompatibleRenderPass(commands,key);
    if(!pass)return false;
    auto& entry=pipelines_[key];
    if(!entry) {
        entry=std::make_shared<CapturePipeline>();
        const auto& framebuffer=*static_cast<const VulkanCommandList&>(commands).targetFramebuffer;
        auto job=std::make_unique<Job>();
        job->spirv=spirv;job->topology=vk_topology;job->render_pass=pass;
        job->color_attachments=uint32_t(framebuffer.colorAttachments.size());
        const VulkanTexture* any=framebuffer.colorAttachments.empty()?framebuffer.depthAttachment:framebuffer.colorAttachments.front();
        job->samples=VkSampleCountFlagBits(any?std::max(1u,uint32_t(any->desc.multisampling.sampleCount)):1u);
        job->depth=framebuffer.depthAttachment!=nullptr;
        job->pipeline=entry;
        {
            std::lock_guard lock(jobs_mutex_);
            jobs_.push_back(std::move(job));
        }
        jobs_changed_.notify_one();
    }
    const VkPipeline pipeline=entry->pipeline.load(std::memory_order_acquire);
    if(!pipeline) {++statistics_.pending_pipelines;return false;}
    const VkCommandBuffer cb=static_cast<VulkanCommandList&>(commands).vk;
    auto& capture=capture_[capture_index_];
    const VkDeviceSize offset=VkDeviceSize(moving?kMovingBase+moving_vertices_:used_vertices_)*16,size=VkDeviceSize(vertices)*16;
    vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);
    bind_feedback_(cb,0,1,&capture.buffer,&offset,&size);
    begin_feedback_(cb,0,0,nullptr,nullptr);
    vkCmdDrawIndexed(cb,index_count,1,0,index_offset,0);
    end_feedback_(cb,0,0,nullptr,nullptr);
    vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,static_cast<const VulkanGraphicsPipeline&>(restore).vk);
    if(moving) {
        moving_vertices_+=vertices;
        capture.dirty_moving=std::max(capture.dirty_moving,moving_vertices_);
        ++statistics_.moving_draws;
        statistics_.max_moving_vertices=std::max<uint64_t>(statistics_.max_moving_vertices,moving_vertices_);
        return true;
    }
    used_vertices_+=vertices;
    capture.dirty_vertices=std::max(capture.dirty_vertices,used_vertices_);
    ++statistics_.captured_draws;
    statistics_.max_vertices=std::max<uint64_t>(statistics_.max_vertices,used_vertices_);
    return true;
}

void NativeRayTracedAo::Worker() {
    for(;;) {
        std::unique_ptr<Job> job;
        {
            std::unique_lock lock(jobs_mutex_);
            jobs_changed_.wait(lock,[&]{return stopping_||!jobs_.empty();});
            if(stopping_)return;
            job=std::move(jobs_.front());jobs_.pop_front();
        }
        VkShaderModuleCreateInfo module_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        module_info.codeSize=job->spirv->size()*4;module_info.pCode=job->spirv->data();
        VkShaderModule module=VK_NULL_HANDLE;
        VkPipeline pipeline=VK_NULL_HANDLE;
        if(vkCreateShaderModule(vk_,&module_info,nullptr,&module)==VK_SUCCESS) {
            VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            stage.stage=VK_SHADER_STAGE_VERTEX_BIT;stage.module=module;stage.pName="main";
            VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
            VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
            assembly.topology=job->topology;
            // As Plume's scene pipelines: strips restart at the all-ones index.
            assembly.primitiveRestartEnable=job->topology==VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
            VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
            viewport.viewportCount=viewport.scissorCount=1;
            VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
            raster.rasterizerDiscardEnable=VK_TRUE;raster.polygonMode=VK_POLYGON_MODE_FILL;raster.lineWidth=1;
            VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
            multisample.rasterizationSamples=job->samples;
            VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
            std::vector<VkPipelineColorBlendAttachmentState> blends(job->color_attachments);
            VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
            blend.attachmentCount=uint32_t(blends.size());blend.pAttachments=blends.data();
            // The scene pipelines' dynamic state, so binding this one between
            // two of their draws keeps the values they set.
            const std::array<VkDynamicState,3> states{VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR,VK_DYNAMIC_STATE_DEPTH_BIAS};
            VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
            dynamic.dynamicStateCount=uint32_t(states.size());dynamic.pDynamicStates=states.data();
            VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
            info.stageCount=1;info.pStages=&stage;
            info.pVertexInputState=&input;info.pInputAssemblyState=&assembly;info.pViewportState=&viewport;
            info.pRasterizationState=&raster;info.pMultisampleState=&multisample;
            info.pDepthStencilState=job->depth?&depth:nullptr;info.pColorBlendState=&blend;info.pDynamicState=&dynamic;
            info.layout=draw_layout_;info.renderPass=job->render_pass;info.subpass=0;
            if(vkCreateGraphicsPipelines(vk_,VK_NULL_HANDLE,1,&info,nullptr,&pipeline)!=VK_SUCCESS)pipeline=VK_NULL_HANDLE;
            vkDestroyShaderModule(vk_,module,nullptr);
        }
        if(pipeline) {
            job->pipeline->pipeline.store(pipeline,std::memory_order_release);
            ++worker_pipelines_;
        } else {
            job->pipeline->failed=true;
            if(worker_failures_++<4)REXLOG_WARN("Ray-traced AO: capture pipeline creation failed");
        }
    }
}

void NativeRayTracedAo::FillCapture(RenderCommandList& commands,uint32_t index) {
    auto& capture=capture_[index];
    const VkCommandBuffer cb=static_cast<VulkanCommandList&>(commands).vk;
    if(capture.dirty_vertices)vkCmdFillBuffer(cb,capture.buffer,0,VkDeviceSize(capture.dirty_vertices)*16,kNaN);
    if(capture.dirty_moving)
        vkCmdFillBuffer(cb,capture.buffer,VkDeviceSize(kMovingBase)*16,VkDeviceSize(capture.dirty_moving)*16,kNaN);
    capture.dirty_vertices=capture.dirty_moving=0;
}

void NativeRayTracedAo::ObserveMovingMesh(uint64_t mesh) {
    auto& meshes=moving_meshes_[moving_index_];
    if(capturing_&&mesh&&meshes.size()<16384)meshes.insert(mesh);
}

void NativeRayTracedAo::EndFrame(RenderCommandList& commands) {
    if(!supported_||!resources_ready_)return;
    if(capture_cleared_&&!capture_[capture_index_].dirty_vertices&&!capture_[capture_index_].dirty_moving)return;
    auto& vk_commands=static_cast<VulkanCommandList&>(commands);
    vk_commands.endActiveRenderPass();
    FullBarrier(vk_commands.vk);
    if(!capture_cleared_) {
        // A settings change can enable capture at any frame; clear both once.
        FillCapture(commands,0);FillCapture(commands,1);capture_cleared_=true;
    } else FillCapture(commands,capture_index_);
    FullBarrier(vk_commands.vk);
    if(!applied_)history_valid_=false;
    capturing_=false;
}

void NativeRayTracedAo::Stamp(VkCommandBuffer cb,TimingStage stage,bool end) {
    if(!settings_.timing)return;
    if(!timing_pool_) {
        VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        info.queryType=VK_QUERY_TYPE_TIMESTAMP;
        info.queryCount=kTimingFrames*kTimingStages*2;
        if(vkCreateQueryPool(vk_,&info,nullptr,&timing_pool_)!=VK_SUCCESS) {timing_pool_=VK_NULL_HANDLE;return;}
        timestamp_period_ns_=static_cast<VulkanDevice&>(device_).physicalDeviceProperties.limits.timestampPeriod;
    }
    const uint32_t slot=uint32_t(frame_%kTimingFrames);
    if(!timing_reset_) {
        vkCmdResetQueryPool(cb,timing_pool_,slot*kTimingStages*2,kTimingStages*2);
        timing_written_[slot]=0;timing_reset_=true;
    }
    const uint32_t index=stage*2+(end?1:0);
    vkCmdWriteTimestamp(cb,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,timing_pool_,slot*kTimingStages*2+index);
    timing_written_[slot]|=1u<<index;
    if(stage==kTimingBuild&&end)timing_vertices_+=used_vertices_+moving_vertices_;
}

void NativeRayTracedAo::CollectTiming() {
    // The slot about to be reused was written kTimingFrames frames ago.
    const uint32_t slot=uint32_t(frame_%kTimingFrames);
    if(!timing_pool_||!timing_written_[slot])return;
    std::array<uint64_t,kTimingStages*2*2> values{};  // value, availability
    vkGetQueryPoolResults(vk_,timing_pool_,slot*kTimingStages*2,kTimingStages*2,sizeof(values),values.data(),
                          2*sizeof(uint64_t),VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    for(uint32_t stage=0;stage<kTimingStages;++stage) {
        const uint32_t start=stage*2,end=start+1;
        if(!(timing_written_[slot]>>start&1)||!(timing_written_[slot]>>end&1))continue;
        if(!values[start*2+1]||!values[end*2+1]||values[end*2]<values[start*2])continue;
        timing_ms_[stage]+=double(values[end*2]-values[start*2])*timestamp_period_ns_*1e-6;
        ++timing_count_[stage];
    }
    timing_written_[slot]=0;
    if(++timing_frames_%600==0) {
        auto average=[&](TimingStage s) {return timing_count_[s]?timing_ms_[s]/timing_count_[s]:0.0;};
        REXLOG_INFO("Ray tracing GPU time per frame: scene build {:.2f} ms ({} vertices), shadows {:.2f} ms, "
                    "AO {:.2f} ms",average(kTimingBuild),
                    timing_count_[kTimingBuild]?timing_vertices_/timing_count_[kTimingBuild]:0,
                    average(kTimingShadows),average(kTimingAo));
        timing_ms_={};timing_count_={};timing_vertices_=0;
    }
}

bool NativeRayTracedAo::BuildScene(RenderCommandList& commands,const Matrix& inverse) {
    if(scene_built_)return scene_ready_;
    scene_built_=true;scene_ready_=false;
    const VkCommandBuffer cb=static_cast<VulkanCommandList&>(commands).vk;
    auto& current=scene_parts_[part_];
    current.valid=moving_part_.valid=false;
    Stamp(cb,kTimingBuild,false);
    if(used_vertices_||moving_vertices_) {
    // 1. Captured clip positions to camera-relative world positions: the
    // static range from the start, the moving range from kMovingBase.
    ConvertPush convert{};
    Columns(inverse,convert.inverse);
    commands.setComputePipelineLayout(convert_layout_.get());
    commands.setPipeline(convert_.get());
    for(const auto [first,count]:{std::pair{0u,used_vertices_},std::pair{kMovingBase,moving_vertices_}}) {
        if(!count)continue;
        convert.clip=capture_[capture_index_].address+uint64_t(first)*16;
        convert.world=positions_->getDeviceAddress()+uint64_t(first)*12;
        convert.count=count;
        commands.setComputePushConstants(0,&convert);
        commands.dispatch((count+255)/256,1,1);
    }
    FullBarrier(cb);
    // The capture buffer is written again two frames later; unwritten slots
    // must read as NaN (inactive triangles).
    FillCapture(commands,capture_index_);

    // 2. Acceleration structures (one at a time: they share the scratch).
    const auto build=[&](Part& part,uint32_t first,uint32_t count,uint64_t capacity) {
        const RenderBottomLevelASMesh mesh(RenderBufferReference(),RenderBufferReference(positions_.get(),uint64_t(first)*12),
            RenderFormat::UNKNOWN,RenderFormat::R32G32B32_FLOAT,0,count,12,true);
        RenderBottomLevelASBuildInfo blas_info;
        device_.setBottomLevelASBuildInfo(blas_info,&mesh,1,settings_.fast_build,!settings_.fast_build);
        if(blas_info.accelerationStructureSize>capacity||blas_info.scratchSize>scratch_capacity_)return;
        commands.buildBottomLevelAS(part.blas.get(),RenderBufferReference(scratch_.get()),blas_info);
        FullBarrier(cb);
        part.translation=camera_->pre_view_translation;part.frame=frame_;part.valid=true;
    };
    if(used_vertices_)build(current,0,used_vertices_,blas_capacity_);
    if(moving_vertices_)build(moving_part_,kMovingBase,moving_vertices_,moving_blas_capacity_);
    }
    // 3. One instance per available part, moved by the camera's change since
    // its build (its positions are relative to that frame's camera), and the
    // moving meshes of this frame.
    std::array<uint8_t,kMaxInstances*sizeof(VkAccelerationStructureInstanceKHR)> data{};
    uint32_t count=0;
    for(uint32_t p=0;p<=parts_;++p) {
        const auto& part=p<parts_?scene_parts_[p]:moving_part_;
        if(!part.valid)continue;
        VkAccelerationStructureInstanceKHR instance;
        std::memcpy(&instance,part.instance.data(),sizeof(instance));
        for(int r=0;r<3;++r)instance.transform.matrix[r][3]=float(camera_->pre_view_translation[r]-part.translation[r]);
        std::memcpy(data.data()+count*sizeof(instance),&instance,sizeof(instance));
        ++count;
        if(p<parts_&&p!=part_)++statistics_.reused_scenes;
    }
    if(!count)return false;
    auto& ring=*instances_[frame_%kInstanceRing];
    void* mapped=ring.map();
    if(!mapped)return false;
    std::memcpy(mapped,data.data(),count*sizeof(VkAccelerationStructureInstanceKHR));
    ring.unmap();
    commands.buildTopLevelAS(tlas_.get(),RenderBufferReference(scratch_.get()),RenderBufferReference(&ring),tlas_infos_[count-1]);
    FullBarrier(cb);
    Stamp(cb,kTimingBuild,true);
    scene_ready_=true;
    return true;
}

std::shared_ptr<RenderTexture> NativeRayTracedAo::Apply(RenderCommandList& commands,RenderTexture& scene,
        RenderTexture& depth,const std::function<void(std::shared_ptr<void>)>& keep,const NativeRtAoObjectMotion& motion) {
    if(!WantsApply())return nullptr;
    applied_=true;
    const auto& scene_texture=static_cast<const VulkanTexture&>(scene);
    if(scene_texture.desc.width!=width_||scene_texture.desc.height!=height_)return nullptr;
    auto* composite=Composite(scene_texture.imageFormat);
    NativeTemporalProjection projection;
    Matrix inverse;
    if(!composite||!NativeTemporalDescribeProjection(*camera_,projection)||!Invert(camera_->view_projection,inverse))return nullptr;
    auto& vk_commands=static_cast<VulkanCommandList&>(commands);
    vk_commands.endActiveRenderPass();
    const VkCommandBuffer cb=vk_commands.vk;
    auto lease=std::make_shared<FrameLease>();
    keep(lease);

    // Images; the lease keeps replaced ones until this frame's work is done.
    // Occlusion is traced and accumulated at half resolution.
    const auto storage=RenderTextureFlag::STORAGE|RenderTextureFlag::UNORDERED_ACCESS;
    // One occlusion texel per 2x2 pixels, or per the same share of the scene
    // when internal resolution scaling enlarges it: as many rays as unscaled.
    const uint32_t step=2*NativeRenderScale();
    const uint32_t half_width=(width_+step-1)/step,half_height=(height_+step-1)/step;
    auto image=[&](RenderFormat format,uint32_t w,uint32_t h) {
        return std::shared_ptr<RenderTexture>(device_.createTexture(RenderTextureDesc::Texture2D(w,h,1,format,storage)));
    };
    if(image_width_!=width_||image_height_!=height_||!ao_raw_) {
        ao_raw_=image(RenderFormat::R16_FLOAT,half_width,half_height);
        for(auto& h:history_)h=image(RenderFormat::R16G16_FLOAT,half_width,half_height);
        for(auto& d:history_depth_)d=image(RenderFormat::R32_FLOAT,half_width,half_height);
        image_width_=width_;image_height_=height_;history_valid_=false;
        if(!ao_raw_||!history_[0]||!history_[1]||!history_depth_[0]||!history_depth_[1]) {ao_raw_.reset();return nullptr;}
    }
    if(!output_||static_cast<VulkanTexture&>(*output_).desc.width!=width_||
       static_cast<VulkanTexture&>(*output_).desc.height!=height_||
       static_cast<VulkanTexture&>(*output_).desc.format!=scene_texture.desc.format) {
        output_=image(scene_texture.desc.format,width_,height_);
        if(!output_)return nullptr;
    }
    const uint32_t previous=history_index_,next=previous^1;
    lease->images={ao_raw_,history_[0],history_[1],history_depth_[0],history_depth_[1],output_};
    const BorrowedLayout scene_layout(&scene),depth_layout(&depth);
    FullBarrier(cb);
    if(!BuildScene(commands,inverse))return nullptr;
    Stamp(cb,kTimingAo,false);

    // 3. Trace.
    auto set=[&](const std::unique_ptr<RenderPipelineLayout>&,std::span<const RenderDescriptorRange> ranges) {
        lease->sets.push_back(device_.createDescriptorSet(RenderDescriptorSetDesc(ranges.data(),uint32_t(ranges.size()))));
        return lease->sets.back().get();
    };
    using T=RenderDescriptorRangeType;
    const std::array<RenderDescriptorRange,3> trace_ranges{{{T::ACCELERATION_STRUCTURE,0,1},{T::TEXTURE,1,1},{T::READ_WRITE_TEXTURE,2,1}}};
    const std::array<RenderDescriptorRange,7> temporal_ranges{{{T::TEXTURE,0,1},{T::TEXTURE,1,1},{T::TEXTURE,2,1},{T::TEXTURE,3,1},
        {T::READ_WRITE_TEXTURE,4,1},{T::READ_WRITE_TEXTURE,5,1},{T::TEXTURE,6,1}}};
    const std::array<RenderDescriptorRange,5> composite_ranges{{{T::TEXTURE,0,1},{T::TEXTURE,1,1},{T::TEXTURE,2,1},
        {T::READ_WRITE_TEXTURE,3,1},{T::TEXTURE,4,1}}};
    auto* trace_set=set(trace_layout_,trace_ranges);
    auto* temporal_set=set(temporal_layout_,temporal_ranges);
    auto* composite_set=set(composite_layout_,composite_ranges);
    if(!trace_set||!temporal_set||!composite_set)return nullptr;
    const float sky_depth=projection.infinity_depth+1e-7f;
    {
        const std::array<RenderTextureBarrier,2> barriers{RenderTextureBarrier(&depth,RenderTextureLayout::SHADER_READ),
            RenderTextureBarrier(ao_raw_.get(),RenderTextureLayout::GENERAL)};
        commands.barriers(RenderBarrierStage::COMPUTE,barriers.data(),uint32_t(barriers.size()));
        trace_set->setAccelerationStructure(0,tlas_.get());
        trace_set->setTexture(1,&depth,RenderTextureLayout::SHADER_READ);
        trace_set->setTexture(2,ao_raw_.get(),RenderTextureLayout::GENERAL);
        TracePush push{};
        Columns(inverse,push.inverse);
        push.size[0]=width_;push.size[1]=height_;push.frame=uint32_t(frame_);
        push.rays=std::clamp(settings_.rays,1u,16u);push.radius=std::max(settings_.radius,1.0f);push.sky_depth=sky_depth;
        push.step=step;
        commands.setComputePipelineLayout(trace_layout_.get());
        commands.setComputePushConstants(0,&push);
        commands.setComputeDescriptorSet(trace_set,0);
        commands.setPipeline(trace_.get());
        commands.dispatch((half_width+7)/8,(half_height+7)/8,1);
    }

    // 4. Accumulate over frames.
    bool history=history_valid_&&previous_camera_;
    Matrix reprojection{};
    if(history) {
        // Previous translated world = current one + (c5_prev - c5_cur).
        double moved=0;
        std::array<double,3> delta{};
        for(int c=0;c<3;++c) {
            delta[c]=previous_camera_->pre_view_translation[c]-camera_->pre_view_translation[c];
            moved+=delta[c]*delta[c];
        }
        const auto& m=previous_camera_->view_projection;
        for(int i=0;i<4;++i)for(int j=0;j<4;++j)reprojection[i][j]=m[i][j];
        for(int j=0;j<4;++j)reprojection[3][j]=m[3][j]+delta[0]*m[0][j]+delta[1]*m[1][j]+delta[2]*m[2][j];
        history=moved<500.0*500.0;  // a cut or teleport
    }
    {
        const std::array<RenderTextureBarrier,5> barriers{RenderTextureBarrier(ao_raw_.get(),RenderTextureLayout::SHADER_READ),
            RenderTextureBarrier(history_[previous].get(),RenderTextureLayout::SHADER_READ),
            RenderTextureBarrier(history_depth_[previous].get(),RenderTextureLayout::SHADER_READ),
            RenderTextureBarrier(history_[next].get(),RenderTextureLayout::GENERAL),
            RenderTextureBarrier(history_depth_[next].get(),RenderTextureLayout::GENERAL)};
        commands.barriers(RenderBarrierStage::COMPUTE,barriers.data(),uint32_t(barriers.size()));
        // The velocity buffer must be half the frame size; ao_raw_ stands in.
        RenderTexture* velocity=motion.velocity&&motion.scale[0]>0&&
            static_cast<VulkanTexture&>(*motion.velocity).desc.width==(width_+1)/2&&
            static_cast<VulkanTexture&>(*motion.velocity).desc.height==(height_+1)/2?motion.velocity:nullptr;
        const std::optional<BorrowedLayout> velocity_layout=velocity?std::optional<BorrowedLayout>(BorrowedLayout(velocity)):std::nullopt;
        if(velocity)commands.barriers(RenderBarrierStage::COMPUTE,RenderTextureBarrier(velocity,RenderTextureLayout::SHADER_READ));
        temporal_set->setTexture(6,velocity?velocity:ao_raw_.get(),RenderTextureLayout::SHADER_READ);
        temporal_set->setTexture(0,ao_raw_.get(),RenderTextureLayout::SHADER_READ);
        temporal_set->setTexture(1,&depth,RenderTextureLayout::SHADER_READ);
        temporal_set->setTexture(2,history_[previous].get(),RenderTextureLayout::SHADER_READ);
        temporal_set->setTexture(3,history_depth_[previous].get(),RenderTextureLayout::SHADER_READ);
        temporal_set->setTexture(4,history_[next].get(),RenderTextureLayout::GENERAL);
        temporal_set->setTexture(5,history_depth_[next].get(),RenderTextureLayout::GENERAL);
        TemporalPush push{};
        Columns(inverse,push.inverse);
        Columns(reprojection,push.reprojection);
        push.size[0]=width_;push.size[1]=height_;push.history_valid=history;
        push.sky_depth=sky_depth;push.max_frames=16;push.flags=step<<8;
        if(velocity) {
            push.velocity_scale[0]=motion.scale[0];push.velocity_scale[1]=motion.scale[1];
            push.flags|=1u|(motion.swapped?2u:0u);
        }
        commands.setComputePipelineLayout(temporal_layout_.get());
        commands.setComputePushConstants(0,&push);
        commands.setComputeDescriptorSet(temporal_set,0);
        commands.setPipeline(temporal_.get());
        commands.dispatch((half_width+7)/8,(half_height+7)/8,1);
        if(velocity_layout)velocity_layout->Restore(commands);
    }

    // 5. Blur and multiply into a copy of the scene.
    {
        const std::array<RenderTextureBarrier,4> barriers{RenderTextureBarrier(history_[next].get(),RenderTextureLayout::SHADER_READ),
            RenderTextureBarrier(history_depth_[next].get(),RenderTextureLayout::SHADER_READ),
            RenderTextureBarrier(&scene,RenderTextureLayout::SHADER_READ),
            RenderTextureBarrier(output_.get(),RenderTextureLayout::GENERAL)};
        commands.barriers(RenderBarrierStage::COMPUTE,barriers.data(),uint32_t(barriers.size()));
        composite_set->setTexture(0,history_[next].get(),RenderTextureLayout::SHADER_READ);
        composite_set->setTexture(1,&depth,RenderTextureLayout::SHADER_READ);
        composite_set->setTexture(2,&scene,RenderTextureLayout::SHADER_READ);
        composite_set->setTexture(3,output_.get(),RenderTextureLayout::GENERAL);
        composite_set->setTexture(4,history_depth_[next].get(),RenderTextureLayout::SHADER_READ);
        CompositePush push{};
        push.size[0]=width_;push.size[1]=height_;
        push.strength=std::clamp(settings_.strength,0.0f,1.0f);push.sky_depth=sky_depth;push.debug_view=settings_.debug_view;
        push.step=step;
        commands.setComputePipelineLayout(composite_layout_.get());
        commands.setComputePushConstants(0,&push);
        commands.setComputeDescriptorSet(composite_set,0);
        commands.setPipeline(composite);
        commands.dispatch((width_+7)/8,(height_+7)/8,1);
    }
    commands.barriers(RenderBarrierStage::COMPUTE|RenderBarrierStage::GRAPHICS,
        RenderTextureBarrier(output_.get(),RenderTextureLayout::SHADER_READ));
    scene_layout.Restore(commands);depth_layout.Restore(commands);
    Stamp(cb,kTimingAo,true);

    history_index_=next;history_valid_=true;previous_camera_=camera_;
    statistics_.pipelines=worker_pipelines_;statistics_.pipeline_failures=worker_failures_;
    if(++statistics_.applied==1||statistics_.applied%1800==0)
        REXLOG_INFO("Ray-traced AO: {} frames, {} draws captured ({} vertices most), {} capture pipelines "
                    "({} failed), {} draws waiting for a pipeline, {} without a shader, {} unpatchable, {} over capacity; "
                    "{} frames with a depth prepass, {} colour draws after it not captured, {} scenes reused; "
                    "{} moving draws ({} vertices most, {} over capacity)",
                    statistics_.applied,statistics_.captured_draws,statistics_.max_vertices,statistics_.pipelines,
                    statistics_.pipeline_failures,statistics_.pending_pipelines,statistics_.missing_shaders,
                    statistics_.unpatchable_shaders,statistics_.overflows,statistics_.prepass_frames,
                    statistics_.skipped_after_prepass,statistics_.reused_scenes,statistics_.moving_draws,
                    statistics_.max_moving_vertices,statistics_.moving_overflows);
    return output_;
}

// The light of a ScreenToShadow matrix S, whose input is (ndc.xy * w, w, 1)
// with w the clip w of the frame's camera VP. World to shadow is T * S, T the
// VP columns x, y and w. A perspective shadow (local light) has its centre of
// projection at the light; an orthographic one (directional light) has the
// light along its depth axis, depth increasing away from it.
static std::optional<std::array<double,4>> ShadowLight(const Matrix& view_projection,const Matrix& screen_to_shadow) {
    Matrix t{};
    for(int i=0;i<4;++i) {t[i][0]=view_projection[i][0];t[i][1]=view_projection[i][1];t[i][2]=view_projection[i][3];}
    t[3][3]=1;
    Matrix w{};
    for(int i=0;i<4;++i)for(int j=0;j<4;++j)
        for(int k=0;k<4;++k)w[i][j]+=t[i][k]*screen_to_shadow[k][j];
    const double perspective=std::sqrt(w[0][3]*w[0][3]+w[1][3]*w[1][3]+w[2][3]*w[2][3]);
    if(perspective*1e5>std::abs(w[3][3])) {
        // p * [col0 col1 col3] = -row3: Cramer's rule.
        const int cols[3]={0,1,3};
        double a[3][3],b[3];
        for(int r=0;r<3;++r) {b[r]=-w[3][cols[r]];for(int c=0;c<3;++c)a[r][c]=w[c][cols[r]];}
        auto det=[](const double m[3][3]) {
            return m[0][0]*(m[1][1]*m[2][2]-m[1][2]*m[2][1])-m[0][1]*(m[1][0]*m[2][2]-m[1][2]*m[2][0])+
                   m[0][2]*(m[1][0]*m[2][1]-m[1][1]*m[2][0]);
        };
        const double d=det(a);
        if(std::abs(d)<1e-20)return std::nullopt;
        std::array<double,4> light{0,0,0,1};
        for(int c=0;c<3;++c) {
            double m[3][3];
            for(int r=0;r<3;++r)for(int k=0;k<3;++k)m[r][k]=k==c?b[r]:a[r][k];
            light[c]=det(m)/d;
        }
        if(!std::isfinite(light[0])||!std::isfinite(light[1])||!std::isfinite(light[2]))return std::nullopt;
        return light;
    }
    const double x[3]={w[0][0],w[1][0],w[2][0]},y[3]={w[0][1],w[1][1],w[2][1]},z[3]={w[0][2],w[1][2],w[2][2]};
    double d[3]={x[1]*y[2]-x[2]*y[1],x[2]*y[0]-x[0]*y[2],x[0]*y[1]-x[1]*y[0]};
    const double length=std::sqrt(d[0]*d[0]+d[1]*d[1]+d[2]*d[2]);
    if(!(length>0))return std::nullopt;
    const double sign=d[0]*z[0]+d[1]*z[1]+d[2]*z[2]>0?-1:1;
    return std::array<double,4>{sign*d[0]/length,sign*d[1]/length,sign*d[2]/length,0};
}

std::shared_ptr<RenderTexture> NativeRayTracedAo::ApplyShadows(RenderCommandList& commands,RenderTexture& attenuation,
        RenderTexture& depth,const std::function<void(std::shared_ptr<void>)>& keep) {
    if(!WantsShadows())return nullptr;
    shadows_applied_=true;
    const auto& texture=static_cast<const VulkanTexture&>(attenuation);
    if(texture.desc.width!=width_||texture.desc.height!=height_)return nullptr;
    auto* pass=Shadow(texture.imageFormat);
    NativeTemporalProjection projection;
    Matrix inverse;
    if(!pass||!NativeTemporalDescribeProjection(*camera_,projection)||!Invert(camera_->view_projection,inverse))return nullptr;
    const auto light=ShadowLight(camera_->view_projection,*shadow_projection_);
    if(!light)return nullptr;
    auto& vk_commands=static_cast<VulkanCommandList&>(commands);
    vk_commands.endActiveRenderPass();
    const VkCommandBuffer cb=vk_commands.vk;
    auto lease=std::make_shared<FrameLease>();
    keep(lease);
    if(!shadow_output_||static_cast<VulkanTexture&>(*shadow_output_).desc.width!=width_||
       static_cast<VulkanTexture&>(*shadow_output_).desc.height!=height_||
       static_cast<VulkanTexture&>(*shadow_output_).desc.format!=texture.desc.format) {
        shadow_output_=std::shared_ptr<RenderTexture>(device_.createTexture(RenderTextureDesc::Texture2D(width_,height_,1,
            texture.desc.format,RenderTextureFlag::STORAGE|RenderTextureFlag::UNORDERED_ACCESS)));
        if(!shadow_output_)return nullptr;
    }
    // With internal resolution scaling, one traced point per scale x scale
    // pixels: the ray count of the guest resolution. The merge's depth-aware
    // filter fills in between.
    const uint32_t step=NativeRenderScale();
    const uint32_t traced_width=(width_+step-1)/step,traced_height=(height_+step-1)/step;
    if(!shadow_visibility_||static_cast<VulkanTexture&>(*shadow_visibility_).desc.width!=traced_width||
       static_cast<VulkanTexture&>(*shadow_visibility_).desc.height!=traced_height) {
        shadow_visibility_=std::shared_ptr<RenderTexture>(device_.createTexture(RenderTextureDesc::Texture2D(traced_width,traced_height,1,
            RenderFormat::R8_UNORM,RenderTextureFlag::STORAGE|RenderTextureFlag::UNORDERED_ACCESS)));
        if(!shadow_visibility_)return nullptr;
    }
    lease->images={shadow_output_,shadow_visibility_};
    const BorrowedLayout attenuation_layout(&attenuation),depth_layout(&depth);
    FullBarrier(cb);
    if(!BuildScene(commands,inverse))return nullptr;
    Stamp(cb,kTimingShadows,false);

    using T=RenderDescriptorRangeType;
    const std::array<RenderDescriptorRange,4> trace_ranges{{{T::ACCELERATION_STRUCTURE,0,1},{T::TEXTURE,1,1},{T::TEXTURE,2,1},
        {T::READ_WRITE_TEXTURE,3,1}}};
    const std::array<RenderDescriptorRange,4> merge_ranges{{{T::TEXTURE,0,1},{T::TEXTURE,1,1},{T::TEXTURE,2,1},
        {T::READ_WRITE_TEXTURE,3,1}}};
    lease->sets.push_back(device_.createDescriptorSet(RenderDescriptorSetDesc(trace_ranges.data(),uint32_t(trace_ranges.size()))));
    auto* trace_set=lease->sets.back().get();
    lease->sets.push_back(device_.createDescriptorSet(RenderDescriptorSetDesc(merge_ranges.data(),uint32_t(merge_ranges.size()))));
    auto* merge_set=lease->sets.back().get();
    if(!trace_set||!merge_set)return nullptr;
    const float softness=std::clamp(settings_.shadow_softness,0.0f,1.0f);
    {
        const std::array<RenderTextureBarrier,3> barriers{RenderTextureBarrier(&depth,RenderTextureLayout::SHADER_READ),
            RenderTextureBarrier(&attenuation,RenderTextureLayout::SHADER_READ),
            RenderTextureBarrier(shadow_visibility_.get(),RenderTextureLayout::GENERAL)};
        commands.barriers(RenderBarrierStage::COMPUTE,barriers.data(),uint32_t(barriers.size()));
        trace_set->setAccelerationStructure(0,tlas_.get());
        trace_set->setTexture(1,&depth,RenderTextureLayout::SHADER_READ);
        trace_set->setTexture(2,&attenuation,RenderTextureLayout::SHADER_READ);
        trace_set->setTexture(3,shadow_visibility_.get(),RenderTextureLayout::GENERAL);
        ShadowPush push{};
        Columns(inverse,push.inverse);
        for(int c=0;c<4;++c)push.light[c]=float((*light)[c]);
        push.size[0]=width_;push.size[1]=height_;push.frame=uint32_t(frame_);
        push.rays=softness>0?4:1;
        // A local light's radius in cm; the sun's cone half-angle in radians.
        push.softness=softness*((*light)[3]!=0?30.0f:0.02f);
        push.sky_depth=projection.infinity_depth+1e-7f;
        push.max_distance=20000;
        push.step=step;
        commands.setComputePipelineLayout(shadow_layout_.get());
        commands.setComputePushConstants(0,&push);
        commands.setComputeDescriptorSet(trace_set,0);
        commands.setPipeline(shadow_trace_.get());
        commands.dispatch((traced_width+7)/8,(traced_height+7)/8,1);
    }
    {
        const std::array<RenderTextureBarrier,2> barriers{RenderTextureBarrier(shadow_visibility_.get(),RenderTextureLayout::SHADER_READ),
            RenderTextureBarrier(shadow_output_.get(),RenderTextureLayout::GENERAL)};
        commands.barriers(RenderBarrierStage::COMPUTE,barriers.data(),uint32_t(barriers.size()));
        merge_set->setTexture(0,shadow_visibility_.get(),RenderTextureLayout::SHADER_READ);
        merge_set->setTexture(1,&depth,RenderTextureLayout::SHADER_READ);
        merge_set->setTexture(2,&attenuation,RenderTextureLayout::SHADER_READ);
        merge_set->setTexture(3,shadow_output_.get(),RenderTextureLayout::GENERAL);
        ShadowMergePush push{};
        push.size[0]=width_;push.size[1]=height_;
        // In traced texels; hard shadows still filter between traced points.
        push.blur_radius=softness>0?2:step>1?1:0;
        push.step=step;
        push.debug_view=settings_.shadow_debug;
        commands.setComputePipelineLayout(shadow_merge_layout_.get());
        commands.setComputePushConstants(0,&push);
        commands.setComputeDescriptorSet(merge_set,0);
        commands.setPipeline(pass);
        commands.dispatch((width_+7)/8,(height_+7)/8,1);
    }
    commands.barriers(RenderBarrierStage::COMPUTE|RenderBarrierStage::GRAPHICS,
        RenderTextureBarrier(shadow_output_.get(),RenderTextureLayout::SHADER_READ));
    attenuation_layout.Restore(commands);depth_layout.Restore(commands);
    Stamp(cb,kTimingShadows,true);
    if(++statistics_.shadow_frames==1||statistics_.shadow_frames%1800==0)
        REXLOG_INFO("Ray-traced shadows: {} frames; {} light at ({:.3f}, {:.3f}, {:.3f}) camera-relative, {} vertices",
                    statistics_.shadow_frames,(*light)[3]!=0?"local":"directional",(*light)[0],(*light)[1],(*light)[2],
                    used_vertices_);
    return shadow_output_;
}

}  // namespace gow3::renderer
