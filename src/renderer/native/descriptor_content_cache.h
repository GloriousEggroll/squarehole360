#pragma once
#include "draw_descriptor_batch.h"
#include <vulkan/vulkan.h>
#include <array>
#include <bit>
#include <cstdint>
#include <stdexcept>

namespace gow3::renderer {
class NativeDescriptorGenerations {
public:
    explicit NativeDescriptorGenerations(uint64_t first=1):next_(first){}
    uint64_t Issue()noexcept {
        const auto result=next_;
        if(result)next_=result==UINT64_MAX?0:result+1;
        return result;
    }
private:
    uint64_t next_;
};
// Only descriptor metadata, never image/view/sampler ownership. Resource IDs
// must be unique nonzero Scene-local immutable binding generations. Recreated
// sets get fresh IDs even if their raw Vulkan handles have been recycled.
struct NativeDescriptorContent {
    uint64_t resource_generation=0;
    VkImageView view=VK_NULL_HANDLE;
    VkSampler sampler=VK_NULL_HANDLE;
    VkImageLayout layout=VK_IMAGE_LAYOUT_UNDEFINED;
    VkDescriptorType type=VK_DESCRIPTOR_TYPE_MAX_ENUM;
    uint32_t binding=0,array_element=0;
    bool operator==(const NativeDescriptorContent&)const=default;
};
// Validate the same real flattened Plume layout even when a write will be
// skipped. A cache hit never substitutes for descriptor-index/type validation.
inline NativeDescriptorContent NativeDrawDescriptorContent(const NativeDescriptorTarget& target,
    uint32_t index,uint64_t generation,VkDescriptorType expected,
    VkImageView view=VK_NULL_HANDLE,VkSampler sampler=VK_NULL_HANDLE) {
    if(index>=target.index_bases.size()||index>=target.binding_indices.size())
        throw std::out_of_range("Native draw descriptor index exceeds layout");
    const uint32_t binding_index=target.binding_indices[index];
    if(binding_index>=target.bindings.size())
        throw std::out_of_range("Native draw descriptor binding exceeds layout");
    const auto& binding=target.bindings[binding_index];
    const uint32_t base=target.index_bases[index];
    if(index<base||index-base>=binding.descriptorCount||binding.descriptorType!=expected)
        throw std::invalid_argument("Native draw descriptor layout does not match image/sampler");
    return {generation,view,sampler,expected==VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE?
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED,
        expected,binding.binding,index-base};
}
class NativeDescriptorContentTransaction;
class NativeDescriptorContentCache {
public:
    static constexpr uint32_t kSets=4,kSlots=32;
    NativeDescriptorContentCache()=default;
    NativeDescriptorContentCache(const NativeDescriptorContentCache&)=delete;
    NativeDescriptorContentCache& operator=(const NativeDescriptorContentCache&)=delete;
    NativeDescriptorContentCache(NativeDescriptorContentCache&&)=delete;
    NativeDescriptorContentCache& operator=(NativeDescriptorContentCache&&)=delete;
    bool Set(uint32_t index,uint64_t generation,VkDescriptorSet handle) {
        if(open_)throw std::logic_error("Descriptor set replacement during an open content transaction");
        if(index>=kSets||!generation||handle==VK_NULL_HANDLE)
            throw std::invalid_argument("Descriptor content set identity is invalid");
        auto& identity=sets_[index];
        if(identity.generation==generation&&identity.handle==handle)return false;
        identity={generation,handle};
        committed_[index].fill({});
        return true;
    }
private:
    friend class NativeDescriptorContentTransaction;
    struct SetIdentity {uint64_t generation=0;VkDescriptorSet handle=VK_NULL_HANDLE;};
    std::array<SetIdentity,kSets> sets_{};
    std::array<std::array<NativeDescriptorContent,kSlots>,kSets> committed_{};
    std::array<std::array<NativeDescriptorContent,kSlots>,kSets> pending_{};
    std::array<uint32_t,kSets> touched_{};
    bool open_=false;
};

// Select descriptor contents only after checking the real Plume mapping and
// retired DrawSlot ownership. Stage before the original write. Commit only
// after every original VkUpdateDescriptorSets call has returned. An exception
// or auto-flush failure invalidates all potentially changed entries, because
// some earlier writes may already have reached Vulkan. Unknown generation0
// always forces a real write and is never admitted to a future hit.
class NativeDescriptorContentTransaction {
public:
    explicit NativeDescriptorContentTransaction(NativeDescriptorContentCache& cache,
        uint64_t* aborted=nullptr):cache_(cache),aborted_(aborted) {
        if(cache_.open_)throw std::logic_error("Nested descriptor content transaction");
        cache_.open_=true;cache_.touched_.fill(0);
    }
    NativeDescriptorContentTransaction(const NativeDescriptorContentTransaction&)=delete;
    NativeDescriptorContentTransaction& operator=(const NativeDescriptorContentTransaction&)=delete;
    ~NativeDescriptorContentTransaction(){Abort();}
    bool NeedsWrite(uint32_t set,uint32_t slot,const NativeDescriptorContent& content) {
        if(done_||!cache_.open_)throw std::logic_error("Descriptor content transaction is closed");
        if(set>=cache_.kSets||slot>=cache_.kSlots||!cache_.sets_[set].generation)
            throw std::out_of_range("Descriptor content target is invalid");
        const bool image=content.type==VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        const bool sampler=content.type==VK_DESCRIPTOR_TYPE_SAMPLER;
        if((image&&(set==3||content.view==VK_NULL_HANDLE||content.sampler!=VK_NULL_HANDLE||
                    content.layout!=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL))||
           (sampler&&(set!=3||content.sampler==VK_NULL_HANDLE||content.view!=VK_NULL_HANDLE||
                      content.layout!=VK_IMAGE_LAYOUT_UNDEFINED))||(!image&&!sampler))
            throw std::invalid_argument("Descriptor content value is invalid");
        const uint32_t bit=uint32_t{1}<<slot;
        const auto& old=(cache_.touched_[set]&bit)?cache_.pending_[set][slot]:cache_.committed_[set][slot];
        if(content.resource_generation&&old.resource_generation&&old==content)return false;
        cache_.pending_[set][slot]=content;
        cache_.touched_[set]|=bit;
        return true;
    }
    // A baseline writer may receive a null native handle even when its wrapper
    // object is nonnull. Do not introduce a new candidate-only exception: mark
    // this destination unknown and let the original writer preserve behavior.
    void ForceWrite(uint32_t set,uint32_t slot) {
        if(done_||!cache_.open_)throw std::logic_error("Descriptor content transaction is closed");
        if(set>=cache_.kSets||slot>=cache_.kSlots||!cache_.sets_[set].generation)
            throw std::out_of_range("Descriptor content target is invalid");
        cache_.pending_[set][slot]={};cache_.touched_[set]|=uint32_t{1}<<slot;
    }
    void Commit() noexcept {
        if(done_)return;
        Visit([&](uint32_t set,uint32_t slot)noexcept {
            const auto& next=cache_.pending_[set][slot];
            cache_.committed_[set][slot]=next.resource_generation?next:NativeDescriptorContent{};
        });
        Close();
    }
    void Abort() noexcept {
        if(done_)return;
        if(aborted_)++*aborted_;
        Visit([&](uint32_t set,uint32_t slot)noexcept {cache_.committed_[set][slot]={};});
        Close();
    }
private:
    template<class F>void Visit(F&& visitor)noexcept {
        for(uint32_t set=0;set<cache_.kSets;++set) {
            auto bits=cache_.touched_[set];
            while(bits){const uint32_t slot=std::countr_zero(bits);bits&=bits-1;visitor(set,slot);}
        }
    }
    void Close()noexcept {cache_.touched_.fill(0);cache_.open_=false;done_=true;}
    NativeDescriptorContentCache& cache_;
    uint64_t* aborted_=nullptr;
    bool done_=false;
};
} // namespace gow3::renderer
