#include "canonical_store_scene_fixture.h"
#include "renderer/native/content_hash.h"
#include "renderer/native/texture_data.h"
#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace gow3::renderer::validation {
namespace {
using Json=nlohmann::json;
using Bytes=std::vector<uint8_t>;
constexpr uint32_t kEdramBytes=2048u*5120;
void Check(bool good,const std::string& reason) {
    if(!good)throw std::runtime_error("Canonical store Scene fixture: "+reason);
}
Bytes Read(const std::filesystem::path& path) {
    std::ifstream file(path,std::ios::binary|std::ios::ate);
    const auto size=file.tellg();
    Check(bool(file)&&size>=0&&uint64_t(size)<=64u*1024*1024,"invalid payload "+path.string());
    Bytes result(static_cast<size_t>(size));file.seekg(0);
    file.read(reinterpret_cast<char*>(result.data()),result.size());
    Check(bool(file),"truncated payload "+path.string());return result;
}
uint32_t Address(const Json& identity) {
    return uint32_t(std::stoul(identity.at("base").get<std::string>(),nullptr,16));
}
std::string AddressText(uint32_t address) {
    std::array<char,8> text{};const auto result=std::to_chars(text.data(),text.data()+text.size(),address,16);
    Check(result.ec==std::errc{},"cannot encode physical address");
    return "0x"+std::string(text.data(),result.ptr);
}
struct Donor {Json resolve,draw;size_t texture=0;uint32_t format=0;};
std::vector<Donor> Donors(const Json& original) {
    const auto& operations=original.at("operations");std::map<uint32_t,Donor> found;
    for(size_t i=0;i<operations.size();++i) {
        const auto& resolve=operations[i];if(resolve.at("type")!="resolve")continue;
        const auto fetch=resolve.at("destination_fetch").get<std::array<uint32_t,6>>();
        const uint32_t format=fetch[1]&63;
        if((format!=26&&format!=31)||found.contains(format)||resolve.at("source_slot")==4)continue;
        const auto& identity=resolve.at("destination");
        if(resolve.at("destination_rectangle")!=Json::array({0,0,identity.at("width"),identity.at("height")}))continue;
        for(size_t j=i+1;j<operations.size();++j) {
            const auto& draw=operations[j];if(draw.at("type")=="present")break;
            if(draw.at("type")!="draw")continue;
            // A bounded donor must have no additional missing GPU history.
            size_t match=0,matches=0,gpu_inputs=0;
            for(size_t slot=0;slot<draw.at("textures").size();++slot) {
                const auto& texture=draw.at("textures")[slot];
                const bool gpu=texture.at("source")=="gpu_resolve"||texture.at("source")=="gpu_alias_or_previous_frame";
                gpu_inputs+=gpu;
                if(gpu&&texture.at("identity")==identity){match=slot;++matches;}
            }
            bool cpu_geometry=true;
            for(const char* name:{"geometry","up_geometry"})for(const auto& geometry:draw.at(name))
                cpu_geometry&=geometry.value("source","")!="gpu_memory";
            if(matches==1&&gpu_inputs==1&&cpu_geometry) {
                found.emplace(format,Donor{resolve,draw,match,format});break;
            }
        }
    }
    Check(found.contains(26)&&found.contains(31),"capture needs actual full raw26 and raw31 resolve/draw donors");
    return {found.at(26),found.at(31)};
}
TextureLayout Layout(const Donor& donor) {
    auto fetch=donor.resolve.at("destination_fetch").get<std::array<uint32_t,6>>();
    fetch[1]=(fetch[1]&4095u)|Address(donor.resolve.at("destination"));return DescribeTexture(fetch);
}
uint32_t BackingBytes(const TextureLayout& layout,uint32_t address) {
    uint64_t end=address;
    for(const auto& [first,count]:TextureMemoryRanges(layout,false))end=std::max(end,uint64_t(first)+count);
    end=(end+4095)&~uint64_t(4095);
    Check(end>address&&end-address<=16u*1024*1024&&end<=512u*1024*1024,"unbounded destination backing");
    return uint32_t(end-address);
}
Json Query(uint64_t generation,uint32_t frame,uint64_t event,bool end) {
    const uint32_t address=0x10000+uint32_t(generation)*128;
    return {{"type",end?"query_end":"query_begin"},{"query_generation",generation},
        {"query_handle",0x7ca1},{"report_address",address+(end?0u:32u)},
        {"frame",frame},{"event",event}};
}
Json Results(const std::vector<CompletedNativeQuery>& queries) {
    Json out=Json::array();for(const auto& q:queries)out.push_back(Json::array({q.generation,q.frame,q.end_event,
        q.result.handle,q.result.begin_address,q.result.end_address,q.result.draws,q.result.samples}));return out;
}
uint64_t Counter(const SceneExecutor& scene,const char* name) {
    return scene.report().at("canonical_resolve_stores").at(name).get<uint64_t>();
}
struct Snapshot {Bytes edram,physical;Json queries,mechanism;};
struct Outcome {std::map<std::string,Snapshot> snapshots;Json report;};

Outcome Run(const SceneExecutorConfig& base,const Donor& donor,bool enabled) {
    auto config=base;
    config.canonical_resolve_stores=enabled;config.native_fsi=true;
    config.async_gpu_submissions=true;config.unified_gpu_queue=true;config.batch_transfers=true;
    config.defer_native_queries=true;config.max_deferred_queries=base.async_submission_capacity;
    // Keep both sampled generations logically pending until the explicit drain.
    // An opportunistic query-prefix poll would make that ownership case timing dependent.
    config.retire_completed_query_prefixes=false;
    config.native_early_depth=false;config.native_early_depth_catalog=false;
    config.reuse_early_depth_readonly_boundaries=false;
    config.native_readonly_depth_attachment=false;config.native_readonly_depth_skip_edram_barrier=false;
    // These options require the D32 query path disabled above. Clear the whole
    // dependent family when the parent replay supplied its qualified preset.
    config.native_readonly_depth_read_only_pass=false;
    config.native_readonly_depth_lower_bound_query=false;
    config.native_readonly_depth_no_write_metadata=false;
    config.native_readonly_depth_conservative=false;
    config.readonly_query_render_pass_groups=false;config.diagnostic_readonly_query_groups=false;
    config.retained_fp24_depth_writer=false;config.retained_opaque_writer=false;
    config.retained_opaque_catalog=false;config.retained_shared_depth_queries=false;
    config.retained_d32_composition=false;config.retain_primary_across_disjoint_queries=false;
    config.reuse_retained_opaque_resources=false;config.queue_retained_exports_for_draws=false;
    config.fused_atlas_depth_clear=false;config.direct_atlas_depth_resolves=false;
    config.fused_packed_atlas_resolves=false;config.direct_packed_color_resolves=false;
    config.atlas_depth_region_restores=false;config.pinned_null_constant_uploads=false;
    config.delta_constant_uploads=false;config.present={};config.output.clear();
    config.debug_raw_images=false;config.detailed_reports=true;config.diagnostic_zero_history=false;
    config.gpu_batch_attribution=false;config.gpu_draw_timestamps=false;config.gpu_timestamps=false;
    const auto replacement=std::make_shared<const Bytes>(Bytes{0,0,0,0});
    const auto replacement_hash=ContentHash(*replacement);
    const auto previous_snapshot=config.payload_snapshot;const auto previous_payload=config.payload;
    const auto capture=config.capture;
    config.payload_snapshot=[previous_snapshot,previous_payload,capture,replacement,replacement_hash](const std::string& key) {
        if(key==replacement_hash)return replacement;
        if(previous_snapshot)return previous_snapshot(key);
        if(previous_payload)return std::make_shared<const Bytes>(previous_payload(key));
        return std::make_shared<const Bytes>(Read(capture/(key+".payload.bin")));
    };
    SceneExecutor scene(std::move(config));Outcome out;
    Check(scene.report().at("device").at("software")==false,"hardware execution required");
    if(enabled)Check(scene.report().at("canonical_resolve_stores").at("configured")==true,"optional pipeline unavailable");
    const auto layout=Layout(donor);const uint32_t address=Address(donor.resolve.at("destination"));
    const uint32_t backing=BackingBytes(layout,address);
    Check(address>=0x100000,"donor overlaps fixture query addresses");
    Bytes seed(kEdramBytes);for(size_t i=0;i<seed.size();++i)seed[i]=uint8_t(i*29+(i>>9)+17);
    scene.InitializeMemory(address,Bytes(backing,0xa5));scene.InitializeMemory(0x10000,Bytes(8192,0));
    uint64_t event=8000000,generation=1;uint32_t frame=1;
    const auto resolve=[&](bool partial=false,uint32_t target_address=0) {
        auto op=donor.resolve;op["event"]=event++;op["frame"]=frame;
        if(target_address) {
            op["destination"]["base"]=AddressText(target_address);
            auto fetch=op.at("destination_fetch").get<std::array<uint32_t,6>>();
            fetch[1]=(fetch[1]&4095u)|target_address;op["destination_fetch"]=fetch;
        }
        if(partial) {
            op["source_rectangle"]=Json::array({0,0,32,16});
            op["destination_rectangle"]=Json::array({0,0,32,16});
        }
        scene.Execute(op);
    };
    const auto clear=[&](bool alternate=false) {
        const auto& surface=donor.resolve.at("surfaces").at("6");
        scene.Execute(Json{{"type","clearrect"},{"event",event++},{"frame",frame},
            {"surfaces",donor.resolve.at("surfaces")},{"rectangle",Json::array({0,0,surface.at("width"),surface.at("height")})},
            {"flags",1},{"depth",0.f},{"stencil",0},
            {"color",alternate?Json::array({.75f,.25f,.125f,.5f}):Json::array({.125f,.375f,.75f,1.f})}});
    };
    const auto draw=[&](const std::string& mode,uint32_t target_address=0) {
        auto op=donor.draw;op["frame"]=frame;
        auto& texture=op["textures"][donor.texture];
        auto fetch=texture.at("fetch").get<std::array<uint32_t,6>>();
        if(target_address) {
            texture["identity"]["base"]=AddressText(target_address);
            fetch[1]=(fetch[1]&4095u)|target_address;
        }
        if(mode=="different-signs")fetch[0]^=1u<<2;
        if(mode=="different-endian") {
            fetch[1]^=1u<<6;texture["identity"]["endian"]=(fetch[1]>>6)&3u;
        }
        if(mode=="binding-only")fetch[3]^=1u<<13; // Exponent is a binding property, not decoded image identity.
        texture["fetch"]=fetch;
        const auto id=generation++;
        scene.Execute(Query(id,frame,event++,false));op["event"]=event++;scene.Execute(op);
        scene.Execute(Query(id,frame,event++,true));
    };
    for(const char* mode:{"immediate","retired-producer","binding-only","different-signs",
                         "different-endian","cpu-write","partial-resolve","pending-generations","cross-frame"}) {
        scene.InitializeEdram(seed);clear();
        const uint64_t before_stores=Counter(scene,"stores"),before_adopted=Counter(scene,"adopted");
        resolve();
        bool pending_owner=false;
        if(std::string_view(mode)=="retired-producer")scene.FlushGpuWork();
        else if(std::string_view(mode)=="cpu-write") {
            const auto at=uint32_t(TextureBlockOffset(layout,layout.levels.front(),0,0,0)+address)&~3u;
            scene.Execute(Json{{"type","cpu_memory_write"},{"event",event++},{"frame",frame},
                {"address",at},{"bytes",4},{"payload",replacement_hash}});
        } else if(std::string_view(mode)=="partial-resolve") {clear(true);resolve(true);}
        else if(std::string_view(mode)=="pending-generations") {
            draw("immediate");pending_owner=scene.HasPendingGpuWork();
            Check(pending_owner,"first sampled generation did not remain pending");
            clear(true);resolve();
        } else if(std::string_view(mode)=="cross-frame") {
            auto fetch=donor.resolve.at("destination_fetch").get<std::array<uint32_t,6>>();
            fetch[1]=(fetch[1]&4095u)|address;
            scene.Execute(Json{{"type","present"},{"event",event++},{"frame",frame},
                {"frontbuffer",donor.resolve.at("destination")},{"frontbuffer_fetch",fetch},
                {"provenance","controlled canonical-store fixture history"}});
            ++frame;
        }
        draw(mode);scene.FlushGpuWork();
        auto queries=scene.TakeCompletedQueries();
        const uint64_t expected_draws=std::string_view(mode)=="pending-generations"?2:1;
        Check(queries.size()==expected_draws,"query cardinality changed");
        for(const auto& q:queries)Check(q.result.draws==1&&q.result.samples>0,"actual donor query lacks positive one-draw samples");
        const uint64_t stores=Counter(scene,"stores")-before_stores;
        const uint64_t adopted=Counter(scene,"adopted")-before_adopted;
        const bool refuse=std::string_view(mode)=="different-signs"||std::string_view(mode)=="different-endian"||
            std::string_view(mode)=="cpu-write"||std::string_view(mode)=="partial-resolve"||
            std::string_view(mode)=="cross-frame";
        Check(stores==(enabled?expected_draws:0),std::string(mode)+" did not exercise expected full stores");
        Check(adopted==(enabled&&!refuse?expected_draws:0),std::string(mode)+" receipt adoption/refusal was vacuous");
        const auto report=scene.report();
        Check(report.at("gpu_work_queue").at("accepted_work")==report.at("gpu_work_queue").at("completed_work"),"accepted work did not retire");
        out.snapshots.emplace(mode,Snapshot{scene.ReadFullEdram(),scene.ReadPhysical(address,backing),Results(queries),
            Json{{"stores",stores},{"adopted",adopted},{"pending_old_generation",pending_owner},
                 {"cumulative",report.at("canonical_resolve_stores")}}});
    }
    if(donor.format==31) {
        // Nineteen actual 640x360 RGBA32F receipts exceed64MiB. Retire each
        // producer while retaining its unconsumed receipt, isolating the Scene
        // cache limit from packet scratch or queue-capacity pressure.
        constexpr uint64_t budget=64u*1024*1024;
        const uint64_t image_bytes=uint64_t(layout.width)*layout.height*16;
        const uint32_t count=uint32_t(budget/image_bytes)+1;
        Check(count>1&&count<=24,"receipt-budget donor needs too many images");
        Check(Counter(scene,"receipt_bytes")==0,"prior frame left an unconsumed receipt");
        const uint32_t first=0x14000000,stride=(backing+65535u)&~65535u;
        Check(uint64_t(first)+uint64_t(count)*stride<=512u*1024*1024,"receipt-budget addresses exceed physical memory");
        Check(uint64_t(address)+backing<=first||address>=uint64_t(first)+uint64_t(count)*stride,
            "receipt-budget addresses overlap the original destination");
        std::vector<uint32_t> addresses;Json ranges=Json::array();
        for(uint32_t i=0;i<count;++i) {
            const uint32_t target=first+i*stride;addresses.push_back(target);
            scene.InitializeMemory(target,Bytes(backing,0xa5));
            ranges.push_back({{"address",target},{"bytes",backing}});
        }
        scene.InitializeEdram(seed);clear();
        const uint64_t before_stores=Counter(scene,"stores"),before_adopted=Counter(scene,"adopted");
        const uint64_t before_refusals=Counter(scene,"budget_refusals");
        uint64_t last_refusals=0;
        for(const auto target:addresses) {
            const auto previous=Counter(scene,"budget_refusals");
            resolve(false,target);scene.FlushGpuWork();
            last_refusals=Counter(scene,"budget_refusals")-previous;
        }
        const uint64_t stores=Counter(scene,"stores")-before_stores;
        const uint64_t refusals=Counter(scene,"budget_refusals")-before_refusals;
        const uint64_t retained=Counter(scene,"receipt_bytes"),peak=Counter(scene,"peak_receipt_bytes");
        Check(retained<=budget&&peak<=budget,"receipt cache exceeded its actual allocation budget");
        Check(enabled?(stores>0&&refusals>0&&last_refusals==1&&retained>0):
            (stores==0&&refusals==0&&retained==0),"receipt cache pressure was not exercised");
        // The final address has a full, defined physical store but no admitted
        // receipt. Its real captured consumer must use the ordinary alias load.
        draw("receipt-budget",addresses.back());scene.FlushGpuWork();
        auto queries=scene.TakeCompletedQueries();
        Check(queries.size()==1&&queries[0].result.draws==1&&queries[0].result.samples>0,
            "receipt-budget donor query lacks positive one-draw samples");
        const auto adopted=Counter(scene,"adopted")-before_adopted;
        Check(adopted==0&&Counter(scene,"receipt_bytes")==retained,"budget-refused consumer adopted a receipt");
        const auto report=scene.report();
        Check(report.at("gpu_work_queue").at("accepted_work")==report.at("gpu_work_queue").at("completed_work"),
            "receipt-budget accepted work did not retire");
        Bytes physical;physical.reserve(size_t(count)*backing);
        for(const auto target:addresses) {
            auto bytes=scene.ReadPhysical(target,backing);physical.insert(physical.end(),bytes.begin(),bytes.end());
        }
        out.snapshots.emplace("receipt-budget",Snapshot{scene.ReadFullEdram(),std::move(physical),Results(queries),
            Json{{"stores",stores},{"adopted",adopted},{"budget_refusals",refusals},{"last_store_budget_refusals",last_refusals},
                {"full_resolves",count},{"physical_ranges",std::move(ranges)},{"retired_producers",true},
                {"cumulative",report.at("canonical_resolve_stores")}}});
    }
    out.report=scene.report();return out;
}
}
void RunNativeCanonicalStoreSceneFixture(const SceneExecutorConfig& config,const Json& original) {
    Check(!config.output.empty(),"output directory is required");
    std::filesystem::create_directories(config.output);
    const auto path=config.output/"canonical-store-scene-fixture.json";
    Check(!std::filesystem::exists(path),"witness already exists");
    Json rows=Json::array();
    for(const auto& donor:Donors(original)) {
        const auto off=Run(config,donor,false),on=Run(config,donor,true);
        Check(off.snapshots.size()==on.snapshots.size(),"mode cardinality differs");
        for(const auto& [name,baseline]:off.snapshots) {
            const auto& candidate=on.snapshots.at(name);
            Check(baseline.edram.size()==kEdramBytes&&baseline.edram==candidate.edram,"full10MiB differs in "+name);
            Check(baseline.physical==candidate.physical,"full destination backing differs in "+name);
            Check(baseline.queries==candidate.queries,"real query identity/sample results differ in "+name);
            rows.push_back({{"raw_format",donor.format},{"mode",name},{"exact",true},
                {"source_resolve_event",donor.resolve.at("event")},{"source_draw_event",donor.draw.at("event")},
                {"edram_bytes",candidate.edram.size()},{"edram_sha256",ContentHash(candidate.edram)},
                {"physical_bytes",candidate.physical.size()},{"physical_sha256",ContentHash(candidate.physical)},
                {"queries",candidate.queries},{"baseline",baseline.mechanism},{"candidate",candidate.mechanism}});
        }
    }
    Json witness={{"capacity",config.async_submission_capacity},{"paired_cases",rows.size()},{"rows",std::move(rows)},
        {"scope","Actual captured raw26/raw31 resolve and shader/geometry donors over identical nonzero full EDRAM and physical backing. Independent future-use, interpretation/version refusal, pending-generation, frame-retirement and bounded current-receipt budget scenarios; exact full memory and real positive query results. Synthetic fixture history is not original capture history or FPS evidence."}};
    std::ofstream file(path);file<<witness.dump(2)<<'\n';Check(bool(file),"cannot write witness");
}
}
