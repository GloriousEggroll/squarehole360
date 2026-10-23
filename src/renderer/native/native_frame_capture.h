#pragma once
#include "renderer/native/content_hash.h"
#include "renderer/native/live_plan.h"
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <string>
#include <unordered_map>

namespace gow3::renderer {

// CPU serialization only. The CP caller supplies a completed, committed Native
// boundary, never a guest-memory substitute, and owns synchronization/query
// closure. This helper does not establish GPU completion or capture equivalence.
struct NativeFrameCaptureRange {
    uint32_t address=0;
    std::vector<uint8_t> bytes;
};
struct NativeFrameCaptureSnapshot {
    std::vector<uint8_t> edram;
    std::vector<NativeFrameCaptureRange> physical;
};

namespace native_frame_capture_detail {
using Json=nlohmann::json;
inline constexpr uint64_t kCpuBytes=256ull*1024*1024;
inline constexpr uint64_t kHistoryBytes=512ull*1024*1024;
inline constexpr uint64_t kMetadataBytes=64ull*1024*1024;
inline constexpr uint64_t kEdramBytes=2048ull*5120;
inline constexpr uint64_t kPhysicalBytes=512ull*1024*1024;
inline constexpr size_t kOperationsPerFrame=65536;
inline void Require(bool condition,const char* message) {
    if(!condition)throw std::runtime_error(message);
}
inline uint64_t Unsigned(const Json& value) {
    Require(value.is_number_integer(),"Capture integer has the wrong type");
    if(value.is_number_unsigned())return value.get<uint64_t>();
    const auto number=value.get<int64_t>();
    Require(number>=0,"Capture integer is negative");return uint64_t(number);
}
inline bool Digest(std::string_view value) {
    return value.size()==64&&value.find_first_not_of("0123456789abcdef")==std::string_view::npos;
}
inline void Charge(uint64_t& used,uint64_t amount,uint64_t limit) {
    Require(used<=limit&&amount<=limit-used,"Native capture byte limit exceeded");used+=amount;
}
inline std::span<const uint8_t> Bytes(const std::string& value) {
    return {reinterpret_cast<const uint8_t*>(value.data()),value.size()};
}
inline Json Identity(std::string_view path,std::span<const uint8_t> bytes,std::string_view kind) {
    return {{"path",path},{"bytes",bytes.size()},{"sha256",ContentHash(bytes)},{"kind",kind}};
}
inline bool FileName(std::string_view name) {
    return !name.empty()&&name!="."&&name!=".."&&name.find_first_of("/\\")==std::string_view::npos;
}
inline std::vector<uint8_t> Read(const std::filesystem::path& path,uint64_t bytes,uint64_t limit) {
    Require(bytes<=limit,"Native capture file exceeds its limit");
    Require(std::filesystem::is_regular_file(std::filesystem::symlink_status(path)),
        "Native capture payload is not a regular non-symlink file");
    Require(std::filesystem::file_size(path)==bytes,"Native capture file extent changed");
    std::ifstream stream(path,std::ios::binary);
    std::vector<uint8_t> result(static_cast<size_t>(bytes));
    if(bytes)stream.read(reinterpret_cast<char*>(result.data()),std::streamsize(bytes));
    Require(bool(stream),"Cannot read Native capture file");return result;
}
inline void VerifyFile(const std::filesystem::path& directory,const Json& identity,uint64_t limit) {
    const auto path=identity.at("path").get<std::string>();
    const auto digest=identity.at("sha256").get<std::string>();
    Require(FileName(path)&&Digest(digest),"Invalid Native capture file identity");
    const auto data=Read(directory/path,Unsigned(identity.at("bytes")),limit);
    Require(ContentHash(data)==digest,"Native capture file SHA-256 mismatch");
}
}

class NativeFrameCaptureWriter {
    using Json=nlohmann::json;
    std::filesystem::path directory_;
    uint32_t first_frame_;
    bool poisoned_=false,finished_=false;
    uint64_t cpu_bytes_=0,history_bytes_=0,metadata_bytes_=0;
    std::optional<uint64_t> last_event_;
    Json context_,operations_=Json::array(),frames_=Json::array();
    std::map<std::string,Json> files_,shaders_;

