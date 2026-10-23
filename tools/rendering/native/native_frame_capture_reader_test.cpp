#include "renderer/native/native_frame_capture_reader.h"
#include <iostream>
#include <unistd.h>
using namespace gow3::renderer;
using Json=nlohmann::json;
namespace fs=std::filesystem;
namespace {
size_t checks=0;
void Check(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
template<class F>void Refuse(F&& f){bool refused=false;try{f();}catch(const std::exception&){refused=true;}Check(refused,"Malformed Native capture accepted");}
Json ReadJson(const fs::path& p){std::ifstream f(p);return Json::parse(f);}
void WriteJson(const fs::path& p,const Json& j){std::ofstream f(p);f<<j.dump(2)<<'\n';f.close();Check(bool(f),"Test metadata write failed");}
std::string HashFile(const fs::path& p){std::ifstream f(p,std::ios::binary);std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)),{});return ContentHash(b);}
CanonicalResourceRef Add(LiveRenderFrame& frame,std::vector<uint8_t> data,bool shader=false){
    auto bytes=std::make_shared<const std::vector<uint8_t>>(std::move(data));const auto hash=ContentHash(*bytes);
    (shader?frame.shaders:frame.payloads).emplace(hash,bytes);return {CanonicalDigest(hash),bytes};
}
TypedTextureIdentity Identity(){return {0x100,3,4,4,32,0,1,1,false,false};}
TypedLiveDraw MakeDraw(LiveRenderFrame& frame,DrawFunction function,uint64_t event){
    TypedLiveDraw d;d.frame=frame.frame;d.event=event;d.function=function;
    d.args={0,1,2,3,4,5,6,UINT32_MAX};d.device_state=Add(frame,std::vector<uint8_t>(0x3600,0x42));
    d.lazy_device_state=std::make_shared<LazyDeviceState>(d.device_state.owner);
    d.vertex_shader=Add(frame,{0x10,0x2a,0x11,1},true);d.pixel_shader=Add(frame,{0x10,0x2a,0x11,0},true);
    const auto payload=Add(frame,{1,2,3,4});const auto mip=Add(frame,{5,6,7,8});
    for(unsigned i=0;i<5;++i)d.surfaces[i]=TypedSurface{0xa000+i*64,1280,720,80,3,900,3,i==4};
    d.geometry.push_back({14,0xa0000000,4,payload,true,95,3});
    d.up_geometry.push_back({96,0x2000,4,payload,false,{},{}});d.vertex_fetch_slots={0,95};
    for(unsigned i=0;i<3;++i){TypedTexture t;t.slot=i;t.fetch={1,2,3,4,5,UINT32_MAX};t.source=TextureSource(i);
        if(i==0){t.base_payload=payload;t.mip_payload=mip;}if(i!=1)t.identity=Identity();d.textures.push_back(std::move(t));}
    d.rectangle_list=function==DrawFunction::BeginVertices;return d;
}
Json Op(uint32_t frame,uint64_t event,std::string type){return { {"frame",frame},{"event",event},{"type",std::move(type)} };}
LiveRenderFrame Frame(uint32_t frame){
    LiveRenderFrame f;f.frame=frame;const uint64_t first=uint64_t(frame)*10;
    auto d=MakeDraw(f,frame==7?DrawFunction::DrawPrimitive:DrawFunction::BeginVertices,first);
    auto write=Op(frame,first,"cpu_memory_write");write.update({{"address",0x100},{"bytes",4},{"payload",d.geometry[0].payload.hash.View()},{"coherence_epoch",UINT64_MAX}});f.operations.push_back(write);
    auto begin=Op(frame,first,"query_begin");begin.update({{"query_handle",99},{"query_generation",frame},{"report_address",0x220},{"record_index",1}});f.operations.push_back(begin);
    f.operations.push_back(d);
    d.event=first+1;d.lazy_device_state.reset();d.rectangle_list=false;d.function=frame==7?DrawFunction::DrawIndexedPrimitive:DrawFunction::BeginIndexedVertices;f.operations.push_back(d);
    auto copy=Op(frame,first+2,"memory_copy");copy.update({{"source",0x100},{"destination",0x180},{"bytes",4},{"source_kind","mixed_memory"},{"gpu_ranges",Json::array({Json::array({0x101,0x103})})},{"payload",d.geometry[0].payload.hash.View()},{"shader",d.vertex_shader->hash.View()},{"published_to_guest",false}});f.operations.push_back(copy);
    auto clear=Op(frame,first+3,"clearrect");clear.update({{"flags",0x31},{"rectangle",Json::array({-4,0,1280,720})},{"depth",0.5},{"stencil",17},{"color",Json::array({0.0,1.0,0.5,1.0})},{"surfaces",Json::object({{"6",ToJson(*d.surfaces[0])}})}});f.operations.push_back(clear);
    auto resolve=Op(frame,first+4,"resolve");resolve.update({{"flags",0},{"source_slot",0},{"level",0},{"exponent_bias",-2},{"source_rectangle",Json::array({0,0,4,4})},{"destination_rectangle",Json::array({0,0,4,4})},{"destination",ToJson(Identity())},{"destination_fetch",Json::array({1,2,3,4,5,6})},{"copy_dest_swap",false},{"surfaces",Json::object({{"6",ToJson(*d.surfaces[0])}})}});f.operations.push_back(resolve);
    auto end=Op(frame,first+5,"query_end");end.update({{"query_handle",99},{"query_generation",frame},{"report_address",0x200},{"record_index",0}});f.operations.push_back(end);
    auto present=Op(frame,first+6,"present");present.update({{"frontbuffer",ToJson(Identity())},{"frontbuffer_fetch",Json::array({1,2,3,4,5,6})},{"provenance","captured_present_resource"}});f.operations.push_back(present);return f;
}
Json Query(uint32_t frame){return Json::array({{{"frame",frame},{"generation",frame},{"end_event",frame*10+5},{"handle",99},{"end_address",0x200},{"begin_address",0x220},{"samples",UINT64_MAX},{"draws",2}}});}
void ReplaceMetadata(const fs::path& directory,const std::string& name,const Json& value){
    auto manifest=ReadJson(directory/"native-frame-capture.json");const auto old=manifest["files"][name];
    WriteJson(directory/name,value);auto identity=old;identity["bytes"]=fs::file_size(directory/name);identity["sha256"]=HashFile(directory/name);
    manifest["files"][name]=identity;
    for(const auto* key:{"scene","initial_history","shader_manifest"})if(manifest[key]["path"]==name)manifest[key]=identity;
    for(auto& f:manifest["frames"])if(f["state"]["path"]==name)f["state"]=identity;
    manifest["metadata_bytes_before_completion"]=manifest.at("metadata_bytes_before_completion").get<uint64_t>()-old.at("bytes").get<uint64_t>()+identity.at("bytes").get<uint64_t>();
    WriteJson(directory/"native-frame-capture.json",manifest);
}
}
int main()try{
    const auto directory=fs::temp_directory_path()/("gow3-native-frame-reader-"+std::to_string(getpid()));
    Check(!fs::exists(directory),"Test path already exists");
    NativeFrameCaptureSnapshot snapshot;snapshot.edram.resize(2048*5120,0x63);snapshot.physical={ {0x100,{1,2,3,4}},{0x200,{5,6,7,8,9,10,11,12}} };
    auto a=Frame(7),b=Frame(8);NativeFrameCaptureWriter writer(directory,7,snapshot);
    snapshot.edram[100]=0x27;writer.AppendCompletedFrame(a,snapshot,Query(7));snapshot.edram[200]=0x28;writer.AppendCompletedFrame(b,snapshot,Query(8));const auto written=writer.Finish();
    auto result=ReadNativeFrameCapture(directory);
    Check(result.frames.size()==2&&result.manifest==written&&result.manifest_sha256==HashFile(directory/"native-frame-capture.json"),"Completed Native capture identity lost");
    Check(result.frames[0]->operations.ToJson()==a.operations.ToJson()&&result.frames[1]->operations.ToJson()==b.operations.ToJson(),"Typed/control round-trip differs");
    Check(result.frames[0]->operations[0].Event()==result.frames[0]->operations[1].Event()&&result.frames[0]->operations[1].Event()==result.frames[0]->operations[2].Event(),"Repeated Native event IDs lost");
    Check(result.frames[0]->operations[2].Draw()&&result.frames[0]->operations[3].Draw()&&!result.frames[0]->operations[0].Draw(),"Draw/control representation changed");
    Check(result.frames[0]->operations[2].Draw()->device_state.owner==result.frames[1]->operations[2].Draw()->device_state.owner,"Immutable bank sharing lost");
    const auto state=ReadNativeFrameCaptureState(directory,"native-state-8.json");Check(state.snapshot.edram==snapshot.edram&&state.snapshot.physical[1].bytes==snapshot.physical[1].bytes&&state.query_results==Query(8),"Completed state bytes/query lost");
    const auto original_scene=ReadJson(directory/"scene.json");const auto original_manifest=ReadJson(directory/"native-frame-capture.json");
    const auto mutate_scene=[&](auto change){auto bad=original_scene;change(bad);ReplaceMetadata(directory,"scene.json",bad);Refuse([&]{(void)ReadNativeFrameCapture(directory);});ReplaceMetadata(directory,"scene.json",original_scene);};
    mutate_scene([](Json& j){j["operations"][2]["args"][0]=-1;});
    mutate_scene([](Json& j){j["operations"][2]["args"][0]=uint64_t(UINT32_MAX)+1;});
    mutate_scene([](Json& j){j["operations"][2]["args"].push_back(0);});
    mutate_scene([](Json& j){j["operations"][2]["function"]="unknown";});
    mutate_scene([](Json& j){j["operations"][2]["extra"]=true;});
    mutate_scene([](Json& j){j["operations"][2]["surfaces"]["6"]["depth"]=0;});
    mutate_scene([](Json& j){j["operations"][2]["geometry"][0]["bytes"]=5;});
    mutate_scene([](Json& j){j["operations"][2]["device_state"]=j["operations"][2]["geometry"][0]["sha256"];});
    mutate_scene([](Json& j){j["operations"][2]["textures"][0]["slot"]=32;});
    mutate_scene([](Json& j){j["operations"][2]["textures"][0]["fetch"][0]=1.0;});
    mutate_scene([](Json& j){j["operations"][2]["textures"][0]["source"]="unknown";});
    mutate_scene([](Json& j){j["operations"][2]["vertex_slots"].push_back(95);});
    mutate_scene([](Json& j){j["operations"][2]["pixel_shader"]=j["operations"][2]["vertex_shader"];});
    mutate_scene([](Json& j){j["operations"][2]["vertex_shader"]=j["operations"][2]["geometry"][0]["sha256"];});
    mutate_scene([](Json& j){j["operations"][2]["event"]=69;});
    mutate_scene([](Json& j){j["operations"][2]["frame"]=8;});
    mutate_scene([](Json& j){j["operations"][8]["type"]="draw";});
    mutate_scene([](Json& j){j["operations"][7]["query_generation"]=123;});
    mutate_scene([](Json& j){j["operations"][0]["bytes"]=uint64_t(1)<<32;});
    mutate_scene([](Json& j){j["operations"][4]["gpu_ranges"][0][1]=0x200;});
    mutate_scene([](Json& j){j["operations"][8]["provenance"]="synthetic";});
    const auto history=ReadJson(directory/"gpu-history.json");
    const auto mutate_history=[&](auto change){auto bad=history;change(bad);ReplaceMetadata(directory,"gpu-history.json",bad);Refuse([&]{(void)ReadNativeFrameCapture(directory);});ReplaceMetadata(directory,"gpu-history.json",history);};
    mutate_history([](Json& j){j["provenance"]="reference_gpu_frame_boundary";});
    mutate_history([](Json& j){j["ranges"][1]["address"]=0x100;});
    mutate_history([](Json& j){j["ranges"][0]["address"]=0x101;});
    mutate_history([](Json& j){j["ranges"][0]["address"]=0x20000000;});
    mutate_history([](Json& j){j["physical_bytes"]=0;});
    mutate_history([](Json& j){j["edram"]["path"]="../outside";});
    auto end=ReadJson(directory/"native-state-7.json");auto bad_end=end;bad_end["query_results"][0]["generation"]=7.0;
    ReplaceMetadata(directory,"native-state-7.json",bad_end);Refuse([&]{(void)ReadNativeFrameCapture(directory);});ReplaceMetadata(directory,"native-state-7.json",end);
    Refuse([&]{(void)ReadNativeFrameCaptureState(directory,"../gpu-history.json");});
    auto bad=original_manifest;bad["complete"]=false;WriteJson(directory/"native-frame-capture.json",bad);Refuse([&]{(void)ReadNativeFrameCapture(directory);});WriteJson(directory/"native-frame-capture.json",original_manifest);
    bad=original_manifest;bad["files"]["scene.json"]["bytes"]=uint64_t(1)<<40;WriteJson(directory/"native-frame-capture.json",bad);Refuse([&]{(void)ReadNativeFrameCapture(directory);});WriteJson(directory/"native-frame-capture.json",original_manifest);
    const auto shader_name=std::string(a.operations[2].Draw()->vertex_shader->hash.View())+".shader.bin";
    {std::fstream f(directory/shader_name,std::ios::in|std::ios::out|std::ios::binary);f.put('X');}
    Refuse([&]{(void)ReadNativeFrameCapture(directory);});
    fs::remove(directory/shader_name);fs::create_symlink("scene.json",directory/shader_name);Refuse([&]{(void)ReadNativeFrameCapture(directory);});
    auto retained=result.frames[0]->operations.Slice(2,3);std::weak_ptr<const std::vector<uint8_t>> lifetime=retained[0].Draw()->device_state.owner;
    result.frames.clear();fs::remove_all(directory);Check(!lifetime.expired()&&retained[0].Draw()->device_state.owner->at(0)==0x42,"Released capture invalidated typed owner");
    retained=LiveOperations{};Check(lifetime.expired(),"Reader leaked immutable resource owner");
    std::cout<<"PASS "<<checks<<" checks: exact typed/control round-trip, repeated events, native state/query identity, bounded malformed inputs, immutable owner lifetime; CPU serialization only\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
