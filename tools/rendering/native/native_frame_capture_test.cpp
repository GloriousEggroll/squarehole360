#include "renderer/native/native_frame_capture.h"
#include <iostream>

using namespace gow3::renderer;
using Json=nlohmann::json;
namespace fs=std::filesystem;
namespace {
size_t checks=0;
void Check(bool value,const char* reason) {++checks;if(!value)throw std::runtime_error(reason);}
template<class Fn>void Refuse(Fn&& fn,const char* reason) {
    bool refused=false;try {fn();}catch(const std::exception&) {refused=true;}Check(refused,reason);
}
Json ReadJson(const fs::path& path) {std::ifstream stream(path);return Json::parse(stream);}
void ReplaceJson(const fs::path& path,const Json& value) {std::ofstream stream(path);stream<<value.dump();}
LiveResourceBytes Owner(std::vector<uint8_t> bytes) {
    return std::make_shared<const std::vector<uint8_t>>(std::move(bytes));
}
CanonicalResourceRef Add(LiveRenderFrame& frame,LiveResourceBytes bytes,bool shader=false) {
    const auto hash=ContentHash(*bytes);(shader?frame.shaders:frame.payloads).emplace(hash,bytes);
    return {CanonicalDigest(hash),std::move(bytes)};
}
NativeFrameCaptureSnapshot Snapshot(uint8_t pattern=0) {
    NativeFrameCaptureSnapshot snapshot;
    snapshot.edram.assign(native_frame_capture_detail::kEdramBytes,pattern);
    snapshot.physical={{0x1000,{0,1,2,3,4,5,6,7}},{0x1010,{8,9,10,11}}};
    return snapshot;
}
constexpr uint64_t kEvent=uint64_t(UINT32_MAX)-9;
LiveRenderFrame Frame(uint32_t number,uint64_t event,bool lazy=false,bool embedded=true) {
    LiveRenderFrame frame;frame.frame=number;
    auto payload=Add(frame,Owner({2,4,6,8}));
    auto state=Add(frame,Owner(std::vector<uint8_t>(LazyDeviceState::kBytes,0x23)));
    // Artificial containers exercise serialization/stage identity only. This
    // test deliberately makes no shader compilation or Scene rendering claim.
    auto vertex=Add(frame,Owner({0x10,0x2a,0x11,0x01,1,2,3,4}),true);
    auto pixel=Add(frame,Owner({0x10,0x2a,0x11,0x00,4,3,2,1}),true);
    frame.operations.push_back(Json{{"type","cpu_memory_write"},{"frame",number},{"event",event},
        {"address",0x1000},{"bytes",4},{"payload",payload.hash.View()},{"coherence_epoch",uint64_t(900)}});
    frame.operations.push_back(Json{{"type","query_begin"},{"frame",number},{"event",event},
        {"query_handle",12},{"query_generation",number},{"report_address",0x2020},{"record_index",1}});
    TypedLiveDraw draw;draw.frame=number;draw.event=event+1;draw.device_state=state;
    draw.vertex_shader=vertex;draw.pixel_shader=pixel;
    draw.geometry.push_back({14,0x1000,4,payload,true,95,0});
    TypedTexture texture;texture.slot=0;texture.base_payload=payload;draw.textures.push_back(texture);
    if(lazy) {
        draw.lazy_device_state=std::make_shared<LazyDeviceState>(state.owner);
        frame.lazy_payloads=std::make_shared<LazyPayloadTable>();
        frame.lazy_payloads->Append(draw.lazy_device_state);
        frame.lazy_payloads->Append(payload.hash.View(),payload.owner);
        frame.lazy_payloads->Seal();frame.payloads.clear();
    }
    if(!embedded) {
        draw.device_state.owner.reset();draw.vertex_shader->owner.reset();draw.pixel_shader->owner.reset();
        for(auto& geometry:draw.geometry)geometry.payload.owner.reset();
        for(auto& texture:draw.textures)texture.base_payload->owner.reset();
    }
    frame.operations.push_back(std::move(draw));
    frame.operations.push_back(Json{{"type","memory_copy"},{"frame",number},{"event",event+2},
        {"source",0x1000},{"destination",0x3000},{"bytes",4},{"source_kind","mixed_memory"},
        {"payload",payload.hash.View()},{"shader",vertex.hash.View()},
        {"gpu_ranges",Json::array({Json::array({0x1000,0x1004})})},{"published_to_guest",false}});
    frame.operations.push_back(Json{{"type","query_end"},{"frame",number},{"event",event+3},
        {"query_handle",12},{"query_generation",number},{"report_address",0x2000},{"record_index",0}});
    frame.operations.push_back(Json{{"type","present"},{"frame",number},{"event",event+4},
        {"frontbuffer_fetch",std::array<uint32_t,6>{}},{"frontbuffer",Json::object()}});
    return frame;
}
Json Results(uint32_t frame,uint64_t event) {
    return Json::array({{{"frame",frame},{"handle",12},{"begin_address",0x2020},{"end_address",0x2000},
        {"generation",frame},{"end_event",event+3},{"samples",uint64_t(12345678901)},
        {"draws",1},{"published_to_guest",true}}});
}
void WritePair(const fs::path& root,bool embedded=true) {
    const auto initial=Snapshot();auto first=Frame(40,kEvent,false,embedded),second=Frame(41,kEvent+5,true,embedded);
    NativeFrameCaptureWriter writer(root,40,initial,{{"fixture","CPU serialization only"}});
    Check(!fs::exists(root/"native-frame-capture.json"),"Constructor published completion");
    writer.AppendCompletedFrame(first,initial,Results(40,kEvent));
    Check(!fs::exists(root/"native-frame-capture.json"),"One frame published completion");
    writer.AppendCompletedFrame(second,initial,Results(41,kEvent+5));
    const auto manifest=writer.Finish();
    Check(ValidateNativeFrameCapture(root)==manifest,"Completion validation changed metadata");
    const auto scene=ReadJson(root/"scene.json");
    Json expected=first.operations.ToJson();for(const auto& op:second.operations.ToJson())expected.push_back(op);
    Check(scene.at("operations")==expected,"Exact typed/control operation serialization changed");
    Check(scene.at("operations")[0].at("event")==scene.at("operations")[1].at("event"),"Repeated event ID lost");
    Check(scene.at("operations")[0].at("event").get<uint64_t>()==kEvent,"High valid event ID changed");
    Check(scene.at("operations")[2].at("geometry")[0].at("source")=="gpu_memory","Geometry provenance lost");
    Check(scene.at("operations")[3].at("gpu_ranges")==Json::array({Json::array({0x1000,0x1004})}),"Mixed-copy ownership lost");
    Check(ReadJson(root/"native-state-40.json").at("query_results")==Results(40,kEvent),"Actual query oracle changed");
    Check(ReadJson(root/"gpu-history.json").at("frame")==40,"Initial boundary mislabeled");
    Check(ReadJson(root/"native-state-40.json").at("frame")==41,"Completed boundary mislabeled");
    Check(manifest.at("history_payload_bytes")==initial.edram.size()+12,"Identical history payloads were not shared");
    Check(ReadJson(root/"manifest.json").at("shaders").size()==2,"Shader manifest failed namespace sharing");
    Refuse([&]{writer.Finish();},"Double completion accepted");
    Refuse([&]{writer.AppendCompletedFrame(second,initial,Results(41,kEvent+5));},"Append after completion accepted");
    Refuse([&]{NativeFrameCaptureWriter duplicate(root,40,initial);},"Existing capture overwritten");
}
void BadFrame(const fs::path& root,const NativeFrameCaptureSnapshot& initial,
              LiveRenderFrame frame,Json results) {
    NativeFrameCaptureWriter writer(root,40,initial);
    Refuse([&]{writer.AppendCompletedFrame(frame,initial,results);},"Invalid frame accepted");
    Refuse([&]{writer.Finish();},"Poisoned writer completed");
    Check(!fs::exists(root/"native-frame-capture.json"),"Failure published completion");
}
}
int main(int argc,char** argv) {
    try {
        if(argc!=2)throw std::runtime_error("Usage: native-frame-capture-test NEW-TEST-DIRECTORY");
        const fs::path root=argv[1];Check(fs::create_directory(root),"Test directory must be new");
        WritePair(root/"good");
        WritePair(root/"frame-owned",false);
        const auto initial=Snapshot();
        {
            NativeFrameCaptureWriter writer(root/"incomplete",40,initial);
            Refuse([&]{writer.Finish();},"Incomplete writer completed");
            Refuse([&]{ValidateNativeFrameCapture(root/"incomplete");},"Incomplete directory validated");
        }
        BadFrame(root/"frame-gap",initial,Frame(41,kEvent),Results(41,kEvent));
        BadFrame(root/"event-overflow",initial,Frame(40,uint64_t(UINT32_MAX)-3),Results(40,uint64_t(UINT32_MAX)-3));
        BadFrame(root/"missing-result",initial,Frame(40,kEvent),Json::array());
        auto wrong=Results(40,kEvent);wrong[0]["end_event"]=kEvent+4;
        BadFrame(root/"wrong-result",initial,Frame(40,kEvent),wrong);
        wrong=Results(40,kEvent);wrong[0]["samples"]=-1;
        BadFrame(root/"negative-samples",initial,Frame(40,kEvent),wrong);
        {
            auto frame=Frame(40,kEvent);frame.operations=frame.operations.Slice(0,5);
            BadFrame(root/"no-present",initial,std::move(frame),Results(40,kEvent));
        }
        {
            auto frame=Frame(40,kEvent);frame.operations.push_back(Json{{"type","present"},{"frame",40},{"event",kEvent+5}});
            BadFrame(root/"double-present",initial,std::move(frame),Results(40,kEvent));
        }
        {
            auto frame=Frame(40,kEvent);frame.payloads.erase(ContentHash(std::vector<uint8_t>{2,4,6,8}));
            BadFrame(root/"missing-owner",initial,std::move(frame),Results(40,kEvent));
        }
        {
            auto frame=Frame(40,kEvent);const auto key=ContentHash(std::vector<uint8_t>{2,4,6,8});
            frame.payloads[key]=Owner({8,6,4,2});
            BadFrame(root/"corrupt-owner",initial,std::move(frame),Results(40,kEvent));
        }
        {
            auto frame=Frame(40,kEvent);LiveOperations operations;
            for(size_t i=0;i<frame.operations.size();++i) {
                if(i==2) {
                    auto draw=*frame.operations[i].Draw();draw.geometry[0].payload.owner=Owner({8,6,4,2});
                    operations.push_back(std::move(draw));
                } else operations.push_back(frame.operations[i].ToJson());
            }
            frame.operations=std::move(operations);
            BadFrame(root/"direct-owner-disagrees",initial,std::move(frame),Results(40,kEvent));
        }
        {
            auto frame=Frame(40,kEvent);LiveOperations operations;
            for(size_t i=0;i<frame.operations.size();++i) {
                auto op=frame.operations[i].ToJson();if(i==3)op["event"]=kEvent-1;operations.push_back(std::move(op));
            }
            frame.operations=std::move(operations);
            BadFrame(root/"event-backward",initial,std::move(frame),Results(40,kEvent));
        }
        {
            auto frame=Frame(40,kEvent);LiveOperations operations;
            for(size_t i=0;i<frame.operations.size();++i)if(i!=4)operations.push_back(frame.operations[i].ToJson());
            frame.operations=std::move(operations);
            BadFrame(root/"active-query",initial,std::move(frame),Json::array());
        }
        for(unsigned mode=0;mode<5;++mode) {
            auto bad=initial;
            if(mode==0)bad.edram.pop_back();
            if(mode==1)bad.physical[0].address++;
            if(mode==2)bad.physical[1].address=0x1004;
            if(mode==3)bad.physical[0].bytes.clear();
            if(mode==4)bad.physical[1].address=0x20000000;
            Refuse([&]{NativeFrameCaptureWriter writer(root/("bad-state-"+std::to_string(mode)),40,bad);},"Invalid history accepted");
        }
        {
            NativeFrameCaptureWriter writer(root/"overwrite",40,initial);
            const auto file=root/"overwrite"/(ContentHash(std::vector<uint8_t>{2,4,6,8})+".payload.bin");
            {std::ofstream stream(file);stream<<"preserved";}
            auto frame=Frame(40,kEvent);
            Refuse([&]{writer.AppendCompletedFrame(frame,initial,Results(40,kEvent));},"Existing resource overwritten");
            Check(fs::file_size(file)==9,"Existing resource changed");
        }
        {
            NativeFrameCaptureWriter writer(root/"duplicate-frame",40,initial);auto first=Frame(40,kEvent);
            writer.AppendCompletedFrame(first,initial,Results(40,kEvent));
            Refuse([&]{writer.AppendCompletedFrame(first,initial,Results(40,kEvent));},"Duplicate completed frame accepted");
            Refuse([&]{writer.Finish();},"Duplicate-frame capture completed");
        }
        {
            NativeFrameCaptureWriter writer(root/"third-frame",40,initial);
            auto first=Frame(40,kEvent),second=Frame(41,kEvent+5),third=Frame(42,kEvent+10);
            writer.AppendCompletedFrame(first,initial,Results(40,kEvent));
            writer.AppendCompletedFrame(second,initial,Results(41,kEvent+5));
            Refuse([&]{writer.AppendCompletedFrame(third,initial,Results(42,kEvent+10));},"Third frame accepted");
            Refuse([&]{writer.Finish();},"Overlong capture completed");
        }
        {
            NativeFrameCaptureWriter writer(root/"partial-corruption",40,initial);
            auto first=Frame(40,kEvent),second=Frame(41,kEvent+5);
            writer.AppendCompletedFrame(first,initial,Results(40,kEvent));
            writer.AppendCompletedFrame(second,initial,Results(41,kEvent+5));
            auto file=root/"partial-corruption"/(ContentHash(std::vector<uint8_t>{2,4,6,8})+".payload.bin");
            {std::fstream stream(file,std::ios::in|std::ios::out|std::ios::binary);stream.put('\x7f');}
            Refuse([&]{writer.Finish();},"Corrupted pending capture completed");
            Check(!fs::exists(root/"partial-corruption"/"native-frame-capture.json"),"Corruption published completion");
        }
        {
            const auto file=root/"good"/"native-frame-capture.json";const auto original=ReadJson(file);
            auto changed=original;changed["frames"][1]["frame"]=44;ReplaceJson(file,changed);
            Refuse([&]{ValidateNativeFrameCapture(root/"good");},"Corrupted completion frame order validated");
            changed=original;changed["complete"]=false;ReplaceJson(file,changed);
            Refuse([&]{ValidateNativeFrameCapture(root/"good");},"Incomplete completion marker validated");
            ReplaceJson(file,original);
        }
        {
            const auto file=root/"good"/(ContentHash(std::vector<uint8_t>{2,4,6,8})+".payload.bin");
            {std::fstream stream(file,std::ios::in|std::ios::out|std::ios::binary);stream.put('\x7f');}
            Refuse([&]{ValidateNativeFrameCapture(root/"good");},"Corrupted completed capture validated");
        }
        {
            using namespace native_frame_capture_detail;
            uint64_t used=kCpuBytes-4;Charge(used,4,kCpuBytes);Check(used==kCpuBytes,"Exact budget edge failed");
            Refuse([&]{Charge(used,1,kCpuBytes);},"Budget overflow accepted");
            used=1;Refuse([&]{Charge(used,UINT64_MAX,kHistoryBytes);},"Integer budget wrap accepted");
        }
        std::cout<<Json{{"passed",true},{"checks",checks},{"gpu_executed",false},
            {"scope","CPU serialization/frame-order/resource-identity/corruption checks only; no completed GPU or whole-frame rendering proof"}}.dump()<<'\n';
        return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