    void Writable()const {
        native_frame_capture_detail::Require(!poisoned_&&!finished_,"Native capture is failed or already complete");
    }
    Json Write(std::string path,std::span<const uint8_t> bytes,std::string_view kind) {
        using namespace native_frame_capture_detail;
        Require(FileName(path),"Invalid Native capture output filename");
        const auto identity=Identity(path,bytes,kind);
        if(const auto found=files_.find(path);found!=files_.end()) {
            Require(found->second==identity,"Conflicting Native capture duplicate");return identity;
        }
        if(kind=="resource")Charge(cpu_bytes_,bytes.size(),kCpuBytes);
        else if(kind=="history")Charge(history_bytes_,bytes.size(),kHistoryBytes);
        else {Require(kind=="metadata","Invalid Native capture file kind");Charge(metadata_bytes_,bytes.size(),kMetadataBytes);}
        // C++23 exclusive creation: a pre-existing file is never truncated.
        std::ofstream stream(directory_/path,std::ios::out|std::ios::binary|std::ios::noreplace);
        Require(bool(stream),"Native capture output already exists or cannot be created");
        if(!bytes.empty())stream.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size()));
        stream.close();Require(bool(stream),"Cannot finish Native capture output");
        files_.emplace(std::move(path),identity);return identity;
    }
    Json WriteJson(std::string path,const Json& value) {
        const auto text=value.dump(2)+"\n";
        return Write(std::move(path),native_frame_capture_detail::Bytes(text),"metadata");
    }
    Json Blob(std::span<const uint8_t> bytes,std::string_view suffix,std::string_view kind) {
        const auto digest=ContentHash(bytes);
        const auto identity=Write(digest+std::string(suffix),bytes,kind);
        return {{"sha256",digest},{"bytes",bytes.size()},{"path",identity.at("path")}};
    }
    Json Snapshot(uint32_t boundary,const NativeFrameCaptureSnapshot& snapshot) {
        using namespace native_frame_capture_detail;
        Require(snapshot.edram.size()==kEdramBytes,"Native snapshot requires actual full 10 MiB EDRAM");
        Require(snapshot.physical.size()<=kMetadataBytes/128,"Too many Native history ranges");
        uint64_t previous=0,total=0;
        // Validate every range before publishing any payload for this boundary.
        for(const auto& range:snapshot.physical) {
            const uint64_t first=range.address,count=range.bytes.size();
            Require(count&&!(first&3)&&!(count&3)&&first>=previous&&
                first<=kPhysicalBytes&&count<=kPhysicalBytes-first,
                "Invalid, unaligned or overlapping Native history range");
            previous=first+count;Charge(total,count,kPhysicalBytes);
        }
        Json ranges=Json::array();
        for(const auto& range:snapshot.physical) {
            auto item=Blob(range.bytes,".gpu.bin","history");item["address"]=range.address;
            ranges.push_back(std::move(item));
        }
        return {{"version",1},{"source","native_gpu_frame_boundary"},
            {"provenance","native_gpu_frame_boundary"},{"frame",boundary},
            {"ranges",std::move(ranges)},{"edram",Blob(snapshot.edram,".gpu.bin","history")},
            {"gamma",nullptr},{"physical_bytes",total},
            {"scope","Actual Native canonical bytes supplied at a completed CP boundary; no API reconstruction or CPU substitute"}};
    }
    void Resource(const std::string& digest,const LiveResourceBytes& owner,bool shader,
                  std::unordered_map<const void*,std::string>& checked) {
        using namespace native_frame_capture_detail;
        Require(owner&&Digest(digest),"Invalid Native resource owner/digest");
        const auto found=checked.find(owner.get());
        if(found!=checked.end())Require(found->second==digest,"Resource owner has conflicting identities");
        else {
            Require(ContentHash(*owner)==digest,"Native resource bytes disagree with their digest");
            checked.emplace(owner.get(),digest);
        }
        const auto path=digest+(shader?".shader.bin":".payload.bin");
        // The same immutable texture/state owner is referenced by many draws.
        // Validate each distinct owner once, not its multi-MiB bytes per use.
        if(const auto existing=files_.find(path);existing!=files_.end())
            Require(Unsigned(existing->second.at("bytes"))==owner->size()&&
                existing->second.at("sha256")==digest,"Conflicting Native resource extent");
        else Write(path,*owner,"resource");
        if(shader) {
            Require(owner->size()>=4,"Truncated Native shader container");
            const auto stage=((*owner)[3]&1)?"vertex":"pixel";
            shaders_[digest]={{"sha256",digest},{"bytes",owner->size()},{"stage",stage},{"path",path}};
        }
    }
    void CheckReference(const LiveRenderFrame& frame,std::string_view digest,bool shader)const {
        using namespace native_frame_capture_detail;
        Require(Digest(digest),"Invalid Native operation resource digest");
        const auto* owner=frame.FindResource(digest,shader);
        Require(owner&&*owner,"Native operation resource is absent from its frame");
        Require(files_.contains(std::string(digest)+(shader?".shader.bin":".payload.bin")),
            "Native operation resource was not serialized");
    }
    void CheckOperationResources(const LiveRenderFrame& frame,const LiveOperation& input,const Json& op,
                                 std::unordered_map<const void*,std::string>& checked) {
        const auto ref=[&](const Json& value,bool shader) {
            if(!value.is_null())CheckReference(frame,value.get<std::string>(),shader);
        };
        if(const auto* draw=input.Draw()) {
            const auto owned=[&](const CanonicalResourceRef& resource,bool shader) {
                const auto hash=resource.hash.View();
                // Some immutable adapters retain the owner in the frame table
                // instead of embedding it in each draw binding. Resolve only the
                // exact canonical digest; never consult current guest memory.
                const auto& owner=resource.owner?resource.owner:frame.ResourceAt(hash,shader);
                Resource(std::string(hash),owner,shader,checked);
                CheckReference(frame,hash,shader);
            };
            if(draw->lazy_device_state) {
                Resource(std::string(draw->lazy_device_state->Digest()),draw->lazy_device_state->Owner(),false,checked);
            } else owned(draw->device_state,false);
            if(draw->vertex_shader)owned(*draw->vertex_shader,true);
            if(draw->pixel_shader)owned(*draw->pixel_shader,true);
            for(const auto* list:{&draw->geometry,&draw->up_geometry})for(const auto& item:*list)owned(item.payload,false);
            for(const auto& texture:draw->textures) {
                if(texture.base_payload)owned(*texture.base_payload,false);
                if(texture.mip_payload)owned(*texture.mip_payload,false);
            }
        }
        const auto type=op.at("type").get<std::string>();
        if(type=="draw") {
            ref(op.at("device_state"),false);ref(op.at("vertex_shader"),true);ref(op.at("pixel_shader"),true);
            for(const auto* key:{"geometry","up_geometry"})for(const auto& item:op.at(key))ref(item.at("sha256"),false);
            for(const auto& item:op.at("textures")) {ref(item.at("base_payload"),false);ref(item.at("mip_payload"),false);}
        } else if(type=="cpu_memory_write"||type=="memory_copy") {
            ref(op.at("payload"),false);
            if(type=="memory_copy")ref(op.at("shader"),true);
        }
    }
