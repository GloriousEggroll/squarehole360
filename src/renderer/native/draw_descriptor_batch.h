#pragma once
#include <vulkan/vulkan.h>
#include <array>
#include <cstdint>
#include <span>
#include <stdexcept>

namespace gow3::renderer {
// Borrowed layout metadata, with the same flattened descriptor-index mapping
// used by Plume. This view is consumed immediately; no resource owner is stored.
struct NativeDescriptorTarget {
    VkDescriptorSet set;
    std::span<const VkDescriptorSetLayoutBinding> bindings;
    std::span<const uint32_t> index_bases;
    std::span<const uint32_t> binding_indices;
};

struct NativeDrawDescriptorStatistics {
    uint64_t draws=0,writes=0,update_calls=0,bind_calls=0;
};

// Current draw resources remain owned by its fenced DrawSlot and image caches.
// Store only copied handles here, never pointers into transient binding vectors.
// Ordinary draws have at most 32 image/sampler pairs. Longer diagnostic arrays
// flush in order at the bound, preserving repeated-slot last-write behavior.
class NativeDrawDescriptorBatch {
public:
    static constexpr uint32_t kCapacity=64;
    explicit NativeDrawDescriptorBatch(VkDevice device,
        PFN_vkUpdateDescriptorSets update)
        :device_(device),update_(update) {}
    NativeDrawDescriptorBatch(const NativeDrawDescriptorBatch&)=delete;
    NativeDrawDescriptorBatch& operator=(const NativeDrawDescriptorBatch&)=delete;
    NativeDrawDescriptorBatch(NativeDrawDescriptorBatch&&)=delete;
    NativeDrawDescriptorBatch& operator=(NativeDrawDescriptorBatch&&)=delete;

    void Texture(const NativeDescriptorTarget& target,uint32_t index,VkImageView view) {
        VkDescriptorImageInfo image{};
        image.imageLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        image.imageView=view;
        Add(target,index,image,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
    }
    void Sampler(const NativeDescriptorTarget& target,uint32_t index,VkSampler sampler) {
        VkDescriptorImageInfo image{};
        image.sampler=sampler;
        Add(target,index,image,VK_DESCRIPTOR_TYPE_SAMPLER);
    }
    void Commit() {
        if(!count_)return;
        update_(device_,count_,writes_.data(),0,nullptr);
        committed_writes_+=count_;++commits_;count_=0;
    }
    uint64_t committed_writes()const{return committed_writes_;}
    uint64_t commits()const{return commits_;}
private:
    void Add(const NativeDescriptorTarget& target,uint32_t index,
             const VkDescriptorImageInfo& image,VkDescriptorType expected) {
        if(index>=target.index_bases.size()||index>=target.binding_indices.size())
            throw std::out_of_range("Native draw descriptor index exceeds layout");
        const uint32_t binding_index=target.binding_indices[index];
        if(binding_index>=target.bindings.size())
            throw std::out_of_range("Native draw descriptor binding exceeds layout");
        const auto& binding=target.bindings[binding_index];
        const uint32_t base=target.index_bases[index];
        if(index<base||index-base>=binding.descriptorCount||binding.descriptorType!=expected)
            throw std::invalid_argument("Native draw descriptor layout does not match image/sampler");
        if(count_==kCapacity)Commit();
        images_[count_]=image;
        auto& write=writes_[count_];
        write={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet=target.set;
        write.dstBinding=binding.binding;
        write.dstArrayElement=index-base;
        write.descriptorCount=1;
        write.descriptorType=binding.descriptorType;
        write.pImageInfo=&images_[count_];
        ++count_;
    }
    VkDevice device_;
    PFN_vkUpdateDescriptorSets update_;
    // Only initialized elements are ever submitted. Avoid clearing both full
    // arrays for every draw when most games bind a small subset of the slots.
    std::array<VkWriteDescriptorSet,kCapacity> writes_;
    std::array<VkDescriptorImageInfo,kCapacity> images_;
    uint32_t count_=0;
    uint64_t committed_writes_=0,commits_=0;
};

inline void BindNativeDrawDescriptorSets(VkCommandBuffer commands,VkPipelineLayout layout,
    std::span<const VkDescriptorSet> sets,
    PFN_vkCmdBindDescriptorSets bind) {
    // Scene's complete consecutive graphics sets: 2D, 3D, cube, samplers, and
    // optional FSI EDRAM. Layout selection remains at its original call site.
    if(sets.size()!=4&&sets.size()!=5)
        throw std::invalid_argument("Native draw requires four or five descriptor sets");
    bind(commands,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,0,
         static_cast<uint32_t>(sets.size()),sets.data(),0,nullptr);
}
} // namespace gow3::renderer
