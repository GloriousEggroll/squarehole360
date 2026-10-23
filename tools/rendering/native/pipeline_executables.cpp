// Compile-only diagnostics of original Native FSI graphics PSOs. No command
// list, framebuffer, queue submission, draw, query, or presentation is created.
#include "renderer/native/fsi_device.h"
#include "renderer/native/pipeline_prewarm.h"
#include "renderer/native/fsi_state_specialization_recipe.h"
#include "renderer/native/content_hash.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <set>
#include <stdexcept>
#include <vector>
using namespace gow3::renderer;
using namespace plume;
using Json=nlohmann::json;
namespace {
constexpr size_t kInputBytes=16u*1024*1024,kManifestBytes=64u*1024;
constexpr uint32_t kPipelines=8,kExecutables=8,kStatistics=64,kRepresentations=16;
constexpr size_t kRepresentationBytes=512u*1024,kTotalIrBytes=8u*1024*1024;
constexpr const char* kVertex="f7dba9374d4cba41785f4e2810d881f559a2f3b0cbc40fc8a1f58ee49f4eb969";
constexpr const char* kPixel="49f26283e1436f4cce68ac4e75695c6375176768b1dbfbf6c1f79d9bef434ced";
void Require(bool yes,const char* what){if(!yes)throw std::runtime_error(what);}
std::vector<uint8_t> Read(const std::filesystem::path& p,size_t maximum) {
    std::ifstream f(p,std::ios::binary|std::ios::ate);Require(bool(f),"Input unavailable");
    const auto n=f.tellg();Require(n>=0&&uint64_t(n)<=maximum,"Input exceeds diagnostic bound");
    std::vector<uint8_t> b(static_cast<size_t>(n));f.seekg(0);
    if(!b.empty())Require(bool(f.read(reinterpret_cast<char*>(b.data()),std::streamsize(b.size()))),"Input truncated");return b;
}
std::vector<uint32_t> Spirv(const Json& entry,uint32_t execution_model) {
    const auto bytes=Read(entry.at("path").get<std::string>(),kInputBytes);
    Require(bytes.size()>=20&&bytes.size()%4==0&&ContentHash(bytes)==entry.at("sha256").get<std::string>(),"SPIR-V extent/hash mismatch");
    std::vector<uint32_t> words(bytes.size()/4);
    for(size_t i=0;i<words.size();++i)words[i]=uint32_t(bytes[i*4])|uint32_t(bytes[i*4+1])<<8|uint32_t(bytes[i*4+2])<<16|uint32_t(bytes[i*4+3])<<24;
    Require(words[0]==0x07230203&&words[1]>=0x10000&&words[1]<=0x10600&&words[3]&&words[4]==0,"Invalid SPIR-V header");
    bool main=false;
    for(size_t at=5;at<words.size();) {
        const size_t count=words[at]>>16;const auto op=words[at]&65535;
        Require(count&&count<=words.size()-at,"Malformed SPIR-V instruction");
        if(op==15) {
            Require(count>=4&&words[at+1]==execution_model,"SPIR-V stage mismatch");
            const auto* name=reinterpret_cast<const char*>(&words[at+3]);
            const auto length=(count-3)*4;const auto* end=static_cast<const char*>(std::memchr(name,0,length));
            Require(end,"Unterminated SPIR-V entrypoint");main|=std::string_view(name,size_t(end-name))=="main";
        }
        at+=count;
    }
    Require(main,"SPIR-V main entrypoint absent");return words;
}
template<size_t N>std::string Text(const char(&s)[N]) {return {s,std::find(s,s+N,char(0))};}
Json Value(const VkPipelineExecutableStatisticKHR& s) {
    switch(s.format) {
    case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR:return bool(s.value.b32);
    case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR:return s.value.i64;
    case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR:return s.value.u64;
    case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_FLOAT64_KHR:
        if(std::isfinite(s.value.f64))return s.value.f64;
        return Json{{"nonfinite",true},{"ieee754_bits",std::bit_cast<uint64_t>(s.value.f64)}};
    default:return Json{{"unknown_format",int(s.format)}};
    }
}
struct Dispatch {
    PFN_vkGetPipelineExecutablePropertiesKHR properties=nullptr;
    PFN_vkGetPipelineExecutableStatisticsKHR statistics=nullptr;
    PFN_vkGetPipelineExecutableInternalRepresentationsKHR representations=nullptr;
    explicit Dispatch(VkDevice d) {
        properties=reinterpret_cast<decltype(properties)>(vkGetDeviceProcAddr(d,"vkGetPipelineExecutablePropertiesKHR"));
        statistics=reinterpret_cast<decltype(statistics)>(vkGetDeviceProcAddr(d,"vkGetPipelineExecutableStatisticsKHR"));
        representations=reinterpret_cast<decltype(representations)>(vkGetDeviceProcAddr(d,"vkGetPipelineExecutableInternalRepresentationsKHR"));
    }
};
Json Internal(const Dispatch& fn,VkDevice device,const VkPipelineExecutableInfoKHR& info,
              const std::filesystem::path& directory,uint32_t pipeline_index,uint32_t executable_index,size_t& consumed) {
    uint32_t count=0;VkResult result=fn.representations(device,&info,&count,nullptr);
    Json report={{"available",count},{"vk_result",int(result)},{"items",Json::array()},{"complete",false}};
    if(result!=VK_SUCCESS&&result!=VK_INCOMPLETE)return report;
    const auto capacity=std::min(count,kRepresentations);std::vector<VkPipelineExecutableInternalRepresentationKHR> ir(capacity);
    for(auto& i:ir)i.sType=VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INTERNAL_REPRESENTATION_KHR;
    if(!capacity){report["complete"]=true;return report;}
    count=capacity;result=fn.representations(device,&info,&count,ir.data());report["metadata_vk_result"]=int(result);
    if(result!=VK_SUCCESS&&result!=VK_INCOMPLETE)return report;
    ir.resize(std::min(count,capacity));std::vector<std::vector<uint8_t>> data(ir.size());
    bool refused=report["available"].get<uint32_t>()>capacity||result==VK_INCOMPLETE;
    for(size_t i=0;i<ir.size();++i) {
        const auto n=ir[i].dataSize;
        if(n>kRepresentationBytes||n>kTotalIrBytes-consumed){refused=true;continue;}
        if(n){data[i].resize(n);consumed+=n;ir[i].pData=data[i].data();}
    }
    count=uint32_t(ir.size());result=fn.representations(device,&info,&count,ir.data());report["data_vk_result"]=int(result);
    if(result!=VK_SUCCESS&&result!=VK_INCOMPLETE)return report;
    for(size_t i=0;i<std::min<size_t>(count,ir.size());++i) {
        Json item={{"name",Text(ir[i].name)},{"description",Text(ir[i].description)},
                   {"is_text",bool(ir[i].isText)},{"reported_bytes",ir[i].dataSize}};
        if(data[i].empty()){item["status"]=ir[i].dataSize?"budget-refused":"empty";report["items"].push_back(item);continue;}
        Require(ir[i].dataSize<=data[i].size(),"Driver returned excessive representation length");data[i].resize(ir[i].dataSize);
        const auto name="pipeline-"+std::to_string(pipeline_index)+"-executable-"+std::to_string(executable_index)+"-representation-"+std::to_string(i)+(ir[i].isText?".txt":".bin");
        std::ofstream out(directory/name,std::ios::binary);Require(bool(out),"Cannot create representation artifact");
        out.write(reinterpret_cast<const char*>(data[i].data()),std::streamsize(data[i].size()));Require(bool(out),"Representation artifact write failed");
        item["file"]=name;item["sha256"]=ContentHash(data[i]);item["status"]=result==VK_SUCCESS?"complete":"incomplete";
        report["items"].push_back(std::move(item));
    }
    report["complete"]=!refused&&result==VK_SUCCESS;return report;
}
Json Executables(const Dispatch& fn,VkDevice device,VkPipeline pipeline,bool capture_ir,
                 const std::filesystem::path& directory,uint32_t pipeline_index,size_t& ir_bytes) {
    VkPipelineInfoKHR info{VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR};info.pipeline=pipeline;
    uint32_t count=0;VkResult result=fn.properties(device,&info,&count,nullptr);
    Json report={{"available",count},{"vk_result",int(result)},{"executables",Json::array()},{"complete",false}};
    if(result!=VK_SUCCESS&&result!=VK_INCOMPLETE)return report;
    const auto capacity=std::min(count,kExecutables);std::vector<VkPipelineExecutablePropertiesKHR> props(capacity);
    for(auto& p:props)p.sType=VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR;
    if(!capacity){report["complete"]=true;return report;}
    count=capacity;result=fn.properties(device,&info,&count,props.data());report["properties_vk_result"]=int(result);
    if(result!=VK_SUCCESS&&result!=VK_INCOMPLETE)return report;
    bool complete=result==VK_SUCCESS&&report["available"].get<uint32_t>()<=capacity;
    for(uint32_t i=0;i<std::min(count,capacity);++i) {
        VkPipelineExecutableInfoKHR executable{VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR};
        executable.pipeline=pipeline;executable.executableIndex=i;
        Json e={{"index",i},{"name",Text(props[i].name)},{"description",Text(props[i].description)},
                {"stages",props[i].stages},{"subgroup_size",props[i].subgroupSize},{"statistics",Json::array()}};
        uint32_t n=0;result=fn.statistics(device,&executable,&n,nullptr);e["statistics_available"]=n;e["statistics_vk_result"]=int(result);
        if(result!=VK_SUCCESS&&result!=VK_INCOMPLETE){complete=false;report["executables"].push_back(std::move(e));continue;}
        const auto cap=std::min(n,kStatistics);std::vector<VkPipelineExecutableStatisticKHR> statistics(cap);
        for(auto& s:statistics)s.sType=VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR;
        if(cap){n=cap;result=fn.statistics(device,&executable,&n,statistics.data());}
        e["statistics_data_vk_result"]=int(result);
        e["statistics_complete"]=result==VK_SUCCESS&&e["statistics_available"].get<uint32_t>()<=cap;
        complete&=e["statistics_complete"].get<bool>();
        if(result==VK_SUCCESS||result==VK_INCOMPLETE)for(uint32_t j=0;j<std::min(n,cap);++j)
            e["statistics"].push_back({{"name",Text(statistics[j].name)},{"description",Text(statistics[j].description)},
                {"format",int(statistics[j].format)},{"value",Value(statistics[j])}});
        if(capture_ir){e["internal_representations"]=Internal(fn,device,executable,directory,pipeline_index,i,ir_bytes);complete&=e["internal_representations"]["complete"].get<bool>();}
        report["executables"].push_back(std::move(e));
    }
    report["complete"]=complete;return report;
}
}
int main(int argc,char** argv) {
    try {
        Require(argc>=3,"Usage: gow3-native-pipeline-executables INPUT.json OUTPUT_DIRECTORY [--enable] [--capture-ir]");
        bool enable=false,capture_ir=false;
        for(int i=3;i<argc;++i){const std::string_view a=argv[i];if(a=="--enable")enable=true;else if(a=="--capture-ir")capture_ir=true;else Require(false,"Unknown option");}
        const auto bytes=Read(argv[1],kManifestBytes);const auto input=Json::parse(bytes.begin(),bytes.end());
        Require(input.at("schema")==1&&input.at("vertex_source")==kVertex&&input.at("pixel_source")==kPixel,"Unsupported diagnostic input identities");
        Require(input.at("vertex_specialization_mask")==0&&input.at("pixel_specialization_mask")==1794,"Unexpected runtime specialization masks");
        const auto& recipes=input.at("recipes");const auto& cases=input.at("cases");
        Require(recipes.is_array()&&cases.is_array()&&!recipes.empty()&&!cases.empty()&&recipes.size()<=2&&cases.size()<=3&&recipes.size()*cases.size()<=kPipelines,"Pipeline selection exceeds bound");
        std::vector<NativeFsiPipelineRecipe> parsed;
        for(const auto& r:recipes) {
            NativeFsiPipelineRecipe recipe{kVertex,std::string(kPixel),r.at("raster").get<std::array<uint32_t,6>>(),r.at("color").get<int32_t>(),r.at("depth").get<int32_t>()};
            Require(ValidNativeFsiPipelineRecipe(recipe)&&recipe.color_format==20&&recipe.depth_format==-1&&recipe.raster[4]==0&&recipe.raster[5]==0&&
                    (recipe.raster==std::array<uint32_t,6>{3,1,1,4,0,0}||recipe.raster==std::array<uint32_t,6>{3,2,1,4,0,0}),"Recipe differs from recorded target host raster keys");parsed.push_back(recipe);
        }
        struct Case {std::string name;std::vector<uint32_t> vertex,pixel;Json provenance;std::optional<NativeFsiStateSpecialization> state;};std::vector<Case> loaded;
        Require(cases.front().at("name")=="original","Original case must precede candidates");
        std::set<std::string> case_names;
        for(const auto& c:cases) {
            const auto name=c.at("name").get<std::string>();
            Require(case_names.insert(name).second&&
                (name=="original"||name=="candidate-vertex"||name=="candidate-pixel"||name=="candidate-state"),"Unknown/duplicate candidate case");
            if(name=="candidate-vertex")Require(c.at("pixel").at("sha256")==cases.front().at("pixel").at("sha256"),"Vertex candidate changes companion pixel stage");
            if(name=="candidate-pixel")Require(c.at("vertex").at("sha256")==cases.front().at("vertex").at("sha256"),"Pixel candidate changes companion vertex stage");
            std::optional<NativeFsiStateSpecialization> state;
            if(name=="candidate-state") {
                Require(c.at("vertex").at("sha256")==cases.front().at("vertex").at("sha256"),"State candidate changes companion vertex");
                auto tuple=recipes.front();tuple["vertex"]=kVertex;tuple["pixel"]=kPixel;tuple["fsi_state"]=c.at("fsi_state");
                state=ParseFsiStateRecipe(tuple).state;
            } else Require(!c.contains("fsi_state"),"Only explicit state candidate can override pipeline constants");
            auto v=Spirv(c.at("vertex"),0),p=Spirv(c.at("pixel"),4);
            loaded.push_back({name,std::move(v),std::move(p),c,state});
        }
        const std::filesystem::path directory=argv[2];std::filesystem::create_directories(directory);
        Json report={{"schema",1},{"diagnostic_enabled",enable},{"capture_ir",capture_ir},{"input_sha256",ContentHash(bytes)},
            {"input",input},{"pipelines",Json::array()},{"complete",false},{"gpu_submissions",0},
            {"limits",{{"pipelines",kPipelines},{"executables_per_pipeline",kExecutables},{"statistics_per_executable",kStatistics},{"representations_per_executable",kRepresentations},{"bytes_per_representation",kRepresentationBytes},{"total_ir_bytes",kTotalIrBytes}}},
            {"limitations",{"Compile-time driver statistics, not a workload profiler or measured occupancy","Owned FsiDevice enabled core/Vulkan12 features differ from the SDK runtime feature subset; exact live-game ISA parity is not established","Statistic names/units and available ISA are implementation-dependent; missing spills/register fields are unknown, not zero","Capture flags preserve compiled pipeline behavior but may disable caching or increase compilation cost; exclude this run from FPS comparisons","No game runtime extension enable, queue submission, frame capture or Xenia rendering"}}};
        auto save=[&]{std::ofstream out(directory/"pipeline-executables.json");out<<report.dump(2)<<'\n';Require(bool(out),"Cannot save diagnostic report");};
        if(!enable){report["status"]="disabled: pass --enable for owned-device compile diagnostics";save();std::cout<<report.dump(2)<<'\n';return 0;}
        FsiDevice owned(false,true);auto& device=owned.device();auto& vk=static_cast<VulkanDevice&>(device);
        VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};properties.pNext=&driver;
        vkGetPhysicalDeviceProperties2(vk.physicalDevice,&properties);
        report["device"]={{"name",properties.properties.deviceName},{"vendor_id",properties.properties.vendorID},{"device_id",properties.properties.deviceID},{"api_version",properties.properties.apiVersion},{"driver_version",properties.properties.driverVersion},{"driver_id",driver.driverID},{"driver_name",Text(driver.driverName)},{"driver_info",Text(driver.driverInfo)}};
        report["pipeline_executable_info_enabled"]=owned.pipeline_executable_info_enabled();
        if(!owned.pipeline_executable_info_enabled()){report["status"]="unsupported device feature/extension";save();std::cout<<report.dump(2)<<'\n';return 3;}
        const Dispatch fn(vk.vk);Require(fn.properties&&fn.statistics&&(!capture_ir||fn.representations),"Enabled extension dispatch unavailable");
        // Same five descriptor-set ABI and 24-byte push range as SceneExecutor.
        const RenderDescriptorRange images(RenderDescriptorRangeType::TEXTURE,0,32),samplers(RenderDescriptorRangeType::SAMPLER,0,32),edram(RenderDescriptorRangeType::READ_WRITE_BYTE_ADDRESS_BUFFER,0,1);
        const std::array<RenderDescriptorSetDesc,5> sets{{{&images,1,true,32},{&images,1,true,32},{&images,1,true,32},{&samplers,1,true,32},{&edram,1}}};
        const RenderPushConstantRange push(0,0,0,24,RenderShaderStageFlag::VERTEX|RenderShaderStageFlag::PIXEL);
        auto layout=device.createPipelineLayout(RenderPipelineLayoutDesc(&push,1,sets.data(),sets.size()));Require(bool(layout),"Pipeline layout creation failed");
        bool complete=true;size_t ir_bytes=0;uint32_t ordinal=0;
        for(const auto& c:loaded) {
            auto vs=device.createShader(c.vertex.data(),c.vertex.size()*4,"main",RenderShaderFormat::SPIRV),ps=device.createShader(c.pixel.data(),c.pixel.size()*4,"main",RenderShaderFormat::SPIRV);
            Require(vs&&ps&&static_cast<VulkanShader*>(vs.get())->vk&&static_cast<VulkanShader*>(ps.get())->vk,"Shader module creation failed");
            for(const auto& r:parsed) {
                NativeFsiPipelineDescriptor descriptor(r.raster,r.color_format,r.depth_format,layout.get(),vs.get(),ps.get(),c.state?&*c.state:nullptr);
                descriptor.descriptor.captureExecutableStatistics=true;descriptor.descriptor.captureExecutableInternalRepresentations=capture_ir;
                auto pipeline=device.createGraphicsPipeline(descriptor.descriptor);
                Require(pipeline&&static_cast<VulkanGraphicsPipeline*>(pipeline.get())->vk,"Diagnostic pipeline creation failed");
                auto data=Executables(fn,vk.vk,static_cast<VulkanGraphicsPipeline*>(pipeline.get())->vk,capture_ir,directory,ordinal,ir_bytes);
                complete&=data["complete"].get<bool>();report["pipelines"].push_back({{"ordinal",ordinal++},{"case",c.name},{"raster",r.raster},{"color",r.color_format},{"depth",r.depth_format},{"specialization_constant_0",r.raster[4]},{"fsi_state",c.state?Json(c.state->words):Json(nullptr)},{"capture_statistics",true},{"capture_ir",capture_ir},{"data",std::move(data)}});
                // All queried metadata is copied before this private PSO dies.
            }
        }
        report["complete"]=complete;report["ir_allocation_volume_bytes"]=ir_bytes;report["status"]=complete?"complete":"incomplete or budget-limited";save();
        std::cout<<Json({{"report",(directory/"pipeline-executables.json").string()},{"complete",complete},{"pipelines",ordinal},{"gpu_submissions",0}}).dump(2)<<'\n';return complete?0:3;
    } catch(const std::exception& e){std::cerr<<"Executable diagnostic failed: "<<e.what()<<'\n';return 1;}
}
