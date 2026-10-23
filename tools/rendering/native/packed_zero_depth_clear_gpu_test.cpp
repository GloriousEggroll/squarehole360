#include "renderer/native/scene_executor.h"
#include "renderer/native/fsi_device.h"
#include "fullscreen_spv.h"
#include <algorithm>
#include <bit>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>

using namespace plume;
using namespace gow3::renderer;
using Json=nlohmann::json;
namespace {
constexpr uint32_t kEdram=2048*5120,kPhysical=0x10000,kPhysicalBytes=4*1024*1024;
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
template<class T>std::vector<uint8_t> Bytes(std::span<const T> data) {
    std::vector<uint8_t> result(data.size_bytes());std::memcpy(result.data(),data.data(),result.size());return result;
}
std::vector<uint8_t> Read(const char* path) {
    std::ifstream f(path,std::ios::binary|std::ios::ate);
    Check(f&&f.tellg()>=20&&f.tellg()%4==0,"Invalid builtin SPIR-V fixture");
    std::vector<uint8_t> result(size_t(f.tellg()));f.seekg(0);
    Check(bool(f.read(reinterpret_cast<char*>(result.data()),result.size())),"Truncated builtin SPIR-V");return result;
}
std::vector<uint8_t> ReadEdram(RenderDevice& device,RenderCommandQueue& queue,const NativeMemoryState& state) {
    auto readback=device.createBuffer(RenderBufferDesc::ReadbackBuffer(kEdram));
    auto commands=queue.createCommandList();auto fence=device.createCommandFence();commands->begin();
    const auto cmd=static_cast<VulkanCommandList&>(*commands).vk;
    auto& buffer=static_cast<VulkanBuffer&>(*readback);
    VkBufferMemoryBarrier dependency{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    dependency.buffer=state.buffer;dependency.size=VK_WHOLE_SIZE;
    dependency.srcQueueFamilyIndex=dependency.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    dependency.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;dependency.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,
        0,nullptr,1,&dependency,0,nullptr);
    const VkBufferCopy range{0,0,kEdram};vkCmdCopyBuffer(cmd,state.buffer,buffer.vk,1,&range);
    VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);
    commands->end();queue.executeCommandLists(commands.get(),fence.get());queue.waitForCommandFence(fence.get());
    const auto* data=readback->map();
    Check(vmaInvalidateAllocation(buffer.device->allocator,buffer.allocation,0,VK_WHOLE_SIZE)==VK_SUCCESS,"Readback invalidate failed");
    std::vector<uint8_t> result(kEdram);std::memcpy(result.data(),data,result.size());readback->unmap();return result;
}
struct Case {
    uint32_t width=80,height=32,pitch=80,base=0,format=1,flags=0x30,stencil=0;
    double depth=0.0;
    bool partial=false,dirty_alias=false,pressure=false,cached_surface=false;
    bool eligible=true;
};
struct Outcome {
    std::vector<uint8_t> edram,after_partial,physical;
    std::vector<uint64_t> samples;
    NativeStateRanges ranges;
};
uint32_t DepthWord(const Case& test,uint32_t x,uint32_t y) {
    const uint32_t tile=(test.base+(y/16)*((test.pitch+79)/80)+x/80)%2048;
    return tile*1280+(y%16)*80+(x%80<40?x%80+40:x%80-40);
}
Json Identity(const Case& test) {
    return {{"depth",true},{"edram_base",test.base},{"format",test.format},
        {"format_info",test.format<<16},{"surface_info",test.pitch},
        {"width",test.width},{"height",test.height}};
}
std::vector<std::pair<uint32_t,uint32_t>> EndRangesOracle(const Case& test) {
    std::set<uint32_t> words;
    for(uint32_t y=0;y<test.height;++y)for(uint32_t x=0;x<test.width;++x)
        words.insert(4*DepthWord(test,x,y));
    std::vector<std::pair<uint32_t,uint32_t>> ranges;
    for(uint32_t word:words) {
        if(!ranges.empty()&&ranges.back().second==word)ranges.back().second=word+4;
        else ranges.emplace_back(word,word+4);
    }
    return ranges;
}
void UndefinedAndValidation(RenderDevice& device,RenderCommandQueue& queue,const Case& test,bool async) {
    for(bool enabled:{false,true}) {
        SceneExecutorConfig config;config.native_fsi=true;config.ordered_pixel_interlock_enabled=true;
        config.packed_zero_depth_clear=enabled;config.batch_transfers=config.unified_gpu_queue=async;
        config.async_gpu_submissions=async;
        Json clear{{"type","clearrect"},{"frame",1},{"event",1},{"flags",0x30},
            {"rectangle",{0,0,test.width,test.height}},{"color",nullptr},{"depth",0.0},{"stencil",0},
            {"surfaces",{{"10",Identity(test)}}}};
        {
            SceneExecutor malformed_scene(config,&device,&queue);
            auto malformed=clear;malformed["color"]="invalid color";
            bool rejected=false;
            try{malformed_scene.Execute(malformed);}catch(const nlohmann::json::exception&){rejected=true;}
            Check(rejected,"Depth-only clear stopped validating malformed color");
        }
        SceneExecutor scene(config,&device,&queue);
        scene.Execute(clear);scene.FlushGpuWork();const auto packed=scene.ExportState();
        const auto expected=EndRangesOracle(test);
        NativeStateRanges exported;
        for(const auto& [first,last]:expected)exported.emplace_back(first,last-first);
        Check(packed.edram.ranges==exported,"Initially unknown clear defined padding or omitted pixels");
        const auto bytes=ReadEdram(device,queue,packed.edram);
        for(const auto& [first,last]:expected)
            Check(std::all_of(bytes.begin()+first,bytes.begin()+last,[](uint8_t b){return b==0;}),
                "Initially unknown clear did not initialize exact zero words");
    }
}
Outcome Run(RenderDevice& device,RenderCommandQueue& queue,const std::vector<uint8_t>& builtin,
            const Case& test,bool enabled,bool async) {
    SceneExecutorConfig config;config.native_fsi=true;config.ordered_pixel_interlock_enabled=true;
    config.packed_zero_depth_clear=enabled;config.batch_transfers=async;config.unified_gpu_queue=async;
    config.async_gpu_submissions=async;config.max_draw_batch_size=16;config.defer_native_queries=true;
    config.retire_completed_query_prefixes=async;
    const std::string state_id(64,'a'),vertex_id(64,'b');
    std::vector<uint8_t> state(0x2a80);
    const auto word=[&](size_t at,uint32_t value){for(unsigned i=0;i<4;++i)state.at(at+i)=uint8_t(value>>(24-8*i));};
    const auto number=[&](size_t at,float value){word(at,std::bit_cast<uint32_t>(value));};
    word(0x2880,test.pitch);word(0x28bc,test.width|(test.height<<16));word(0x28c4,0x80000000);
    word(0x28c8,test.width|(test.height<<16));word(0x28dc,0);word(0x2934,0x700762);
    word(0x2944,1<<19);word(0x294c,0x43f);word(0x29c0,1);
    number(0x2908,test.width/2.f);number(0x290c,test.width/2.f);
    // Guest Y scale is negative; DecodeDrawState maps this to Vulkan [0,height].
    number(0x2910,-float(test.height)/2.f);number(0x2914,test.height/2.f);number(0x2918,1);
    config.payload=[&](const std::string& key){Check(key==state_id,"Unexpected fixture payload");return state;};
    config.shader=[&](const std::string& key){Check(key==vertex_id,"Unexpected fixture vertex");
        return Bytes(std::span<const uint32_t>(fullscreen_spv));};
    config.fsi_depth_shader=[&]{return builtin;};
    SceneExecutor scene(config,&device,&queue);
    const std::vector<uint32_t> seed(kEdram/4,0xe000005a);scene.InitializeEdram(Bytes(std::span<const uint32_t>(seed)));
    scene.InitializeMemory(kPhysical,std::vector<uint8_t>(kPhysicalBytes,0x6e));
    const Json depth=Identity(test);
    uint64_t event=0;
    const auto query=[&](bool begin,uint64_t generation) {
        scene.Execute({{"type",begin?"query_begin":"query_end"},{"frame",1},{"event",++event},
            {"query_generation",generation},{"query_handle",0x1000+32*generation},
            {"report_address",0x2000+64*generation+(begin?32:0)}});
    };
    const auto draw=[&] {
        scene.Execute({{"type","draw"},{"frame",1},{"event",++event},{"function","DrawPrimitive"},
            {"args",{0,4,0,3,0,0,0,0}},{"device_state",state_id},{"vertex_shader",vertex_id},
            {"pixel_shader",nullptr},{"surfaces",{{"10",depth}}},{"geometry",Json::array()},
            {"vertex_slots",Json::array()},{"textures",Json::array()}});
    };
    const uint32_t repetitions=test.pressure?65:2;
    if(test.cached_surface) {
        scene.Execute({{"type","clearrect"},{"frame",1},{"event",++event},{"flags",0x30},
            {"rectangle",{0,0,test.width,test.height}},{"color",nullptr},{"depth",1.0},{"stencil",0x5a},
            {"surfaces",{{"10",depth}}}});
    }
    query(true,1);for(uint32_t i=0;i<repetitions;++i)draw();
    if(test.dirty_alias) {
        auto color=depth;color["depth"]=false;color["format"]=0;color["format_info"]=0;
        scene.Execute({{"type","clearrect"},{"frame",1},{"event",++event},{"flags",1},
            {"rectangle",{0,0,1,1}},{"color",{1.f,0.f,0.f,1.f}},{"depth",1.f},{"stencil",0},
            {"surfaces",{{"6",color}}}});
    }
    const int right=int(test.width)-(test.partial?1:0);
    Json clear{{"type","clearrect"},{"frame",1},{"event",++event},{"flags",test.flags},
        {"rectangle",{0,0,right,int(test.height)}},{"color",nullptr},{"depth",test.depth},{"stencil",test.stencil},
        {"surfaces",{{"10",depth}}}};
    scene.Execute(clear);
    for(uint32_t i=0;i<repetitions;++i)draw();
    query(false,1);
    // Another fill while the older query/draw owners remain in flight, followed
    // by a zero-draw query whose watermark includes transfer-only accepted work.
    clear["event"]=++event;scene.Execute(clear);query(true,2);query(false,2);
    if(async)Check(scene.HasPendingGpuWork(),"Transfer-only predecessor was silently completed");
    scene.FlushGpuWork();Outcome result;
    const auto completed=scene.TakeCompletedQueries();
    Check(completed.size()==2&&completed[0].generation==1&&completed[1].generation==2,
          "Clear changed real query identity/order");
    const uint64_t expected=(test.flags&0x10)&&test.depth==0.0?
        uint64_t(right)*test.height*repetitions:0;
    Check(completed[0].result.samples==expected&&completed[0].result.draws==2*repetitions&&
        completed[1].result.samples==0&&completed[1].result.draws==0,
        "Depth clear changed actual depth-test sample counts");
    for(const auto& q:completed)result.samples.push_back(q.result.samples);
    const auto packed=scene.ExportState();result.ranges=packed.edram.ranges;
    result.edram=ReadEdram(device,queue,packed.edram);
    if(test.eligible&&!test.dirty_alias) {
        auto expected_bytes=Bytes(std::span<const uint32_t>(seed));
        for(const auto& [first,last]:EndRangesOracle(test))
            std::fill(expected_bytes.begin()+first,expected_bytes.begin()+last,0);
        Check(result.edram==expected_bytes,"Clear footprint differs from independent scalar address oracle");
    }
    // A generic partial clear must reload the stale depth/stencil image, retain
    // zero pixels outside its rectangle and preserve the packed stencil bytes.
    if(test.eligible) {
        const uint32_t w=std::min(40u,test.width),h=std::min(2u,test.height);
        auto partial=clear;partial["event"]=++event;partial["rectangle"]={0,0,w,h};
        partial["depth"]=0.5;partial["stencil"]=0xa7;
        scene.Execute(partial);query(true,3);draw();query(false,3);scene.FlushGpuWork();
        const auto after=scene.TakeCompletedQueries();
        Check(after.size()==1&&after[0].generation==3&&after[0].result.draws==1&&
            after[0].result.samples==uint64_t(test.width)*test.height-uint64_t(w)*h,
            "Partial clear changed preserved-depth query samples");
        result.samples.push_back(after[0].result.samples);
        result.after_partial=ReadEdram(device,queue,scene.ExportState().edram);
    }
    // A subsequent resolve must reload any stale retained host depth image,
    // preserve the exact stencil byte and expose the new canonical bytes.
    const uint32_t format=test.format?23:22;
    const std::array<uint32_t,6> fetch{2u|(((test.width+31)/32)<<22),kPhysical|format,
        (test.width-1)|((test.height-1)<<13),0,0,1u<<9};
    scene.Execute({{"type","resolve"},{"frame",1},{"event",++event},{"level",0},{"flags",0},
        {"source_slot",4},{"source_rectangle",{0,0,test.width,test.height}},
        {"destination_rectangle",{0,0,test.width,test.height}},
        {"destination",{{"base","00010000"},{"format",format},{"width",test.width},{"height",test.height}}},
        {"destination_fetch",fetch},{"exponent_bias",0},{"copy_dest_swap",false},{"surfaces",{{"10",depth}}}});
    result.physical=scene.ReadPhysical(kPhysical,kPhysicalBytes);
    if(enabled) {
        const auto& counters=scene.report().at("packed_zero_depth_clear");
        Check(counters.at("selected")==uint64_t(test.eligible?2:0),"Clear gate selected/refused wrong fixture case");
    }
    return result;
}
}
int main(int argc,char** argv)try {
    Check(argc==2,"Usage: packed-zero-depth-clear-gpu-test original-builtin.fsi.spv");
    const auto builtin=Read(argv[1]);FsiDevice host;auto& device=host.device();
    auto queue=device.createCommandQueue(RenderCommandListType::DIRECT);
    std::vector<Case> cases(1);
    Case odd;odd.width=81;odd.height=17;odd.pitch=160;odd.base=2047;cases.push_back(odd);
    Case room;room.width=1280;room.height=720;room.pitch=1280;cases.push_back(room);
    Case tiny;tiny.width=tiny.height=1;cases.push_back(tiny);
    Case dirty=odd;dirty.dirty_alias=true;cases.push_back(dirty);
    Case cached=odd;cached.cached_surface=true;cases.push_back(cached);
    Case pressure;pressure.width=pressure.height=16;pressure.pressure=true;cases.push_back(pressure);
    for(uint32_t flags:{0x10u,0x20u}){Case c;c.flags=flags;c.eligible=false;cases.push_back(c);}
    {Case c;c.depth=1;c.eligible=false;cases.push_back(c);}
    {Case c;c.depth=-0.0;c.eligible=false;cases.push_back(c);}
    {Case c;c.stencil=5;c.eligible=false;cases.push_back(c);}
    {Case c;c.partial=true;c.eligible=false;cases.push_back(c);}
    {Case c;c.format=0;c.eligible=false;cases.push_back(c);}
    for(bool async:{false,true})for(const auto& test:cases) {
        const auto before=Run(device,*queue,builtin,test,false,async);
        const auto after=Run(device,*queue,builtin,test,true,async);
        Check(before.edram==after.edram&&before.physical==after.physical&&
              before.after_partial==after.after_partial&&before.samples==after.samples&&before.ranges==after.ranges,
              "Packed zero clear changed full EDRAM, alias readback, real query or ownership range");
    }
    for(bool async:{false,true})UndefinedAndValidation(device,*queue,odd,async);
    std::cout<<"PASS: "<<cases.size()*2<<" paired Scene schedules,full10MiB EDRAM+4MiB alias readback,"
        "real depth queries,zero-transfer ordering,65-slot pressure,wrapped odd extents,dirty aliases,partial clears,\n"
        "initially undefined ownership,malformed color and refused states\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