public:
    NativeFrameCaptureWriter(std::filesystem::path new_directory,uint32_t first_frame,
                             const NativeFrameCaptureSnapshot& initial,
                             Json context=Json::object())
        :directory_(std::move(new_directory)),first_frame_(first_frame),context_(std::move(context)) {
        using namespace native_frame_capture_detail;
        Require(first_frame_>0&&first_frame_<UINT32_MAX-1,"Invalid Native capture first frame");
        Require(context_.is_object()&&context_.dump().size()<=kMetadataBytes/4,"Invalid/oversized Native capture context");
        Require(std::filesystem::is_directory(directory_.parent_path()),"Native capture parent must exist");
        Require(std::filesystem::create_directory(directory_),"Native capture directory must be new");
        try {WriteJson("gpu-history.json",Snapshot(first_frame_,initial));}
        catch(...) {poisoned_=true;throw;}
    }
    NativeFrameCaptureWriter(const NativeFrameCaptureWriter&)=delete;
    NativeFrameCaptureWriter& operator=(const NativeFrameCaptureWriter&)=delete;

    void AppendCompletedFrame(const LiveRenderFrame& frame,const NativeFrameCaptureSnapshot& completed,
                              const Json& query_results) {
        using namespace native_frame_capture_detail;
        Writable();
        try {
            Require(frames_.size()<2&&frame.frame==uint64_t(first_frame_)+frames_.size(),
                "Native capture requires exactly two consecutive frames");
            Require(!frame.operations.empty()&&frame.operations.size()<=kOperationsPerFrame,
                "Invalid Native completed operation count");
            Require(query_results.is_array(),"Native query results must be an array");
            std::unordered_map<const void*,std::string> checked;
            for(bool shader:{false,true})frame.ForEachResource(shader,[&](std::string_view hash,const LiveResourceBytes& owner) {
                Resource(std::string(hash),owner,shader,checked);
            });
            Json frame_operations=Json::array(),query_ends=Json::array();
            std::optional<Json> active_query;
            for(size_t i=0;i<frame.operations.size();++i) {
                const auto& input=frame.operations[i];auto op=input.ToJson();
                const auto event=Unsigned(op.at("event"));
                Require(event<=UINT32_MAX&&Unsigned(op.at("frame"))==frame.frame&&
                    (!last_event_||event>=*last_event_),
                    "Native operation frame/event order or replay extent mismatch");
                last_event_=event;
                const auto type=op.at("type").get<std::string>();
                Require(type=="draw"||type=="clearrect"||type=="resolve"||type=="present"||
                    type=="cpu_memory_write"||type=="memory_copy"||type=="query_begin"||type=="query_end",
                    "Unsupported Native capture operation");
                Require((type=="present")==bool(i+1==frame.operations.size()),
                    "Native completed frame must end with its sole Present");
                CheckOperationResources(frame,input,op,checked);
                if(type=="query_begin") {
                    const auto address=Unsigned(op.at("report_address"));
                    Require(!active_query&&address<=UINT32_MAX&&address>=32&&!(address&31)&&
                        (address&32)&&Unsigned(op.at("query_generation"))>0,
                        "Nested or invalid Native captured query");active_query=op;
                } else if(type=="query_end") {
                    const auto address=Unsigned(op.at("report_address"));
                    Require(active_query&&op.at("query_handle")==active_query->at("query_handle")&&
                        op.at("query_generation")==active_query->at("query_generation")&&
                        address<=UINT32_MAX-32&&!(address&63)&&
                        Unsigned(active_query->at("report_address"))==address+32,
                        "Native captured query has no matching begin");
                    query_ends.push_back(op);active_query.reset();
                }
                frame_operations.push_back(std::move(op));
            }
            Require(!active_query,"Native capture boundary contains an active query");
            Require(query_results.size()==query_ends.size(),"Native query result count mismatch");
            for(size_t i=0;i<query_results.size();++i) {
                const auto& result=query_results[i];const auto& end=query_ends[i];
                Require(Unsigned(result.at("frame"))==frame.frame&&result.at("generation")==end.at("query_generation")&&
                    result.at("end_event")==end.at("event")&&result.at("handle")==end.at("query_handle")&&
                    result.at("end_address")==end.at("report_address")&&
                    Unsigned(result.at("begin_address"))==Unsigned(result.at("end_address"))+32,
                    "Native query result identity mismatch");
                (void)Unsigned(result.at("samples"));(void)Unsigned(result.at("draws"));
            }
            auto state=Snapshot(frame.frame+1,completed);
            state["completed_frame"]=frame.frame;state["query_results"]=query_results;
            const auto state_file=WriteJson("native-state-"+std::to_string(frame.frame)+".json",state);
            const auto operation_bytes=frame_operations.dump().size();
            // Reserve against the final scene metadata before retaining a second frame.
            Require(operation_bytes<=kMetadataBytes&&operations_.dump().size()<=kMetadataBytes-operation_bytes,
                "Native operation metadata exceeds limit");
            frames_.push_back({{"frame",frame.frame},{"operations",frame_operations.size()},
                {"first_event",frame_operations.front().at("event")},{"last_event",frame_operations.back().at("event")},
                {"state",state_file},{"queries",query_results.size()}});
            for(auto& op:frame_operations)operations_.push_back(std::move(op));
        } catch(...) {poisoned_=true;throw;}
    }

    Json Finish() {
        using namespace native_frame_capture_detail;
        Writable();
        try {
            Require(frames_.size()==2,"Native capture is incomplete");
            Json numbers=Json::array();for(const auto& frame:frames_)numbers.push_back(frame.at("frame"));
            const auto scene=WriteJson("scene.json",{{"version",3},{"frames",numbers},{"operations",operations_},
                {"issues",Json::array()},{"scope","Completed Native typed operations, not reconstructed API observations"}});
            Json shader_entries=Json::array();for(const auto& [hash,item]:shaders_)shader_entries.push_back(item);
            const auto shader_manifest=WriteJson("manifest.json",{{"source","native-frame-capture.json"},
                {"status","Exact captured Native shader containers; compilation is not image equivalence"},
                {"shaders",std::move(shader_entries)}});
            // Catch any partial/corrupted/replaced output before publishing completion.
            for(const auto& [name,identity]:files_)VerifyFile(directory_,identity,
                identity.at("kind")=="metadata"?kMetadataBytes:kHistoryBytes);
            Json files=Json::object();for(const auto& [name,identity]:files_)files[name]=identity;
            Json manifest={{"schema","native-completed-frame-capture-v1"},{"complete",true},
                {"source","native_gpu_frame_boundary"},{"first_frame",first_frame_},{"frame_count",2},
                {"frames",frames_},{"scene",scene},{"shader_manifest",shader_manifest},
                {"initial_history",files_.at("gpu-history.json")},{"files",std::move(files)},
                {"context",context_},{"cpu_resource_bytes",cpu_bytes_},{"history_payload_bytes",history_bytes_},
                {"metadata_bytes_before_completion",metadata_bytes_},
                {"limits",{{"frames",2},{"cpu_resources",kCpuBytes},{"history_payloads",kHistoryBytes},{"metadata",kMetadataBytes}}},
                {"scope","Serialization and identity proof only; caller establishes actual CP completion and baseline replay must establish rendering equivalence"}};
            // Publish only the already-closed metadata inode. Hard-link creation
            // is atomic and refuses an existing destination; a failed write or
            // link leaves no completion marker. No overwrite rename is used.
            const auto pending=".native-frame-capture.json.pending";
            WriteJson(pending,manifest);
            std::filesystem::create_hard_link(directory_/pending,directory_/"native-frame-capture.json");
            finished_=true;
            std::error_code ignored;std::filesystem::remove(directory_/pending,ignored);
            return manifest;
        } catch(...) {poisoned_=true;throw;}
    }
};

