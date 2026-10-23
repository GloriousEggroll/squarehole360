#include "renderer/native/scene_executor.h"
#include "renderer/native/fsi_device.h"
#include "renderer/native/resolve_memory.h"
#include "fullscreen_spv.h"
#include <bit>
#include <cstring>
#include <fstream>
#include <iostream>
using namespace plume;
using namespace gow3::renderer;
using Json=nlohmann::json;
namespace {
using Words=std::vector<uint32_t>;
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
Words Read(const char* path) {
    std::ifstream file(path,std::ios::binary|std::ios::ate);
    Check(file&&file.tellg()>=20&&file.tellg()%4==0,"Invalid fixture shader");
    Words result(size_t(file.tellg())/4);file.seekg(0);
    Check(bool(file.read(reinterpret_cast<char*>(result.data()),result.size()*4)),"Shader read failed");return result;
}
std::vector<uint8_t> Bytes(std::span<const uint32_t> words) {
    std::vector<uint8_t> result(words.size_bytes());std::memcpy(result.data(),words.data(),result.size());return result;
}
std::vector<uint8_t> ReadEdram(RenderDevice& device,RenderCommandQueue& queue,const NativeMemoryState& source) {
    auto buffer=device.createBuffer(RenderBufferDesc::ReadbackBuffer(source.capacity));
    auto commands=queue.createCommandList();auto fence=device.createCommandFence();commands->begin();
    auto& native=static_cast<VulkanBuffer&>(*buffer);const auto cmd=static_cast<VulkanCommandList&>(*commands).vk;
    VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};barrier.buffer=source.buffer;barrier.size=VK_WHOLE_SIZE;
    barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    barrier.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,1,&barrier,0,nullptr);
    const VkBufferCopy copy{0,0,source.capacity};vkCmdCopyBuffer(cmd,source.buffer,native.vk,1,&copy);
    VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);
    commands->end();queue.executeCommandLists(commands.get(),fence.get());queue.waitForCommandFence(fence.get());
    const auto* mapped=static_cast<const uint8_t*>(buffer->map());
    Check(vmaInvalidateAllocation(native.device->allocator,native.allocation,0,VK_WHOLE_SIZE)==VK_SUCCESS,"Readback invalidate failed");
    std::vector<uint8_t> result(mapped,mapped+source.capacity);buffer->unmap();return result;
}
}
int main(int argc,char** argv)try {
    if(argc!=3)throw std::runtime_error("Usage: fsi-variant-scene-test red-native-fsi.spv fresh-builtin-depth.spv");
    const auto red=Read(argv[1]),depth=Read(argv[2]);
    FsiDevice host;auto& device=host.device();auto queue=device.createCommandQueue(RenderCommandListType::DIRECT);
    const std::string state_id(64,'a'),vs_id(64,'b'),ps_id(64,'c');std::vector<uint8_t> state(0x2a80);
    auto word=[&](size_t at,uint32_t value){for(unsigned i=0;i<4;++i)state.at(at+i)=uint8_t(value>>(24-i*8));};
    auto number=[&](size_t at,float value){word(at,std::bit_cast<uint32_t>(value));};
    word(0x2880,80);word(0x28bc,16|(16<<16));word(0x28c4,0x80000000);word(0x28c8,16|(16<<16));
    word(0x28dc,15);word(0x2944,1<<19);word(0x294c,0x43f);
    for(unsigned at:{0x2938,0x2958,0x295c,0x2960})word(at,0x10001);
    number(0x2908,8);number(0x290c,8);number(0x2910,-8);number(0x2914,8);number(0x2918,1);
    SceneExecutorConfig defaults;Check(!defaults.implicit_fsi_barriers,"Variant default changed");
    defaults.implicit_fsi_barriers=true;
    bool rejected=false;try{SceneExecutor invalid(defaults,&device,queue.get());}catch(const std::invalid_argument&){rejected=true;}
    Check(rejected,"Variant accepted fixed-function Scene");
    const Json surface{{"depth",false},{"edram_base",0},{"format",0},{"format_info",0},
        {"surface_info",80},{"width",16},{"height",16}};
    auto depth_surface=surface;depth_surface["depth"]=true;depth_surface["edram_base"]=1000;
    std::vector<uint8_t> initial(10u*1024*1024,0x5a);
    size_t cases=0;
    for(bool builtin:{false,true})for(bool prewarm:{false,true}) {
        std::vector<uint8_t> explicit_result;
        for(bool implicit:{false,true}) {
            unsigned vs_reads=0,ps_reads=0,depth_reads=0;
            SceneExecutorConfig config;config.native_fsi=true;config.ordered_pixel_interlock_enabled=true;
            config.implicit_fsi_barriers=implicit;config.defer_native_queries=true;
            config.payload=[&](const std::string& key){Check(key==state_id,"Unexpected payload");return state;};
            config.shader=[&](const std::string& key){Check(key==vs_id,"Unexpected VS");++vs_reads;return Bytes(fullscreen_spv);};
            config.fsi_shader=[&](const std::string& key){Check(key==ps_id,"Unexpected PS");++ps_reads;return Bytes(red);};
            config.fsi_depth_shader=[&]{++depth_reads;return Bytes(depth);};
            SceneExecutor scene(config,&device,queue.get());
            const NativeFsiPipelineRecipe recipe{vs_id,builtin?std::nullopt:std::optional<std::string>(ps_id),
                {uint32_t(RenderCullMode::NONE),uint32_t(RenderFrontFace::COUNTER_CLOCKWISE),1,
                    uint32_t(RenderPrimitiveTopology::TRIANGLE_LIST),0,0},int32_t(RenderFormat::R8G8B8A8_UNORM),-1};
            if(prewarm) {
                scene.PrewarmFsiPipeline(recipe,fullscreen_spv,builtin?depth:red);
                scene.PrewarmFsiPipeline(recipe,fullscreen_spv,builtin?depth:red);
                Check(!scene.mutation_serial()&&!scene.HasPendingGpuWork(),"Prewarm mutated state");
                Check(scene.report().at("pipelines_created")==1,"Prewarm recreated pipeline");
            }
            scene.InitializeEdram(initial);
            scene.Execute({{"type","query_begin"},{"frame",1},{"event",1},{"query_generation",1},{"query_handle",0x1000},{"report_address",0x2020}});
            for(unsigned n=0;n<2;++n)scene.Execute({{"type","draw"},{"frame",1},{"event",n+2},{"function","DrawPrimitive"},
                {"args",{0,4,0,3,0,0,0,0}},{"device_state",state_id},{"vertex_shader",vs_id},
                {"pixel_shader",builtin?Json(nullptr):Json(ps_id)},
                {"surfaces",builtin?Json{{"6",surface},{"10",depth_surface}}:Json{{"6",surface}}},
                {"geometry",Json::array()},{"vertex_slots",Json::array()},{"textures",Json::array()}});
            scene.Execute({{"type","query_end"},{"frame",1},{"event",4},{"query_generation",1},{"query_handle",0x1000},{"report_address",0x2000}});
            auto actual=ReadEdram(device,*queue,scene.ExportState().edram);
            const auto completed=scene.TakeCompletedQueries();
            Check(completed.size()==1&&completed[0].result.samples==512&&completed[0].result.draws==2,"Variant query samples changed");
            Check(vs_reads==(prewarm?0u:1u)&&ps_reads==(!builtin&&!prewarm?1u:0u)&&depth_reads==(builtin&&!prewarm?1u:0u),"Provider hit/miss or builtin selection changed");
            Check(scene.report().at("pipelines_created")==1,"First Draw missed prewarmed variant");
            Check(scene.report().contains("shader_variant")==implicit,"Variant report identity changed");
            if(builtin)Check(actual==initial,"Builtin depth-only changed bound color");
            else for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
                const auto address=SurfaceByteOffset(SurfaceLayout{16,16,80,0,0,false},x,y);uint32_t color;
                std::memcpy(&color,actual.data()+address,4);Check(color==0xff0000ff,"Actual native red pixel changed");
            }
            if(!implicit)explicit_result=std::move(actual);else Check(actual==explicit_result,"Explicit/implicit EDRAM differs");
            ++cases;
        }
    }
    std::cout<<"PASS: "<<cases<<" Scene variants, ordinary/prewarm/builtin, 4096 real samples and full10MiB pair parity\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
