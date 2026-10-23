#include "fused_atlas_depth_clear_scene_fixture.h"
#include "renderer/native/content_hash.h"
#include "renderer/native/texture_data.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <fstream>
#include <map>
#include <vector>

namespace gow3::renderer::validation {
namespace {
using Json=nlohmann::json;
constexpr uint32_t kEdramBytes=2048u*5120,kTextureBytes=4u*1024*1024;
void Check(bool good,const char* reason) {if(!good)throw std::runtime_error(reason);}
struct Snapshot {std::vector<uint8_t> edram,texture,preview;};
struct Outcome {std::map<std::string,Snapshot> snapshots;Json report,queries;};

std::vector<uint8_t> Seed(uint32_t salt) {
    std::vector<uint8_t> result(kEdramBytes);
    for(size_t i=0;i<result.size();++i)result[i]=uint8_t(i*29+(i>>8)+salt);
    constexpr std::array<uint32_t,10> depths{
        0,1,0x3fffff,0x400000,0x400001,0x7fffff,0x800000,0x800001,0xfffffe,0xffffff};
    for(uint32_t y=0;y<688;++y)for(uint32_t x=0;x<1120;++x) {
        // Independent 80x16 half-tile address, entirely inside10MiB.
        const auto word=(depths[(x+y*3+salt)%depths.size()]<<8)|((x*17+y*29+salt)&255);
        const uint32_t at=((1440+(y/16)*14+x/80)*1280+(y%16)*80+(x%80+40)%80)*4;
        Check(at+4<=result.size(),"Fixture atlas address escaped EDRAM");
        for(unsigned b=0;b<4;++b)result[at+b]=uint8_t(word>>(b*8));
    }
    return result;
}

Outcome Run(const SceneExecutorConfig& base,const Json& donor,bool enabled,bool stencil_export) {
    auto config=base;
    config.fused_atlas_depth_clear=enabled;
    config.atlas_depth_region_restores=false;
    config.pinned_null_constant_uploads=false;
    config.delta_constant_uploads=false;
    config.native_early_depth=false;config.native_early_depth_catalog=false;
    config.native_readonly_depth_attachment=false;
    config.native_fsi=true;config.async_gpu_submissions=true;
    config.unified_gpu_queue=true;config.batch_transfers=true;
    config.defer_native_queries=true;
    config.max_deferred_queries=base.async_submission_capacity;
    config.restore_depth_stencil_export=stencil_export;
    config.present={};config.debug_raw_images=true;config.detailed_reports=true;
    config.output=base.output/(std::string(stencil_export?"export-":"portable-")+(enabled?"on":"off"));
    Check(!std::filesystem::exists(config.output),"Fixture output already exists");
    std::filesystem::create_directories(config.output);
    Outcome out;
    SceneExecutor scene(std::move(config));
    Check(scene.report().at("device").at("software")==false,"Scene fixture requires hardware");
    if(stencil_export)Check(scene.report().at("depth_stencil_restores").at("extension_enabled")==true,
        "Scene fixture requires actual stencil export for the export arm");
    const uint32_t address=uint32_t(std::stoul(donor.at("destination").at("base").get<std::string>(),nullptr,16));
    Check(uint64_t(address)+kTextureBytes<=512u*1024*1024,"Fixture destination extent invalid");
    std::vector<uint8_t> poison(kTextureBytes,0xa5);
    scene.InitializeMemory(address,poison);
    scene.InitializeMemory(0x10000,std::vector<uint8_t>(64,0));
    scene.InitializeEdram(Seed(0));
    uint64_t event=1000000;
    const auto clear=[&](std::array<int32_t,4> rect,float depth=1.0f,uint32_t flags=16,uint32_t stencil=0) {
        scene.Execute(Json{{"type","clearrect"},{"frame",1},{"event",event++},
            {"surfaces",donor.at("surfaces")},{"rectangle",rect},{"depth",depth},
            {"flags",flags},{"stencil",stencil},{"color",Json::array({0.f,0.f,0.f,0.f})}});
    };
    const auto snapshot=[&](const char* name) {
        auto resolve=donor;resolve["frame"]=1;resolve["event"]=event++;
        resolve["source_rectangle"]=std::array{0,0,1120,688};
        resolve["destination_rectangle"]=std::array{0,0,1120,688};
        scene.Execute(resolve);
        out.snapshots.emplace(name,Snapshot{scene.ReadFullEdram(),scene.ReadPhysical(address,kTextureBytes)});
    };
    const auto query=[&](bool end) {
        scene.Execute(Json{{"type",end?"query_end":"query_begin"},{"query_generation",1},
            {"query_handle",0x6655},{"report_address",end?0x10000:0x10020},{"frame",1},{"event",event++}});
    };
    clear({0,0,74,74});
    snapshot("packed-clear-immediate-resolve");

    // A noncanonical current host float makes loss of the pre-encode receipt
    // observable: its first UNORM24 pack is odd and its packed roundtrip is even.
    clear({300,200,360,260},std::bit_cast<float>(0x3e800003u));
    clear({0,0,138,74});
    snapshot("host-float-clear-immediate-resolve");

    clear({41,17,179,91});
    clear({1046,614,1120,688});
    snapshot("pending-receipt-next-clear");

    clear({0,268,138,342});
    scene.InitializeEdram(Seed(7));
    snapshot("receipt-invalidated-by-physical-write");

    query(false);clear({0,0,138,42});query(true);
    scene.FlushGpuWork();
    for(const auto& q:scene.TakeCompletedQueries())out.queries.push_back(Json::array({
        q.generation,q.frame,q.end_event,q.result.handle,q.result.begin_address,
        q.result.end_address,q.result.draws,q.result.samples}));
    snapshot("active-query-original-clear");

    clear({0,0,138,74});
    clear({79,15,82,18},0.f,32,0x5a);
    snapshot("receipt-stencil-only-followup");
    scene.FlushGpuWork();out.report=scene.report();
    const auto& queue=out.report.at("gpu_work_queue");
    Check(queue.at("accepted_work")==queue.at("completed_work"),"Fixture accepted work did not retire");
    Check(out.queries.size()==1&&out.queries[0][7]==0,"Fixture active-query fallback did not retire the empty query");
    if(enabled) {
        const auto& s=out.report.at("fused_atlas_depth_clear");
        Check(s.at("configured")==true&&s.at("packed_sources").get<uint64_t>()>0&&
            s.at("host_sources").get<uint64_t>()>0&&
            s.at("deferred_host_materializations").get<uint64_t>()>=3&&
            s.at("deferred_host_discards").get<uint64_t>()>0&&
            s.at("provenance_refusals").get<uint64_t>()>0,
            "Controlled Scene did not exercise both sources and receipt future-use cases");
    }
    return out;
}
Outcome RunDirect(const SceneExecutorConfig& base,const Json& donor,bool enabled,bool stencil_export) {
    auto config=base;
    config.fused_atlas_depth_clear=enabled;config.direct_atlas_depth_resolves=enabled;
    config.atlas_depth_region_restores=false;config.pinned_null_constant_uploads=false;
    config.delta_constant_uploads=false;config.native_early_depth=false;
    config.native_early_depth_catalog=false;config.native_readonly_depth_attachment=false;
    config.native_fsi=true;config.async_gpu_submissions=true;
    config.unified_gpu_queue=true;config.batch_transfers=true;config.defer_native_queries=true;
    config.max_deferred_queries=base.async_submission_capacity;
    config.restore_depth_stencil_export=stencil_export;
    config.present={};config.debug_raw_images=true;config.detailed_reports=true;
    const auto output=base.output/(std::string(stencil_export?"export-":"portable-")+(enabled?"on":"off"));
    config.output=output;
    Check(!std::filesystem::exists(output),"Direct fixture output already exists");
    std::filesystem::create_directories(output);
    SceneExecutor scene(std::move(config));Outcome out;
    Check(scene.report().at("device").at("software")==false,"Direct Scene fixture requires hardware");
    if(stencil_export)Check(scene.report().at("depth_stencil_restores").at("extension_enabled")==true,
        "Direct fixture requires actual stencil export for the export arm");
    const uint32_t address=uint32_t(std::stoul(donor.at("destination").at("base").get<std::string>(),nullptr,16));
    Check(uint64_t(address)+kTextureBytes<=512u*1024*1024,"Direct fixture destination extent invalid");
    scene.InitializeMemory(address,std::vector<uint8_t>(kTextureBytes,0xa5));
    scene.InitializeMemory(0x10000,std::vector<uint8_t>(64,0));
    scene.InitializeEdram(Seed(0));uint64_t event=2000000;
    const auto clear=[&](std::array<int32_t,4> rect,float depth=1.f,uint32_t flags=16,uint32_t stencil=0) {
        scene.Execute(Json{{"type","clearrect"},{"frame",1},{"event",event++},
            {"surfaces",donor.at("surfaces")},{"rectangle",rect},{"depth",depth},
            {"flags",flags},{"stencil",stencil},{"color",Json::array({0.f,0.f,0.f,0.f})}});
    };
    const auto copy=[&](std::array<int32_t,4> src,std::array<int32_t,4> dst) {
        auto op=donor;op["event"]=event++;op["frame"]=1;
        op["source_rectangle"]=src;op["destination_rectangle"]=dst;
        scene.Execute(op);return event-1;
    };
    const auto snapshot=[&](const char* name,uint64_t id) {
        const auto path=output/("resolve-"+std::to_string(id)+".rgba32f");
        std::ifstream file(path,std::ios::binary|std::ios::ate);
        Check(bool(file)&&file.tellg()==std::streamoff(1120u*688u*16u),"Direct fixture raw resolved image missing");
        std::vector<uint8_t> preview(1120u*688u*16u);file.seekg(0);
        file.read(reinterpret_cast<char*>(preview.data()),preview.size());
        Check(bool(file),"Truncated direct fixture preview");
        out.snapshots.emplace(name,Snapshot{scene.ReadFullEdram(),scene.ReadPhysical(address,kTextureBytes),std::move(preview)});
    };
    const auto query=[&](bool end) {
        scene.Execute(Json{{"type",end?"query_end":"query_begin"},{"query_generation",1},
            {"query_handle",0x7766},{"report_address",end?0x10000:0x10020},{"frame",1},{"event",event++}});
    };
    snapshot("packed-offset-resolve",copy({41,17,179,91},{0,0,138,74}));
    snapshot("same-packed-version-second-resolve",copy({982,614,1120,688},{79,15,217,89}));
    clear({300,200,360,260},std::bit_cast<float>(0x3e800003u));
    snapshot("current-host-noncanonical-float-refusal",copy({300,200,438,274},{0,100,138,174}));
    clear({0,0,138,74});
    snapshot("fused-receipt-resolve",copy({300,200,438,274},{200,100,338,174}));
    snapshot("same-receipt-second-resolve",copy({0,0,74,74},{400,100,474,174}));
    clear({79,15,82,18},0.f,32,0x5a);
    snapshot("receipt-later-stencil-clear",copy({41,0,179,74},{600,100,738,174}));
    clear({0,268,138,342});scene.InitializeEdram(Seed(7));
    snapshot("changed-packed-version",copy({0,268,138,342},{0,200,138,274}));
    query(false);const auto query_copy=copy({0,0,138,42},{200,200,338,242});query(true);
    scene.FlushGpuWork();
    for(const auto& q:scene.TakeCompletedQueries())out.queries.push_back(Json::array({
        q.generation,q.frame,q.end_event,q.result.handle,q.result.begin_address,
        q.result.end_address,q.result.draws,q.result.samples}));
    snapshot("active-query-original-resolve",query_copy);
    scene.FlushGpuWork();out.report=scene.report();
    Check(out.report.at("gpu_work_queue").at("accepted_work")==out.report.at("gpu_work_queue").at("completed_work"),
        "Direct fixture work did not retire");
    Check(out.queries.size()==1&&out.queries[0][7]==0,"Direct fixture query did not retire");
    if(enabled) {
        const auto& stats=out.report.at("direct_atlas_depth_resolves");
        Check(stats.at("configured")==true&&stats.at("candidates")==8&&stats.at("selected")==5&&
            stats.at("packed_sources")==3&&stats.at("receipt_sources")==2&&
            stats.at("current_host_refusals")==2&&stats.at("query_refusals")==1&&
            stats.at("layout_refusals")==0&&stats.at("provenance_refusals")==0&&stats.at("resource_refusals")==0,
            "Direct Scene fixture did not exercise the intended source/refusal sequence");
        const auto& clears=out.report.at("fused_atlas_depth_clear");
        Check(clears.at("deferred_host_materializations")==1&&clears.at("deferred_host_discards")==1,
            "Direct Scene fixture did not preserve the receipt for later materialization/invalidation");
    }
    return out;
}

struct FusedPackedAtlasOutcome : Outcome {uint64_t checked_untouched_bytes=0,partial_copies=0;Json precision_witnesses;};
FusedPackedAtlasOutcome RunFusedPackedAtlas(const SceneExecutorConfig& base,const Json& donor,bool enabled,bool stencil_export) {
    auto config=base;
    config.fused_atlas_depth_clear=true;config.direct_atlas_depth_resolves=true;
    config.packed_primary_stencil_clear=true;config.packed_secondary_stencil_clear=false;
    config.direct_packed_color_resolves=false;config.reuse_cpu_write_upload=false;
    config.fused_packed_atlas_resolves=enabled;
    config.retained_fp24_depth_writer=false;config.retained_opaque_writer=false;
    config.retained_opaque_catalog=false;config.retained_shared_depth_queries=false;
    config.retain_primary_across_disjoint_queries=false;
    config.atlas_depth_region_restores=false;config.pinned_null_constant_uploads=false;
    config.delta_constant_uploads=false;config.native_early_depth=false;
    config.native_early_depth_catalog=false;config.native_readonly_depth_attachment=false;config.native_readonly_depth_skip_edram_barrier=false;
    config.native_fsi=true;config.async_gpu_submissions=true;
    config.unified_gpu_queue=true;config.batch_transfers=true;config.defer_native_queries=true;
    config.max_deferred_queries=base.async_submission_capacity;
    config.restore_depth_stencil_export=stencil_export;
    config.present={};config.debug_raw_images=true;config.detailed_reports=true;
    const auto output=base.output/(std::string(stencil_export?"fused-packed-export-":"fused-packed-portable-")+(enabled?"on":"off"));
    config.output=output;
    Check(!std::filesystem::exists(output),"Direct fixture output already exists");
    std::filesystem::create_directories(output);
    SceneExecutor scene(std::move(config));FusedPackedAtlasOutcome out;
    Check(scene.report().at("device").at("software")==false,"Direct Scene fixture requires hardware");
    if(stencil_export)Check(scene.report().at("depth_stencil_restores").at("extension_enabled")==true,
        "Direct fixture requires actual stencil export for the export arm");
    const uint32_t address=uint32_t(std::stoul(donor.at("destination").at("base").get<std::string>(),nullptr,16));
    Check(uint64_t(address)+kTextureBytes<=512u*1024*1024,"Direct fixture destination extent invalid");
    const auto layout=DescribeTexture(donor.at("destination_fetch").get<std::array<uint32_t,6>>());
    Check(layout.format==22&&layout.endian==2&&layout.tiled&&layout.levels.size()==1&&
        layout.width==1120&&layout.height==688,"Fused packed fixture destination layout changed");
    scene.InitializeMemory(address,std::vector<uint8_t>(kTextureBytes,0xa5));
    scene.InitializeMemory(0x10000,std::vector<uint8_t>(64,0));
    scene.InitializeEdram(Seed(0));uint64_t event=6000000;
    const auto clear=[&](std::array<int32_t,4> rect,float depth=1.f,uint32_t flags=16,uint32_t stencil=0) {
        scene.Execute(Json{{"type","clearrect"},{"frame",1},{"event",event++},
            {"surfaces",donor.at("surfaces")},{"rectangle",rect},{"depth",depth},
            {"flags",flags},{"stencil",stencil},{"color",Json::array({0.f,0.f,0.f,0.f})}});
    };
    const auto copy=[&](std::array<int32_t,4> rect) {
        const auto prior=scene.ReadPhysical(address,kTextureBytes);
        auto op=donor;op["event"]=event++;op["frame"]=1;
        op["source_rectangle"]=rect;op["destination_rectangle"]=rect;
        scene.Execute(op);
        const auto actual=scene.ReadPhysical(address,kTextureBytes);
        std::vector<uint8_t> written(kTextureBytes,0);
        for(int32_t y=rect[1];y<rect[3];++y)for(int32_t x=rect[0];x<rect[2];++x) {
            const auto at=TextureBlockOffset(layout,layout.levels[0],uint32_t(x),uint32_t(y),0);
            Check(at+4<=written.size(),"Fused packed fixture destination word escaped allocation");
            for(unsigned b=0;b<4;++b) {
                Check(!written[at+b],"Fused packed fixture destination word alias");written[at+b]=1;
            }
        }
        for(size_t i=0;i<actual.size();++i)if(!written[i]) {
            Check(actual[i]==prior[i],"Fused packed partial resolve changed untouched physical bytes");
            ++out.checked_untouched_bytes;
        }
        ++out.partial_copies;return event-1;
    };
    const auto snapshot=[&](const char* name,uint64_t id) {
        const auto path=output/("resolve-"+std::to_string(id)+".rgba32f");
        std::ifstream file(path,std::ios::binary|std::ios::ate);
        Check(bool(file)&&file.tellg()==std::streamoff(1120u*688u*16u),"Direct fixture raw resolved image missing");
        std::vector<uint8_t> preview(1120u*688u*16u);file.seekg(0);
        file.read(reinterpret_cast<char*>(preview.data()),preview.size());
        Check(bool(file),"Truncated direct fixture preview");
        out.snapshots.emplace(name,Snapshot{scene.ReadFullEdram(),scene.ReadPhysical(address,kTextureBytes),std::move(preview)});
    };
    const auto query=[&](bool end) {
        scene.Execute(Json{{"type",end?"query_end":"query_begin"},{"query_generation",1},
            {"query_handle",0xbb77},{"report_address",end?0x10000:0x10020},{"frame",1},{"event",event++}});
    };
    const auto precision_witness=[&](const char* name) {
        const auto& pixels=out.snapshots.at(name).preview;
        const size_t at=(size_t(20)*1120+80)*16;uint32_t actual=0;
        Check(at+4<=pixels.size(),"Fused packed precision witness escaped raw output");
        std::memcpy(&actual,pixels.data()+at,4);
        Check(actual==0x3e800002u,"Current host or receipt lost the pre-encode UNORM precision witness");
        out.precision_witnesses.push_back({{"case",name},{"x",80},{"y",20},{"host_clear_bits",0x3e800003u},
            {"actual_resolve_bits",actual},{"canonical_packed_code",0x400001u},
            {"canonical_float32_redecode_model_bits",0x3e800000u}});
    };
    snapshot("packed96-resolve",copy({0,0,96,96}));
    // A stale destination host must be repaired before a partial MRT resolve.
    std::vector<uint8_t> poison(kTextureBytes);
    for(size_t i=0;i<poison.size();++i)poison[i]=uint8_t(0x5d+i*19+(i>>7));
    scene.InitializeMemory(address,poison);
    snapshot("same-packed-version160-destination-repair",copy({0,0,160,96}));
    clear({80,20,92,60},std::bit_cast<float>(0x3e800003u));
    snapshot("current-host-noncanonical-refusal",copy({0,0,96,96}));
    precision_witness("current-host-noncanonical-refusal");
    // This receipt preserves the noncanonical host float at x80; decoding
    // packed bytes instead would quantize it. The new path must refuse it.
    clear({0,0,74,74});
    snapshot("consequential-preencode-receipt-refusal",copy({0,0,96,96}));
    precision_witness("consequential-preencode-receipt-refusal");
    snapshot("same-receipt-second-refusal",copy({0,0,160,96}));
    clear({79,15,82,18},0.f,32,0x5a);
    snapshot("receipt-later-stencil-host-refusal",copy({0,0,96,96}));
    clear({0,268,138,342});scene.InitializeEdram(Seed(7));
    snapshot("changed-packed-version160",copy({0,0,160,64}));
    query(false);const auto query_copy=copy({0,0,160,96});query(true);
    scene.FlushGpuWork();
    for(const auto& q:scene.TakeCompletedQueries())out.queries.push_back(Json::array({
        q.generation,q.frame,q.end_event,q.result.handle,q.result.begin_address,
        q.result.end_address,q.result.draws,q.result.samples}));
    snapshot("active-query-original-future-consumer",query_copy);
    scene.FlushGpuWork();out.report=scene.report();
    Check(out.report.at("gpu_work_queue").at("accepted_work")==out.report.at("gpu_work_queue").at("completed_work"),
        "Direct fixture work did not retire");
    Check(out.queries.size()==1&&out.queries[0][7]==0,"Direct fixture query did not retire");
    const auto& direct=out.report.at("direct_atlas_depth_resolves");
    Check(direct.at("configured")==true&&direct.at("candidates")==8&&direct.at("selected")==5&&
        direct.at("packed_sources")==3&&direct.at("receipt_sources")==2&&
        direct.at("current_host_refusals")==2&&direct.at("query_refusals")==1&&
        direct.at("layout_refusals")==0&&direct.at("provenance_refusals")==0&&direct.at("resource_refusals")==0,
        "Fused packed Scene fixture changed original source/refusal selection");
    Check(direct.at("packed_decode_pixels")==uint64_t(enabled?0:3)*1120*688,
        "Fused packed Scene fixture did not remove exactly three full source decodes");
    const auto& clears=out.report.at("fused_atlas_depth_clear");
    Check(clears.at("deferred_host_materializations")==1&&clears.at("deferred_host_discards")==1,
        "Fused packed fixture changed receipt future materialization/invalidation");
    Check(out.report.value("texture_alias_restores",Json::array()).size()>=1,
        "Fused packed fixture did not exercise partial destination repair");
    Check(out.partial_copies==8&&out.checked_untouched_bytes==33181696,
        "Fused packed fixture untouched-byte coverage changed");
    if(enabled) {
        const auto& stats=out.report.at("fused_packed_atlas_resolves");
        Check(stats.at("configured")==true&&stats.at("candidates")==8&&stats.at("selected")==3&&
            stats.at("receipt_refusals")==2&&stats.at("current_host_refusals")==2&&stats.at("query_refusals")==1&&
            stats.at("layout_refusals")==0&&stats.at("provenance_refusals")==0&&stats.at("resource_refusals")==0&&
            stats.at("source_pixels")==uint64_t(3)*1120*688&&stats.at("resolve_pixels")==34816,
            "Fused packed Scene fixture missed intended selection/refusal counts");
    } else Check(!out.report.contains("fused_packed_atlas_resolves"),"Disabled fused packed path emitted active report");
    return out;
}


std::vector<uint8_t> PrimaryStencilSeed(uint32_t salt) {
    auto result=Seed(salt);
    constexpr std::array<uint32_t,12> depths{
        0,1,0xfffff,0x100000,0x100001,0x7fffff,0x800000,0xefffff,
        0xf00000,0xf00001,0xfffffe,0xffffff};
    for(uint32_t y=0;y<720;++y)for(uint32_t x=0;x<1280;++x) {
        const uint32_t word=(depths[(x+y*3+salt)%depths.size()]<<8)|((x*17+y*29+salt)&255);
        const uint32_t at=(((y/16)*16+x/80)*1280+(y%16)*80+(x%80+40)%80)*4;
        for(unsigned b=0;b<4;++b)result[at+b]=uint8_t(word>>(b*8));
    }
    return result;
}
std::vector<uint8_t> PrimaryStencilOracle(std::vector<uint8_t> bytes,
        const std::array<int32_t,4>& r,uint32_t stencil) {
    for(uint32_t y=0;y<720;++y)for(uint32_t x=0;x<1280;++x) {
        const uint32_t at=(((y/16)*16+x/80)*1280+(y%16)*80+(x%80+40)%80)*4;
        uint32_t old=0;for(unsigned b=0;b<4;++b)old|=uint32_t(bytes[at+b])<<(b*8);
        const bool inside=int32_t(x)>=r[0]&&int32_t(x)<r[2]&&int32_t(y)>=r[1]&&int32_t(y)<r[3];
        const uint32_t depth=(old>>8)>0xf00000u?0xf00000u:old>>8;
        const uint32_t value=(depth<<8)|(inside?(stencil&255u):(old&255u));
        for(unsigned b=0;b<4;++b)bytes[at+b]=uint8_t(value>>(b*8));
    }
    return bytes;
}
Outcome RunPrimaryStencil(const SceneExecutorConfig& base,const Json& donor,bool enabled,bool stencil_export) {
    auto config=base;
    config.packed_primary_stencil_clear=enabled;
    config.fused_atlas_depth_clear=false;config.direct_atlas_depth_resolves=false;
    config.packed_zero_depth_clear=false;config.atlas_depth_region_restores=false;
    config.pinned_null_constant_uploads=false;config.delta_constant_uploads=false;
    config.native_early_depth=false;config.native_early_depth_catalog=false;
    config.reuse_early_depth_readonly_boundaries=false;
    config.native_readonly_depth_attachment=false;config.native_readonly_depth_skip_edram_barrier=false;
    config.native_fsi=true;config.async_gpu_submissions=true;
    config.unified_gpu_queue=true;config.batch_transfers=true;config.defer_native_queries=true;
    config.max_deferred_queries=base.async_submission_capacity;
    config.restore_depth_stencil_export=stencil_export;
    config.present={};config.debug_raw_images=true;config.detailed_reports=true;
    const auto output=base.output/(std::string(stencil_export?"export-":"portable-")+(enabled?"on":"off"));
    config.output=output;
    Check(!std::filesystem::exists(output),"Primary stencil fixture output already exists");
    std::filesystem::create_directories(output);
    SceneExecutor scene(std::move(config));Outcome out;
    Check(scene.report().at("device").at("software")==false,"Primary stencil Scene fixture requires hardware");
    if(stencil_export)Check(scene.report().at("depth_stencil_restores").at("extension_enabled")==true,
        "Primary stencil fixture requires actual export in its export arm");
    const uint32_t address=uint32_t(std::stoul(donor.at("destination").at("base").get<std::string>(),nullptr,16));
    Check(uint64_t(address)+kTextureBytes<=512u*1024*1024,"Primary stencil destination extent invalid");
    scene.InitializeMemory(address,std::vector<uint8_t>(kTextureBytes,0xa5));
    scene.InitializeMemory(0x10000,std::vector<uint8_t>(64,0));
    auto seed=PrimaryStencilSeed(0);scene.InitializeEdram(seed);uint64_t event=3000000;
    const auto clear=[&](std::array<int32_t,4> rect,uint32_t stencil,uint32_t flags=32,float depth=0.f,
                         const Json* surfaces=nullptr) {
        scene.Execute(Json{{"type","clearrect"},{"frame",1},{"event",event++},
            {"surfaces",surfaces?*surfaces:donor.at("surfaces")},{"rectangle",rect},
            {"depth",depth},{"flags",flags},{"stencil",stencil},{"color",Json::array({0.25f,0.5f,0.75f,1.f})}});
    };
    const auto copy=[&] {
        auto op=donor;op["event"]=event++;op["frame"]=1;
        op["source_rectangle"]=std::array{0,0,1280,720};
        op["destination_rectangle"]=std::array{0,0,1280,720};
        scene.Execute(op);return event-1;
    };
    const auto snapshot=[&](const char* name) {
        const uint64_t id=copy();
        const auto path=output/("resolve-"+std::to_string(id)+".rgba32f");
        std::ifstream file(path,std::ios::binary|std::ios::ate);
        Check(bool(file)&&file.tellg()==std::streamoff(1280u*720u*16u),"Primary future-consumer raw resolve image missing");
        std::vector<uint8_t> preview(1280u*720u*16u);file.seekg(0);
        file.read(reinterpret_cast<char*>(preview.data()),preview.size());
        Check(bool(file),"Primary future-consumer raw image truncated");
        out.snapshots.emplace(name,Snapshot{scene.ReadFullEdram(),scene.ReadPhysical(address,kTextureBytes),std::move(preview)});
    };
    const auto restores=[&] {return scene.report().at("depth_stencil_restores").at("calls").get<uint64_t>();};
    const auto expect_stale_bypass=[&](uint64_t before) {
        Check(restores()-before==(enabled?0u:1u),"Stale clear did not exercise original full restore versus packed bypass");
    };
    const auto check_bytes=[&](const auto& expected) {
        Check(scene.ReadFullEdram()==expected,"Primary stencil full10MiB normalization/S8/outside oracle differs");
    };
    const auto query=[&](bool end) {
        scene.Execute(Json{{"type",end?"query_end":"query_begin"},{"query_generation",1},
            {"query_handle",0x8877},{"report_address",end?0x10000:0x10020},{"frame",1},{"event",event++}});
    };
    // 1: no cached host; first clear needs original full restoration, candidate
    // must perform zero restorations until the subsequent genuine consumer.
    auto before=restores();clear({0,0,1280,720},0xa5);expect_stale_bypass(before);
    check_bytes(PrimaryStencilOracle(seed,{0,0,1280,720},0xa5));
    snapshot("absent-full-clear-future-consumer");
    // 2: the same-token future consumer made the host current. A repeated
    // stencil clear must refuse the optional path and preserve the real image.
    clear({512,320,608,384},0x23);snapshot("same-version-current-host-repeat");
    // 3: a full Surface restore can make the host CURRENT while packed codes
    // above one still exist. Reading is not StoreSurface; raw masking is unsafe.
    seed=PrimaryStencilSeed(3);scene.InitializeEdram(seed);(void)copy();
    Check(scene.ReadFullEdram()==seed,"Warm host consumer normalized packed bytes before stencil clear");
    clear({41,17,179,91},0x5a);
    check_bytes(PrimaryStencilOracle(seed,{41,17,179,91},0x5a));
    snapshot("restore-only-current-host-above-one-refusal");
    // 4: create an actual non-FP24 D32 clear value; preserve that cached float
    // through the refused stencil shortcut and its later D32 consumer.
    clear({300,200,360,260},0,16,std::bit_cast<float>(0x3e800003u));
    clear({300,200,360,260},0xc7);snapshot("current-host-noncanonical-depth-refusal");
    // 5: a real packed initialization changes the version without promoting
    // host cache state. A partial S8 clear still normalizes ALL primary depth.
    seed=PrimaryStencilSeed(7);scene.InitializeEdram(seed);before=restores();
    clear({41,17,179,91},0x7b);expect_stale_bypass(before);
    const auto normalized=PrimaryStencilOracle(seed,{41,17,179,91},0x7b);
    uint64_t outside_normalized_words=0;
    for(uint32_t y=0;y<720;++y)for(uint32_t x=0;x<1280;++x)if(!(x>=41&&x<179&&y>=17&&y<91)) {
        const uint32_t at=(((y/16)*16+x/80)*1280+(y%16)*80+(x%80+40)%80)*4;
        const uint32_t old_depth=uint32_t(seed[at+1])|(uint32_t(seed[at+2])<<8)|(uint32_t(seed[at+3])<<16);
        outside_normalized_words+=old_depth>0xf00000u;
        Check(normalized[at]==seed[at],"Partial clear changed outside stencil in its independent oracle");
    }
    Check(outside_normalized_words>0,"Partial outside-depth normalization witness is vacuous");
    check_bytes(normalized);snapshot("changed-version-partial-outside-normalization");
    // 6: two clears before a host read. The candidate retains stale host state
    // across both; baseline only needs one restore, not one per selected clear.
    seed=PrimaryStencilSeed(11);scene.InitializeEdram(seed);before=restores();
    clear({79,15,82,18},0x11);clear({1206,646,1280,720},0xef);expect_stale_bypass(before);
    check_bytes(PrimaryStencilOracle(PrimaryStencilOracle(seed,{79,15,82,18},0x11),
        {1206,646,1280,720},0xef));snapshot("repeated-stale-clears-before-consumer");
    // 7: a live logical query uses the original Native image path, preserves
    // its actual empty query identity/sample value, and retires fully.
    scene.InitializeEdram(PrimaryStencilSeed(13));query(false);clear({512,320,608,384},0x49);query(true);
    scene.FlushGpuWork();
    for(const auto& q:scene.TakeCompletedQueries())out.queries.push_back(Json::array({
        q.generation,q.frame,q.end_event,q.result.handle,q.result.begin_address,
        q.result.end_address,q.result.draws,q.result.samples}));
    snapshot("active-query-original-fallback");
    // 8: a real differently interpreted color image writes overlapping depth
    // tiles and commits in the old scope-tail before primary provenance is read.
    // Public Execute commits dirty images before the next operation; this is
    // real alias FIFO proof, not an injected Image::dirty admission witness.
    Json alias=donor.at("surfaces");auto color=alias.at("6");
    color["edram_base"]=0;color["format"]=0;color["format_info"]=0;color["depth"]=false;
    alias["6"]=color;
    const auto before_alias=scene.ReadFullEdram();clear({0,0,1280,720},0,1,0.f,&alias);
    Check(scene.ReadFullEdram()!=before_alias,"Actual overlapping color-alias write was vacuous");
    before=restores();clear({512,320,608,384},0x96);expect_stale_bypass(before);
    snapshot("overlapping-color-alias-commit-then-primary");
    scene.FlushGpuWork();out.report=scene.report();
    Check(out.report.at("gpu_work_queue").at("accepted_work")==out.report.at("gpu_work_queue").at("completed_work"),
        "Primary stencil fixture accepted work did not retire");
    Check(out.queries.size()==1&&out.queries[0][0]==1&&out.queries[0][3]==0x8877&&
        out.queries[0][4]==0x10020&&out.queries[0][5]==0x10000&&out.queries[0][6]==0&&out.queries[0][7]==0,
        "Primary stencil query identity/empty sample result did not retire");
    if(enabled) {
        const auto& stats=out.report.at("packed_primary_stencil_clear");
        Check(stats.at("configured")==true&&stats.at("candidates")==9&&stats.at("selected")==5&&
            stats.at("packed_sources")==5&&stats.at("current_host_refusals")==3&&stats.at("query_refusals")==1&&
            stats.at("provenance_refusals")==0&&stats.at("resource_refusals")==0&&
            stats.at("source_pixels")==uint64_t(5)*1280*720&&stats.at("clear_pixels")==943441,
            "Primary stencil Scene fixture did not reach the exact source/refusal sequence");
    }
    return out;
}


uint32_t PrimaryColorByteOffset(uint32_t x,uint32_t y) {
    // Independent32-bit80x16 color tiles, without depth's swapped half-tiles.
    return ((720+(y/16)*16+x/80)*1280+(y%16)*80+x%80)*4;
}
uint32_t ColorWord(const std::vector<uint8_t>& bytes,size_t at,bool big_endian=false) {
    Check(at+4<=bytes.size(),"Color fixture word escaped allocation");
    uint32_t word=0;for(unsigned b=0;b<4;++b)word|=uint32_t(bytes[at+b])<<(8*(big_endian?3-b:b));
    return word;
}
std::vector<uint8_t> PrimaryColorSeed(uint32_t salt) {
    std::vector<uint8_t> bytes(kEdramBytes);
    for(size_t i=0;i<bytes.size();++i)bytes[i]=uint8_t(i*29+(i>>8)+salt*43);
    for(uint32_t y=0;y<720;++y)for(uint32_t x=0;x<1280;++x) {
        const uint32_t r=(x+y*3+salt*13)&1023,g=(x*7+y*11+salt*29)&1023;
        const uint32_t b=(x*17+y*23+salt*37)&1023,a=(x+y+salt)&3;
        const uint32_t word=r|(g<<10)|(b<<20)|(a<<30),at=PrimaryColorByteOffset(x,y);
        Check(at+4<=bytes.size(),"Color fixture primary address escaped EDRAM");
        for(unsigned k=0;k<4;++k)bytes[at+k]=uint8_t(word>>(k*8));
    }
    return bytes;
}
std::vector<uint8_t> ColorDestinationOracle(std::vector<uint8_t> prior,const std::vector<uint8_t>& packed,
        const TextureLayout& layout,const std::array<int32_t,4>& rectangle) {
    Check(layout.format==54&&layout.endian==2&&layout.levels.size()==1,"Color fixture destination oracle layout differs");
    // Shader-independent CPU tiled-address helper, plus explicit endian2 byte
    // order. Encoded UNORM10/2 texels stay on their exact grid through resolve.
    for(uint32_t y=rectangle[1];y<uint32_t(rectangle[3]);++y)
        for(uint32_t x=rectangle[0];x<uint32_t(rectangle[2]);++x) {
            const auto at=TextureBlockOffset(layout,layout.levels.front(),x,y,0);
            Check(at+4<=prior.size(),"Color fixture tiled destination escaped allocation");
            const auto word=ColorWord(packed,PrimaryColorByteOffset(x,y));
            for(unsigned b=0;b<4;++b)prior[at+b]=uint8_t(word>>(8*(3-b)));
        }
    return prior;
}
struct ColorOutcome {Outcome state;Json witnesses;};
ColorOutcome RunDirectColor(const SceneExecutorConfig& base,const Json& donor,bool enabled) {
    auto config=base;
    config.direct_packed_color_resolves=enabled;
    config.fused_atlas_depth_clear=false;config.direct_atlas_depth_resolves=false;
    config.packed_primary_stencil_clear=false;config.packed_zero_depth_clear=false;
    config.atlas_depth_region_restores=false;config.skip_full_color_clear_restore=false;
    config.pinned_null_constant_uploads=false;config.delta_constant_uploads=false;
    config.native_early_depth=false;config.native_early_depth_catalog=false;
    config.reuse_early_depth_readonly_boundaries=false;config.native_readonly_depth_attachment=false;
    config.native_readonly_depth_skip_edram_barrier=false;
    config.native_fsi=true;config.async_gpu_submissions=true;config.unified_gpu_queue=true;
    config.batch_transfers=true;config.defer_native_queries=true;
    config.max_deferred_queries=base.async_submission_capacity;
    config.present={};config.debug_raw_images=true;config.detailed_reports=true;
    const auto output=base.output/(enabled?"color-on":"color-off");config.output=output;
    Check(!std::filesystem::exists(output),"Direct color fixture output already exists");
    std::filesystem::create_directories(output);
    SceneExecutor scene(std::move(config));ColorOutcome result;auto& out=result.state;
    Check(scene.report().at("device").at("software")==false,"Direct color fixture requires real hardware");
    const uint32_t address=uint32_t(std::stoul(donor.at("destination").at("base").get<std::string>(),nullptr,16));
    Check(uint64_t(address)+kTextureBytes<=512u*1024*1024,"Direct color destination allocation is invalid");
    auto fetch=donor.at("destination_fetch").get<std::array<uint32_t,6>>();
    fetch[1]=(fetch[1]&0xfff)|address;const auto layout=DescribeTexture(fetch);
    scene.InitializeMemory(address,std::vector<uint8_t>(kTextureBytes,0xa5));
    scene.InitializeMemory(0x10000,std::vector<uint8_t>(64,0));
    auto seed=PrimaryColorSeed(0);scene.InitializeEdram(seed);uint64_t event=4000000;
    constexpr std::array<int32_t,4> full{0,0,1280,720},partial{512,320,608,384};
    const auto copy=[&](std::array<int32_t,4> r) {
        auto op=donor;op["event"]=event++;op["frame"]=1;
        op["source_rectangle"]=r;op["destination_rectangle"]=r;
        scene.Execute(op);return event-1;
    };
    const auto snapshot=[&](const char* name,uint64_t id) {
        const auto path=output/("resolve-"+std::to_string(id)+".rgba32f");
        std::ifstream file(path,std::ios::binary|std::ios::ate);
        Check(bool(file)&&file.tellg()==std::streamoff(1280u*720u*16u),"Direct color raw resolved image missing");
        std::vector<uint8_t> preview(1280u*720u*16u);file.seekg(0);
        file.read(reinterpret_cast<char*>(preview.data()),preview.size());Check(bool(file),"Direct color raw image truncated");
        out.snapshots.emplace(name,Snapshot{scene.ReadFullEdram(),scene.ReadPhysical(address,kTextureBytes),std::move(preview)});
    };
    const auto primary_restores=[&] {
        uint64_t count=0;
        for(const auto& row:scene.report().value("surface_alias_restores",Json::array())) {
            const auto& s=row.at("identity");
            count+=s.at("depth")==false&&s.at("width")==1280&&s.at("height")==720&&
                s.at("edram_base")==720&&s.at("format")==2;
        }
        return count;
    };
    const auto clear=[&](std::array<int32_t,4> r,std::array<float,4> color,const Json* surfaces=nullptr) {
        scene.Execute(Json{{"type","clearrect"},{"frame",1},{"event",event++},
            {"surfaces",surfaces?*surfaces:donor.at("surfaces")},{"rectangle",r},
            {"depth",0.f},{"flags",1},{"stencil",0},{"color",color}});
    };
    const auto query=[&](bool end) {
        scene.Execute(Json{{"type",end?"query_end":"query_begin"},{"query_generation",1},
            {"query_handle",0x9988},{"report_address",end?0x10000:0x10020},{"frame",1},{"event",event++}});
    };
    const auto expect_copy=[&](const std::vector<uint8_t>& prior,const std::vector<uint8_t>& source,
                               const std::array<int32_t,4>& r) {
        const auto actual=scene.ReadPhysical(address,kTextureBytes);
        Check(actual==ColorDestinationOracle(prior,source,layout,r),
            "Direct color actual full4MiB destination or untouched bytes differ from CPU address/word oracle");
        Check(scene.ReadFullEdram()==source,"Read-only color resolve changed complete10MiB EDRAM");
    };
    // 1. Absent host: compare full destination and padding, not only a preview.
    auto before=primary_restores();auto id=copy(full);
    Check(primary_restores()-before==(enabled?0u:1u),"Absent color source did not bypass the identity host copy");
    expect_copy(std::vector<uint8_t>(kTextureBytes,0xa5),seed,full);
    snapshot("absent-full-resolve",id);
    // 2. Same packed version, but poison the physical destination so its
    // original partial alias repair and actual96x64 store are both observable.
    const std::vector<uint8_t> poison(kTextureBytes,0x3c);scene.InitializeMemory(address,poison);
    before=primary_restores();id=copy(partial);Check(primary_restores()==before,"Same-version copy unexpectedly restored a host");
    expect_copy(poison,seed,partial);
    Check(scene.ReadPhysical(address,kTextureBytes)!=poison,"Partial destination write witness is vacuous");
    snapshot("same-version-partial-destination-repair",id);
    // 3. Real new packed bytes make the old host stale; outside the partial
    // destination keeps the previous allocation contents byte-for-byte.
    auto prior=scene.ReadPhysical(address,kTextureBytes);seed=PrimaryColorSeed(7);scene.InitializeEdram(seed);
    before=primary_restores();id=copy(partial);
    Check(primary_restores()-before==(enabled?0u:1u),"Changed source version did not exercise the identity-copy bypass");
    expect_copy(prior,seed,partial);
    Check(scene.ReadPhysical(address,kTextureBytes)!=prior,"Changed source partial write witness is vacuous");
    snapshot("changed-version-partial-untouched-destination",id);
    // 4. An excluded rectangle is an actual future generic Surface consumer.
    // Candidate must materialize its whole stale image before the next current
    // host resolve, which must refuse the optional source route.
    before=primary_restores();(void)copy({41,17,179,91});
    Check(primary_restores()-before==(enabled?1u:0u),"Future generic color consumer did not materialize the stale host");
    id=copy(full);snapshot("future-generic-consumer-then-current-host",id);
    // 5. EDRAM stores round ties up, but the actual color resolve rounds even.
    // Preserving a current pre-encode host float is therefore consequential:
    // source R/alpha code1 MUST yield destination R/alpha code0 here.
    const float half_rgb=0.5f/1023.f,half_alpha=0.5f/3.f;
    clear(partial,{half_rgb,0.25f,0.75f,half_alpha});
    const auto packed=scene.ReadFullEdram();const auto source_word=ColorWord(packed,PrimaryColorByteOffset(512,320));
    Check((source_word&1023u)==1u&&(source_word>>30)==1u,"Actual host tie did not produce the required packed half-up witness");
    id=copy(full);snapshot("current-host-halfway-rounding-refusal",id);
    const auto& tie=out.snapshots.at("current-host-halfway-rounding-refusal");
    const auto destination_word=ColorWord(tie.texture,TextureBlockOffset(layout,layout.levels.front(),512,320,0),true);
    const auto pixel=(size_t(320)*1280+512)*16;
    Check((destination_word&1023u)==0u&&(destination_word>>30)==0u&&
        ColorWord(tie.preview,pixel)==0u&&ColorWord(tie.preview,pixel+12)==0u,
        "Current host tie was replaced by its differently quantized packed source");
    result.witnesses={{"half_rgb_bits",std::bit_cast<uint32_t>(half_rgb)},
        {"half_alpha_bits",std::bit_cast<uint32_t>(half_alpha)},
        {"source_packed_red",source_word&1023u},{"source_packed_alpha",source_word>>30},
        {"destination_red",destination_word&1023u},{"destination_alpha",destination_word>>30}};
    // 6. A real differently interpreted color write commits to the same tiles
    // and makes the primary color view stale. Public Execute commits at scope
    // tail, so this is alias/FIFO proof, not dirty-at-admission injection.
    Json alias=donor.at("surfaces");auto color=alias.at("6");
    color["format"]=0;color["format_info"]=0;alias["6"]=color;
    const auto before_alias=scene.ReadFullEdram();clear(partial,{0.1f,0.2f,0.3f,0.4f},&alias);
    const auto after_alias=scene.ReadFullEdram();Check(after_alias!=before_alias,"Color alias write witness is vacuous");
    before=primary_restores();prior=scene.ReadPhysical(address,kTextureBytes);id=copy(full);
    Check(primary_restores()-before==(enabled?0u:1u),"Committed color alias did not make the primary host stale");
    expect_copy(prior,after_alias,full);snapshot("overlapping-format0-alias-commit",id);
    // 7. Active query must use original Surface/resolve, then retire its actual
    // empty query result. The wrap is a refusal witness, not positive samples.
    seed=PrimaryColorSeed(13);scene.InitializeEdram(seed);query(false);id=copy(full);query(true);
    scene.FlushGpuWork();for(const auto& q:scene.TakeCompletedQueries())out.queries.push_back(Json::array({
        q.generation,q.frame,q.end_event,q.result.handle,q.result.begin_address,q.result.end_address,q.result.draws,q.result.samples}));
    snapshot("active-query-original-resolve",id);
    // 8. A later generic COLOR CLEAR must materialize the stale source before
    // editing its tiny rectangle; a generic read observes that actual result.
    seed=PrimaryColorSeed(17);scene.InitializeEdram(seed);(void)copy(full);
    Check(scene.ReadFullEdram()==seed,"Read-only last color resolve mutated EDRAM");
    before=primary_restores();clear({79,15,82,18},{0.25f,0.5f,0.75f,1.f});
    Check(primary_restores()-before==(enabled?1u:0u),"Future color clear failed to materialize stale source");
    Check(scene.ReadFullEdram()!=seed,"Future color clear witness is vacuous");
    id=copy({0,0,128,128});snapshot("stale-resolve-then-generic-color-clear",id);
    scene.FlushGpuWork();out.report=scene.report();
    const auto& queue=out.report.at("gpu_work_queue");
    Check(queue.at("accepted_work")==queue.at("completed_work"),"Direct color fixture work did not retire");
    Check(out.queries.size()==1&&out.queries[0][0]==1&&out.queries[0][1]==1&&
        out.queries[0][3]==0x9988&&out.queries[0][4]==0x10020&&out.queries[0][5]==0x10000&&
        out.queries[0][6]==0&&out.queries[0][7]==0,"Direct color query identity or empty sample result differs");
    Check(primary_restores()==(enabled?3u:5u),"Direct color fixture actual primary restore count differs");
    Check(out.report.value("texture_alias_restores",Json::array()).size()>=1,"Partial destination alias repair was not exercised");
    if(enabled) {
        const auto& s=out.report.at("direct_packed_color_resolves");
        Check(s.at("configured")==true&&s.at("candidates")==8&&s.at("selected")==5&&s.at("packed_sources")==5&&
            s.at("current_host_refusals")==2&&s.at("query_refusals")==1&&s.at("provenance_refusals")==0&&
            s.at("source_pixels")==uint64_t(5)*1280*720&&s.at("resolve_pixels")==uint64_t(3)*1280*720+uint64_t(2)*96*64&&
            out.report.at("direct_packed_color_resolve_events").size()==5,
            "Direct color fixture did not reach the exact source/refusal sequence");
    } else Check(!out.report.contains("direct_packed_color_resolves"),"Disabled direct color path emitted an active report");
    return result;
}


uint32_t SecondaryDepthByteOffset(uint32_t x,uint32_t y) {
    return ((1440+(y/16)*8+x/80)*1280+(y%16)*80+(x%80+40)%80)*4;
}
std::vector<uint8_t> SecondaryStencilSeed(uint32_t salt) {
    auto result=Seed(salt);
    constexpr std::array<uint32_t,12> depths{
        0,1,0xfffff,0x100000,0x100001,0x7fffff,0x800000,0xefffff,
        0xf00000,0xf00001,0xfffffe,0xffffff};
    for(uint32_t y=0;y<360;++y)for(uint32_t x=0;x<640;++x) {
        const uint32_t word=(depths[(x+y*3+salt)%depths.size()]<<8)|((x*17+y*29+salt)&255);
        const uint32_t at=SecondaryDepthByteOffset(x,y);
        Check(at+4<=result.size(),"Secondary stencil seed escaped EDRAM");
        for(unsigned b=0;b<4;++b)result[at+b]=uint8_t(word>>(b*8));
    }
    return result;
}
std::vector<uint8_t> SecondaryStencilOracle(std::vector<uint8_t> bytes,
        const std::array<int32_t,4>& rectangle,uint32_t stencil) {
    for(uint32_t y=0;y<360;++y)for(uint32_t x=0;x<640;++x) {
        const auto at=SecondaryDepthByteOffset(x,y);uint32_t old=0;
        for(unsigned b=0;b<4;++b)old|=uint32_t(bytes[at+b])<<(8*b);
        const bool inside=int32_t(x)>=rectangle[0]&&int32_t(x)<rectangle[2]&&
            int32_t(y)>=rectangle[1]&&int32_t(y)<rectangle[3];
        const auto word=(std::min(old>>8,0xf00000u)<<8)|(inside?(stencil&255u):(old&255u));
        for(unsigned b=0;b<4;++b)bytes[at+b]=uint8_t(word>>(8*b));
    }
    return bytes;
}
struct SecondarySnapshot {Snapshot depth;std::vector<uint8_t> atlas_texture,atlas_preview;};
struct SecondaryOutcome {std::map<std::string,SecondarySnapshot> snapshots;Json report,queries;};
SecondaryOutcome RunSecondaryStencil(const SceneExecutorConfig& base,const Json& secondary_clear,
        const Json& secondary_resolve,const Json& atlas_clear,const Json& atlas_resolve,
        bool enabled,bool stencil_export) {
    auto config=base;
    config.packed_secondary_stencil_clear=enabled;config.packed_primary_stencil_clear=true;
    config.fused_atlas_depth_clear=true;config.direct_atlas_depth_resolves=true;
    config.direct_packed_color_resolves=false;config.packed_zero_depth_clear=false;
    config.atlas_depth_region_restores=false;config.pinned_null_constant_uploads=false;
    config.delta_constant_uploads=false;config.native_early_depth=false;config.native_early_depth_catalog=false;
    config.reuse_early_depth_readonly_boundaries=false;config.native_readonly_depth_attachment=false;
    config.native_readonly_depth_skip_edram_barrier=false;
    config.native_fsi=true;config.async_gpu_submissions=true;config.unified_gpu_queue=true;
    config.batch_transfers=true;config.defer_native_queries=true;
    config.max_deferred_queries=base.async_submission_capacity;
    config.restore_depth_stencil_export=stencil_export;
    config.present={};config.debug_raw_images=true;config.detailed_reports=true;
    const auto output=base.output/(std::string(stencil_export?"export-":"portable-")+(enabled?"on":"off"));
    config.output=output;Check(!std::filesystem::exists(output),"Secondary stencil fixture output already exists");
    std::filesystem::create_directories(output);SceneExecutor scene(std::move(config));SecondaryOutcome out;
    Check(scene.report().at("device").at("software")==false,"Secondary stencil fixture requires hardware");
    if(stencil_export)Check(scene.report().at("depth_stencil_restores").at("extension_enabled")==true,
        "Secondary stencil fixture requires actual stencil export in its export arm");
    const auto address_of=[](const Json& op){return uint32_t(std::stoul(op.at("destination").at("base").get<std::string>(),nullptr,16));};
    const uint32_t depth_address=address_of(secondary_resolve),atlas_address=address_of(atlas_resolve);
    Check(uint64_t(depth_address)+kTextureBytes<=512u*1024*1024&&uint64_t(atlas_address)+kTextureBytes<=512u*1024*1024&&
        (uint64_t(depth_address)+kTextureBytes<=atlas_address||uint64_t(atlas_address)+kTextureBytes<=depth_address),
        "Secondary and atlas fixture destinations overlap or escape memory");
    scene.InitializeMemory(depth_address,std::vector<uint8_t>(kTextureBytes,0xa5));
    scene.InitializeMemory(atlas_address,std::vector<uint8_t>(kTextureBytes,0x5a));
    scene.InitializeMemory(0x10000,std::vector<uint8_t>(64,0));
    auto seed=SecondaryStencilSeed(0);scene.InitializeEdram(seed);uint64_t event=5000000;
    const auto clear=[&](std::array<int32_t,4> rectangle,uint32_t stencil,uint32_t flags=32,float depth=0.f) {
        auto op=secondary_clear;op["frame"]=1;op["event"]=event++;op["rectangle"]=rectangle;
        op["stencil"]=stencil;op["flags"]=flags;op["depth"]=depth;scene.Execute(op);
    };
    const auto copy=[&](const Json& donor,uint32_t width,uint32_t height) {
        auto op=donor;op["frame"]=1;op["event"]=event++;
        op["source_rectangle"]=std::array{0,0,int32_t(width),int32_t(height)};
        op["destination_rectangle"]=std::array{0,0,int32_t(width),int32_t(height)};
        scene.Execute(op);return event-1;
    };
    const auto raw=[&](uint64_t id,uint32_t width,uint32_t height) {
        const auto path=output/("resolve-"+std::to_string(id)+".rgba32f");
        std::ifstream file(path,std::ios::binary|std::ios::ate);
        const size_t size=size_t(width)*height*16;
        Check(bool(file)&&file.tellg()==std::streamoff(size),"Secondary fixture raw resolved image missing");
        std::vector<uint8_t> bytes(size);file.seekg(0);file.read(reinterpret_cast<char*>(bytes.data()),size);
        Check(bool(file),"Secondary fixture raw image truncated");return bytes;
    };
    const auto snapshot=[&](const char* name,bool with_atlas=false) {
        auto depth_preview=raw(copy(secondary_resolve,640,360),640,360);
        std::vector<uint8_t> atlas_preview,atlas_texture;
        if(with_atlas) {
            atlas_preview=raw(copy(atlas_resolve,1120,688),1120,688);
            atlas_texture=scene.ReadPhysical(atlas_address,kTextureBytes);
        }
        out.snapshots.emplace(name,SecondarySnapshot{
            {scene.ReadFullEdram(),scene.ReadPhysical(depth_address,kTextureBytes),std::move(depth_preview)},
            std::move(atlas_texture),std::move(atlas_preview)});
    };
    const auto restores=[&]{return scene.report().at("depth_stencil_restores").at("calls").get<uint64_t>();};
    const auto expect_stale=[&](uint64_t previous){Check(restores()-previous==(enabled?0u:1u),
        "Secondary stale clear did not select original restore versus packed bypass");};
    const auto oracle=[&](const std::vector<uint8_t>& prior,std::array<int32_t,4> rectangle,uint32_t stencil) {
        const auto actual=scene.ReadFullEdram();
        Check(actual==SecondaryStencilOracle(prior,rectangle,stencil),
            "Secondary full10MiB normalization/stencil/outside oracle differs");
        // Last8 rows are allocated in the final tile row but OUTSIDE640x360.
        // Test actual bytes, including potentially live UNORM24 atlas aliases.
        for(uint32_t y=360;y<368;++y)for(uint32_t x=0;x<640;++x) {
            const auto at=SecondaryDepthByteOffset(x,y);
            Check(std::equal(prior.begin()+at,prior.begin()+at+4,actual.begin()+at),
                "Secondary clear overwrote padded bottom rows or an aliased atlas texel");
        }
        return actual;
    };
    const auto query=[&](bool end){scene.Execute(Json{{"type",end?"query_end":"query_begin"},
        {"query_generation",1},{"query_handle",0xaa99},{"report_address",end?0x10000:0x10020},
        {"frame",1},{"event",event++}});};
    // 1. Absent host, full clear, then actual future D32 consumer.
    auto before=restores();clear({0,0,640,360},0xa5);expect_stale(before);
    (void)oracle(seed,{0,0,640,360},0xa5);snapshot("absent-full-clear-future-consumer");
    // 2. Restore can make a host CURRENT while packed depth still exceeds1.
    // Such state and a later noncanonical host depth both keep the image path.
    seed=SecondaryStencilSeed(3);scene.InitializeEdram(seed);(void)copy(secondary_resolve,640,360);
    Check(scene.ReadFullEdram()==seed,"Secondary host read unexpectedly normalized packed source");
    clear({41,17,179,91},0x5a);(void)oracle(seed,{41,17,179,91},0x5a);
    clear({300,200,360,260},0,16,std::bit_cast<float>(0x3e800003u));
    clear({300,200,360,260},0xc7);snapshot("current-host-above-one-and-noncanonical-refusal");
    // 3. Changed source version; rectangle straddles the last logical tile row.
    seed=SecondaryStencilSeed(7);scene.InitializeEdram(seed);before=restores();
    clear({579,351,640,360},0x7b);expect_stale(before);
    (void)oracle(seed,{579,351,640,360},0x7b);snapshot("changed-version-bottom-partial-padding");
    // 4. Candidate leaves its host stale across two clears before a consumer.
    seed=SecondaryStencilSeed(11);scene.InitializeEdram(seed);before=restores();
    clear({79,15,82,18},0x11);clear({0,352,640,360},0xef);expect_stale(before);
    (void)oracle(SecondaryStencilOracle(seed,{79,15,82,18},0x11),{0,352,640,360},0xef);
    snapshot("repeated-stale-clears-before-consumer");
    // 5. Query guard retains the actual original image operation and retirement.
    seed=SecondaryStencilSeed(13);scene.InitializeEdram(seed);query(false);
    clear({0,0,640,360},0x49);query(true);scene.FlushGpuWork();
    for(const auto& q:scene.TakeCompletedQueries())out.queries.push_back(Json::array({
        q.generation,q.frame,q.end_event,q.result.handle,q.result.begin_address,q.result.end_address,q.result.draws,q.result.samples}));
    (void)oracle(seed,{0,0,640,360},0x49);snapshot("active-query-original-clear");
    // 6. A REAL fused UNORM24 receipt aliases this FP24 surface atbase1440.
    // Updating secondary packed bytes must invalidate that receipt by version;
    // the later atlas resolve must decode NEW bytes instead of using it.
    scene.InitializeEdram(SecondaryStencilSeed(17));auto atlas_op=atlas_clear;
    atlas_op["frame"]=1;atlas_op["event"]=event++;atlas_op["rectangle"]=std::array{0,0,74,74};
    atlas_op["depth"]=1.f;atlas_op["stencil"]=0;scene.Execute(atlas_op);
    Check(scene.report().at("fused_atlas_depth_clear").at("selected")==1&&
        scene.report().at("fused_atlas_depth_clear").at("retained_receipts")==1,
        "Secondary alias fixture failed to create an actual pending atlas receipt");
    const auto after_atlas=scene.ReadFullEdram();before=restores();clear({0,0,640,360},0x96);expect_stale(before);
    (void)oracle(after_atlas,{0,0,640,360},0x96);snapshot("atlas-receipt-invalidated-by-secondary-clear",true);
    scene.FlushGpuWork();out.report=scene.report();
    const auto& fused=out.report.at("fused_atlas_depth_clear");const auto& direct=out.report.at("direct_atlas_depth_resolves");
    Check(fused.at("selected")==1&&fused.at("packed_sources")==1&&fused.at("host_sources")==0&&
        fused.at("deferred_host_discards")==1&&fused.at("deferred_host_materializations")==0&&fused.at("retained_receipts")==0&&
        direct.at("selected")==1&&direct.at("packed_sources")==1&&direct.at("receipt_sources")==0,
        "Secondary alias reused stale atlas receipt or failed to decode its new packed version");
    Check(out.report.at("packed_primary_stencil_clear").at("candidates")==0,
        "Secondary fixture unexpectedly exercised primary stencil candidate");
    Check(out.report.at("gpu_work_queue").at("accepted_work")==out.report.at("gpu_work_queue").at("completed_work"),
        "Secondary fixture accepted work did not retire");
    Check(out.queries.size()==1&&out.queries[0][0]==1&&out.queries[0][1]==1&&
        out.queries[0][3]==0xaa99&&out.queries[0][4]==0x10020&&out.queries[0][5]==0x10000&&
        out.queries[0][6]==0&&out.queries[0][7]==0,"Secondary fixture query identity/empty samples did not retire");
    if(enabled) {
        const auto& s=out.report.at("packed_secondary_stencil_clear");
        Check(s.at("configured")==true&&s.at("candidates")==8&&s.at("selected")==5&&s.at("packed_sources")==5&&
            s.at("current_host_refusals")==2&&s.at("query_refusals")==1&&s.at("provenance_refusals")==0&&
            s.at("resource_refusals")==0&&s.at("source_pixels")==uint64_t(5)*640*360&&s.at("clear_pixels")==466478,
            "Secondary fixture source/refusal/pixel sequence differs");
    } else Check(!out.report.contains("packed_secondary_stencil_clear"),"Disabled secondary path emitted active report");
    return out;
}

} // namespace

void RunNativeFusedAtlasDepthClearSceneFixture(const SceneExecutorConfig& base,const Json& original) {
    Check(base.async_submission_capacity==64||base.async_submission_capacity==128,
        "Fixture requires capacity64 or128");
    Json donor;
    for(const auto& op:original.at("operations"))if(op.at("type")=="resolve"&&
        op.at("source_slot")==4&&op.at("surfaces").at("10").at("width")==1120&&
        op.at("surfaces").at("10").at("height")==688&&
        op.at("surfaces").at("10").at("edram_base")==1440&&
        op.at("surfaces").at("10").at("format")==0&&
        op.at("destination").at("format")==22) {donor=op;break;}
    Check(!donor.is_null(),"Fixture requires a captured canonical atlas resolve identity");
    Json results=Json::array();
    for(bool stencil_export:{false,true}) {
        auto off=Run(base,donor,false,stencil_export);
        auto on=Run(base,donor,true,stencil_export);
        Check(off.snapshots.size()==6&&on.snapshots.size()==6,"Fixture snapshot coverage changed");
        Check(off.queries==on.queries,"Fixture actual query identities or results changed");
        Json rows=Json::array();
        for(const auto& [name,a]:off.snapshots) {
            const auto& b=on.snapshots.at(name);
            Check(a.edram==b.edram&&a.texture==b.texture,"Fused Scene changed depth/stencil/history or resolved texture");
            rows.push_back({{"case",name},{"edram_bytes",a.edram.size()},
                {"physical_texture_bytes",a.texture.size()},{"edram_sha256",ContentHash(a.edram)},
                {"physical_texture_sha256",ContentHash(a.texture)}});
        }
        results.push_back({{"stencil_export_requested",stencil_export},{"cases",rows},
            {"queries",on.queries},{"off_report",off.report},{"on_report",on.report}});
    }
    std::ofstream out(base.output/"fused-atlas-scene-fixture.json");
    out<<Json({{"passed",true},{"capacity",base.async_submission_capacity},{"pairs",results},
        {"scope","Actual Scene cached-host receipt consumption/invalidation and Native fallback; full10MiB plus complete4MiB destination allocation, both stencil paths. Synthetic initial history is explicit; no FPS claim."}}).dump(2)<<'\n';
    Check(bool(out),"Cannot write controlled fused Scene proof");
}
void RunNativeDirectAtlasDepthResolveSceneFixture(const SceneExecutorConfig& base,const Json& original) {
    Check(base.async_submission_capacity==64||base.async_submission_capacity==128,"Direct fixture capacity invalid");
    Json donor;
    for(const auto& op:original.at("operations"))if(op.at("type")=="resolve"&&
        op.at("source_slot")==4&&op.at("surfaces").at("10").at("width")==1120&&
        op.at("surfaces").at("10").at("height")==688&&op.at("surfaces").at("10").at("edram_base")==1440&&
        op.at("surfaces").at("10").at("format")==0&&op.at("destination").at("format")==22) {donor=op;break;}
    Check(!donor.is_null(),"Direct fixture requires a captured atlas resolve");Json results=Json::array();
    for(bool stencil_export:{false,true}) {
        auto off=RunDirect(base,donor,false,stencil_export),on=RunDirect(base,donor,true,stencil_export);
        Check(off.snapshots.size()==8&&on.snapshots.size()==8&&off.queries==on.queries,"Direct fixture coverage/query mismatch");
        Json rows=Json::array();
        for(const auto& [name,a]:off.snapshots) {
            const auto& b=on.snapshots.at(name);
            Check(a.edram==b.edram&&a.texture==b.texture&&a.preview==b.preview,
                "Direct atlas resolve changed full packed memory or exact pre-store image");
            rows.push_back({{"case",name},{"edram_bytes",a.edram.size()},{"physical_texture_bytes",a.texture.size()},
                {"resolved_image_bytes",a.preview.size()},{"edram_sha256",ContentHash(a.edram)},
                {"physical_texture_sha256",ContentHash(a.texture)},{"resolved_image_sha256",ContentHash(a.preview)}});
        }
        results.push_back({{"stencil_export_requested",stencil_export},{"cases",rows},{"queries",on.queries},
            {"off_report",off.report},{"on_report",on.report}});
    }
    std::ofstream out(base.output/"direct-atlas-scene-fixture.json");
    out<<Json({{"passed",true},{"capacity",base.async_submission_capacity},{"pairs",results},
        {"scope","Both flags OFF versus ON; eight small atlas-copy future-use cases, complete EDRAM, destination allocation, and exact pre-store RGBA image. Explicit synthetic history; no FPS claim."}}).dump(2)<<'\n';
    Check(bool(out),"Cannot write direct atlas Scene proof");
}

void RunNativePackedPrimaryStencilClearSceneFixture(const SceneExecutorConfig& base,const Json& original) {
    Check(base.async_submission_capacity==64||base.async_submission_capacity==128,"Primary stencil fixture capacity invalid");
    Json donor;
    for(const auto& op:original.at("operations"))if(op.at("type")=="resolve"&&op.at("source_slot")==4) {
        const auto& surface=op.at("surfaces").at("10");
        if(surface.at("width")==1280&&surface.at("height")==720&&surface.at("edram_base")==0&&
           surface.at("format")==1&&(surface.at("surface_info").get<uint32_t>()&16383u)==1280&&
           !(surface.at("surface_info").get<uint32_t>()&0x30000u)&&op.at("destination").at("format")==23&&
           op.at("destination").at("width")==1280&&op.at("destination").at("height")==720&&
           op.at("destination").at("pitch")==1280&&op.at("flags")==4&&op.at("exponent_bias")==0&&
           op.at("level")==0&&op.at("copy_dest_swap")==false) {donor=op;break;}
    }
    Check(!donor.is_null(),"Primary stencil fixture requires captured canonical primary FP24 resolve");
    Json results=Json::array();
    for(bool stencil_export:{false,true}) {
        auto off=RunPrimaryStencil(base,donor,false,stencil_export),on=RunPrimaryStencil(base,donor,true,stencil_export);
        Check(off.snapshots.size()==8&&on.snapshots.size()==8&&off.queries==on.queries,
            "Primary stencil paired case/query coverage differs");
        Check(off.report.at("device")==on.report.at("device"),"Primary stencil paired device identity differs");
        Json rows=Json::array();
        for(const auto& [name,a]:off.snapshots) {
            const auto& b=on.snapshots.at(name);
            Check(a.edram==b.edram&&a.texture==b.texture&&a.preview==b.preview,
                "Primary stencil Scene changed full packed/physical memory or future D32-consumer resolve image");
            rows.push_back({{"case",name},{"exact",true},{"edram_bytes",a.edram.size()},
                {"physical_texture_bytes",a.texture.size()},{"resolved_image_bytes",a.preview.size()},
                {"edram_sha256",ContentHash(a.edram)},{"physical_texture_sha256",ContentHash(a.texture)},
                {"resolved_image_sha256",ContentHash(a.preview)}});
        }
        results.push_back({{"stencil_export_requested",stencil_export},{"cases",rows},{"queries",on.queries},
            {"off_report",off.report},{"on_report",on.report}});
    }
    std::ofstream out(base.output/"packed-primary-stencil-scene-fixture.json");
    out<<Json({{"schema",1},{"passed",true},{"scene_orchestration_exercised",true},
        {"capacity",base.async_submission_capacity},{"pairs",results},
        {"noncanonical_host_clear_bits",0x3e800003u},
        {"scope","Eight paired actual Scene cases; complete10MiB and4MiB destination plus actual future Surface/D32-consumer resolve image. Synthetic initial history explicit; both legacy/export restoration arms; no FPS claim."},
        {"qualification","Resolved images observe the original depth resolve quantizer, not raw Scene D32 bits; exhaustive primitive fixture separately proves cached/future raw D32 and S8 equality. Public Execute commits dirty host images before the next operation, so overlap case proves actual alias commit/FIFO, not dirty-at-admission injection. All work is fenced; pending-owner destruction scope is separate."}}).dump(2)<<'\n';
    Check(bool(out),"Cannot write packed primary stencil Scene proof");
}

void RunNativeDirectPackedColorResolveSceneFixture(const SceneExecutorConfig& base,const Json& original) {
    Check(base.async_submission_capacity==64||base.async_submission_capacity==128,"Direct color fixture capacity invalid");
    Json donor;
    for(const auto& op:original.at("operations"))if(op.at("type")=="resolve"&&op.at("source_slot")==0) {
        const auto& s=op.at("surfaces").at("6");const auto& d=op.at("destination");
        if(s.at("depth")==false&&s.at("width")==1280&&s.at("height")==720&&
           s.at("edram_base")==720&&s.at("format")==2&&(s.at("surface_info").get<uint32_t>()&16383u)==1280&&
           !(s.at("surface_info").get<uint32_t>()&0x30000u)&&d.at("format")==7&&
           d.at("width")==1280&&d.at("height")==720&&d.at("pitch")==1280&&d.at("tiled")==true&&
           d.at("endian")==2&&d.at("dimension")==1&&d.at("layers")==1&&
           op.at("flags")==0&&op.at("exponent_bias")==0&&op.at("level")==0&&op.at("copy_dest_swap")==false) {
            donor=op;break;
        }
    }
    Check(!donor.is_null(),"Direct color fixture requires captured primary UNORM10 resolve");
    const auto off=RunDirectColor(base,donor,false),on=RunDirectColor(base,donor,true);
    Check(off.state.snapshots.size()==8&&on.state.snapshots.size()==8&&off.state.queries==on.state.queries,
        "Direct color fixture paired coverage or query identity differs");
    Check(off.state.report.at("device")==on.state.report.at("device"),"Direct color fixture device differs");
    Check(off.witnesses==on.witnesses,"Consequential current-host rounding witness differs");
    Json rows=Json::array();
    for(const auto& [name,a]:off.state.snapshots) {
        const auto& b=on.state.snapshots.at(name);
        Check(a.edram==b.edram&&a.texture==b.texture&&a.preview==b.preview,
            "Direct packed color changed full EDRAM, full destination including untouched bytes, or raw resolved image");
        rows.push_back({{"case",name},{"exact",true},{"edram_bytes",a.edram.size()},
            {"physical_texture_bytes",a.texture.size()},{"resolved_image_bytes",a.preview.size()},
            {"edram_sha256",ContentHash(a.edram)},{"physical_texture_sha256",ContentHash(a.texture)},
            {"resolved_image_sha256",ContentHash(a.preview)}});
    }
    std::ofstream file(base.output/"direct-packed-color-scene-fixture.json");
    file<<Json({{"schema",1},{"passed",true},{"scene_orchestration_exercised",true},
        {"capacity",base.async_submission_capacity},{"cases",rows},{"queries",on.state.queries},
        {"current_host_rounding_witness",on.witnesses},{"off_report",off.state.report},{"on_report",on.state.report},
        {"scope","Eight paired actual Scene cases; complete10MiB EDRAM, complete4MiB physical destination and rawRGBA32F resolve image. Full and96x64 partial stores independently checked including untouched bytes; actual current-host half-up versus nearest-even witness."},
        {"qualification","Explicit synthetic initial history. Generic color clear/read materializes the unchanged source path; no raw Scene image access is invented. Empty query proves active-query refusal/retirement, not positive occlusion samples. Public Execute commits aliases at scope tail; no dirty-at-admission injection. Every snapshot waits for GPU completion; raw-copy/pending-resource equivalence is a separate primitive proof. No timing or default-enable claim."}}).dump(2)<<'\n';
    Check(bool(file),"Cannot write direct packed color Scene proof");
}

void RunNativePackedSecondaryStencilClearSceneFixture(const SceneExecutorConfig& base,const Json& original) {
    Check(base.async_submission_capacity==64||base.async_submission_capacity==128,"Secondary fixture capacity invalid");
    Json secondary_clear,atlas_clear,depth_resolve,atlas_resolve;
    for(const auto& op:original.at("operations")) {
        if(!op.contains("surfaces")||!op.at("surfaces").contains("10"))continue;
        const auto& s=op.at("surfaces").at("10");
        if(op.at("type")=="clearrect"&&s.at("depth")==true) {
            if(op.at("flags")==32&&s.at("width")==640&&s.at("height")==360&&s.at("edram_base")==1440&&
               s.at("format")==1&&(s.at("surface_info").get<uint32_t>()&0x33fffu)==640&&secondary_clear.is_null())secondary_clear=op;
            if(op.at("flags")==16&&op.at("depth")==1.f&&s.at("width")==1120&&s.at("height")==688&&
               s.at("edram_base")==1440&&s.at("format")==0&&atlas_clear.is_null())atlas_clear=op;
        }
        if(op.at("type")=="resolve"&&op.at("source_slot")==4&&op.at("flags")==4&&op.at("level")==0&&
           op.at("exponent_bias")==0&&op.at("copy_dest_swap")==false) {
            if(s.at("width")==1280&&s.at("height")==720&&s.at("format")==1&&op.at("destination").at("format")==23&&depth_resolve.is_null())depth_resolve=op;
            if(s.at("width")==1120&&s.at("height")==688&&s.at("edram_base")==1440&&s.at("format")==0&&
               op.at("destination").at("format")==22&&atlas_resolve.is_null())atlas_resolve=op;
        }
    }
    Check(!secondary_clear.is_null()&&!atlas_clear.is_null()&&!depth_resolve.is_null()&&!atlas_resolve.is_null(),
        "Secondary fixture requires captured secondary/atlas clears and canonical depth resolve donors");
    // Deliberately constructed future consumer: reuse the actual format23
    // resolve semantics with the captured secondary attachment and640x360
    // destination dimensions/pitch. Do not mutate any original capture object.
    auto secondary_resolve=depth_resolve;secondary_resolve["surfaces"]=secondary_clear.at("surfaces");
    auto& destination=secondary_resolve["destination"];destination["width"]=640;destination["height"]=360;destination["pitch"]=640;
    auto fetch=secondary_resolve.at("destination_fetch").get<std::array<uint32_t,6>>();
    fetch[0]=(fetch[0]&~(511u<<22))|(20u<<22);
    fetch[2]=(fetch[2]&~(8191u|(8191u<<13)))|639u|(359u<<13);
    secondary_resolve["destination_fetch"]=fetch;
    const auto layout=DescribeTexture(fetch);
    Check(layout.width==640&&layout.height==360&&layout.format==23&&layout.endian==2&&layout.levels.size()==1&&
        layout.levels.front().pitch_blocks==640,"Constructed secondary future-consumer layout differs");
    Json pairs=Json::array();
    for(bool stencil_export:{false,true}) {
        const auto off=RunSecondaryStencil(base,secondary_clear,secondary_resolve,atlas_clear,atlas_resolve,false,stencil_export);
        const auto on=RunSecondaryStencil(base,secondary_clear,secondary_resolve,atlas_clear,atlas_resolve,true,stencil_export);
        Check(off.snapshots.size()==6&&on.snapshots.size()==6&&off.queries==on.queries,"Secondary fixture case/query coverage differs");
        Check(off.report.at("device")==on.report.at("device"),"Secondary fixture actual devices differ");
        Json rows=Json::array();
        for(const auto& [name,a]:off.snapshots) {
            const auto& b=on.snapshots.at(name);
            Check(a.depth.edram==b.depth.edram&&a.depth.texture==b.depth.texture&&a.depth.preview==b.depth.preview&&
                a.atlas_texture==b.atlas_texture&&a.atlas_preview==b.atlas_preview,
                "Secondary stencil changed full EDRAM/padding, physical destination or future secondary/atlas resolved image");
            Json row{{"case",name},{"exact",true},{"edram_bytes",a.depth.edram.size()},
                {"physical_texture_bytes",a.depth.texture.size()},{"resolved_image_bytes",a.depth.preview.size()},
                {"edram_sha256",ContentHash(a.depth.edram)},{"physical_texture_sha256",ContentHash(a.depth.texture)},
                {"resolved_image_sha256",ContentHash(a.depth.preview)}};
            if(!a.atlas_texture.empty()) {
                row["atlas_physical_texture_bytes"]=a.atlas_texture.size();row["atlas_resolved_image_bytes"]=a.atlas_preview.size();
                row["atlas_physical_texture_sha256"]=ContentHash(a.atlas_texture);row["atlas_resolved_image_sha256"]=ContentHash(a.atlas_preview);
            }
            rows.push_back(std::move(row));
        }
        pairs.push_back({{"stencil_export_requested",stencil_export},{"cases",rows},{"queries",on.queries},
            {"off_report",off.report},{"on_report",on.report}});
    }
    std::ofstream file(base.output/"packed-secondary-stencil-scene-fixture.json");
    file<<Json({{"schema",1},{"passed",true},{"scene_orchestration_exercised",true},
        {"capacity",base.async_submission_capacity},{"pairs",pairs},{"noncanonical_host_clear_bits",0x3e800003u},
        {"logical_extent",{640,360}},{"preserved_bottom_padding_words",5120},
        {"scope","Six paired actualScene cases in portable/export arms. Full10MiB EDRAM,4MiB secondary destination and640x360 RGBA32F images; final sharedbase1440 case additionally compares4MiB atlas destination and1120x688 RGBA32F output after invalidating an actual fused UNORM24 receipt."},
        {"qualification","Explicit synthetic history and constructed640x360 futureconsumer based on captured format23resolve; original captures unchanged. Last8padded tile rows independently unchanged after stencil clears. Primary and bothatlas options TRUE; only secondary toggled. Empty query proves refusal/retirement, not positive sample coverage. Raw D32 precision/owner lifetime remain separate primitive proof. AllScene snapshots are fenced; no performance claim."}}).dump(2)<<'\n';
    Check(bool(file),"Cannot write secondary stencil Scene proof");
}
void RunNativeFusedPackedAtlasResolveSceneFixture(const SceneExecutorConfig& base,const Json& original) {
    Check(base.async_submission_capacity==64||base.async_submission_capacity==128,"Fused packed fixture capacity invalid");
    Json donor;
    for(const auto& op:original.at("operations"))if(op.at("type")=="resolve"&&
        op.at("source_slot")==4&&op.at("surfaces").at("10").at("width")==1120&&
        op.at("surfaces").at("10").at("height")==688&&op.at("surfaces").at("10").at("edram_base")==1440&&
        op.at("surfaces").at("10").at("format")==0&&op.at("destination").at("format")==22) {donor=op;break;}
    Check(!donor.is_null(),"Fused packed fixture needs actual captured atlas resolve");Json pairs=Json::array();
    for(bool stencil_export:{false,true}) {
        const auto off=RunFusedPackedAtlas(base,donor,false,stencil_export);
        const auto on=RunFusedPackedAtlas(base,donor,true,stencil_export);
        Check(off.snapshots.size()==8&&on.snapshots.size()==8&&off.queries==on.queries,
            "Fused packed fixture case/query coverage differs");
        Check(off.report.at("device")==on.report.at("device"),"Fused packed paired hardware differs");
        Json rows=Json::array();
        for(const auto& [name,a]:off.snapshots) {
            const auto& b=on.snapshots.at(name);
            Check(a.edram==b.edram&&a.texture==b.texture&&a.preview==b.preview,
                "Fused packed resolve changed full EDRAM, physical destination or raw image");
            rows.push_back({{"case",name},{"exact",true},{"edram_bytes",a.edram.size()},
                {"physical_texture_bytes",a.texture.size()},{"resolved_image_bytes",a.preview.size()},
                {"edram_sha256",ContentHash(a.edram)},{"physical_texture_sha256",ContentHash(a.texture)},
                {"resolved_image_sha256",ContentHash(a.preview)}});
        }
        Check(off.checked_untouched_bytes==on.checked_untouched_bytes&&off.partial_copies==on.partial_copies&&
            off.precision_witnesses==on.precision_witnesses&&on.precision_witnesses.size()==2,
            "Fused packed untouched byte witness differs");
        pairs.push_back({{"stencil_export_requested",stencil_export},{"cases",rows},{"queries",on.queries},
            {"partial_copies",on.partial_copies},{"checked_untouched_bytes",on.checked_untouched_bytes},
            {"precision_witnesses",on.precision_witnesses},
            {"off_report",off.report},{"on_report",on.report}});
    }
    std::ofstream output(base.output/"fused-packed-atlas-scene-fixture.json");
    output<<Json({{"schema",1},{"passed",true},{"scene_orchestration_exercised",true},
        {"capacity",base.async_submission_capacity},{"pairs",pairs},{"noncanonical_host_clear_bits",0x3e800003u},
        {"scope","Eight paired actual Scene cases, portable/export arms,10MiB EDRAM,4MiB physical destination and complete1120x688 RGBA32F output; each partial copy independently preserves bytes outside its physical destination footprint."},
        {"qualification","Explicit synthetic history; actual captured resolve identity with rectangles restricted to supported origin0 same-source/destination96x96,160x96,160x64. Three original packed selects, two consequential pre-encode receipt refusals, two host refusals and one active query refusal. Later generic query consumer refreshes stale host; receipt stencil consumer materializes original receipt. Three existing optimizations stayON; only fused packed option toggles. Snapshots are fenced, and no performance claim is made."}}).dump(2)<<'\n';
    Check(bool(output),"Cannot write fused packed Scene proof");
}

} // namespace gow3::renderer::validation
