#include "renderer/native/scene_executor.h"
#include "renderer/native/fsi_device.h"
#include "fullscreen_spv.h"
#include <algorithm>
#include <bit>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <limits>
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
    uint32_t width=80,height=32,pitch=240,base=0,format=0;
    bool partial=false,mixed_depth=false,multiple=false,pressure=false;
};
Json Identity(const Case& t) {
    return {{"depth",false},{"edram_base",t.base},{"format",t.format},
        {"format_info",t.format<<16},{"surface_info",t.pitch},{"width",t.width},{"height",t.height}};
}
Json Clear(const Json& surfaces,uint64_t event,uint32_t flags,const Json& rect,
           const Json& value=Json::array({0.25,0.5,0.75,1.0})) {
    return {{"type","clearrect"},{"frame",1},{"event",event},{"flags",flags},
        {"rectangle",rect},{"color",value},{"depth",0.5},{"stencil",0},{"surfaces",surfaces}};
}
SceneExecutorConfig Configuration(bool enabled,bool async) {
    SceneExecutorConfig c;c.native_fsi=true;c.ordered_pixel_interlock_enabled=true;
    c.skip_full_color_clear_restore=enabled;c.batch_transfers=c.unified_gpu_queue=async;
    c.async_gpu_submissions=async;c.max_draw_batch_size=16;c.defer_native_queries=true;
    c.retire_completed_query_prefixes=async;return c;
}
struct Outcome {
    std::vector<uint8_t> edram,physical,partial_edram,partial_physical;
    std::vector<uint64_t> samples;
    NativeStateRanges ranges;
};
Outcome Run(RenderDevice& device,RenderCommandQueue& queue,const std::vector<uint8_t>& builtin,
            const Case& t,bool enabled,bool async) {
    auto config=Configuration(enabled,async);
    const std::string state_id(64,'a'),vertex_id(64,'b');
    std::vector<uint8_t> state(0x2a80);
    const auto word=[&](size_t at,uint32_t v){for(unsigned i=0;i<4;++i)state.at(at+i)=uint8_t(v>>(24-8*i));};
    const auto number=[&](size_t at,float v){word(at,std::bit_cast<uint32_t>(v));};
    word(0x2880,t.pitch);word(0x28bc,t.width|(t.height<<16));word(0x28c4,0x80000000);
    word(0x28c8,t.width|(t.height<<16));word(0x2944,1<<19);word(0x294c,0x43f);word(0x29c0,1);
    // No color/depth writes: actual occlusion results still count every fragment.
    number(0x2908,t.width/2.f);number(0x290c,t.width/2.f);
    number(0x2910,-float(t.height)/2.f);number(0x2914,t.height/2.f);number(0x2918,1);
    config.payload=[&](const std::string& key){Check(key==state_id,"Unexpected state payload");return state;};
    config.shader=[&](const std::string& key){Check(key==vertex_id,"Unexpected vertex shader");
        return Bytes(std::span<const uint32_t>(fullscreen_spv));};
    config.fsi_depth_shader=[&]{return builtin;};
    SceneExecutor scene(config,&device,&queue);
    std::vector<uint8_t> seed(kEdram);for(size_t i=0;i<seed.size();++i)seed[i]=uint8_t(i*73+19);
    scene.InitializeEdram(seed);scene.InitializeMemory(kPhysical,std::vector<uint8_t>(kPhysicalBytes,0x6e));
    const auto color=Identity(t);Json surfaces{{"6",color}};
    if(t.multiple) {auto alias=color;alias["format"]=7;alias["format_info"]=7<<16;surfaces["7"]=alias;}
    if(t.mixed_depth) {
        auto depth=color;depth["depth"]=true;depth["format"]=1;depth["format_info"]=1<<16;
        depth["edram_base"]=(t.base+1024)%2048;surfaces["10"]=depth;
    }
    uint64_t event=0;
    const auto query=[&](bool begin,uint64_t generation) {
        scene.Execute({{"type",begin?"query_begin":"query_end"},{"frame",1},{"event",++event},
            {"query_generation",generation},{"query_handle",0x1000+32*generation},
            {"report_address",0x2000+64*generation+(begin?32:0)}});
    };
    // Null-pixel-shader draws require a depth identity for native raster
    // extent selection. Depth/stencil remain disabled in the captured state.
    auto query_depth=color;query_depth["depth"]=true;query_depth["format"]=1;
    query_depth["format_info"]=1<<16;query_depth["edram_base"]=(t.base+1024)%2048;
    const auto draw=[&] {
        scene.Execute({{"type","draw"},{"frame",1},{"event",++event},{"function","DrawPrimitive"},
            {"args",{0,4,0,3,0,0,0,0}},{"device_state",state_id},{"vertex_shader",vertex_id},
            {"pixel_shader",nullptr},{"surfaces",{{"6",color},{"10",query_depth}}},{"geometry",Json::array()},
            {"vertex_slots",Json::array()},{"textures",Json::array()}});
    };
    const auto resolve=[&] {
        // Actual subsequent texture sampling through Surface/PassTransfer. The
        // full physical buffer comparison also detects writes to row padding.
        const std::array<uint32_t,6> fetch{2u|(((t.width+31)/32)<<22),kPhysical|6u,
            (t.width-1)|((t.height-1)<<13),0,0,1u<<9};
        scene.Execute({{"type","resolve"},{"frame",1},{"event",++event},{"level",0},{"flags",0},
            {"source_slot",0},{"source_rectangle",{0,0,t.width,t.height}},
            {"destination_rectangle",{0,0,t.width,t.height}},
            {"destination",{{"base","00010000"},{"format",6},{"width",t.width},{"height",t.height}}},
            {"destination_fetch",fetch},{"exponent_bias",0},{"copy_dest_swap",false},
            {"surfaces",{{"6",color}}}});
        return scene.ReadPhysical(kPhysical,kPhysicalBytes);
    };
    const uint32_t repeats=t.pressure?65:2;
    const uint32_t flags=t.mixed_depth?0x11:0xf;
    const Json rectangle=t.partial?Json::array({1,1,t.width-1,t.height-1}):Json::array({0,0,t.width,t.height});
    query(true,1);for(uint32_t i=0;i<repeats;++i)draw();
    scene.Execute(Clear(surfaces,++event,flags,rectangle));
    for(uint32_t i=0;i<repeats;++i)draw();query(false,1);
    // Differently sized alias has a modified pixel outside the target's full
    // overwrite. Its complete StoreSurface must still precede the target clear.
    auto alias=color;alias["width"]=t.width+80;alias["height"]=t.height+64;
    alias["format"]=0;alias["format_info"]=0;
    scene.Execute(Clear({{"6",alias}},++event,1,
        {t.width+79,t.height+63,t.width+80,t.height+64},{1.0,0.0,0.0,1.0}));
    // No draw between these clears: in synchronous mode the alias stays
    // dirty until Surface(target) itself commits it before replacing contents.
    query(true,2);scene.Execute(Clear(surfaces,++event,flags,rectangle));draw();draw();query(false,2);
    query(true,3);query(false,3);scene.FlushGpuWork();
    Outcome result;const auto completed=scene.TakeCompletedQueries();
    Check(completed.size()==3,"Full color clear changed query count/order");
    const std::array<uint64_t,3> expected{uint64_t(t.width)*t.height*repeats*2,uint64_t(t.width)*t.height*2,0};
    for(size_t i=0;i<3;++i) {
        Check(completed[i].generation==i+1&&completed[i].result.samples==expected[i],
            "Full color clear changed real query identity/sample count");
        Check(completed[i].result.draws==(i==0?2*repeats:i==1?2:0),"Full color clear changed query draw count");
        result.samples.push_back(completed[i].result.samples);
    }
    const auto packed=scene.ExportState();result.ranges=packed.edram.ranges;
    result.edram=ReadEdram(device,queue,packed.edram);result.physical=resolve();
    // Independent packed RGBA8 oracle for the alias pixel beyond the target.
    const uint32_t x=t.width+79,y=t.height+63;
    const uint32_t tile=(t.base+(y/16)*((t.pitch+79)/80)+x/80)%2048;
    const uint32_t byte=4*(tile*1280+(y%16)*80+x%80);
    const std::array<uint8_t,4> red{0xff,0,0,0xff};
    Check(std::equal(red.begin(),red.end(),result.edram.begin()+byte),
        "Dirty alias pixel outside full overwrite did not survive");
    // Generic partial update must retain all other pixels of the previously
    // overwritten working image, then provide those exact values to sampling.
    scene.Execute(Clear({{"6",color}},++event,1,{0,0,1,1},{0.0,1.0,0.0,1.0}));
    scene.FlushGpuWork();result.partial_edram=ReadEdram(device,queue,scene.ExportState().edram);
    result.partial_physical=resolve();
    if(enabled) {
        const auto& stats=scene.report().at("full_color_clear_restore");
        if(!t.partial&&!t.mixed_depth)Check(stats.at("skipped_restores").get<uint64_t>()>=1,
            "Eligible fixture did not actually omit a stale restore");
        else Check(stats.at("skipped_restores")==0,"Refused clear omitted its contents restore");
    }
    return result;
}
void Malformed(RenderDevice& device,RenderCommandQueue& queue,bool async) {
    const Json surfaces{{"6",Identity(Case{})}};
    const auto valid=Clear(surfaces,1,1,{0,0,80,32});
    std::vector<Json> cases;
    for(const char* field:{"color","depth","stencil"}) {
        auto bad=valid;bad[field]="invalid";cases.push_back(bad);
        bad=valid;bad.erase(field);cases.push_back(bad);
    }
    for(double d:{-1.0,2.0,std::numeric_limits<double>::infinity()}) {
        auto bad=valid;bad["depth"]=d;cases.push_back(bad);
    }
    {auto bad=valid;bad["color"]={1.0,0.0,0.0};cases.push_back(bad);}
    for(const auto& operation:cases) {
        std::string errors[2];
        for(unsigned mode=0;mode<2;++mode) {
            SceneExecutor scene(Configuration(mode,async),&device,&queue);
            scene.InitializeEdram(std::vector<uint8_t>(kEdram,0x5a));
            try {scene.Execute(operation);}catch(const std::exception& e){errors[mode]=e.what();}
            Check(!errors[mode].empty(),"Malformed color-only clear unexpectedly succeeded");
            if(mode)Check(scene.report().at("full_color_clear_restore").at("skipped_restores")==0,
                "Malformed clear omitted a restore before validation");
        }
        Check(errors[0]==errors[1],"Full color omission changed original validation error");
    }
}
}
int main(int argc,char** argv)try {
    Check(argc==2,"Usage: full-color-clear-gpu-test original-builtin.fsi.spv");
    const auto builtin=Read(argv[1]);FsiDevice host;auto& device=host.device();
    auto queue=device.createCommandQueue(RenderCommandListType::DIRECT);
    std::vector<Case> cases(1);
    Case odd;odd.width=81;odd.height=17;odd.base=2047;cases.push_back(odd);
    for(uint32_t format:{2u,3u,4u,5u,6u,7u,10u,12u,14u,15u}) {
        auto c=odd;c.width=41;c.format=format;cases.push_back(c);
    }
    {Case c;c.width=c.height=1;cases.push_back(c);}
    {Case c;c.partial=true;cases.push_back(c);}
    {Case c;c.mixed_depth=true;cases.push_back(c);}
    {Case c;c.multiple=true;cases.push_back(c);}
    {Case c;c.width=c.height=16;c.pressure=true;cases.push_back(c);}
    for(bool async:{false,true}) {
        for(const auto& c:cases) {
            const auto old=Run(device,*queue,builtin,c,false,async),now=Run(device,*queue,builtin,c,true,async);
            Check(old.edram==now.edram&&old.partial_edram==now.partial_edram&&
                old.physical==now.physical&&old.partial_physical==now.partial_physical&&
                old.samples==now.samples&&old.ranges==now.ranges,
                "Full color clear changed packed bytes, sampled resolve, partial contents or queries");
        }
        Malformed(device,*queue,async);
    }
    std::cout<<"PASS: "<<cases.size()*2<<" paired full-color schedules; full10MiB packed bytes,4MiB sampled resolves,"
        "actual queries,dirty overlapping aliases,multiple attachments,all supported color formats,"
        "wrapped extents,partial clears,65-draw pressure and20 paired malformed cases\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
