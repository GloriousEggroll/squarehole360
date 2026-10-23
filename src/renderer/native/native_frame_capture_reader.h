#pragma once
#include "native_frame_capture.h"
#include <charconv>
#include <cmath>

namespace gow3::renderer {
struct NativeFrameCaptureState {
    NativeFrameCaptureSnapshot snapshot;
    nlohmann::json query_results=nlohmann::json::array(),metadata;
    std::string metadata_sha256;
};
struct NativeFrameCaptureReadResult {
    std::vector<std::shared_ptr<LiveRenderFrame>> frames;
    nlohmann::json manifest;
    std::string manifest_sha256;
    // Distinct immutable CPU/shader file bytes, shared by both frame banks.
    uint64_t retained_bytes=0;
};

namespace native_frame_reader_detail {
using namespace native_frame_capture_detail;
inline Json Parse(std::span<const uint8_t> bytes) {
    std::vector<std::set<std::string>> keys;
    return Json::parse(bytes.begin(),bytes.end(),[&](int depth,Json::parse_event_t event,Json& value) {
        Require(depth<=64,"Native capture JSON nesting exceeds limit");
        if(event==Json::parse_event_t::object_start)keys.emplace_back();
        else if(event==Json::parse_event_t::key)
            Require(!keys.empty()&&keys.back().insert(value.get<std::string>()).second,"Duplicate Native capture JSON key");
        else if(event==Json::parse_event_t::object_end)keys.pop_back();
        return true;
    });
}
inline uint32_t U32(const Json& j) {
    const auto n=Unsigned(j);Require(n<=UINT32_MAX,"Native capture integer exceeds uint32");return uint32_t(n);
}
inline int32_t I32(const Json& j) {
    Require(j.is_number_integer(),"Native capture signed integer has wrong type");
    if(j.is_number_unsigned()) {Require(j.get<uint64_t>()<=INT32_MAX,"Native capture signed integer overflow");return int32_t(j.get<uint64_t>());}
    const auto n=j.get<int64_t>();Require(n>=INT32_MIN&&n<=INT32_MAX,"Native capture signed integer overflow");return int32_t(n);
}
inline bool BooleanValue(const Json& j) {Require(j.is_boolean(),"Native capture boolean has wrong type");return j.get<bool>();}
inline uint32_t Hex(const Json& j) {
    const auto s=j.get<std::string>();
    Require(s.size()==8&&s.find_first_not_of("0123456789abcdef")==std::string::npos,"Invalid Native capture hex address");
    uint32_t value=0;const auto r=std::from_chars(s.data(),s.data()+s.size(),value,16);
    Require(r.ec==std::errc{}&&r.ptr==s.data()+s.size(),"Invalid Native capture hex address");return value;
}
template<size_t N>inline std::array<uint32_t,N> Words(const Json& j) {
    Require(j.is_array()&&j.size()==N,"Invalid Native capture word array");std::array<uint32_t,N> out;
    for(size_t i=0;i<N;++i)out[i]=U32(j[i]);return out;
}
inline void Rectangle(const Json& j) {
    Require(j.is_array()&&j.size()==4,"Invalid Native capture rectangle");for(const auto& v:j)(void)I32(v);
}
inline TypedSurface Surface(const Json& j) {
    TypedSurface s{Hex(j.at("address")),U32(j.at("width")),U32(j.at("height")),U32(j.at("surface_info")),
        U32(j.at("format_info")),U32(j.at("edram_base")),U32(j.at("format")),BooleanValue(j.at("depth"))};
    Require(ToJson(s)==j,"Noncanonical Native captured surface");return s;
}
inline TypedTextureIdentity TextureIdentity(const Json& j) {
    TypedTextureIdentity t{Hex(j.at("base")),U32(j.at("format")),U32(j.at("width")),U32(j.at("height")),
        U32(j.at("pitch")),U32(j.at("endian")),U32(j.at("dimension")),U32(j.at("layers")),BooleanValue(j.at("tiled")),BooleanValue(j.at("packed"))};
    Require(ToJson(t)==j,"Noncanonical Native captured texture identity");return t;
}
inline std::array<std::optional<TypedSurface>,5> Surfaces(const Json& j) {
    Require(j.is_object()&&j.size()<=5,"Invalid Native capture surfaces");
    std::array<std::optional<TypedSurface>,5> result;
    for(const auto& [key,value]:j.items()) {
        size_t slot=0;while(slot<5&&key!=std::to_string(slot+6))++slot;
        Require(slot<5,"Invalid Native capture surface slot");result[slot]=Surface(value);
    }
    return result;
}
inline void Range(uint32_t address,uint32_t bytes) {
    Require(bytes&&uint64_t(address)+bytes<=kPhysicalBytes,"Native capture physical range overflow");
}

// The completion manifest is an integrity boundary, not cryptographic proof of
// who produced it. External execution receipts must bind manifest_sha256.
class Archive {
    std::filesystem::path directory_;
    std::set<std::string> verified_;
public:
    Json manifest;
    std::string manifest_sha256;
    explicit Archive(std::filesystem::path directory):directory_(std::move(directory)) {
        Require(std::filesystem::is_directory(std::filesystem::symlink_status(directory_)),"Native capture directory is not a regular directory");
        const auto path=directory_/"native-frame-capture.json";
        const auto bytes=Read(path,std::filesystem::file_size(path),kMetadataBytes);
        manifest_sha256=ContentHash(bytes);manifest=Parse(bytes);
        Require(manifest.at("schema")=="native-completed-frame-capture-v1"&&BooleanValue(manifest.at("complete"))&&
            manifest.at("source")=="native_gpu_frame_boundary"&&Unsigned(manifest.at("frame_count"))==2,
            "Invalid completed Native capture manifest");
        const auto first=Unsigned(manifest.at("first_frame"));
        Require(first>0&&first<UINT32_MAX-1&&manifest.at("frames").is_array()&&manifest.at("frames").size()==2,
            "Invalid Native capture frame range");
        const auto& files=manifest.at("files");Require(files.is_object()&&files.size()<=kMetadataBytes/128,"Invalid Native capture file table");
        uint64_t cpu=0,history=0,metadata=bytes.size();
        for(const auto& [name,id]:files.items()) {
            Require(FileName(name)&&name!="native-frame-capture.json"&&id.at("path")==name&&Digest(id.at("sha256").get<std::string>()),"Invalid Native capture file identity");
            const auto size=Unsigned(id.at("bytes"));const auto kind=id.at("kind").get<std::string>();
            if(kind=="resource")Charge(cpu,size,kCpuBytes);
            else if(kind=="history")Charge(history,size,kHistoryBytes);
            else {Require(kind=="metadata","Unknown Native capture file kind");Charge(metadata,size,kMetadataBytes);}
            if(kind!="metadata") {
                const auto digest=id.at("sha256").get<std::string>();
                Require(kind=="resource"?(name==digest+".payload.bin"||name==digest+".shader.bin"):name==digest+".gpu.bin",
                    "Native capture namespace/digest filename mismatch");
            }
        }
        Require(cpu==Unsigned(manifest.at("cpu_resource_bytes"))&&history==Unsigned(manifest.at("history_payload_bytes"))&&
            metadata-bytes.size()==Unsigned(manifest.at("metadata_bytes_before_completion")),"Native capture accounting mismatch");
        const auto root=[&](const Json& id,std::string expected) {
            Require(id.at("path")==expected&&files.contains(expected)&&files.at(expected)==id&&id.at("kind")=="metadata",
                "Native capture root identity mismatch");
        };
        root(manifest.at("scene"),"scene.json");root(manifest.at("shader_manifest"),"manifest.json");root(manifest.at("initial_history"),"gpu-history.json");
        for(size_t i=0;i<2;++i) {
            const auto& frame=manifest.at("frames")[i];Require(Unsigned(frame.at("frame"))==first+i,"Nonconsecutive Native capture frames");
            const auto count=Unsigned(frame.at("operations"));Require(count&&count<=kOperationsPerFrame,"Native capture operation count exceeds limit");
            root(frame.at("state"),"native-state-"+std::to_string(first+i)+".json");
        }
    }
    const Json& Identity(std::string_view path,std::string_view kind)const {
        Require(FileName(path),"Invalid Native capture path");const auto& id=manifest.at("files").at(std::string(path));
        Require(id.at("kind")==kind,"Native capture file kind mismatch");return id;
    }
    std::vector<uint8_t> Load(std::string_view path,std::string_view kind) {
        const auto& id=Identity(path,kind);const auto data=Read(directory_/path,Unsigned(id.at("bytes")),kind=="metadata"?kMetadataBytes:kind=="resource"?kCpuBytes:kHistoryBytes);
        Require(ContentHash(data)==id.at("sha256").get<std::string>(),"Native capture file SHA-256 mismatch");verified_.emplace(path);return data;
    }
    Json Metadata(std::string_view path) {return Parse(Load(path,"metadata"));}
    std::vector<uint8_t> Blob(const Json& blob) {
        const auto name=blob.at("path").get<std::string>();const auto& id=Identity(name,"history");
        Require(blob.at("sha256")==id.at("sha256")&&Unsigned(blob.at("bytes"))==Unsigned(id.at("bytes")),"Native capture history identity mismatch");
        return Load(name,"history");
    }
    void Finish() {
        for(const auto& [name,id]:manifest.at("files").items())if(!verified_.contains(name))(void)Load(name,id.at("kind").get<std::string>());
        const auto path=directory_/"native-frame-capture.json";
        Require(ContentHash(Read(path,std::filesystem::file_size(path),kMetadataBytes))==manifest_sha256,"Native capture manifest changed during read");
    }
};
inline NativeFrameCaptureState State(Archive& archive,const std::filesystem::path& metadata_path) {
    const auto name=metadata_path.string();bool initial=name=="gpu-history.json";std::optional<uint32_t> completed;
    if(!initial)for(const auto& f:archive.manifest.at("frames"))if(f.at("state").at("path")==name)completed=U32(f.at("frame"));
    Require(initial||completed,"Native capture state is outside completed manifest");
    NativeFrameCaptureState out;out.metadata=archive.Metadata(name);out.metadata_sha256=archive.Identity(name,"metadata").at("sha256");
    const auto& j=out.metadata;
    Require(Unsigned(j.at("version"))==1&&j.at("source")=="native_gpu_frame_boundary"&&j.at("provenance")=="native_gpu_frame_boundary"&&j.at("gamma").is_null(),"Invalid Native capture state provenance");
    Require(Unsigned(j.at("frame"))==(initial?Unsigned(archive.manifest.at("first_frame")):uint64_t(*completed)+1),"Native capture state boundary mismatch");
    if(initial)Require(!j.contains("completed_frame")&&!j.contains("query_results"),"Initial Native history contains completed-frame results");
    else {
        Require(U32(j.at("completed_frame"))==*completed&&j.at("query_results").is_array()&&j.at("query_results").size()<=kOperationsPerFrame/2,"Invalid Native completed query results");
        out.query_results=j.at("query_results");
    }
    out.snapshot.edram=archive.Blob(j.at("edram"));Require(out.snapshot.edram.size()==kEdramBytes,"Native capture requires full 10 MiB EDRAM");
    const auto& ranges=j.at("ranges");Require(ranges.is_array()&&ranges.size()<=kMetadataBytes/128,"Too many Native capture physical ranges");
    uint64_t end=0,total=0;
    for(const auto& r:ranges) {
        const auto address=U32(r.at("address"));const auto size=Unsigned(r.at("bytes"));
        Require(size&&!(address&3)&&!(size&3)&&address>=end&&address<=kPhysicalBytes&&size<=kPhysicalBytes-address,"Invalid, overlapping or unaligned Native capture physical range");
        Charge(total,size,kPhysicalBytes);end=uint64_t(address)+size;
        out.snapshot.physical.push_back({address,archive.Blob(r)});
    }
    Require(total==Unsigned(j.at("physical_bytes")),"Native capture physical byte accounting mismatch");return out;
}
inline CanonicalResourceRef Ref(const Json& j,const LiveRenderFrame& frame,bool shader) {
    const auto key=j.get<std::string>();Require(Digest(key),"Invalid Native capture resource digest");
    return {CanonicalDigest(key),frame.ResourceAt(key,shader)};
}
inline std::optional<CanonicalResourceRef> OptionalRef(const Json& j,const LiveRenderFrame& frame,bool shader) {
    if(j.is_null())return {};return Ref(j,frame,shader);
}
inline TypedLiveDraw Draw(const Json& j,const LiveRenderFrame& frame) {
    TypedLiveDraw d;d.event=Unsigned(j.at("event"));d.frame=U32(j.at("frame"));
    const auto function=j.at("function").get<std::string>();bool known=false;
    for(auto f:{DrawFunction::DrawPrimitive,DrawFunction::DrawIndexedPrimitive,DrawFunction::BeginVertices,DrawFunction::BeginIndexedVertices})
        if(function==DrawFunctionName(f)){d.function=f;known=true;}
    Require(known,"Unknown Native captured draw function");d.args=Words<8>(j.at("args"));
    if(j.contains("vertex_input")){Require(j.at("vertex_input")=="affine_float4_rectangle","Unknown Native captured vertex input");d.rectangle_list=true;}
    d.device_state=Ref(j.at("device_state"),frame,false);
    Require(d.device_state.owner->size()==LazyDeviceState::kBytes,"Native captured device-state extent mismatch");
    d.vertex_shader=OptionalRef(j.at("vertex_shader"),frame,true);d.pixel_shader=OptionalRef(j.at("pixel_shader"),frame,true);
    for(unsigned stage=0;stage<2;++stage)if(const auto& shader=stage?d.pixel_shader:d.vertex_shader)
        Require(shader->owner->size()>=4&&bool((*shader->owner)[3]&1)==!stage,"Native captured shader stage mismatch");
    d.surfaces=Surfaces(j.at("surfaces"));
    for(auto pair:{std::pair{"geometry",&d.geometry},std::pair{"up_geometry",&d.up_geometry}}) {
        const auto& list=j.at(pair.first);Require(list.is_array()&&list.size()<=98,"Too many Native captured geometry records");std::set<uint32_t> kinds;
        for(const auto& g:list) {
            TypedGeometry v;v.kind=U32(g.at("kind"));v.address=Hex(g.at("address"));v.bytes=U32(g.at("bytes"));v.payload=Ref(g.at("sha256"),frame,false);
            Require(v.kind<98&&kinds.insert(v.kind).second&&v.bytes==v.payload.owner->size(),"Native captured geometry kind/extent mismatch");
            if(g.contains("source")){Require(g.at("source")=="gpu_memory","Unknown Native captured geometry source");v.gpu_memory=true;}
            if(g.contains("fetch_slot")){v.fetch_slot=U32(g.at("fetch_slot"));Require(*v.fetch_slot<96,"Invalid Native captured vertex slot");}
            if(g.contains("endian")){v.endian=U32(g.at("endian"));Require(*v.endian<4,"Invalid Native captured geometry endian");}
            pair.second->push_back(std::move(v));
        }
    }
    const auto& slots=j.at("vertex_slots");Require(slots.is_array()&&slots.size()<=96,"Too many Native captured vertex slots");std::set<uint32_t> seen;
    for(const auto& slot:slots){const auto s=U32(slot);Require(s<96&&seen.insert(s).second,"Invalid/duplicate Native captured vertex slot");d.vertex_fetch_slots.push_back(s);}
    const auto& textures=j.at("textures");Require(textures.is_array()&&textures.size()<=32,"Too many Native captured textures");seen.clear();
    for(const auto& t:textures) {
        TypedTexture v;v.slot=U32(t.at("slot"));Require(v.slot<32&&seen.insert(v.slot).second,"Invalid/duplicate Native captured texture slot");v.fetch=Words<6>(t.at("fetch"));
        const auto source=t.at("source").get<std::string>();
        if(source=="cpu_snapshot")v.source=TextureSource::CpuSnapshot;
        else if(source=="gpu_alias_or_previous_frame")v.source=TextureSource::GpuPreviousOrAlias;
        else {Require(source=="gpu_resolve","Unknown Native captured texture source");v.source=TextureSource::GpuResolve;}
        v.base_payload=OptionalRef(t.at("base_payload"),frame,false);v.mip_payload=OptionalRef(t.at("mip_payload"),frame,false);
        if(t.contains("identity"))v.identity=TextureIdentity(t.at("identity"));d.textures.push_back(std::move(v));
    }
    Require(ToJson(d)==j,"Native typed draw did not round-trip exactly");return d;
}
inline void Control(const Json& op,const LiveRenderFrame& frame) {
    const auto type=op.at("type").get<std::string>();if(op.contains("surfaces"))(void)Surfaces(op.at("surfaces"));
    if(type=="cpu_memory_write"||type=="memory_copy") {
        const auto count=U32(op.at("bytes"));
        if(type=="cpu_memory_write") {Range(U32(op.at("address")),count);(void)Unsigned(op.at("coherence_epoch"));}
        else {
            Range(U32(op.at("source")),count);Range(U32(op.at("destination")),count);(void)Ref(op.at("shader"),frame,true);
            const auto source=op.at("source_kind").get<std::string>();Require(source=="cpu_snapshot"||source=="gpu_memory"||source=="mixed_memory","Unknown Native captured copy source");
            Require(!BooleanValue(op.at("published_to_guest")),"Native captured copy claims guest publication");
            Require((source=="gpu_memory")==op.at("payload").is_null(),"Native captured copy payload ownership mismatch");
            if(source=="mixed_memory") {
                uint64_t end=U32(op.at("source"));const auto& ranges=op.at("gpu_ranges");Require(ranges.is_array()&&ranges.size()<=kMetadataBytes/16,"Invalid Native captured mixed ranges");
                for(const auto& range:ranges) {const auto r=Words<2>(range);Require(r[0]>=end&&r[1]>r[0]&&uint64_t(r[1])<=uint64_t(U32(op.at("source")))+count,"Invalid Native captured mixed copy extent");end=r[1];}
            }
        }
        if(!op.at("payload").is_null())Require(Ref(op.at("payload"),frame,false).owner->size()==count,"Native captured control payload extent mismatch");
    } else if(type=="query_begin"||type=="query_end") {
        (void)U32(op.at("query_handle"));Require(Unsigned(op.at("query_generation"))>0,"Invalid Native captured query generation");
        const auto address=U32(op.at("report_address"));Range(address,32);
        Require(!(address&31)&&bool(address&32)==(type=="query_begin")&&U32(op.at("record_index"))==uint32_t(type=="query_begin"),"Native captured query address/index mismatch");
    } else if(type=="present") {
        (void)TextureIdentity(op.at("frontbuffer"));(void)Words<6>(op.at("frontbuffer_fetch"));
        Require(op.at("provenance")=="captured_present_resource","Unknown Native present provenance");
    } else if(type=="clearrect") {
        (void)Surfaces(op.at("surfaces"));
        (void)U32(op.at("flags"));(void)U32(op.at("stencil"));Rectangle(op.at("rectangle"));
        Require(op.at("depth").is_number()&&std::isfinite(op.at("depth").get<double>()),"Nonfinite Native captured clear depth");
        if(!op.at("color").is_null()) {const auto& c=op.at("color");Require(c.is_array()&&c.size()==4,"Invalid Native clear color");for(const auto& x:c)Require(x.is_number()&&std::isfinite(x.get<double>()),"Nonfinite Native clear color");}
    } else if(type=="resolve") {
        (void)Surfaces(op.at("surfaces"));
        (void)U32(op.at("flags"));Require(U32(op.at("source_slot"))<5,"Invalid Native resolve slot");(void)U32(op.at("level"));(void)I32(op.at("exponent_bias"));
        Rectangle(op.at("source_rectangle"));Rectangle(op.at("destination_rectangle"));(void)TextureIdentity(op.at("destination"));(void)Words<6>(op.at("destination_fetch"));(void)BooleanValue(op.at("copy_dest_swap"));
    } else throw std::runtime_error("Unsupported Native captured control");
}
inline void Queries(const LiveRenderFrame& frame,const Json& results) {
    std::optional<Json> active;size_t at=0;
    for(const auto& input:frame.operations) {
        if(input.Kind()=="query_begin") {Require(!active,"Nested Native captured query");active=input.Control();}
        if(input.Kind()!="query_end")continue;
        const auto& end=input.Control();Require(active&&at<results.size(),"Unmatched Native captured query end/result");const auto& result=results[at++];
        (void)Unsigned(result.at("generation"));(void)Unsigned(result.at("end_event"));(void)U32(result.at("handle"));
        (void)U32(result.at("end_address"));(void)U32(result.at("begin_address"));
        Require(end.at("query_handle")==active->at("query_handle")&&end.at("query_generation")==active->at("query_generation")&&
            uint64_t(U32(end.at("report_address")))+32==U32(active->at("report_address")),"Native captured query pair mismatch");
        Require(U32(result.at("frame"))==frame.frame&&result.at("generation")==end.at("query_generation")&&result.at("end_event")==end.at("event")&&
            result.at("handle")==end.at("query_handle")&&result.at("end_address")==end.at("report_address")&&result.at("begin_address")==active->at("report_address"),"Native captured query result identity mismatch");
        (void)Unsigned(result.at("samples"));(void)Unsigned(result.at("draws"));active.reset();
    }
    Require(!active&&at==results.size(),"Native capture has active query or excess results");
}
} // namespace native_frame_reader_detail

inline NativeFrameCaptureState ReadNativeFrameCaptureState(const std::filesystem::path& directory,const std::filesystem::path& metadataPath,
                                                          std::string_view expected_manifest_sha256={}) {
    native_frame_reader_detail::Archive archive(directory);
    native_frame_capture_detail::Require(expected_manifest_sha256.empty()||archive.manifest_sha256==expected_manifest_sha256,
        "Native capture manifest no longer matches the opened capture");
    auto result=native_frame_reader_detail::State(archive,metadataPath);
    // This standalone accessor verifies the requested metadata and all its
    // payloads. ReadNativeFrameCapture verifies the complete archive first.
    return result;
}
inline NativeFrameCaptureReadResult ReadNativeFrameCapture(const std::filesystem::path& directory) {
    using namespace native_frame_reader_detail;
    Archive archive(directory);NativeFrameCaptureReadResult result;
    result.manifest=archive.manifest;result.manifest_sha256=archive.manifest_sha256;
    LiveResourceMap payloads,shaders;
    for(const auto& [name,id]:archive.manifest.at("files").items())if(id.at("kind")=="resource") {
        auto bytes=std::make_shared<const std::vector<uint8_t>>(archive.Load(name,"resource"));
        const auto hash=id.at("sha256").get<std::string>();const bool shader=name==hash+".shader.bin";
        Require((shader?shaders:payloads).emplace(hash,bytes).second,"Duplicate Native captured namespace resource");result.retained_bytes+=bytes->size();
    }
    const auto shader_manifest=archive.Metadata("manifest.json");Require(shader_manifest.at("source")=="native-frame-capture.json"&&shader_manifest.at("shaders").is_array()&&shader_manifest.at("shaders").size()==shaders.size(),"Native shader manifest mismatch");
    std::set<std::string> shader_keys;
    for(const auto& item:shader_manifest.at("shaders")) {
        const auto hash=item.at("sha256").get<std::string>();const auto owner=shaders.at(hash);
        Require(shader_keys.insert(hash).second&&item.at("path")==hash+".shader.bin"&&Unsigned(item.at("bytes"))==owner->size()&&owner->size()>=4&&item.at("stage")==(((*owner)[3]&1)?"vertex":"pixel"),"Native shader manifest stage/identity mismatch");
    }
    const auto scene=archive.Metadata("scene.json");Require(Unsigned(scene.at("version"))==3&&scene.at("frames").is_array()&&scene.at("frames").size()==2&&scene.at("issues").is_array()&&scene.at("issues").empty(),"Invalid Native captured scene");
    const auto& operations=scene.at("operations");Require(operations.is_array()&&operations.size()<=2*kOperationsPerFrame,"Native capture operation limit exceeded");
    (void)State(archive,"gpu-history.json");
    size_t at=0;std::optional<uint64_t> last_event;
    for(size_t index=0;index<2;++index) {
        const auto& expected=archive.manifest.at("frames")[index];auto frame=std::make_shared<LiveRenderFrame>();frame->frame=U32(expected.at("frame"));
        Require(U32(scene.at("frames")[index])==frame->frame,"Native scene frame list mismatch");
        frame->payloads=payloads;frame->shaders=shaders;frame->retained_bytes=result.retained_bytes;
        const auto count=Unsigned(expected.at("operations"));Require(count<=operations.size()-at,"Truncated Native captured frame");
        for(size_t i=0;i<count;++i) {
            const auto& op=operations[at++];Require(op.is_object()&&U32(op.at("frame"))==frame->frame,"Native operation frame mismatch");
            const auto event=Unsigned(op.at("event"));Require(event<=UINT32_MAX&&(!last_event||event>=*last_event),"Native captured events are out of order or exceed Scene event bounds");last_event=event;
            const auto type=op.at("type").get<std::string>();Require((type=="present")==bool(i+1==count),"Native captured frame lacks its sole final Present");
            if(type=="draw")frame->operations.push_back(Draw(op,*frame));else {Control(op,*frame);frame->operations.push_back(op);}
        }
        Require(frame->operations[0].Event()==Unsigned(expected.at("first_event"))&&frame->operations.back().Event()==Unsigned(expected.at("last_event")),"Native captured event endpoints mismatch");
        const auto state=State(archive,expected.at("state").at("path").get<std::string>());Queries(*frame,state.query_results);
        Require(state.query_results.size()==Unsigned(expected.at("queries")),"Native captured query count mismatch");result.frames.push_back(std::move(frame));
    }
    Require(at==operations.size(),"Excess Native captured operations");archive.Finish();return result;
}
} // namespace gow3::renderer
