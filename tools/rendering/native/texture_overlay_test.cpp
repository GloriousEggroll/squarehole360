#include "renderer/native/resolve_memory.h"
#include "memory_store_spv.h"
#include "memory_load_spv.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>
namespace plume {std::unique_ptr<RenderInterface> CreateVulkanInterface();}
using namespace gow3::renderer;
void Check(bool value,const char* text){if(!value)throw std::runtime_error(text);}
int main(int argc,char** argv) try {
    bool block_versions=false,range_versions=false,coalesced_ranges=false;
    for(int i=1;i<argc;++i) {
        if(std::string_view(argv[i])=="--block-page-versions")block_versions=true;
        else if(std::string_view(argv[i])=="--range-texture-versions")range_versions=true;
        else if(std::string_view(argv[i])=="--coalesced-texture-ranges")coalesced_ranges=true;
        else Check(false,"Unknown texture overlay fixture option");
    }
    auto interface=plume::CreateVulkanInterface();auto device=interface->createDevice();
    Check(bool(device),"Vulkan unavailable");
    auto queue=device->createCommandQueue(plume::RenderCommandListType::DIRECT);
    ResolveMemory memory(*device,*queue,memory_store_spv,memory_load_spv,1024*1024,0,nullptr,false,block_versions,range_versions,coalesced_ranges);
    // CPU base plus GPU-written packed BC3 mips, matching streamed resources.
    const auto layout=DescribeTexture({0x810000fe,0x00010054,0x000fe07f,0x00280d10,0x000001c3,0x00020a00});
    std::vector<uint8_t> base(16384,0x39),mips(49152,0x72),gpu(mips.size());
    for(size_t i=0;i<gpu.size();++i)gpu[i]=uint8_t((i*37+11)%251);
    Check(memory.CpuTextureVersion(layout)==0,"Unknown CPU texture has a GPU version");
    uint32_t drains=0;
    Check(memory.OverlayCpuTexture(layout,base,mips,nullptr,[&]{++drains;})==0&&drains==0,
          "Pure CPU snapshot requested GPU completion");
    bool malformed=false;
    try {memory.OverlayCpuTexture(layout,std::span(base).first(1),mips,nullptr,[&]{++drains;});}
    catch(const std::runtime_error&){malformed=true;}
    Check(malformed&&drains==0,"Pure CPU snapshot skipped payload validation or drained malformed input");
    memory.Initialize(layout.mip_address,gpu);
    const auto owned=memory.ExportState().ranges;
    const auto version=memory.CpuTextureVersion(layout);
    const auto count=memory.OverlayCpuTexture(layout,base,mips);
    std::vector<uint8_t> expected(mips.size(),0x72),mask(mips.size());
    uint64_t sampled=0;
    // Independent per-block byte enumeration includes endian byte permutation.
    for(const auto& level:layout.levels)if(level.mips)
        for(uint32_t y=0;y<(level.height+3)/4;++y)for(uint32_t x=0;x<(level.width+3)/4;++x)
            for(uint32_t byte=0;byte<16;++byte) {
                const auto offset=(TextureBlockOffset(layout,level,x,y,0)+byte)^1;
                expected[offset]=gpu[offset];if(!mask[offset]){mask[offset]=1;++sampled;}
            }
    Check(count==sampled&&mips==expected,"GPU mip overlay missed bytes or copied padding");
    std::fill(mips.begin(),mips.end(),0x72);
    Check(memory.OverlayCpuTexture(layout,base,mips,nullptr,[&]{++drains;})==count&&
          drains==1&&mips==expected,"Deferred readback lost exact GPU-owned packed mips");
    Check(std::all_of(base.begin(),base.end(),[](uint8_t b){return b==0x39;}),"GPU mip replaced CPU base");
    Check(memory.ExportState().ranges==owned,"CPU texture upload changed GPU byte ownership");
    // A CPU overwrite revokes just one byte, even inside a compressed block.
    const auto first=TextureMemoryRanges(layout,true).front().first;
    memory.Invalidate(first+1,1);
    Check(memory.CpuTextureVersion(layout)>version,"CPU invalidation failed to invalidate cached texture");
    std::fill(mips.begin(),mips.end(),0x72);expected[first+1-layout.mip_address]=0x72;
    Check(memory.OverlayCpuTexture(layout,base,mips)==sampled-1&&mips==expected,
          "Sub-word CPU ownership was overwritten by adjacent GPU bytes");
    // An unrelated physical write does not re-upload this texture.
    const auto stable=memory.CpuTextureVersion(layout);
    memory.Initialize(0x80000,std::array<uint8_t,4>{1,2,3,4});
    Check(memory.CpuTextureVersion(layout)==stable,"Unrelated write invalidated cached texture");
    if(range_versions)Check(memory.CpuTextureVersionStatistics().range_hits>0,
        "Actual CPU texture did not reuse a version across a disjoint write");
    // Endian-swapped one-byte texels have a nontrivial physical footprint.
    const auto narrow=DescribeTexture({(1u<<22)|2u,0x00090082,0,0,0,1u<<9});
    const auto ranges=TextureMemoryRanges(narrow,false);
    Check(ranges==std::vector<std::pair<uint32_t,uint32_t>>{{0x90003,1}},"Narrow endian footprint is wrong");
    memory.Initialize(0x90000,std::array<uint8_t,4>{4,5,6,7});
    std::vector<uint8_t> small(4,0xaa);
    Check(memory.OverlayCpuTexture(narrow,small,{})==1&&small==std::vector<uint8_t>({0xaa,0xaa,0xaa,7}),
          "Narrow texture overlay changed adjacent CPU bytes");
    {
        // Repeated cached queries must still see every writer. The expected
        // values below follow each physical page's most recent mutation,
        // independently of the footprint/version cache's implementation.
        ResolveMemory versions(*device,*queue,memory_store_spv,memory_load_spv,10*1024*1024,0,nullptr,false,block_versions,range_versions,coalesced_ranges);
        const auto one=DescribeTexture({2u|(1u<<22),0x1000u|2u,0,0,0,1u<<9});
        auto swapped_fetch=one.fetch;swapped_fetch[1]|=0x80;
        const auto swapped=DescribeTexture(swapped_fetch);
        auto check_versions=[&](uint64_t expected) {
            for(unsigned i=0;i<32;++i)
                Check(versions.CpuTextureVersion(one)==expected&&versions.CpuTextureVersion(swapped)==expected,
                    "Memoized physical-page version missed a writer or another fetch interpretation");
        };
        check_versions(0);
        versions.Initialize(0x1000,std::array<uint8_t,4>{1,2,3,4});check_versions(1);
        versions.Initialize(0x2000,std::array<uint8_t,4>{5,6,7,8});check_versions(1);
        versions.Invalidate(0x1003,1);check_versions(3);
        versions.CopyFromCpu(0x3000,0x1000,std::array<uint8_t,4>{9,10,11,12});check_versions(4);
        versions.Copy(0x2000,0x1000,4);check_versions(5);
        versions.RecordImportedBytes(0x1000,4);check_versions(6);
        // Raster metadata also updates page versions. Tile1 starts at byte5120,
        // on this texture's page1; the unrelated page2 must retain its version.
        const auto other=DescribeTexture({2u|(1u<<22),0x2000u|2u,0,0,0,1u<<9});
        Check(versions.CpuTextureVersion(other)==2,"Unrelated page inherited another texture's version");
        versions.RecordSurfaceWrite({1,1,80,1,0,false});
        check_versions(7);
        for(unsigned i=0;i<32;++i)Check(versions.CpuTextureVersion(other)==2,
            "Raster page write changed an unrelated memoized version");
    }
    {
        ResolveMemory fragmented(*device,*queue,memory_store_spv,memory_load_spv,1024*1024,0,nullptr,false,block_versions,range_versions,coalesced_ranges);
        const auto linear=DescribeTexture({(32u<<22)|2u,0x1000u|2u,1023u|(511u<<13),0,0,1u<<9});
        std::vector<uint8_t> source(1024*512),cpu(source.size(),0xcd);
        for(size_t i=0;i<source.size();++i)source[i]=uint8_t(i*31+i/256);
        fragmented.Initialize(linear.base_address,source);
        // Thousands of GPU-owned three-byte islands share words and rows with
        // newer CPU bytes. Transfer coalescing may read those gaps, never copy
        // them back over the CPU snapshot or claim GPU ownership of them.
        for(uint32_t offset=0;offset<source.size();offset+=16)
            fragmented.Invalidate(linear.base_address+offset+3,13);
        const auto ownership=fragmented.ExportState().ranges;
        TextureOverlayStats stats;
        const auto copied=fragmented.OverlayCpuTexture(linear,cpu,{},&stats);
        Check(copied==source.size()/16*3&&stats.owned_bytes==copied,
              "Fragmented overlay counted CPU-owned gaps");
        Check(stats.sampled_bytes==std::array<uint64_t,2>{source.size(),0}&&
              stats.owned_region_bytes==std::array<uint64_t,2>{copied,0},
              "Fragmented footprint metadata incorrectly certifies full coverage");
        Check(stats.copy_regions==1&&stats.readback_bytes==source.size()-12,
              "Nearby owned islands were not coalesced into one transfer");
        for(size_t i=0;i<cpu.size();++i)
            Check(cpu[i]==(i%16<3?source[i]:0xcd),"Coalesced readback overwrote newer CPU bytes");
        Check(fragmented.ExportState().ranges==ownership,"Coalesced readback expanded GPU ownership");
        fragmented.Invalidate(linear.base_address,source.size());
        Check(fragmented.OverlayCpuTexture(linear,cpu,{},&stats)==0&&
              !stats.copy_regions&&!stats.readback_bytes&&!stats.owned_bytes,
              "Empty overlay retained previous transfer statistics");
        Check(stats.sampled_bytes==std::array<uint64_t,2>{source.size(),0}&&
              stats.owned_region_bytes==std::array<uint64_t,2>{0,0},
              "Empty overlay retained previous owned-footprint metadata");
    }
    {
        ResolveMemory large(*device,*queue,memory_store_spv,memory_load_spv,4*1024*1024,0,nullptr,false,block_versions,range_versions,coalesced_ranges);
        const auto linear=DescribeTexture({(64u<<22)|2u,0x1000u|2u,2047u|(1023u<<13),0,0,1u<<9});
        std::vector<uint8_t> source(2*1024*1024),cpu(source.size(),0xcd);
        for(size_t i=0;i<source.size();++i)source[i]=uint8_t(i*13+i/512);
        large.Initialize(linear.base_address,source);
        TextureOverlayStats stats;
        Check(large.OverlayCpuTexture(linear,cpu,{},&stats)==source.size()&&cpu==source,
              "Owned range spanning readback chunks was truncated");
        Check(stats.copy_regions==2&&stats.readback_bytes==source.size(),
              "Readback chunks exceeded the one-MiB region bound");
        Check(stats.sampled_bytes==std::array<uint64_t,2>{source.size(),0}&&
              stats.sampled_bytes==stats.owned_region_bytes,
              "Fully owned texture metadata omitted exact sampled bytes");
    }
    {
        // Mips begin below the base image and partially overlap it physically.
        // Readback regions sort/deduplicate addresses, while CPU copies retain
        // their independent destination buffers and exact sampled byte masks.
        auto fetch=layout.fetch;fetch[1]=(fetch[1]&0xfff)|0x30000;fetch[5]=(fetch[5]&0xfff)|0x2f000;
        const auto overlap=DescribeTexture(fetch);
        std::vector<uint8_t> source(49152),cpu_base(16384,0x91),cpu_mips(49152,0x92);
        for(size_t i=0;i<source.size();++i)source[i]=uint8_t(i*7+i/128);
        memory.Initialize(overlap.mip_address,source);
        auto expected_base=cpu_base,expected_mips=cpu_mips;uint64_t expected_count=0;
        for(unsigned region=0;region<2;++region) {
            auto& destination=region?expected_mips:expected_base;
            const auto address=region?overlap.mip_address:overlap.base_address;
            for(const auto& [first,count]:TextureMemoryRanges(overlap,region!=0)) {
                std::copy_n(source.begin()+(first-overlap.mip_address),count,destination.begin()+(first-address));
                expected_count+=count;
            }
        }
        TextureOverlayStats stats;
        Check(memory.OverlayCpuTexture(overlap,cpu_base,cpu_mips,&stats)==expected_count&&
              cpu_base==expected_base&&cpu_mips==expected_mips,
              "Overlapping or reversed base/mip addresses changed byte destinations");
        Check(stats.owned_bytes==expected_count&&stats.readback_bytes<expected_count,
              "Overlapping base/mip readback ranges were not deduplicated");
        Check(stats.sampled_bytes==stats.owned_region_bytes&&
              stats.sampled_bytes[0]+stats.sampled_bytes[1]==expected_count&&
              stats.sampled_bytes[0]&&stats.sampled_bytes[1],
              "Full overlap metadata merged distinct base and mip provenance");
    }
    std::cout<<"Mixed CPU/GPU texture test passed: packed BC3 mips, exact ownership, padding, endian bytes, fragmented coalescing, bounded chunks and overlapping base/mip ranges\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