// Cold post-write integrity check for the replay entry point. It does not prove
// GPU provenance or replace Scene's operation/layout/range validation.
inline nlohmann::json ValidateNativeFrameCapture(const std::filesystem::path& directory) {
    using namespace native_frame_capture_detail;
    const auto path=directory/"native-frame-capture.json";
    Require(std::filesystem::is_regular_file(std::filesystem::symlink_status(path)),"Native capture is incomplete");
    const auto data=Read(path,std::filesystem::file_size(path),kMetadataBytes);
    const auto manifest=Json::parse(data);
    Require(manifest.at("schema")=="native-completed-frame-capture-v1"&&manifest.at("complete")==true&&
        manifest.at("source")=="native_gpu_frame_boundary"&&Unsigned(manifest.at("frame_count"))==2,
        "Invalid completed Native capture manifest");
    const auto first=Unsigned(manifest.at("first_frame"));
    Require(first>0&&first<UINT32_MAX-1&&manifest.at("frames").is_array()&&manifest.at("frames").size()==2,
        "Invalid Native capture frame range");
    for(size_t i=0;i<2;++i)Require(Unsigned(manifest.at("frames")[i].at("frame"))==first+i,
        "Nonconsecutive completed Native frames");
    uint64_t cpu=0,history=0,metadata=data.size();
    const auto& files=manifest.at("files");Require(files.is_object(),"Invalid Native capture file table");
    for(const auto& [name,identity]:files.items()) {
        Require(FileName(name)&&identity.at("path")==name&&name!="native-frame-capture.json",
            "Invalid Native capture file table path");
        const auto bytes=Unsigned(identity.at("bytes"));const auto kind=identity.at("kind").get<std::string>();
        if(kind=="resource")Charge(cpu,bytes,kCpuBytes);
        else if(kind=="history")Charge(history,bytes,kHistoryBytes);
        else {Require(kind=="metadata","Unknown Native capture file kind");Charge(metadata,bytes,kMetadataBytes);}
        VerifyFile(directory,identity,kind=="metadata"?kMetadataBytes:kHistoryBytes);
    }
    Require(cpu==Unsigned(manifest.at("cpu_resource_bytes"))&&history==Unsigned(manifest.at("history_payload_bytes"))&&
        metadata-data.size()==Unsigned(manifest.at("metadata_bytes_before_completion")),"Native capture accounting mismatch");
    for(const auto* key:{"scene","shader_manifest","initial_history"}) {
        const auto& identity=manifest.at(key);const auto name=identity.at("path").get<std::string>();
        Require(files.contains(name)&&files.at(name)==identity,"Native capture root identity mismatch");
    }
    for(const auto& frame:manifest.at("frames")) {
        const auto& identity=frame.at("state");const auto name=identity.at("path").get<std::string>();
        Require(files.contains(name)&&files.at(name)==identity,"Native capture completed state identity mismatch");
    }
    return manifest;
}
} // namespace gow3::renderer
