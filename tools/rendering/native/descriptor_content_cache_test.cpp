#include "renderer/native/descriptor_content_cache.h"
#include <array>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <type_traits>

using namespace gow3::renderer;
namespace {
uint64_t checks=0,writes=0,skips=0;
void Require(bool condition) {
    ++checks;
    if(!condition)throw std::runtime_error("Descriptor content independent oracle mismatch");
}
template<class T>T Handle(uintptr_t value) {
    if constexpr(std::is_pointer_v<T>)return reinterpret_cast<T>(value);
    else return static_cast<T>(value);
}
NativeDescriptorContent Image(uint64_t generation,uintptr_t handle,uint32_t element=0,uint32_t binding=0) {
    return {generation,Handle<VkImageView>(handle),VK_NULL_HANDLE,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,binding,element};
}
NativeDescriptorContent Sampler(uint64_t generation,uintptr_t handle,uint32_t element=0,uint32_t binding=0) {
    return {generation,VK_NULL_HANDLE,Handle<VkSampler>(handle),
        VK_IMAGE_LAYOUT_UNDEFINED,VK_DESCRIPTOR_TYPE_SAMPLER,binding,element};
}
struct PhysicalDescriptor {
    uint64_t generation=0;
    VkImageView view=VK_NULL_HANDLE;
    VkSampler sampler=VK_NULL_HANDLE;
    VkImageLayout layout=VK_IMAGE_LAYOUT_UNDEFINED;
    VkDescriptorType type=VK_DESCRIPTOR_TYPE_MAX_ENUM;
    uint32_t binding=0,element=0;
};
struct Oracle {
    std::array<std::array<PhysicalDescriptor,32>,4> contents{};
    // This is actual modeled descriptor storage updated only when the original
    // write executes; no cache/transaction state is consulted here.
    void Write(unsigned set,unsigned slot,const NativeDescriptorContent& wanted) {
        ++writes;
        contents[set][slot]={wanted.resource_generation,wanted.view,wanted.sampler,
            wanted.layout,wanted.type,wanted.binding,wanted.array_element};
    }
    void CheckRead(unsigned set,unsigned slot,const NativeDescriptorContent& wanted)const {
        const auto& current=contents[set][slot];
        Require(current.generation==wanted.resource_generation&&current.view==wanted.view&&
            current.sampler==wanted.sampler&&current.layout==wanted.layout&&current.type==wanted.type&&
            current.binding==wanted.binding&&current.element==wanted.array_element);
    }
};
void Configure(NativeDescriptorContentCache& cache,uint64_t base) {
    for(unsigned set=0;set<4;++set)cache.Set(set,base+set,Handle<VkDescriptorSet>(base+set));
}
void Write(NativeDescriptorContentTransaction& tx,Oracle& oracle,unsigned set,unsigned slot,
           const NativeDescriptorContent& wanted) {
    if(tx.NeedsWrite(set,slot,wanted))oracle.Write(set,slot,wanted);
    else {++skips;oracle.CheckRead(set,slot,wanted);}
}
template<class F>void Reject(F&& f) {
    bool refused=false;
    try{f();}catch(const std::exception&){refused=true;}
    Require(refused);
}
}
int main() {
    static_assert(std::is_trivially_copyable_v<NativeDescriptorContent>);
    // Generation IDs cannot recycle at overflow;0 forces ordinary writes.
    NativeDescriptorGenerations ids(UINT64_MAX-1);
    Require(ids.Issue()==UINT64_MAX-1&&ids.Issue()==UINT64_MAX&&ids.Issue()==0&&ids.Issue()==0);
    NativeDescriptorContentCache cache;
    Configure(cache,100);
    Oracle oracle;
    const auto a=Image(1,17),b=Image(2,18),sampler=Sampler(1,19);
    // The factory must validate the actual flattened mapping even on a hit.
    // Independent expected binding/array indexes cover sparse arrays and
    // malformed real layouts; no physical Vulkan object is dereferenced.
    std::array<VkDescriptorSetLayoutBinding,2> bindings{};
    bindings[0].binding=7;bindings[0].descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    bindings[0].descriptorCount=32;bindings[1].binding=9;
    bindings[1].descriptorType=VK_DESCRIPTOR_TYPE_SAMPLER;bindings[1].descriptorCount=32;
    std::array<uint32_t,64> bases{},indices{};
    for(unsigned i=32;i<64;++i){bases[i]=32;indices[i]=1;}
    NativeDescriptorTarget target{Handle<VkDescriptorSet>(100),bindings,bases,indices};
    const auto actual_image=NativeDrawDescriptorContent(target,31,11,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
        Handle<VkImageView>(17));
    Require(actual_image==Image(11,17,31,7));
    const auto actual_sampler=NativeDrawDescriptorContent(target,63,11,VK_DESCRIPTOR_TYPE_SAMPLER,
        VK_NULL_HANDLE,Handle<VkSampler>(19));
    Require(actual_sampler==Sampler(11,19,31,9));
    Reject([&]{NativeDrawDescriptorContent(target,64,11,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);});
    Reject([&]{NativeDrawDescriptorContent(target,31,11,VK_DESCRIPTOR_TYPE_SAMPLER);});
    bases[31]=32;Reject([&]{NativeDrawDescriptorContent(target,31,11,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);});
    bases[31]=0;indices[31]=2;Reject([&]{NativeDrawDescriptorContent(target,31,11,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);});
    indices[31]=0;bindings[0].descriptorCount=31;
    Reject([&]{NativeDrawDescriptorContent(target,31,11,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);});
    bindings[0].descriptorCount=32;
    {
        NativeDescriptorContentTransaction tx(cache);
        Write(tx,oracle,0,0,a);Write(tx,oracle,3,0,sampler);tx.Commit();
    }
    {
        NativeDescriptorContentTransaction tx(cache);
        Require(!tx.NeedsWrite(0,0,a)&&!tx.NeedsWrite(3,0,sampler));
        tx.Commit();oracle.CheckRead(0,0,a);oracle.CheckRead(3,0,sampler);
    }
    // A physical raw-handle ABA has identical numeric VkImageView bits but a
    // new immutable object generation, so a fresh original write is necessary.
    const auto recycled=Image(3,17);
    {
        NativeDescriptorContentTransaction tx(cache);
        Require(tx.NeedsWrite(0,0,recycled));oracle.Write(0,0,recycled);tx.Commit();
    }
    // Recreated descriptor set with the same raw handle must invalidate its
    // cache. Same layout/set identity retains the committed stamp otherwise.
    cache.Set(0,100,Handle<VkDescriptorSet>(100));
    {NativeDescriptorContentTransaction tx(cache);Require(!tx.NeedsWrite(0,0,recycled));tx.Commit();}
    cache.Set(0,1000,Handle<VkDescriptorSet>(100));
    {NativeDescriptorContentTransaction tx(cache);Require(tx.NeedsWrite(0,0,recycled));oracle.Write(0,0,recycled);tx.Commit();}
    // Exact flattened binding/index/type/layout forms the content address.
    auto mapped=recycled;mapped.binding=4;mapped.array_element=7;
    {NativeDescriptorContentTransaction tx(cache);Require(tx.NeedsWrite(0,0,mapped));oracle.Write(0,0,mapped);tx.Commit();}
    // Repeated-slot writes retain actual last-write behavior even if a real
    // batch auto-flushes before the full transaction's final commit.
    {
        NativeDescriptorContentTransaction tx(cache);
        Write(tx,oracle,0,0,a);Write(tx,oracle,0,0,b);Write(tx,oracle,0,0,a);
        Require(!tx.NeedsWrite(0,0,a));tx.Commit();oracle.CheckRead(0,0,a);
    }
    // Partial actual update followed by failure cannot leave the old committed
    // stamp trusted. Restoring A must execute even though it was cached earlier.
    try {
        NativeDescriptorContentTransaction tx(cache);
        Require(tx.NeedsWrite(0,0,b));oracle.Write(0,0,b);
        Require(tx.NeedsWrite(0,31,Image(4,21,31)));
        throw std::runtime_error("simulated second update/validation failure");
    }catch(const std::runtime_error&){}
    {
        NativeDescriptorContentTransaction tx(cache);
        Require(tx.NeedsWrite(0,0,a));oracle.Write(0,0,a);
        Require(tx.NeedsWrite(0,31,Image(4,21,31)));oracle.Write(0,31,Image(4,21,31));tx.Commit();
    }
    // Unknown generation is never admitted, even when its raw handle is stable.
    for(unsigned n=0;n<3;++n) {
        NativeDescriptorContentTransaction tx(cache);
        Require(tx.NeedsWrite(0,0,Image(0,17)));oracle.Write(0,0,Image(0,17));tx.Commit();
    }
    {NativeDescriptorContentTransaction tx(cache);Require(tx.NeedsWrite(0,0,a));oracle.Write(0,0,a);tx.Commit();}
    // Null native handles preserve the ordinary writer path and leave unknown
    // metadata. A later valid value must write even if the previous cached
    // generation was equal. Failed/partial unknown writes also invalidate.
    {
        NativeDescriptorContentTransaction tx(cache);
        tx.ForceWrite(0,0);oracle.Write(0,0,Image(0,0));tx.Commit();
    }
    {NativeDescriptorContentTransaction tx(cache);Write(tx,oracle,0,0,a);tx.Commit();}
    uint64_t aborts=0;
    {
        NativeDescriptorContentTransaction tx(cache,&aborts);
        tx.ForceWrite(0,0);oracle.Write(0,0,Image(0,0));
    }
    Require(aborts==1);
    {NativeDescriptorContentTransaction tx(cache,&aborts);Write(tx,oracle,0,0,a);tx.Commit();}
    Require(aborts==1);
    // Metadata stores no extra resource ownership and cannot prevent normal
    // cache retirement. Destroy/recreate dormant objects is a fresh generation.
    std::weak_ptr<int> weak;
    {
        auto owner=std::make_shared<int>(9);weak=owner;
        NativeDescriptorContentTransaction tx(cache);Write(tx,oracle,1,11,Image(5,23,11));tx.Commit();
    }
    Require(weak.expired());
    {NativeDescriptorContentTransaction tx(cache);Require(tx.NeedsWrite(1,11,Image(6,23,11)));oracle.Write(1,11,Image(6,23,11));tx.Commit();}
    // Invalid forms are rejected without publishing a false successful stamp.
    Reject([&]{cache.Set(4,1,Handle<VkDescriptorSet>(1));});
    Reject([&]{cache.Set(0,0,Handle<VkDescriptorSet>(1));});
    Reject([&]{cache.Set(0,1,VK_NULL_HANDLE);});
    {
        NativeDescriptorContentTransaction tx(cache);
        Reject([&]{NativeDescriptorContentTransaction nested(cache);});
        Reject([&]{cache.Set(0,2,Handle<VkDescriptorSet>(2));});
        Reject([&]{tx.NeedsWrite(4,0,a);});Reject([&]{tx.NeedsWrite(0,32,a);});
        Reject([&]{tx.NeedsWrite(3,0,a);});Reject([&]{tx.NeedsWrite(0,0,sampler);});
        auto invalid=a;invalid.layout=VK_IMAGE_LAYOUT_GENERAL;
        Reject([&]{tx.NeedsWrite(0,0,invalid);});
        invalid=a;invalid.view=VK_NULL_HANDLE;Reject([&]{tx.NeedsWrite(0,0,invalid);});
        invalid=a;invalid.type=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;Reject([&]{tx.NeedsWrite(0,0,invalid);});
        tx.Commit();Reject([&]{tx.NeedsWrite(0,0,a);});
    }
    // Independent modeled physical descriptors over retired64/128 DrawSlots.
    // Reuse produces nonvacuous write skips; handle-preserving generation
    // replacement and interrupted writes still reproduce every active value.
    for(unsigned capacity:{64u,128u}) {
        auto caches=std::make_unique<NativeDescriptorContentCache[]>(capacity);
        auto actual=std::make_unique<Oracle[]>(capacity);
        for(unsigned slot=0;slot<capacity;++slot)Configure(caches[slot],10000+slot*4);
        for(unsigned frame=0;frame<12;++frame)for(unsigned slot=0;slot<capacity;++slot) {
            NativeDescriptorContentTransaction tx(caches[slot]);
            for(unsigned texture_slot:{0u,7u,31u}) {
                const unsigned set=(slot+texture_slot)%3;
                const uint64_t resource=100+slot*32+texture_slot+(frame>=6?10000:0);
                const auto image=Image(resource,100000+slot*32+texture_slot,texture_slot);
                const auto sample=Sampler(resource,200000+slot*32+texture_slot,texture_slot);
                Write(tx,actual[slot],set,texture_slot,image);
                Write(tx,actual[slot],3,texture_slot,sample);
                actual[slot].CheckRead(set,texture_slot,image);actual[slot].CheckRead(3,texture_slot,sample);
            }
            tx.Commit();
        }
    }
    Require(skips>10000&&writes>2000);
    std::cout<<"Descriptor content pure oracle passed "<<checks<<" checks;writes="<<writes<<",skips="<<skips
             <<",metadata_bytes_per_draw_slot="<<sizeof(NativeDescriptorContentCache)<<"\n";
}
