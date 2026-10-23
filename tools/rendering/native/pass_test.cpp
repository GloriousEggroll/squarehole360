#include "gpu_helpers.h"
#include "renderer/native/pass_transfer.h"
#include "renderer/native/resolve_memory.h"
#include "renderer/native/resolve_state.h"
#include "fullscreen_spv.h"
#include "resolve_spv.h"
#include "memory_store_spv.h"
#include "memory_load_spv.h"
#include "restore_depth_spv.h"
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
using namespace plume;
using gow3::renderer::PassTransfer;
int main() try {
    auto interface = CreateVulkanInterface();
    auto device = interface ? interface->createDevice() : nullptr;
    if (!device) throw std::runtime_error("Vulkan device unavailable");
    auto queue = device->createCommandQueue(RenderCommandListType::DIRECT);
    PassTransfer pass(*device, *queue, fullscreen_spv, resolve_spv,restore_depth_spv);
    constexpr auto format = RenderFormat::R32G32B32A32_FLOAT;
    auto a = device->createTexture(RenderTextureDesc::ColorTarget(16, 16, format));
    auto b = device->createTexture(RenderTextureDesc::ColorTarget(16, 16, format));
    auto c = device->createTexture(RenderTextureDesc::ColorTarget(16, 16, format));
    auto depth = device->createTexture(RenderTextureDesc::DepthTarget(16, 16, RenderFormat::D32_FLOAT_S8_UINT));
    const RenderRect full(0, 0, 16, 16);
    size_t checks = 0;
    auto verify = [&](RenderTexture& texture, auto expected) {
        auto bytes = validation::ReadImage(*device, *queue, texture, 16);
        const auto& d=static_cast<VulkanTexture&>(texture).desc;
        for (uint32_t y=0; y<d.height; ++y) for (uint32_t x=0; x<d.width; ++x) {
            std::array<float, 4> actual;
            std::memcpy(actual.data(), bytes.data() + (y*d.width+x)*16, 16);
            auto want = expected(x,y);
            for (uint32_t k=0; k<4; ++k) {
                if (!std::isfinite(actual[k]) || std::abs(actual[k]-want[k]) > 1e-7f)
                    throw std::runtime_error("Pass result mismatch at " + std::to_string(x) + "," + std::to_string(y)
                        + " channel " + std::to_string(k) + ": " + std::to_string(actual[k]) + " != " + std::to_string(want[k]));
                ++checks;
            }
        }
    };
    const std::array<float,4> base{.25f,.5f,.75f,1}, patch{1,2,4,.5f};
    // Guest swap fetches commonly select B,G,R,1. The presentation provider
    // receives display RGB, even on hosts lacking image-view swizzle support.
    pass.Clear(a.get(),nullptr,true,false,false,full,{.125f,.25f,.5f,.75f},0);
    pass.Resolve(*a,*b,full,full,0,PassTransfer::Conversion::Color,0xa0a);
    verify(*b,[&](auto,auto){return std::array<float,4>{.5f,.25f,.125f,1};});
    pass.Resolve(*a,*b,full,full,0,PassTransfer::Conversion::Color,4|(3<<3)|(3<<6)|(5<<9));
    verify(*b,[&](auto,auto){return std::array<float,4>{0,.75f,.75f,1};});
    bool swizzle_rejected=false;
    try {pass.Resolve(*a,*b,full,full,0,PassTransfer::Conversion::Color,6);}
    catch(const std::runtime_error&){swizzle_rejected=true;}
    if(!swizzle_rejected)throw std::runtime_error("Invalid presentation swizzle was accepted");
    // Destination channel swap is independent of subsequent fetch swizzle,
    // preserves alpha, and must happen before packed bytes are stored.
    for(uint32_t selector=0;selector<8;++selector) {
        const std::array<uint32_t,6> fetch{0,0,0,selector<<1,0,0};
        if(gow3::renderer::ResolveDestinationSwap(fetch,0)!=(selector!=0)||
           gow3::renderer::ResolveDestinationSwap(fetch,4))
            throw std::runtime_error("Resolve destination swap derivation mismatch");
    }
    pass.Resolve(*a,*b,full,full,0,PassTransfer::Conversion::Color,0x60a);
    verify(*b,[&](auto,auto){return std::array<float,4>{.5f,.25f,.125f,.75f};});
    {
        gow3::renderer::ResolveMemory storage(*device,*queue,memory_store_spv,memory_load_spv,8192);
        const std::array<uint32_t,6> fetch{2|(1u<<22),0x1000|6,15|(15<<13),0x60a<<1,0,1<<9};
        storage.Store(*b,gow3::renderer::DescribeTexture(fetch),full,0x1000);
        if(storage.ReadBytes(0x1000,4)!=std::vector<uint8_t>{128,64,32,191})
            throw std::runtime_error("Resolve swap did not reach packed physical bytes");
    }
    pass.Resolve(*b,*c,full,full,0,PassTransfer::Conversion::Color,0xa0a);
    verify(*c,[&](auto,auto){return std::array<float,4>{.125f,.25f,.5f,1};});
    pass.Clear(a.get(), nullptr, true,false,false,full,{base[0],base[1],base[2],base[3]},0);
    pass.Clear(a.get(), nullptr, true,false,false,{3,4,8,10},{patch[0],patch[1],patch[2],patch[3]},0);
    verify(*a,[&](auto x,auto y){return x>=3&&x<8&&y>=4&&y<10?patch:base;});
    pass.Clear(b.get(),nullptr,true,false,false,full,{-1,-2,-3,-4},0);
    pass.Resolve(*a,*b,{3,4,8,10},{1,2,6,8},-1);
    verify(*b,[&](auto x,auto y){return x>=1&&x<6&&y>=2&&y<8?std::array<float,4>{.5f,1,2,.25f}:std::array<float,4>{-1,-2,-3,-4};});
    // A second resolve reads the previous GPU write without a CPU re-upload.
    pass.Resolve(*b,*c,full,full,1);
    verify(*c,[&](auto x,auto y){return x>=1&&x<6&&y>=2&&y<8?patch:std::array<float,4>{-2,-4,-6,-8};});
    // Halfway rounding, exponent, depth-only sampled view, and float24 denormals.
    const std::array<float,7> values{0,1,0.5f,0.25f,std::ldexp(1.f,-34),std::ldexp(1.f,-15),std::ldexp(1.f,-35)};
    for (float v : values) {
        pass.Clear(nullptr,depth.get(),false,true,true,full,{},v,0x57);
        pass.Resolve(*depth,*c,full,full,0,PassTransfer::Conversion::DepthFloat24);
        const float want = v==std::ldexp(1.f,-35)?0:v;
        verify(*c,[&](auto,auto){return std::array<float,4>{want,want,want,want};});
    }
    pass.Clear(nullptr,depth.get(),false,true,false,full,{},.5f);
    pass.Resolve(*depth,*c,full,full,0,PassTransfer::Conversion::DepthUNorm24);
    verify(*c,[&](auto,auto){const float v=8388609.f/16777216.f;return std::array<float,4>{v,v,v,v};});
    pass.Clear(a.get(),nullptr,true,false,false,full,{-.25f,.5f,2,.5f},0);
    pass.Resolve(*a,*c,full,full,0,PassTransfer::Conversion::UNorm10);
    verify(*c,[&](auto,auto){return std::array<float,4>{0,512.f/1023,1,2.f/3};});
    const std::array<std::array<float,3>,5> float7_samples{{{0,0,0},{1.f/512,1,1.f/512},{.25f,128,.25f},{.5f,256,.5f},{31.875f,1023,31.875f}}};
    for(const auto& v:float7_samples) {
        pass.Clear(a.get(),nullptr,true,false,false,full,{v[0],v[0],v[0],1},0);
        pass.Resolve(*a,*b,full,full,0,PassTransfer::Conversion::Float7e3ToUNorm10);
        verify(*b,[&](auto,auto){return std::array<float,4>{v[1]/1023,v[1]/1023,v[1]/1023,1};});
        pass.Resolve(*b,*c,full,full,0,PassTransfer::Conversion::UNorm10ToFloat7e3);
        verify(*c,[&](auto,auto){return std::array<float,4>{v[2],v[2],v[2],1};});
    }
    // Byte-exact reinterpretation of RGBA8 and RGB10A2, including alpha bits.
    pass.Clear(a.get(),nullptr,true,false,false,full,{17.f/255,85.f/255,170.f/255,238.f/255},0);
    pass.Resolve(*a,*b,full,full,0,PassTransfer::Conversion::UNorm8ToUNorm10);
    constexpr uint32_t packed=0xeeaa5511;
    verify(*b,[&](auto,auto){return std::array<float,4>{float(packed&1023)/1023,float((packed>>10)&1023)/1023,float((packed>>20)&1023)/1023,float(packed>>30)/3};});
    pass.Resolve(*b,*c,full,full,0,PassTransfer::Conversion::UNorm10ToUNorm8);
    verify(*c,[&](auto,auto){return std::array<float,4>{17.f/255,85.f/255,170.f/255,238.f/255};});
    // Write a varying 64x64 RGBA8 image into tiled GPU memory, then read it
    // through a narrower D24 view with a different pitch and endian mode.
    auto pattern=device->createTexture(RenderTextureDesc::ColorTarget(64,64,format));
    auto upload=device->createBuffer(RenderBufferDesc::UploadBuffer(64*64*16));
    auto mapped=static_cast<float*>(upload->map());
    for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x) {
        const std::array<float,4> v{x/255.f,y/255.f,float(x^y)/255.f,1};std::memcpy(mapped+(y*64+x)*4,v.data(),16);
    }
    validation::Flush(*upload);upload->unmap();
    auto commands=queue->createCommandList();auto fence=device->createCommandFence();commands->begin();
    commands->barriers(RenderBarrierStage::COPY,RenderTextureBarrier(pattern.get(),RenderTextureLayout::COPY_DEST));
    commands->copyTextureRegion(RenderTextureCopyLocation::Subresource(pattern.get(),0),RenderTextureCopyLocation::PlacedFootprint(upload.get(),format,64,64,1,64),0,0,0,nullptr);
    commands->end();queue->executeCommandLists(commands.get(),fence.get());queue->waitForCommandFence(fence.get());
    for(bool tiled:{false,true})for(unsigned endian=0;endian<4;++endian) {
        // Force 4 KiB descriptor windows even on hardware supporting larger
        // ranges. The 64x64 allocation must split safely across windows.
        gow3::renderer::ResolveMemory storage(*device,*queue,memory_store_spv,memory_load_spv,65536,4096);
        std::array<uint32_t,6> fetch{2u|(2u<<22)|(uint32_t(tiled)<<31),0x1000u|6|(endian<<6),63|(63<<13),0,0,1<<9};
        const auto initial=gow3::renderer::DescribeTexture(fetch);
        if(!tiled&&!endian) {
            // Fill the footprint cache just below its bound. The full image
            // enters last, then its first recursive descriptor-window split
            // evicts the cache. No caller may retain ranges across that split.
            auto probe=fetch;probe[2]=0;
            for(uint32_t i=0;i<2047;++i) {
                probe[3]=i<<13;
                const auto tiny=gow3::renderer::DescribeTexture(probe);
                if(storage.AnyDefined(tiny)||storage.FullyDefined(tiny)||storage.Version(tiny))
                    throw std::runtime_error("Footprint cache populated undefined texture bytes");
            }
        }
        storage.Store(*pattern,initial,{0,0,64,64});
        std::vector<uint8_t> reference(65536);constexpr unsigned swaps[]{0,1,3,2};
        for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x) {
            const auto offset=gow3::renderer::TextureBlockOffset(initial,initial.levels[0],x,y,0);
            const std::array<uint8_t,4> v{uint8_t(x),uint8_t(y),uint8_t(x^y),255};
            for(unsigned k=0;k<4;++k)reference[(offset+k)^swaps[endian]]=v[k];
        }
        const unsigned other_endian=(endian+1)%4;
        fetch[0]=2|(1u<<22)|(uint32_t(tiled)<<31);fetch[1]=0x1000|22|(other_endian<<6);fetch[2]=31|(63<<13);
        const auto alias=gow3::renderer::DescribeTexture(fetch);auto image=storage.Load(alias);
        verify(*image,[&](auto x,auto y){
            const auto offset=gow3::renderer::TextureBlockOffset(alias,alias.levels[0],x,y,0);uint32_t word=0;
            for(unsigned k=0;k<4;++k)word|=uint32_t(reference[(offset+k)^swaps[other_endian]])<<(8*k);
            const float value=gow3::renderer::DecodeDepthTexel(word,false);return std::array<float,4>{value,value,value,value};
        });
        // Partial depth write, followed by the original RGBA8 view. Compare all
        // bytes, including unchanged pixels outside the differently tiled region.
        auto partial=device->createTexture(RenderTextureDesc::ColorTarget(32,64,format));
        pass.Clear(partial.get(),nullptr,true,false,false,{0,0,32,64},{.5f,.5f,.5f,.5f},0);
        storage.Store(*partial,alias,{3,5,11,13});
        for(unsigned y=5;y<13;++y)for(unsigned x=3;x<11;++x) {
            const auto offset=gow3::renderer::TextureBlockOffset(alias,alias.levels[0],x,y,0);constexpr uint32_t word=0x80000000;
            for(unsigned k=0;k<4;++k)reference[(offset+k)^swaps[other_endian]]=uint8_t(word>>(8*k));
        }
        image=storage.Load(initial);
        verify(*image,[&](auto x,auto y){
            const auto offset=gow3::renderer::TextureBlockOffset(initial,initial.levels[0],x,y,0);std::array<float,4> v;
            for(unsigned k=0;k<4;++k)v[k]=reference[(offset+k)^swaps[endian]]/255.f;return v;
        });
    }
    // Resolved scalar/vector, half/float, packed and signed formats. Check
    // every byte-order mode; this catches atomic halfword packing corruption.
    pass.Clear(a.get(),nullptr,true,false,false,full,{.25f,.5f,.75f,1},0);
    for(unsigned f:{2u,6u,7u,22u,23u,24u,25u,26u,27u,28u,29u,30u,31u,32u,36u,37u,38u,54u})for(unsigned endian=0;endian<4;++endian) {
        gow3::renderer::ResolveMemory storage(*device,*queue,memory_store_spv,memory_load_spv,65536);
        std::array<uint32_t,6> fetch{2u|(4u<<22)|(1u<<31),0x1000u|f|(endian<<6),15|(15<<13),0,0,1<<9};
        auto layout=gow3::renderer::DescribeTexture(fetch);storage.Store(*a,layout,full);auto image=storage.Load(layout);
        std::array<float,4> expected{.25f,.5f,.75f,1};
        if(f==2)expected={64.f/255,0,0,1};
        if(f==6)expected={64.f/255,128.f/255,191.f/255,1};
        if(f==7||f==54)expected={256.f/1023,512.f/1023,767.f/1023,1};
        if(f==22||f==23||f==27||f==30)expected.fill(.25f);
        if(f==24)expected.fill(16384.f/65535);
        if(f==25)expected={16384.f/65535,32768.f/65535,0,1};
        if(f==26)expected={16384.f/65535,32768.f/65535,49151.f/65535,1};
        if(f==28||f==31||f==37)expected={.25f,.5f,0,1};
        if(f==36)expected={.25f,0,0,1};
        verify(*image,[&](auto,auto){return expected;});
    }
    pass.Clear(a.get(),nullptr,true,false,false,full,{-.25f,-.5f,.75f,1},0);
    for(unsigned f:{2u,6u,24u,25u,26u})for(unsigned endian=0;endian<4;++endian) {
        gow3::renderer::ResolveMemory storage(*device,*queue,memory_store_spv,memory_load_spv,65536);
        std::array<uint32_t,6> fetch{2u|(85u<<2)|(4u<<22)|(1u<<31),0x1000u|f|(endian<<6),15|(15<<13),0,0,1<<9};
        auto layout=gow3::renderer::DescribeTexture(fetch);storage.Store(*a,layout,full);auto image=storage.Load(layout);
        std::array<float,4> expected{-8192.f/32767,-16384.f/32767,24575.f/32767,1};
        if(f==2)expected={-32.f/127,0,0,1};
        if(f==6)expected={-32.f/127,-64.f/127,95.f/127,1};
        if(f==24)expected.fill(-8192.f/32767);
        if(f==25)expected={-8192.f/32767,-16384.f/32767,0,1};
        verify(*image,[&](auto,auto){return expected;});
    }
    // R8 history uses byte masks, including partial rectangles sharing dwords
    // with unchanged texels. The resolve address is independent of fetch base.
    for(bool tiled:{false,true})for(unsigned endian=0;endian<4;++endian) {
        using namespace gow3::renderer;
        ResolveMemory storage(*device,*queue,memory_store_spv,memory_load_spv,65536,4096);
        const std::array<uint32_t,6> fetch{2u|(2u<<22)|(uint32_t(tiled)<<31),0x4000u|2|(endian<<6),63|(63<<13),0,0,1<<9};
        const auto layout=DescribeTexture(fetch);
        constexpr uint32_t destination=0x5000,swaps[]{0,1,3,2};
        std::vector<uint8_t> reference(65536,0xa7);
        storage.Initialize(0,reference);
        auto r8=device->createTexture(RenderTextureDesc::ColorTarget(64,64,RenderFormat::R8_UNORM));
        pass.Resolve(*pattern,*r8,{0,0,64,64},{0,0,64,64},0,PassTransfer::Conversion::UNorm8);
        storage.Store(*r8,layout,{0,0,64,64},destination);
        for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x)
            reference[(destination+TextureBlockOffset(layout,layout.levels[0],x,y,0))^swaps[endian]]=uint8_t(x);
        pass.Clear(r8.get(),nullptr,true,false,false,{0,0,64,64},{1,0,0,1},0);
        storage.Store(*r8,layout,{3,5,11,13},destination);
        for(unsigned y=5;y<13;++y)for(unsigned x=3;x<11;++x)
            reference[(destination+TextureBlockOffset(layout,layout.levels[0],x,y,0))^swaps[endian]]=255;
        const auto actual=storage.ReadBytes(0,65536);
        for(size_t i=0;i<actual.size();++i) {
            if(actual[i]!=reference[i])throw std::runtime_error("R8 resolve corrupted physical byte "+std::to_string(i));
            ++checks;
        }
        auto image=storage.Load(layout,destination);
        verify(*image,[&](auto x,auto y){return std::array<float,4>{x>=3&&x<11&&y>=5&&y<13?1.f:x/255.f,0,0,1};});
        storage.Invalidate(0,65536);
        storage.Store(*r8,layout,{3,5,11,13},destination);
        std::vector<bool> owned(65536);
        for(unsigned y=5;y<13;++y)for(unsigned x=3;x<11;++x)
            owned[(destination+TextureBlockOffset(layout,layout.levels[0],x,y,0))^swaps[endian]]=true;
        for(size_t i=0;i<owned.size();++i) {
            if(storage.BytesDefined(i,1)!=owned[i])throw std::runtime_error("R8 ownership includes an unwritten neighbor");
            ++checks;
        }
    }
    // Quantize BEFORE the host R8 attachment: nearest-even host conversion
    // would lose half-up guest ties. Include blue selection and exponent bias.
    {
        using namespace gow3::renderer;
        ResolveMemory storage(*device,*queue,memory_store_spv,memory_load_spv,8192);
        const auto layout=DescribeTexture({2u|(1u<<22),0x1000u|2,15|(15<<13),0,0,1<<9});
        auto r8=device->createTexture(RenderTextureDesc::ColorTarget(16,16,RenderFormat::R8_UNORM));
        for(unsigned swap:{0u,1u})for(int exponent:{0,1}) {
            pass.Clear(a.get(),nullptr,true,false,false,full,{.5f/255,.25f,2.5f/255,1},0);
            pass.Resolve(*a,*r8,full,full,exponent,PassTransfer::Conversion::UNorm8,swap?0x60a:0x688);
            storage.Store(*r8,layout,full);
            auto image=storage.Load(layout);
            const float expected=(swap?(exponent?5.f:3.f):1.f)/255;
            verify(*image,[&](auto,auto){return std::array<float,4>{expected,0,0,1};});
        }
    }
    // The same bytes can be sampled with independent signedness per channel.
    // The decoded image is float4; destination UNORM image caches cannot be
    // reused for this signed interpretation without decoding again.
    {
        using namespace gow3::renderer;
        ResolveMemory storage(*device,*queue,memory_store_spv,memory_load_spv,65536);
        std::array<uint32_t,6> fetch{2u|(1u<<22),0x1000u|6,15|(15<<13),0,0,1<<9};
        const auto unorm=DescribeTexture(fetch);
        pass.Clear(a.get(),nullptr,true,false,false,full,{128.f/255,1,64.f/255,254.f/255},0);
        storage.Store(*a,unorm,full);
        fetch[0]|=0x41u<<2;
        auto mixed=storage.Load(DescribeTexture(fetch));
        verify(*mixed,[&](auto,auto){return std::array<float,4>{-1,1,64.f/255,-2.f/127};});
        auto unsigned_again=storage.Load(unorm);
        verify(*unsigned_again,[&](auto,auto){return std::array<float,4>{128.f/255,1,64.f/255,254.f/255};});
    }
    // Offset views overlap the same allocation. Frame N+1 can bind a later
    // base, write a partial region, and frame N+2 sees it through the old base.
    for(unsigned endian=0;endian<4;++endian) {
        gow3::renderer::ResolveMemory storage(*device,*queue,memory_store_spv,memory_load_spv,65536);
        std::array<uint32_t,6> fetch{2u|(2u<<22),0x1000u|6|(endian<<6),63|(63<<13),0,0,1<<9};
        auto layout=gow3::renderer::DescribeTexture(fetch);
        storage.Store(*pattern,layout,{0,0,64,64},4096);
        fetch[2]=63|(31<<13);auto alias=gow3::renderer::DescribeTexture(fetch);
        auto image=storage.Load(alias,4096+64*32*4);
        verify(*image,[&](auto x,auto y){return std::array<float,4>{x/255.f,(y+32)/255.f,float(x^(y+32))/255.f,1};});
        auto patch_image=device->createTexture(RenderTextureDesc::ColorTarget(64,32,format));
        pass.Clear(patch_image.get(),nullptr,true,false,false,{7,9,13,15},{1,0,0,1},0);
        storage.Store(*patch_image,alias,{7,9,13,15},4096+64*32*4);
        image=storage.Load(layout,4096);
        verify(*image,[&](auto x,auto y){return x>=7&&x<13&&y>=41&&y<47?std::array<float,4>{1,0,0,1}:
            std::array<float,4>{x/255.f,y/255.f,float(x^y)/255.f,1};});
        // Depth resolves preserve stencil, including a shifted source rect.
        for(unsigned f:{22u,23u}) {
            fetch={2u|(1u<<22),0x1000u|f|(endian<<6),15|(15<<13),0,0,1<<9};
            auto d=gow3::renderer::DescribeTexture(fetch);
            pass.Clear(nullptr,depth.get(),false,true,true,full,{},.5f,0x57);
            pass.Clear(nullptr,depth.get(),false,false,true,{4,5,8,9},{},0,0xa9);
            pass.Resolve(*depth,*c,full,full,0,f==22?PassTransfer::Conversion::DepthUNorm24:PassTransfer::Conversion::DepthFloat24);
            storage.StoreDepth(*c,d,full,32768,*depth);
            storage.StoreDepth(*c,d,{2,3,6,7},32768,*depth,2,2);
            fetch[1]=(fetch[1]&~63u)|6;auto bytes_layout=gow3::renderer::DescribeTexture(fetch);
            image=storage.Load(bytes_layout,32768);
            verify(*image,[&](auto x,auto y){
                const bool patch=(x>=4&&x<8&&y>=5&&y<9)||(x>=2&&x<6&&y>=3&&y<7);
                const uint32_t dword=f==22?0x80000000u:0xe0000000u;
                return std::array<float,4>{(patch?169.f:87.f)/255,float((dword>>8)&255)/255,
                    float((dword>>16)&255)/255,float(dword>>24)/255};
            });
        }
    }
    // CPU writes and frees invalidate only the touched physical byte range.
    // Reuse must replace stale data, including bytes previously written by GPU.
    {
        gow3::renderer::ResolveMemory memory(*device,*queue,memory_store_spv,memory_load_spv,16384);
        std::vector<uint8_t> original(12288,0x35);memory.Initialize(0,original);
        memory.Invalidate(4096,4096);
        if(!memory.BytesDefined(0,4096)||memory.BytesDefined(4096,1)||
           !memory.BytesDefined(8192,4096)||memory.BytesDefined(0,12288))
            throw std::runtime_error("Physical memory free did not preserve neighboring ranges");
        bool rejected=false;try{memory.ReadBytes(4096,4);}catch(const std::runtime_error&){rejected=true;}
        if(!rejected)throw std::runtime_error("Freed physical memory retained readable GPU history");
        std::vector<uint8_t> replacement(4096,0xc7);memory.Initialize(4096,replacement);
        auto actual=memory.ReadBytes(0,12288);
        for(size_t i=0;i<actual.size();++i) {
            if(actual[i]!=(i>=4096&&i<8192?0xc7:0x35))
                throw std::runtime_error("Reused physical memory retained stale bytes");
            ++checks;
        }
        memory.Invalidate(0,12288);
        if(memory.BytesDefined(0,1)||memory.BytesDefined(8192,1))
            throw std::runtime_error("Whole resource free did not remove GPU ownership");
    }
    // EDRAM tiles have a different layout from resolved texture memory.
    // Test wraparound, changed pitch/base, 64-bit surfaces and depth half-tiles.
    {
        using gow3::renderer::SurfaceLayout;
        gow3::renderer::ResolveMemory edram(*device,*queue,memory_store_spv,memory_load_spv,2048*5120);
        auto tiles=device->createTexture(RenderTextureDesc::ColorTarget(160,32,format));
        auto data=device->createBuffer(RenderBufferDesc::UploadBuffer(160*32*16));
        auto values=static_cast<float*>(data->map());
        for(unsigned y=0;y<32;++y)for(unsigned x=0;x<160;++x) {
            const std::array<float,4> v{x/255.f,y/255.f,float(x^y)/255.f,1};
            std::memcpy(values+(y*160+x)*4,v.data(),16);
        }
        validation::Flush(*data);data->unmap();commands->begin();
        commands->barriers(RenderBarrierStage::COPY,RenderTextureBarrier(tiles.get(),RenderTextureLayout::COPY_DEST));
        commands->copyTextureRegion(RenderTextureCopyLocation::Subresource(tiles.get(),0),RenderTextureCopyLocation::PlacedFootprint(data.get(),format,160,32,1,160),0,0,0,nullptr);
        commands->end();queue->executeCommandLists(commands.get(),fence.get());queue->waitForCommandFence(fence.get());
        const SurfaceLayout original{160,32,160,2047,0,false};
        edram.StoreSurface(*tiles,original);
        auto result=edram.LoadSurface({80,16,80,0,0,false});
        verify(*result,[&](auto x,auto y){return std::array<float,4>{(x+80)/255.f,y/255.f,float((x+80)^y)/255.f,1};});
        result=edram.LoadSurface({80,16,80,1,0,false});
        verify(*result,[&](auto x,auto y){return std::array<float,4>{x/255.f,(y+16)/255.f,float(x^(y+16))/255.f,1};});
        // Quantization of the signed fixed range must be 32/32767, not half float.
        pass.Clear(a.get(),nullptr,true,false,false,full,{-8,16,24,32},0);
        const SurfaceLayout fixed{16,16,80,20,5,false};edram.StoreSurface(*a,fixed);
        result=edram.LoadSurface(fixed);
        verify(*result,[&](auto,auto){return std::array<float,4>{-8192.f/32767*32,16384.f/32767*32,24575.f/32767*32,32};});
        // Read back the two dwords per fixed pixel as RGBA8 at another pitch.
        result=edram.LoadSurface({32,16,80,20,0,false});
        verify(*result,[&](auto x,auto){return x%2?std::array<float,4>{255.f/255,95.f/255,255.f/255,127.f/255}:
            std::array<float,4>{0,224.f/255,0,64.f/255};});
        // Guest EDRAM fixed/UNORM color rounds halfway away from zero.
        // Floating 7e3 and depth24 deliberately retain nearest-even behavior.
        for(unsigned f:{0u,2u,3u,5u}) {
            const SurfaceLayout quantized{16,16,80,100,f,false};
            std::array<float,4> input{},expected{};
            if(f==0) {input={.5f/255,2.5f/255,4.5f/255,6.5f/255};expected={1.f/255,3.f/255,5.f/255,7.f/255};}
            if(f==2) {input={.5f/1023,2.5f/1023,4.5f/1023,.5f/3};expected={1.f/1023,3.f/1023,5.f/1023,1.f/3};}
            if(f==3) {input={.5f/512,2.5f/512,4.5f/512,.5f/3};expected={0,2.f/512,4.f/512,1.f/3};}
            if(f==5) {input={-.5f*32/32767,-2.5f*32/32767,2.5f*32/32767,4.5f*32/32767};expected={-32.f/32767,-96.f/32767,96.f/32767,160.f/32767};}
            pass.Clear(a.get(),nullptr,true,false,false,full,{input[0],input[1],input[2],input[3]},0);
            edram.StoreSurface(*a,quantized);result=edram.LoadSurface(quantized);
            verify(*result,[&](auto,auto){return expected;});
            const float nan=std::numeric_limits<float>::quiet_NaN();
            pass.Clear(a.get(),nullptr,true,false,false,full,{nan,nan,nan,nan},0);
            edram.StoreSurface(*a,quantized);result=edram.LoadSurface(quantized);
            verify(*result,[&](auto,auto){return std::array<float,4>{0,0,0,0};});
        }
        // A fixed16 source must clamp before resolve exponent scaling, even
        // when resolving to half/float storage capable of holding more range.
        pass.Clear(a.get(),nullptr,true,false,false,full,{-1735,4096,-16,16},0);
        edram.StoreSurface(*a,fixed);result=edram.LoadSurface(fixed);
        pass.Clear(b.get(),nullptr,true,false,false,full,{.25f,.25f,.25f,.25f},0);
        pass.Resolve(*result,*b,{4,5,8,9},{1,2,5,6},-5);
        verify(*b,[&](auto x,auto y){return x>=1&&x<5&&y>=2&&y<6?
            std::array<float,4>{-1,1,-16384.f/32767,16384.f/32767}:
            std::array<float,4>{.25f,.25f,.25f,.25f};});
        pass.Clear(nullptr,depth.get(),false,true,true,full,{},.5f,0x57);
        pass.Clear(nullptr,depth.get(),false,false,true,{4,5,8,9},{},0,0xa9);
        const SurfaceLayout ds{16,16,80,0,1,true};edram.StoreSurface(*depth,ds);
        result=edram.LoadSurface(ds);
        verify(*result,[&](auto x,auto y){return std::array<float,4>{.5f,.5f,.5f,(x>=4&&x<8&&y>=5&&y<9?169.f:87.f)/255};});
        // Reconstruct a hardware depth/stencil attachment from the byte store.
        auto restored=device->createTexture(RenderTextureDesc::DepthTarget(16,16,RenderFormat::D32_FLOAT_S8_UINT));
        pass.RestoreDepth(*result,*restored);
        const SurfaceLayout other{16,16,80,40,1,true};edram.StoreSurface(*restored,other);
        result=edram.LoadSurface(other);
        verify(*result,[&](auto x,auto y){return std::array<float,4>{.5f,.5f,.5f,(x>=4&&x<8&&y>=5&&y<9?169.f:87.f)/255};});
        // At base zero the depth surface overwrote color columns 40..55.
        result=edram.LoadSurface({80,16,80,0,0,false});
        verify(*result,[&](auto x,auto y){
            if(x>=40&&x<56)return std::array<float,4>{(x>=44&&x<48&&y>=5&&y<9?169.f:87.f)/255,0,0,224.f/255};
            return std::array<float,4>{(x+80)/255.f,y/255.f,float((x+80)^y)/255.f,1};
        });
    }
    // A stacked lookup texture occupies separately tiled, aligned slices.
    // Seed bytes exactly as captured, then verify every slice of the 3D view.
    for(unsigned endian=0;endian<4;++endian) {
        using namespace gow3::renderer;
        ResolveMemory storage(*device,*queue,memory_store_spv,memory_load_spv,131072);
        std::array<uint32_t,6> fetch{2u|(1u<<22)|(1u<<31),0x1000u|1024|6|(endian<<6),
            15|(15<<13)|(15u<<26),0,0,1<<9};
        auto layout=DescribeTexture(fetch);
        std::vector<uint8_t> bytes(65536);constexpr unsigned swaps[]{0,1,3,2};
        for(unsigned z=0;z<16;++z)for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
            const auto offset=TextureBlockOffset(layout,layout.levels[z],x,y,0);
            const std::array<uint8_t,4> value{uint8_t(x*17),uint8_t(y*17),uint8_t(z*17),255};
            for(unsigned k=0;k<4;++k)bytes[(offset+k)^swaps[endian]]=value[k];
        }
        if(storage.AnyDefined(layout,4096))throw std::runtime_error("Uninitialized history marked defined");
        storage.Initialize(4096,bytes);
        if(!storage.FullyDefined(layout,4096))throw std::runtime_error("Captured history not resident");
        auto volume=storage.Load(layout,4096);
        auto result=validation::ReadImage(*device,*queue,*volume,16);
        for(unsigned z=0;z<16;++z)for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
            std::array<float,4> actual;std::memcpy(actual.data(),result.data()+((z*16+y)*16+x)*16,16);
            const std::array<float,4> expected{x/15.f,y/15.f,z/15.f,1};
            for(unsigned k=0;k<4;++k) {
                if(!std::isfinite(actual[k])||std::abs(actual[k]-expected[k])>1e-7f)
                    throw std::runtime_error("Stacked GPU history differs at slice "+std::to_string(z));
                ++checks;
            }
        }
    }
    size_t rejected=0;
    for (int kind=0;kind<3;++kind) try {
        if(kind==0) pass.Resolve(*a,*a,full,full);
        if(kind==1) pass.Resolve(*a,*b,{-1,0,15,16},full);
        if(kind==2) pass.Resolve(*a,*b,full,{0,0,8,16});
    } catch(const std::runtime_error&) {++rejected;}
    if(rejected!=3) throw std::runtime_error("Invalid resolve was accepted");
    size_t invalid_storage=0;
    {
        using namespace gow3::renderer;
        ResolveMemory storage(*device,*queue,memory_store_spv,memory_load_spv,2048*5120);
        const std::array<uint32_t,6> fetch{2u|(1u<<22),0x1000u|6,15|(15<<13),0,0,1<<9};
        const auto layout=DescribeTexture(fetch);
        for(unsigned kind=0;kind<7;++kind)try {
            if(kind==0)storage.Store(*a,layout,{0,0,0,16});
            if(kind==1)storage.Store(*a,layout,full,2048*5120-4);
            if(kind==2)storage.Store(*a,layout,full,1);
            if(kind==3)storage.SurfaceVersion({0,16,80,0,0,false});
            if(kind==4)storage.SurfaceVersion({16,0,80,0,0,false});
            if(kind==5)storage.SurfaceVersion({16,16,0,0,0,false});
            if(kind==6)storage.SurfaceVersion({16,16,80,2048,0,false});
        } catch(const std::runtime_error&) {++invalid_storage;}
    }
    if(invalid_storage!=7)throw std::runtime_error("Invalid storage region was accepted");
    std::cout << "PASS: native clears, ordered GPU resolves, partial writes, depth/color conversion, aliased GPU texture storage; "
              << checks << " component comparisons, " << rejected+invalid_storage << " invalid transfers rejected.\n";
} catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
