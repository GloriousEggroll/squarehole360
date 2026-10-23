#include "renderer/native/draw_descriptor_batch.h"
#include <algorithm>
#include <iostream>
#include <map>
#include <tuple>
#include <type_traits>
#include <vector>
using namespace gow3::renderer;
namespace {
void Require(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
template<class T>T Handle(uintptr_t value) {
    if constexpr(std::is_pointer_v<T>)return reinterpret_cast<T>(value);
    else return static_cast<T>(value);
}
struct Entry {
    VkDescriptorSet set;uint32_t binding,index;VkDescriptorType type;
    VkImageView view;VkSampler sampler;VkImageLayout layout;
    bool operator==(const Entry&)const=default;
};
std::vector<Entry> actual;
std::vector<uint32_t> chunks;
const VkDevice device=Handle<VkDevice>(0x1000);
VKAPI_ATTR void VKAPI_CALL Update(VkDevice received,uint32_t count,const VkWriteDescriptorSet* writes,
                                 uint32_t copies,const VkCopyDescriptorSet* copy) {
    Require(received==device&&count>0&&count<=64&&copies==0&&!copy,"Unexpected update arguments");
    chunks.push_back(count);
    for(uint32_t i=0;i<count;++i) {
        const auto& w=writes[i];
        Require(w.sType==VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET&&!w.pNext&&w.descriptorCount==1&&
            w.pImageInfo&&!w.pBufferInfo&&!w.pTexelBufferView,"Descriptor write has stale fields");
        const auto& info=*w.pImageInfo;
        actual.push_back({w.dstSet,w.dstBinding,w.dstArrayElement,w.descriptorType,
            info.imageView,info.sampler,info.imageLayout});
    }
}
std::vector<VkDescriptorSet> bound;
uint32_t binds=0;
VKAPI_ATTR void VKAPI_CALL Bind(VkCommandBuffer commands,VkPipelineBindPoint point,VkPipelineLayout layout,
                              uint32_t first,uint32_t count,const VkDescriptorSet* sets,
                              uint32_t offset_count,const uint32_t* offsets) {
    Require(commands==Handle<VkCommandBuffer>(0x2000)&&point==VK_PIPELINE_BIND_POINT_GRAPHICS&&
        layout==Handle<VkPipelineLayout>(0x3000)&&first==0&&offset_count==0&&!offsets,
        "Combined bind changed command/layout/dynamic offsets");
    bound.assign(sets,sets+count);++binds;
}
template<class F>void Reject(F&& f) {
    bool rejected=false;try{f();}catch(const std::exception&){rejected=true;}
    Require(rejected,"Malformed descriptor mapping was accepted");
}
// The oracle walks original descriptor ranges, not the production flattened
// tables, and emits one write at a time as the old Plume methods do.
Entry Legacy(const std::vector<VkDescriptorSetLayoutBinding>& ranges,VkDescriptorSet set,
             uint32_t index,uintptr_t resource) {
    uint32_t base=0;
    for(const auto& range:ranges) {
        if(index<base+range.descriptorCount) {
            const bool texture=range.descriptorType==VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            return {set,range.binding,index-base,range.descriptorType,
                texture?Handle<VkImageView>(resource):VK_NULL_HANDLE,
                texture?VK_NULL_HANDLE:Handle<VkSampler>(resource),
                texture?VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED};
        }
        base+=range.descriptorCount;
    }
    throw std::runtime_error("Oracle index outside ranges");
}
}
int main()try {
    static_assert(!std::is_copy_constructible_v<NativeDrawDescriptorBatch>);
    static_assert(!std::is_move_constructible_v<NativeDrawDescriptorBatch>);
    std::vector<VkDescriptorSetLayoutBinding> ranges{
        {7,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,32,VK_SHADER_STAGE_ALL,nullptr},
        {12,VK_DESCRIPTOR_TYPE_SAMPLER,32,VK_SHADER_STAGE_ALL,nullptr},
        {45,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,3,VK_SHADER_STAGE_ALL,nullptr},
        {99,VK_DESCRIPTOR_TYPE_SAMPLER,5,VK_SHADER_STAGE_ALL,nullptr}};
    std::vector<uint32_t> bases,indices;
    for(uint32_t binding=0;binding<ranges.size();++binding) {
        const auto base=uint32_t(bases.size());
        for(uint32_t n=0;n<ranges[binding].descriptorCount;++n) {
            bases.push_back(base);indices.push_back(binding);
        }
    }
    const NativeDescriptorTarget target{Handle<VkDescriptorSet>(0x4000),ranges,bases,indices};
    uint64_t checked=0;
    for(uint32_t count:{0u,1u,2u,31u,32u,63u,64u,65u,66u,127u,128u,129u,1000u}) {
        actual.clear();chunks.clear();std::vector<Entry> expected;
        NativeDrawDescriptorBatch batch(device,Update);
        for(uint32_t i=0;i<count;++i) {
            const uint32_t index=(i*31+i/7)%bases.size();
            const uintptr_t resource=0x5000+i;
            expected.push_back(Legacy(ranges,target.set,index,resource));
            if(expected.back().type==VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE)
                batch.Texture(target,index,Handle<VkImageView>(resource));
            else batch.Sampler(target,index,Handle<VkSampler>(resource));
        }
        batch.Commit();batch.Commit();
        Require(actual==expected,"Batch differs from independent legacy write sequence");
        Require(batch.committed_writes()==count&&batch.commits()==(count+63)/64,
            "Descriptor update count or fixed-capacity chunking is wrong");
        // Duplicate destinations must produce the same final descriptor table.
        std::map<std::tuple<VkDescriptorSet,uint32_t,uint32_t>,Entry> old_table,new_table;
        for(const auto& e:expected)old_table.insert_or_assign({e.set,e.binding,e.index},e);
        for(const auto& e:actual)new_table.insert_or_assign({e.set,e.binding,e.index},e);
        Require(old_table==new_table,"Duplicate destination final contents changed");
        checked+=count;
    }
    actual.clear();chunks.clear();
    {
        NativeDrawDescriptorBatch batch(device,Update);
        batch.Texture(target,31,Handle<VkImageView>(0x9000));
        // No borrowed pointer to the caller's temporary mapping or later entry
        // data survives Add. Stored image-info pointers remain object-local.
        const auto saved=ranges[0].binding;ranges[0].binding=1234;
        batch.Sampler(target,63,Handle<VkSampler>(0x9001));
        batch.Commit();ranges[0].binding=saved;
        Require(actual[0].binding==7&&actual[0].index==31&&actual[1].index==31,
            "Pending update retained borrowed layout or wrong high slot");
        Reject([&]{batch.Texture(target,72,Handle<VkImageView>(1));});
        Reject([&]{batch.Texture(target,32,Handle<VkImageView>(1));});
        auto invalid=target;invalid.index_bases=std::span(bases).first(1);
        Reject([&]{batch.Texture(invalid,1,Handle<VkImageView>(1));});
        auto original=bases[0];bases[0]=1;
        Reject([&]{batch.Texture(target,0,Handle<VkImageView>(1));});bases[0]=original;
        original=indices[0];indices[0]=999;
        Reject([&]{batch.Texture(target,0,Handle<VkImageView>(1));});indices[0]=original;
    }
    const auto calls=chunks.size();
    {NativeDrawDescriptorBatch abandoned(device,Update);abandoned.Texture(target,0,Handle<VkImageView>(1));}
    Require(chunks.size()==calls,"Destructor committed an abandoned draw after resource teardown");
    const std::array<VkDescriptorSet,5> sets{Handle<VkDescriptorSet>(1),Handle<VkDescriptorSet>(2),
        Handle<VkDescriptorSet>(3),Handle<VkDescriptorSet>(4),Handle<VkDescriptorSet>(5)};
    for(uint32_t count:{4u,5u}) {
        BindNativeDrawDescriptorSets(Handle<VkCommandBuffer>(0x2000),Handle<VkPipelineLayout>(0x3000),
            std::span(sets).first(count),Bind);
        Require(bound==std::vector(sets.begin(),sets.begin()+count),"Combined bind changed set ordering");
    }
    Reject([&]{BindNativeDrawDescriptorSets({}, {},std::span(sets).first(3),Bind);});
    Require(binds==2,"Bad graphics set count reached the driver");
    std::cout<<"PASS: "<<checked<<" legacy descriptor writes, sparse/high/multiple ranges, duplicates, bounded chunks, "
        "copied handles, abandoned-draw lifetime, invalid mappings, contiguous four/five-set binds\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
