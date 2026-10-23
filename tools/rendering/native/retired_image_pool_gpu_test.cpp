#include "gpu_helpers.h"
#include "renderer/native/pass_transfer.h"
#include "renderer/native/resolve_memory.h"
#include "renderer/native/transfer_batch.h"
#include "renderer/native/gpu_work_queue.h"
#include "fullscreen_spv.h"
#include "resolve_spv.h"
#include "restore_depth_spv.h"
#include "memory_store_spv.h"
#include "memory_load_spv.h"
#include <iostream>
using namespace plume;
using namespace gow3::renderer;
namespace {
void Require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void Pixels(RenderDevice& device,RenderCommandQueue& queue,RenderTexture& image,std::array<float,4> expected) {
    const auto bytes=validation::ReadImage(device,queue,image,16);
    for(size_t offset=0;offset<bytes.size();offset+=16) {
        std::array<float,4> actual;std::memcpy(actual.data(),bytes.data()+offset,16);
        Require(actual==expected,"Reused image contains stale/missing texels or channels");
    }
}
VkImage Handle(RenderTexture& image){return static_cast<VulkanTexture&>(image).vk;}
struct DrainOnExit {
    NativeTransferBatch& batch;
    ~DrainOnExit(){try{batch.Flush();}catch(...){std::terminate();}}
};
}
int main() try {
    auto interface=CreateVulkanInterface();auto device=interface->createDevice();
    Require(bool(device),"Vulkan unavailable");auto queue=device->createCommandQueue(RenderCommandListType::DIRECT);
    NativeGpuWorkQueue work(*device,*queue,[](const NativeGpuWorkQueue::Completion&){},true);
    NativeTransferBatch batch(*device,*queue,&work);
    PassTransfer passes(*device,*queue,fullscreen_spv,resolve_spv,restore_depth_spv,&batch);
    const RenderRect rectangle(0,0,17,13);
    // Non-workgroup-aligned dimensions and a small descriptor window exercise
    // partial groups plus recursive disjoint rectangles spanning the full image.
    const auto layout=DescribeTexture({2u|(1u<<22),0x1000u|38u,16u|(12u<<13),0u,0u,1u<<9});
    const auto one_channel=DescribeTexture({2u|(1u<<22),0x1000u|2u,16u|(12u<<13),0u,0u,1u<<9});
    auto source=device->createTexture(RenderTextureDesc::ColorTarget(17,13,RenderFormat::R32G32B32A32_FLOAT));
    std::shared_ptr<RenderTexture> after_parent;
    {
        ResolveMemory memory(*device,*queue,memory_store_spv,memory_load_spv,1024*1024,1024,&batch);
        memory.SetImagePoolEnabled(true);DrainOnExit drain{batch};
        std::shared_ptr<RenderTexture> a,b;
        batch.Run([&] {
            passes.Clear(source.get(),nullptr,true,false,false,rectangle,{1,0,0,1},0);
            memory.Store(*source,layout,rectangle,0x1000);a=memory.LoadShared(layout,0x1000);
        });
        work.SubmitPending();
        batch.Run([&] {
            passes.Clear(source.get(),nullptr,true,false,false,rectangle,{0,0,1,1},0);
            memory.Store(*source,layout,rectangle,0x1000);b=memory.LoadShared(layout,0x1000);
        });
        work.SubmitPending();
        Require(work.inflight_batches()==2&&Handle(*a)!=Handle(*b)&&memory.ImagePoolStatistics().hits==0,
            "Unretired image reused across independent GPU batches");
        const auto original=Handle(*a);auto first_draw=a,later_draw=a;
        std::weak_ptr<RenderTexture> old_generation=a;a.reset();
        work.WaitOldest();first_draw.reset();
        Require(memory.ImagePoolStatistics().idle_images==0&&!old_generation.expired(),
            "First fence released a later draw's image owner");
        work.WaitOldest();
        Pixels(*device,*queue,*later_draw,{1,0,0,1});Pixels(*device,*queue,*b,{0,0,1,1});
        later_draw.reset();
        Require(old_generation.expired()&&memory.ImagePoolStatistics().idle_images==1,
            "Last retired owner failed to recycle the image");
        std::shared_ptr<RenderTexture> c;
        batch.Run([&]{c=memory.LoadShared(layout,0x1000);});
        Require(Handle(*c)==original&&memory.ImagePoolStatistics().hits==1,
            "Fully retired exact image was not reused");
        batch.Flush();Pixels(*device,*queue,*c,{0,0,1,1});
        // Recycled COPY_SOURCE layout must transition back to GENERAL. A
        // different input format must overwrite all output channels and texels.
        c.reset();
        batch.Run([&] {
            passes.Clear(source.get(),nullptr,true,false,false,rectangle,{1,0,0,0},0);
            memory.Store(*source,one_channel,rectangle,0x1000);c=memory.LoadShared(one_channel,0x1000);
        });
        Require(Handle(*c)==original&&memory.ImagePoolStatistics().hits==2,"Changed source format lost exact output pool key");
        batch.Flush();Pixels(*device,*queue,*c,{1,0,0,1});
        auto unpooled=memory.Load(one_channel,0x1000);
        Require(validation::ReadImage(*device,*queue,*unpooled,16)==validation::ReadImage(*device,*queue,*c,16),
            "Pooled decode differs from independent original allocation path");
        // Final external release is insufficient while an accepted transfer
        // packet still retains the same image. Old weak handles never revive.
        c.reset();std::weak_ptr<RenderTexture> transfer_owner;
        batch.Run([&]{c=memory.LoadShared(one_channel,0x1000);transfer_owner=c;});c.reset();
        Require(!transfer_owner.expired()&&memory.ImagePoolStatistics().idle_images==0,
            "Accepted transfer lost its only image lease before its fence");
        std::shared_ptr<RenderTexture> different;
        batch.Run([&]{different=memory.LoadShared(one_channel,0x1000);});
        Require(Handle(*different)!=original,"Unfenced recycled image admitted to pool");
        batch.Flush();Require(transfer_owner.expired(),"Completed packet did not retire its lease");
        Pixels(*device,*queue,*different,{1,0,0,1});
        // Outstanding images must not call a destroyed/replaced pool via this.
        after_parent=different;memory.SetImagePoolEnabled(false);
        Require(memory.ImagePoolStatistics().idle_images==0,"Disabled pool retained idle resources");
        memory.SetImagePoolEnabled(true);different.reset();b.reset();
        Require(memory.ImagePoolStatistics().idle_images==0,"Old pool lease entered replacement pool");
        batch.Run([&]{c=memory.LoadShared(one_channel,0x1000);});batch.Flush();
        Require(Handle(*c)!=Handle(*after_parent),"New pool reused a live old-pool image");
        Pixels(*device,*queue,*c,{1,0,0,1});
    }
    Pixels(*device,*queue,*after_parent,{1,0,0,1});after_parent.reset();
    Require(!work.pending(),"Pool fixture abandoned GPU ownership");
    std::cout<<"PASS: actual GPU old generations, two fences, exact image reuse, complete odd-extent/channel overwrite, parent teardown\n";
}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
