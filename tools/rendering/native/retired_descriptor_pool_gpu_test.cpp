#include "gpu_helpers.h"
#include "renderer/native/gpu_work_queue.h"
#include "renderer/native/pass_transfer.h"
#include "renderer/native/resolve_memory.h"
#include "renderer/native/transfer_batch.h"
#include "fullscreen_spv.h"
#include "memory_load_spv.h"
#include "memory_load_r32_spv.h"
#include "memory_load_rg32_spv.h"
#include "memory_store_spv.h"
#include "resolve_spv.h"
#include "restore_depth_spv.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <iostream>
#include <string_view>

using namespace plume;
using namespace gow3::renderer;
namespace {
constexpr uint32_t kWidth=17,kHeight=13,kCapacity=1024*1024;
uint64_t checks=0;
void Require(bool value,const char* message) {
    ++checks;if(!value)throw std::runtime_error(message);
}
struct DrainOnExit {
    NativeTransferBatch& batch;
    ~DrainOnExit(){try{batch.Flush();}catch(...){std::terminate();}}
};
TextureLayout Layout(uint32_t format,uint32_t endian=0) {
    return DescribeTexture({2u|(1u<<22),0x1000u|format|(endian<<6),
        (kWidth-1)|((kHeight-1)<<13),0u,0u,1u<<9});
}
void ExpectedStore(std::vector<uint8_t>& bytes,const TextureLayout& layout,
        uint32_t address,std::array<float,4> value,uint32_t stencil=0) {
    constexpr std::array<uint32_t,4> swaps{0,1,3,2};
    for(uint32_t y=0;y<kHeight;++y)for(uint32_t x=0;x<kWidth;++x) {
        const auto offset=uint32_t(TextureBlockOffset(layout,layout.levels.front(),x,y,0))+address;
        if(layout.format==2)bytes.at(offset^swaps[layout.endian])=value[0]==1.f?255:0;
        else if(layout.format==22) {
            // This fixture uses only exact depth endpoints, independent of the
            // production shader's interior depth rounding implementation.
            const uint32_t word=((value[0]==1.f?0xffffffu:0u)<<8)|stencil;
            for(uint32_t b=0;b<4;++b)bytes.at((offset+b)^swaps[layout.endian])=uint8_t(word>>(b*8));
        } else {
            Require(layout.format==38&&layout.endian==0,"Unexpected fixture scalar format");
            const auto* source=reinterpret_cast<const uint8_t*>(value.data());
            for(uint32_t b=0;b<16;++b)bytes.at(offset+b)=source[b];
        }
    }
}
void CompareImage(RenderDevice& device,RenderCommandQueue& queue,RenderTexture& candidate,
        RenderTexture& baseline,std::array<float,4> expected) {
    const auto actual=validation::ReadImage(device,queue,candidate,16);
    const auto reference=validation::ReadImage(device,queue,baseline,16);
    Require(actual==reference&&actual.size()==kWidth*kHeight*16,
        "Retired descriptor output differs from unpooled full image");
    for(size_t at=0;at<actual.size();at+=16) {
        std::array<float,4> pixel{};std::memcpy(pixel.data(),actual.data()+at,16);
        Require(pixel==expected,"Changed descriptor window/image left stale channels or texels");
    }
}
void CompareMemory(ResolveMemory& candidate,ResolveMemory& baseline,const std::vector<uint8_t>& expected) {
    const auto actual=candidate.ReadBytes(0,kCapacity),reference=baseline.ReadBytes(0,kCapacity);
    Require(actual==reference&&actual==expected&&actual.size()==kCapacity,
        "Pooled full physical bytes differ from unpooled path or independent scalar oracle");
}
void Conservation(const ResolveMemory::DescriptorPool::Statistics& s) {
    Require(s.requests==s.hits+s.misses&&s.live_sets==s.leased_sets+s.idle_sets&&
        s.live_sets<=128&&s.peak_live_sets<=128,"Descriptor backing/counter conservation failed");
}
void IdentityConservation(const ResolveMemory::IdentityViewReuseStatistics& s,bool enabled) {
    Require(s.enabled==enabled&&s.load_requests==s.load_reused+s.load_explicit_views&&
        s.load_explicit_views==s.load_refusals,"Identity view choice conservation failed");
    if(!enabled)Require(!s.load_requests&&!s.load_reused&&!s.load_explicit_views&&
        !s.load_refusals&&!s.store_bypasses,"Disabled identity view path recorded choices");
}
}
int main(int argc,char** argv) try {
    uint32_t capacity=64;
    bool reuse_identity_views=false;
    for(int i=1;i<argc;++i) {
        const std::string_view option=argv[i];
        if(option=="--async-submission-capacity=64")capacity=64;
        else if(option=="--async-submission-capacity=128")capacity=128;
        else if(option=="--reuse-resolve-identity-views")reuse_identity_views=true;
        else throw std::invalid_argument("Unknown descriptor fixture argument");
    }
    auto interface=CreateVulkanInterface();Require(bool(interface),"Vulkan interface unavailable");
    auto device=interface->createDevice();Require(bool(device),"Vulkan device unavailable");
    const auto& native=static_cast<VulkanDevice&>(*device);
    Require(native.physicalDeviceProperties.deviceType!=VK_PHYSICAL_DEVICE_TYPE_CPU,
        "Actual descriptor GPU fixture requires hardware, software Vulkan was selected");
    std::cout<<"DEVICE: hardware_required=true vendor="<<native.physicalDeviceProperties.vendorID
        <<" device="<<native.physicalDeviceProperties.deviceID<<" name="
        <<native.physicalDeviceProperties.deviceName<<'\n';
    auto queue=device->createCommandQueue(RenderCommandListType::DIRECT);
    NativeGpuWorkQueue work(*device,*queue,{},true,false,capacity);
    NativeTransferBatch batch(*device,*queue,&work);
    PassTransfer passes(*device,*queue,fullscreen_spv,resolve_spv,restore_depth_spv,&batch);
    ResolveMemory candidate(*device,*queue,memory_store_spv,memory_load_spv,kCapacity,1024,&batch);
    ResolveMemory baseline(*device,*queue,memory_store_spv,memory_load_spv,kCapacity,1024,&batch);
    candidate.SetDescriptorPoolEnabled(true);baseline.SetDescriptorPoolEnabled(false);
    candidate.SetIdentityViewReuseEnabled(reuse_identity_views);baseline.SetIdentityViewReuseEnabled(false);
    const RenderRect rect(0,0,kWidth,kHeight);
    const auto rgba=Layout(38),single=Layout(2,1),depth_layout=Layout(22);
    std::vector<uint8_t> expected(kCapacity,0x5a);
    candidate.Initialize(0,expected);baseline.Initialize(0,expected);
    std::array<std::unique_ptr<RenderTexture>,5> inputs;
    for(auto& image:inputs)image=device->createTexture(RenderTextureDesc::ColorTarget(
        kWidth,kHeight,RenderFormat::R32G32B32A32_FLOAT));
    std::array<std::unique_ptr<RenderTexture>,2> stencils;
    for(auto& image:stencils)image=device->createTexture(RenderTextureDesc::DepthTarget(
        kWidth,kHeight,RenderFormat::D32_FLOAT_S8_UINT));
    std::array<std::shared_ptr<RenderTexture>,5> outputs,references;
    // Declared last: a failing assertion first drains every accepted packet,
    // before destroying the referenced images, memories or owned device.
    DrainOnExit drain{batch};
    const auto color=[&](uint32_t row,const TextureLayout& layout,uint32_t address,
            std::array<float,4> value) {
        passes.Clear(inputs[row].get(),nullptr,true,false,false,rect,
            {value[0],value[1],value[2],value[3]},0);
        candidate.Store(*inputs[row],layout,rect,address);outputs[row]=candidate.LoadShared(layout,address);
        baseline.Store(*inputs[row],layout,rect,address);references[row]=baseline.LoadShared(layout,address);
    };
    batch.Run([&]{color(0,rgba,0x1000,{1,0,0,1});});work.SubmitPending();
    const auto first=candidate.DescriptorPoolStatistics();const auto first_frontier=work.accepted_work();
    Require(first.requests>2&&first.requests==first.allocations&&first.leased_sets==first.requests&&
        first.hits==0&&first.returns==0&&first.idle_sets==0,"First split transfer did not retain its descriptors");
    batch.Run([&]{color(1,rgba,0x6000,{0,0,1,1});});work.SubmitPending();
    const auto second=candidate.DescriptorPoolStatistics();
    Require(work.inflight_batches()==2&&work.completed_work()==0&&second.returns==0&&
        second.hits==0&&second.capacity_fallbacks==0&&second.leased_sets==second.requests,
        "Descriptor reused before either independently accepted packet retired");
    work.WaitOldest();const auto partial=candidate.DescriptorPoolStatistics();
    Require(work.completed_work()==first_frontier&&work.inflight_batches()==1&&
        partial.returns==first.requests&&partial.idle_sets==first.requests&&
        partial.leased_sets==second.requests-first.requests,
        "Oldest fence released a later packet or failed to return its own backings");
    batch.Run([&]{color(2,single,0xa000,{1,0,0,0});});work.SubmitPending();
    const auto reused=candidate.DescriptorPoolStatistics();
    Require(work.inflight_batches()==2&&reused.hits>0&&reused.returns==partial.returns,
        "Retired descriptors were not reused while the newer packet remained owned");
    batch.Flush();
    ExpectedStore(expected,rgba,0x1000,{1,0,0,1});ExpectedStore(expected,rgba,0x6000,{0,0,1,1});
    ExpectedStore(expected,single,0xa000,{1,0,0,0});
    CompareImage(*device,*queue,*outputs[0],*references[0],{1,0,0,1});
    CompareImage(*device,*queue,*outputs[1],*references[1],{0,0,1,1});
    CompareImage(*device,*queue,*outputs[2],*references[2],{1,0,0,1});
    CompareMemory(candidate,baseline,expected);
    // Rewrite the store family's third binding to two different actual stencil
    // views. Exact packed byte oracles catch a stale sampled-stencil descriptor.
    batch.Run([&] {
        for(uint32_t i=0;i<2;++i) {
            const uint32_t row=3+i,address=0x10000+i*0x4000,stencil=i?0xa6:0x35;
            const float z=i?0.f:1.f;
            passes.Clear(inputs[row].get(),stencils[i].get(),true,true,true,rect,{z,z,z,z},z,stencil);
            candidate.StoreDepth(*inputs[row],depth_layout,rect,address,*stencils[i]);
            outputs[row]=candidate.LoadShared(depth_layout,address);
            baseline.StoreDepth(*inputs[row],depth_layout,rect,address,*stencils[i]);
            references[row]=baseline.LoadShared(depth_layout,address);
            ExpectedStore(expected,depth_layout,address,{z,z,z,z},stencil);
        }
    });batch.Flush();
    CompareImage(*device,*queue,*outputs[3],*references[3],{1,1,1,1});
    CompareImage(*device,*queue,*outputs[4],*references[4],{0,0,0,0});
    CompareMemory(candidate,baseline,expected);
    // Deliberate application exception after an accepted descriptor reference:
    // Run must fence its prefix before the lease is reusable or error escapes.
    const auto prior=candidate.DescriptorPoolStatistics();bool rejected=false;
    std::shared_ptr<RenderTexture> accepted_prefix;
    try {batch.Run([&] {
        accepted_prefix=candidate.LoadShared(single,0xa000);
        throw std::runtime_error("intentional descriptor fixture prefix error");
    });} catch(const NativeTransferBatchError&) {rejected=true;}
    const auto after_error=candidate.DescriptorPoolStatistics();
    Require(rejected&&!work.pending()&&work.accepted_work()==work.completed_work()&&
        after_error.leased_sets==0&&after_error.returns>prior.returns&&after_error.hits>prior.hits,
        "Partial recording failure returned a live lease or abandoned accepted work");
    CompareImage(*device,*queue,*accepted_prefix,*references[2],{1,0,0,1});
    const auto reconfigure_before=candidate.DescriptorPoolStatistics();rejected=false;
    try {candidate.SetDescriptorPoolEnabled(false);}catch(const std::logic_error&){rejected=true;}
    Require(rejected&&candidate.DescriptorPoolStatistics().enabled&&
        candidate.DescriptorPoolStatistics().requests==reconfigure_before.requests,
        "Startup-only pool configuration mutated after dispatch");
    Conservation(candidate.DescriptorPoolStatistics());
    IdentityConservation(candidate.identity_view_reuse_statistics(),reuse_identity_views);
    IdentityConservation(baseline.identity_view_reuse_statistics(),false);
    if(reuse_identity_views) {
        const auto prior_view=candidate.identity_view_reuse_statistics();rejected=false;
        try {candidate.SetIdentityViewReuseEnabled(false);}catch(const std::logic_error&){rejected=true;}
        Require(rejected&&candidate.identity_view_reuse_statistics().enabled&&
            candidate.identity_view_reuse_statistics().load_requests==prior_view.load_requests,
            "Identity view configuration changed after dispatch");
        Require(prior_view.load_reused>0&&prior_view.load_refusals==0&&prior_view.store_bypasses>0,
            "Queued identity view coverage did not omit actual eligible load views");
        // Probe both optional compact outputs on the same actual hardware.
        // Plume standalone enables its supported core features, matching the
        // existing compact-alias fixture; unsupported formats stay RGBA32F.
        VkPhysicalDeviceFeatures features{};
        vkGetPhysicalDeviceFeatures(native.physicalDevice,&features);
        ResolveMemory compact_on(*device,*queue,memory_store_spv,memory_load_spv,kCapacity,1024,&batch);
        ResolveMemory compact_off(*device,*queue,memory_store_spv,memory_load_spv,kCapacity,1024,&batch);
        compact_on.SetDescriptorPoolEnabled(true);compact_off.SetDescriptorPoolEnabled(false);
        compact_on.SetIdentityViewReuseEnabled(true);compact_off.SetIdentityViewReuseEnabled(false);
        compact_on.EnableCompactLoadFormats(memory_load_r32_spv,memory_load_rg32_spv,
            features.shaderStorageImageExtendedFormats);
        compact_off.EnableCompactLoadFormats(memory_load_r32_spv,memory_load_rg32_spv,
            features.shaderStorageImageExtendedFormats);
        const auto support=compact_on.compact_load_statistics();
        Require(support.r32_supported==compact_off.compact_load_statistics().r32_supported&&
            support.rg32_supported==compact_off.compact_load_statistics().rg32_supported,
            "Identity-view reference changed compact format support");
        std::vector<uint8_t> compact_expected(kCapacity,0xa7);
        const std::array<TextureLayout,2> layouts{Layout(36),Layout(37)};
        const std::array<float,4> values{0.25f,0.75f,0.f,1.f};
        for(uint32_t i=0;i<2;++i)for(uint32_t y=0;y<kHeight;++y)for(uint32_t x=0;x<kWidth;++x) {
            const auto at=TextureBlockOffset(layouts[i],layouts[i].levels.front(),x,y,0)+0x30000+i*0x10000;
            std::memcpy(compact_expected.data()+at,values.data(),layouts[i].block.bytes);
        }
        compact_on.Initialize(0,compact_expected);compact_off.Initialize(0,compact_expected);
        std::array<std::shared_ptr<RenderTexture>,2> a,b;
        DrainOnExit compact_drain{batch};
        batch.Run([&] {for(uint32_t i=0;i<2;++i) {
            const uint32_t address=0x30000+i*0x10000;
            a[i]=compact_on.LoadShared(layouts[i],address);b[i]=compact_off.LoadShared(layouts[i],address);
        }});batch.Flush();
        for(uint32_t i=0;i<2;++i) {
            const auto expected_format=i==0&&support.r32_supported?RenderFormat::R32_FLOAT:
                i==1&&support.rg32_supported?RenderFormat::R32G32_FLOAT:RenderFormat::R32G32B32A32_FLOAT;
            Require(static_cast<VulkanTexture&>(*a[i]).desc.format==expected_format&&
                static_cast<VulkanTexture&>(*b[i]).desc.format==expected_format,
                "Identity-view compact load changed format or capability fallback");
            const uint32_t pixel_bytes=expected_format==RenderFormat::R32_FLOAT?4:
                expected_format==RenderFormat::R32G32_FLOAT?8:16;
            const auto actual=validation::ReadImage(*device,*queue,*a[i],pixel_bytes);
            const auto reference=validation::ReadImage(*device,*queue,*b[i],pixel_bytes);
            Require(actual==reference&&actual.size()==kWidth*kHeight*pixel_bytes,
                "Compact identity view differs from explicit reference");
            for(size_t at=0;at<actual.size();at+=pixel_bytes)
                Require(std::memcmp(actual.data()+at,values.data(),layouts[i].block.bytes)==0,
                    "Compact identity-view component bits differ from independent input oracle");
        }
        CompareMemory(compact_on,compact_off,compact_expected);
        const auto compact_views=compact_on.identity_view_reuse_statistics();
        IdentityConservation(compact_views,true);IdentityConservation(compact_off.identity_view_reuse_statistics(),false);
        Require(compact_views.load_reused>=2&&compact_views.load_refusals==0,
            "Compact identity-view positive choices were vacuous");
        Conservation(compact_on.DescriptorPoolStatistics());
        std::cout<<"COMPACT_IDENTITY_VIEWS: r32_supported="<<support.r32_supported
            <<" rg32_supported="<<support.rg32_supported<<" load_reused="<<compact_views.load_reused<<'\n';
    }
    // A batch without a unified work queue retains its original synchronous
    // descriptor allocation/wait path, despite the optional pool being enabled.
    {
        NativeTransferBatch synchronous(*device,*queue);
        PassTransfer sync_passes(*device,*queue,fullscreen_spv,resolve_spv,restore_depth_spv,&synchronous);
        ResolveMemory sync_on(*device,*queue,memory_store_spv,memory_load_spv,kCapacity,1024,&synchronous);
        ResolveMemory sync_off(*device,*queue,memory_store_spv,memory_load_spv,kCapacity,1024,&synchronous);
        sync_on.SetDescriptorPoolEnabled(true);sync_off.SetDescriptorPoolEnabled(false);
        sync_on.SetIdentityViewReuseEnabled(reuse_identity_views);sync_off.SetIdentityViewReuseEnabled(false);
        std::vector<uint8_t> sync_expected(kCapacity,0x91);
        sync_on.Initialize(0,sync_expected);sync_off.Initialize(0,sync_expected);
        std::shared_ptr<RenderTexture> a,b;DrainOnExit sync_drain{synchronous};
        synchronous.Run([&] {
            sync_passes.Clear(inputs[0].get(),nullptr,true,false,false,rect,{0,1,0,1},0);
            sync_on.Store(*inputs[0],rgba,rect,0x2000);a=sync_on.LoadShared(rgba,0x2000);
            sync_off.Store(*inputs[0],rgba,rect,0x2000);b=sync_off.LoadShared(rgba,0x2000);
        });
        const auto bypass=sync_on.DescriptorPoolStatistics();
        Require(bypass.enabled&&bypass.synchronous_bypasses>0&&bypass.requests==0&&
            bypass.allocations==0&&bypass.live_sets==0,"Non-unified synchronous dispatch entered the lease pool");
        ExpectedStore(sync_expected,rgba,0x2000,{0,1,0,1});
        CompareImage(*device,*queue,*a,*b,{0,1,0,1});CompareMemory(sync_on,sync_off,sync_expected);
        IdentityConservation(sync_on.identity_view_reuse_statistics(),reuse_identity_views);
        IdentityConservation(sync_off.identity_view_reuse_statistics(),false);
        if(reuse_identity_views)Require(sync_on.identity_view_reuse_statistics().load_reused>0&&
            sync_on.identity_view_reuse_statistics().store_bypasses>0,
            "Synchronous identity-view path lacked eligible load/store coverage");
    }
    Require(!work.pending()&&work.accepted_work()==work.completed_work(),"Fixture abandoned GPU ownership");
    const auto stats=candidate.DescriptorPoolStatistics();
    const auto views=candidate.identity_view_reuse_statistics();
    std::cout<<"PASS: "<<checks<<" actual hardware descriptor checks; capacity="<<capacity
        <<" requests="<<stats.requests<<" hits="<<stats.hits<<" allocations="<<stats.allocations
        <<" returns="<<stats.returns<<" peak_cached="<<stats.peak_live_sets
        <<" identity_views="<<views.enabled<<" load_view_requests="<<views.load_requests
        <<" load_view_reused="<<views.load_reused<<" load_view_refusals="<<views.load_refusals
        <<" store_view_bypasses="<<views.store_bypasses
        <<"; two in-flight packets, oldest-only retirement, odd split windows, all bindings, full "
        <<kCapacity<<" bytes, accepted-prefix exception, synchronous bypass, startup-only config\n";
}catch(const std::exception& error){std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}
