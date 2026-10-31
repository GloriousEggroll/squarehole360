#include "renderer/native/live_plan.h"
#include "renderer/native/shader_bindings.h"
#include "renderer/native/immutable_snapshots.h"
#include "renderer/native/live_metadata_capture.h"
#include "renderer/native/shader_input_cache.h"
#include "renderer/native/deferred_texture_snapshots.h"
#include "renderer/native/content_hash.h"
#include "renderer/native/boundary_block_pool.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <picosha2.h>
#include <span>
#include <type_traits>

using namespace gow3::renderer;
using Json=nlohmann::json;
#ifdef GOW3_NATIVE_VERIFY_PREFIX_DEPENDENCIES
namespace gow3::renderer::test {
extern size_t live_prefix_dependency_checks;
extern size_t live_flat_prefixes,live_map_prefixes;
extern size_t live_shader_fingerprint_mask,live_payload_fingerprint_mask;
extern size_t live_shader_snapshot_allocations;
extern size_t live_state_fingerprint_mask,live_snapshot_bytes_limit;
}
#endif
namespace {
bool test_lazy_states=false,test_direct_refs=false,test_fast_fingerprints=false;
class TestLivePlan : public gow3::renderer::GearsLivePlan {
public:
    explicit TestLivePlan(std::function<void(uint32_t,uint32_t)> watch={},bool query_prefixes=false,bool lazy=false)
        :gow3::renderer::GearsLivePlan(std::move(watch),query_prefixes,lazy,test_direct_refs,test_fast_fingerprints){}
};
void CheckDeferredSnapshotScope() {
    size_t captured=0;
    GraphicsBoundaryBlock block{32,0x10000};block.deferred_physical_bytes=4096;
    std::function<void(GraphicsBoundaryBlock&)> snapshot;
    {
        DeferredTextureSnapshotScope scope;
        snapshot=scope.Bind([&](uint32_t address,uint32_t bytes) {
            if(address!=0x10000||bytes!=4096)throw std::runtime_error("Deferred snapshot changed its physical extent");
            ++captured;return std::make_shared<const std::vector<uint8_t>>(bytes,0x37);
        });
        bool wrong_thread=false;
        std::thread other([&] {try {snapshot(block);}catch(const std::runtime_error&){wrong_thread=true;}});
        other.join();
        if(!wrong_thread||captured||!block.deferred_physical_bytes)
            throw std::runtime_error("Deferred snapshot was allowed on another thread");
        snapshot(block);
        if(captured!=1||block.deferred_physical_bytes||block.data().size()!=4096||block.data().back()!=0x37)
            throw std::runtime_error("Deferred snapshot did not become immutable owned bytes");
    }
    block.shared_bytes.reset();block.deferred_physical_bytes=4096;
    bool expired=false;try {snapshot(block);}catch(const std::runtime_error&){expired=true;}
    if(!expired||captured!=1)throw std::runtime_error("Deferred snapshot outlived its D3D caller");
    DeferredTextureSnapshotScope scope;
    const auto partial=scope.Bind([](uint32_t,uint32_t bytes) {
        return std::make_shared<const std::vector<uint8_t>>(bytes-1);
    });
    bool rejected=false;try {partial(block);}catch(const std::runtime_error&){rejected=true;}
    if(!rejected||!block.deferred_physical_bytes||!block.data().empty())
        throw std::runtime_error("Truncated deferred snapshot changed captured provenance");
    const auto unreadable=scope.Bind([](uint32_t,uint32_t) {return DeferredTextureSnapshotScope::Bytes{};});
    unreadable(block);
    if(block.deferred_physical_bytes||!block.data().empty())
        throw std::runtime_error("Unreadable deferred snapshot fabricated CPU bytes");
}

void CheckImmutableOperations() {
    static_assert(std::forward_iterator<LiveOperations::const_iterator>);
    static_assert(std::is_same_v<decltype(*std::declval<LiveOperations&>().begin()),const LiveOperation&>);
    static_assert(std::is_same_v<decltype(std::declval<LiveOperations&>().at(0)),const LiveOperation&>);
    LiveOperations prefix;
    const Json expected={{"type","draw"},{"nested",{{"values",{1,2,3}},{"name","immutable"}}}};
    const LiveOperation* original=nullptr;
    {
        LiveOperations source;
        source.push_back({{"type","earlier"}});source.push_back(expected);
        original=&source.at(1);prefix=source.Slice(1,2);
        if(&prefix.at(0)!=original)throw std::runtime_error("Prefix cloned operation metadata");
        auto iterator=prefix.begin();
        for(unsigned i=0;i<4096;++i)source.push_back({{"later",i}});
        if(&*iterator!=original||iterator->ToJson()!=expected||std::next(iterator)!=prefix.end())
            throw std::runtime_error("Producer growth changed a detached operation prefix");
        if(!source.Slice(source.size(),source.size()).empty())throw std::runtime_error("Empty prefix retained operations");
        for(auto range:{std::pair{2u,1u},std::pair{0u,99999u}}) {
            bool rejected=false;
            try{source.Slice(range.first,range.second);}catch(const std::out_of_range&){rejected=true;}
            if(!rejected)throw std::runtime_error("Invalid shared operation range was accepted");
        }
    }
    if(&prefix.back()!=original||prefix.back().ToJson()!=expected)
        throw std::runtime_error("Source destruction invalidated a detached operation");
    auto diagnostic=prefix.ToJson();
    if(diagnostic!=Json::array({expected}))throw std::runtime_error("Operation diagnostics changed serialized metadata");
    diagnostic[0]["nested"]["name"]="changed";
    if(prefix[0].ToJson()!=expected)throw std::runtime_error("Diagnostic JSON mutated a published operation");
}
uint32_t LE(std::span<const uint8_t> bytes,size_t offset) {
    if(offset+4>bytes.size())throw std::runtime_error("Truncated capture");
    return uint32_t(bytes[offset])|uint32_t(bytes[offset+1])<<8|uint32_t(bytes[offset+2])<<16|uint32_t(bytes[offset+3])<<24;
}
std::vector<uint8_t> Read(const char* path) {
    std::ifstream f(path,std::ios::binary|std::ios::ate);
    const auto size=f.tellg();if(!f||size<16||size>256*1024*1024)throw std::runtime_error("Invalid capture size");
    std::vector<uint8_t> out(size);f.seekg(0);f.read(reinterpret_cast<char*>(out.data()),size);
    if(!f)throw std::runtime_error("Cannot read capture");return out;
}
void CheckMemoryCopyPrefixes(const std::filesystem::path& folder) {
    std::ifstream source(folder/"event.json");Json manifest;source>>manifest;
    GraphicsBoundaryEvent copy;copy.function=manifest.at("function");copy.phase=manifest.at("phase");
    copy.sequence=120;copy.args=manifest.at("args").get<std::array<uint32_t,8>>();
    for(const auto& block:manifest.at("blocks")) {
        const unsigned kind=block.at("kind");
        if(kind!=1&&kind!=2&&kind!=3&&kind!=14)continue;
        const auto path=(folder/block.at("path").get<std::string>()).string();
        copy.blocks.push_back({kind,block.at("address"),Read(path.c_str())});
    }
    auto shader=copy.blocks.at(1).bytes;
    shader.insert(shader.end(),copy.blocks.at(2).bytes.begin(),copy.blocks.at(2).bytes.end());
    const auto digest=picosha2::hash256_hex_string(shader);
    std::optional<LivePacketPrefix> first;
    {
        TestLivePlan planner({},false,test_lazy_states);planner.Push(GraphicsBoundaryEvent(copy));
        // This is intentionally interned after the copy operation. Although
        // the copy's shader field is diagnostic, the legacy recursive walk
        // retains a matching payload when its packet is committed later.
        planner.InvalidateMemory(0x10000,uint32_t(shader.size()),shader);
        copy.sequence=121;planner.Push(GraphicsBoundaryEvent(copy));
        GraphicsBoundaryEvent commit;commit.span_only=true;
        commit.command_span=GraphicsCommandSpan{1,120,0x2000,{0xc0,0,0x36,0,0,3,0,0x84}};
        planner.Push(GraphicsBoundaryEvent(commit));first=planner.TakePacketPrefix();
        if(!first||first->batch->operations.size()!=1||first->batch->operations[0].ToJson().at("type")!="memory_copy"||
           first->batch->ResourceCount(false)!=2||(first->batch->ResourceCount(true)!=0)||
           first->batch->retained_bytes!=8192+shader.size()||*first->batch->ResourceAt(digest,false)!=shader)
            throw std::runtime_error("Late memory-copy diagnostic hash lost its legacy payload match");
        commit.command_span->generation=2;commit.command_span->api_sequence=121;
        planner.Push(GraphicsBoundaryEvent(commit));const auto second=planner.TakePacketPrefix();
        if(!second||second->batch->operations.size()!=2||second->batch->ResourceCount(false)!=2||
           second->batch->retained_bytes!=first->batch->retained_bytes)
            throw std::runtime_error("CPU write and memory-copy dependencies changed duplicate byte accounting");
        auto put=[](auto& bytes,size_t offset,uint32_t value) {
            for(unsigned i=0;i<4;++i)bytes.at(offset+i)=uint8_t(value>>(24-i*8));
        };
        const uint32_t previous_source=first->batch->operations[0].ToJson().at("source");
        const uint32_t previous_destination=first->batch->operations[0].ToJson().at("destination");
        put(copy.blocks[0].bytes,0x480+95*8,previous_destination|3);
        put(copy.blocks[0].bytes,0x780+6*16,0x40000000|(previous_source>>2));
        copy.sequence=122;copy.command_span=GraphicsCommandSpan{3,122,0x3000,{0xc0,0,0x36,0,0,3,0,0x84}};
        planner.Push(std::move(copy));const auto gpu=planner.TakePacketPrefix();
        if(!gpu||gpu->batch->operations.size()!=1||gpu->batch->operations[0].ToJson().at("source_kind")!="gpu_memory"||
           !gpu->batch->operations[0].ToJson().at("payload").is_null()||gpu->batch->ResourceCount(false)!=1||
           gpu->batch->retained_bytes!=shader.size()||*gpu->batch->ResourceAt(digest,false)!=shader)
            throw std::runtime_error("GPU memory-copy prefix retained CPU source bytes or lost its diagnostic alias");
    }
    if(!first||*first->batch->ResourceAt(digest,false)!=shader)
        throw std::runtime_error("Memory-copy diagnostic payload lost immutable ownership");
}
// The live bridge snapshots only a memcpy draw's chunk (block 14 at its
// source); a full capture holds the whole bound buffer. Both plan one copy.
void CheckChunkMemoryCopyBlock(const std::filesystem::path& folder) {
    std::ifstream source(folder/"event.json");Json manifest;source>>manifest;
    GraphicsBoundaryEvent full;full.function=manifest.at("function");full.phase=manifest.at("phase");
    full.sequence=200;full.args=manifest.at("args").get<std::array<uint32_t,8>>();
    full.args[2]=1;full.args[3]=1;  // The second 64-byte unit only.
    for(const auto& block:manifest.at("blocks")) {
        const unsigned kind=block.at("kind");
        if(kind!=1&&kind!=2&&kind!=3&&kind!=14)continue;
        const auto path=(folder/block.at("path").get<std::string>()).string();
        full.blocks.push_back({kind,block.at("address"),Read(path.c_str())});
    }
    const auto plan=[](GraphicsBoundaryEvent event)->std::pair<Json,std::vector<uint8_t>> {
        TestLivePlan planner({},false,test_lazy_states);planner.Push(std::move(event));
        GraphicsBoundaryEvent commit;commit.span_only=true;
        commit.command_span=GraphicsCommandSpan{1,200,0x2000,{0xc0,0,0x36,0,0,3,0,0x84}};
        planner.Push(GraphicsBoundaryEvent(commit));
        const auto prefix=planner.TakePacketPrefix();
        if(!prefix||prefix->batch->operations.size()!=1)throw std::runtime_error("Chunk memcpy produced no single operation");
        auto op=prefix->batch->operations[0].ToJson();
        const auto bytes=prefix->batch->ResourceAt(op.at("payload").get<std::string>(),false);
        if(!bytes)throw std::runtime_error("Chunk memcpy lost its payload");
        return {op,*bytes};
    };
    auto& stream=full.blocks.back();
    if(stream.kind!=14||stream.bytes.size()<128)throw std::runtime_error("Fixture lacks its memcpy source stream");
    GraphicsBoundaryEvent chunk=full;
    auto& narrow=chunk.blocks.back();narrow.address+=64;
    narrow.bytes.assign(stream.bytes.begin()+64,stream.bytes.begin()+128);
    const auto [full_op,full_bytes]=plan(full);
    const auto [chunk_op,chunk_bytes]=plan(chunk);
    if(full_bytes!=std::vector<uint8_t>(stream.bytes.begin()+64,stream.bytes.begin()+128)||chunk_bytes!=full_bytes||
       chunk_op.at("source")!=full_op.at("source")||chunk_op.at("destination")!=full_op.at("destination")||
       chunk_op.at("bytes")!=full_op.at("bytes"))
        throw std::runtime_error("Chunk-only memcpy source block planned a different copy");
    // A block that misses the copied bytes is refused, never read past.
    auto missing=chunk;missing.blocks.back().address+=64;
    bool refused=false;try{plan(missing);}catch(const std::exception&){refused=true;}
    if(!refused)throw std::runtime_error("Memcpy accepted a source block that misses its chunk");
    // Direct execution performed a CPU-sourced copy at its API point: the op
    // only orders the destination's invalidation and carries no payload.
    TestLivePlan direct({},false,test_lazy_states);direct.SetDirectCpuMemoryCopies(true);
    direct.Push(GraphicsBoundaryEvent(chunk));
    GraphicsBoundaryEvent commit;commit.span_only=true;
    commit.command_span=GraphicsCommandSpan{1,200,0x2000,{0xc0,0,0x36,0,0,3,0,0x84}};
    direct.Push(std::move(commit));
    const auto prefix=direct.TakePacketPrefix();
    if(!prefix||prefix->batch->operations.size()!=1)throw std::runtime_error("Direct chunk memcpy produced no single operation");
    const auto direct_op=prefix->batch->operations[0].ToJson();
    if(direct_op.at("source_kind")!="cpu_direct"||!direct_op.at("payload").is_null()||
       direct_op.at("destination")!=chunk_op.at("destination")||direct_op.at("bytes")!=chunk_op.at("bytes"))
        throw std::runtime_error("Direct CPU memcpy kept a payload or changed its copy");
}
// Live frames intentionally carry no offline producer diagnostics. Both GPU
// source labels use the same native storage path. Payload bytes for GPU-backed
// bindings are deliberately discarded by the live planner.
void Normalize(Json& op,bool selective=false) {
    if(op.at("type")!="draw")return;
    if(selective) {
        const auto slots=op.at("vertex_slots").get<std::set<unsigned>>();
        auto& geometry=op["geometry"];
        for(auto it=geometry.begin();it!=geometry.end();) {
            if(it->contains("fetch_slot")&&!slots.contains(it->at("fetch_slot").get<unsigned>()))it=geometry.erase(it);
            else ++it;
        }
    }
    for(auto& t:op["textures"]) {
        t.erase("producers");t.erase("needs_prior_contents");
        if(t.at("source")=="gpu_resolve"||t.at("source")=="gpu_alias_or_previous_frame") {
            t["source"]="gpu";t["base_payload"]=nullptr;t["mip_payload"]=nullptr;
        }
    }
}
void CheckThinLiveMetadata() {
    size_t checks=0;
    for(unsigned bits=0;bits<16;++bits)for(uint32_t kind=0;kind<105;++kind) {
        const LiveMetadataCapturePolicy p{bool(bits&1),bool(bits&2),bool(bits&4),bool(bits&8)};
        const bool expected=(bits&1)&&(bits&2)&&!(bits&4)&&
            (kind==12||kind==13||(kind==11&&(bits&8)));
        if(p.Omit(kind)!=expected)throw std::runtime_error("Live metadata policy escaped its closed kinds/gates");
        ++checks;
    }
    LiveMetadataCaptureStatistics stats;
    stats.Record(11,1536,true);stats.Record(12,256,true);stats.Record(13,512,false);
    stats.Record(14,8192,true);stats.Record(11,0,true);
    if(stats.omitted_blocks!=4||stats.readable_copies_avoided!=2||stats.copy_bytes_avoided!=1792||
       stats.unreadable_blocks!=2||stats.copies_by_kind!=std::array<uint64_t,3>{1,1,0}||
       stats.bytes_by_kind!=std::array<uint64_t,3>{1536,256,0})
        throw std::runtime_error("Thin metadata counted unreadable/empty/unrelated copies as avoided");
    // Exercise the real planner's missing-VS diagnostic, including the retained
    // 0x600-byte object header. The candidate must leave the exact error intact.
    GraphicsBoundaryEvent bad;bad.function=0x82AEFF00;bad.phase=1;bad.args[0]=0x83000000;
    bad.blocks={{1,bad.args[0],std::vector<uint8_t>(0x3600)},
        {2,0x83100368,{}},{3,0x1000,{}},{11,0x83100000,std::vector<uint8_t>(0x600,0x32)},
        {12,0x83110000,std::vector<uint8_t>(0x100,0x43)},
        {13,0x83120000,std::vector<uint8_t>(0x200,0x54)}};
    auto error=[](GraphicsBoundaryEvent event) {
        try{TestLivePlan planner;planner.Push(std::move(event));}
        catch(const std::runtime_error& e){return std::string(e.what());}
        throw std::runtime_error("Missing VS unexpectedly accepted");
    };
    const auto original=error(bad);
    const LiveMetadataCapturePolicy p{true,true,false,false};
    for(auto& block:bad.blocks)if(p.Omit(block.kind))block.bytes.clear();
    if(error(bad)!=original||original.find("shader_header")==std::string::npos||bad.blocks[3].bytes.size()!=0x600)
        throw std::runtime_error("Thin metadata lost missing-VS object diagnostics");
    std::cout<<"Thin live metadata policy: "<<checks+2<<" checks; exact missing-VS diagnostic preserved\n";
}
void CheckQueryABI() {
    {
        ImmutableSnapshots cache(8192);
        std::vector<uint8_t> guest(4096,0x31);
        auto first=cache.Get(0x1000,guest);
        if(cache.Get(0x1000,guest)!=first)throw std::runtime_error("Unchanged snapshot was copied again");
        guest[2047]=0x72;auto changed=cache.Get(0x1000,guest);
        if(changed==first||first->at(2047)!=0x31||changed->at(2047)!=0x72)
            throw std::runtime_error("Guest write changed an in-flight immutable snapshot");
        auto second=cache.Get(0x2000,guest);std::weak_ptr<const std::vector<uint8_t>> oldest=changed;
        changed.reset();cache.Get(0x3000,guest);
        if(!oldest.expired()||cache.retained_bytes()!=8192||second->at(2047)!=0x72)
            throw std::runtime_error("Snapshot eviction lost lifetime or exceeded capacity");
        cache.Clear();
        if(cache.retained_bytes()||first->at(2047)!=0x31)
            throw std::runtime_error("Snapshot cache shutdown changed a consumer's bytes");
    }
    auto put=[](std::vector<uint8_t>& bytes,size_t offset,uint32_t value) {
        for(unsigned i=0;i<4;++i)bytes.at(offset+i)=uint8_t(value>>(24-i*8));
    };
    auto query=[&](uint32_t virtual_base,uint32_t record,uint32_t allocations=1) {
        GraphicsBoundaryEvent event;event.function=0x82AFF740;
        event.args[0]=0x83000000;event.args[1]=0x83010000;event.args[2]=record;
        event.blocks={{1,event.args[0],std::vector<uint8_t>(0x3600)},
                      {104,event.args[1],std::vector<uint8_t>(0x98)}};
        put(event.blocks[1].bytes,0x1c,virtual_base);put(event.blocks[1].bytes,0x94,allocations);
        return event;
    };
    auto present=[&] {
        GraphicsBoundaryEvent event;event.function=0x82AF6B10;
        event.blocks={{1,0x83000000,std::vector<uint8_t>(0x3600)},
                      {103,0x83020000,std::vector<uint8_t>(64)}};
        auto& header=event.blocks[1].bytes;
        put(header,28,(40u<<22)|2);put(header,32,0x10000006);
        put(header,36,1279|(719<<13));put(header,48,1<<9);
        return event;
    };
    for(const auto [virtual_base,physical_base]:{
        std::pair{0xB4DF0B00u,0x14DF0B00u},std::pair{0xE0001000u,0x00002000u}}) {
        TestLivePlan planner({},false,test_lazy_states);planner.Push(query(virtual_base,1));planner.Push(query(virtual_base,0));
        const auto frame=planner.Push(present());
        if(!frame||frame->operations.size()!=3||frame->operations.at(0).ToJson().at("type")!="query_begin"||
            frame->operations.at(1).ToJson().at("type")!="query_end"||
            frame->operations.at(0).ToJson().at("report_address")!=physical_base+32||
            frame->operations.at(1).ToJson().at("report_address")!=physical_base||
            frame->operations.at(0).ToJson().at("query_handle")!=0x83010000u)
            throw std::runtime_error("Query emitter ABI does not match physical PM4 report addresses");
    }
    for(auto invalid:{query(0xB4DF0B00,1,2),query(0xB4DF0B04,1),query(0xB4DF0B00,2)}) {
        bool rejected=false;
        try {TestLivePlan planner({},false,test_lazy_states);planner.Push(std::move(invalid));}
        catch(const std::runtime_error&) {rejected=true;}
        if(!rejected)throw std::runtime_error("Invalid or multi-allocation query accepted");
    }
    {
        TestLivePlan planner({},false,test_lazy_states);
        planner.InvalidateMemory(0x1000,4096,std::vector<uint8_t>(4096,0x5a),42);
        planner.InvalidateMemory(0x2000,4096);
        const auto frame=planner.Push(present());
        if(!frame||frame->operations.size()!=3||frame->operations[0].ToJson().at("type")!="cpu_memory_write"||
           frame->operations[0].ToJson().at("address")!=0x1000||frame->operations[0].ToJson().at("bytes")!=4096||frame->operations[0].ToJson().at("coherence_epoch")!=42||
           frame->MaterializeResources(false).at(frame->operations[0].ToJson().at("payload").get<std::string>())->at(4095)!=0x5a||
           !frame->operations[1].ToJson().at("payload").is_null()||frame->operations[1].ToJson().at("coherence_epoch")!=0||frame->operations[2].ToJson().at("type")!="present")
            throw std::runtime_error("CPU writes/frees did not precede their consuming API frame");
    }
    {
        // Identical data in consecutive frames may share immutable storage.
        // Reusing the same address with different bytes must preserve old
        // consumers and produce a new content identity.
        TestLivePlan planner({},false,test_lazy_states);
        std::array<std::shared_ptr<LiveRenderFrame>,4> frames;
        for(unsigned i=0;i<frames.size();++i) {
            planner.InvalidateMemory(0x1000,4096,std::vector<uint8_t>(4096,i==2?0x72:0x31));
            auto swap=present();swap.frame=i;frames[i]=planner.Push(std::move(swap));
            if(frames[i]->retained_bytes!=4096)throw std::runtime_error("Snapshot reuse changed frame byte accounting");
        }
        const auto first_digest=frames[0]->operations[0].ToJson().at("payload").get<std::string>();
        if(frames[1]->MaterializeResources(false).at(first_digest)!=frames[0]->MaterializeResources(false).at(first_digest)||
           frames[2]->MaterializeResources(false).contains(first_digest)||!frames[3]->MaterializeResources(false).contains(first_digest)||
           frames[0]->MaterializeResources(false).at(first_digest)->at(4095)!=0x31)
            throw std::runtime_error("Previous-frame snapshot reuse lost content identity or immutability");
    }
    {
        std::optional<LiveQueryPrefix> first,second,third;
        {
            TestLivePlan planner({},true,test_lazy_states);
            planner.InvalidateMemory(0x1000,4096,std::vector<uint8_t>(4096,0x31));
            planner.Push(query(0xB4DF0B00,1));
            if(planner.TakeQueryPrefix())throw std::runtime_error("Query begin exposed an unfinished prefix");
            planner.Push(query(0xB4DF0B00,0));first=planner.TakeQueryPrefix();
            if(!first||first->first_operation!=0||first->operation_end!=3||first->generation!=1||
                first->end_address!=0x14DF0B00||first->batch->operations.size()!=3||
                first->batch->retained_bytes!=4096||planner.TakeQueryPrefix())
                throw std::runtime_error("First live query prefix ordering is incorrect");
            planner.InvalidateMemory(0x1000,4096,std::vector<uint8_t>(4096,0x72));
            planner.Push(query(0xB4DF0B00,1));planner.Push(query(0xB4DF0B00,0));
            second=planner.TakeQueryPrefix();
            if(!second||second->first_operation!=3||second->operation_end!=6||second->generation!=2||
                second->batch->ResourceCount(false)!=1||second->batch->retained_bytes!=4096)
                throw std::runtime_error("Reused report did not produce an ordered new generation");
            const auto frame=planner.Push(present());
            if(frame->operations.size()!=7||frame->operations.at(second->operation_end).ToJson().at("type")!="present")
                throw std::runtime_error("Present lost the complete frame or duplicated prefix range");
            if(&frame->operations.at(0)!=&first->batch->operations.at(0)||
               &frame->operations.at(second->first_operation)!=&second->batch->operations.at(0))
                throw std::runtime_error("Query prefixes do not share immutable frame operations");
            auto begin=query(0xB4DF0B00,1),end=query(0xB4DF0B00,0),swap=present();
            begin.frame=end.frame=swap.frame=1;
            planner.Push(std::move(begin));planner.Push(std::move(end));third=planner.TakeQueryPrefix();
            if(!third||third->first_operation!=0||third->operation_end!=2||third->generation!=3||
                third->batch->frame!=1||third->batch->retained_bytes!=0)
                throw std::runtime_error("Report generation or prefix offset reset incorrectly at Present");
            planner.Push(std::move(swap));
        }
        const auto old_digest=first->batch->operations.at(0).ToJson().at("payload").get<std::string>();
        const auto new_digest=second->batch->operations.at(0).ToJson().at("payload").get<std::string>();
        if(old_digest==new_digest||first->batch->ResourceAt(old_digest,false)->at(4095)!=0x31||
            second->batch->ResourceAt(new_digest,false)->at(4095)!=0x72||
            first->batch->operations.at(2).ToJson().at("query_generation")!=1||
            second->batch->operations.at(2).ToJson().at("query_generation")!=2)
            throw std::runtime_error("Detached query prefix mutated with later CPU writes or planner destruction");
    }
    {
        bool rejected=false;
        try {TestLivePlan planner({},true,test_lazy_states);planner.Push(query(0xB4DF0B00,0));}
        catch(const std::runtime_error&) {rejected=true;}
        if(!rejected)throw std::runtime_error("Query prefix accepted an end without a begin");
    }
}
void CheckLazyStateLifecycle(const GraphicsBoundaryEvent& source,const GraphicsBoundaryEvent& swap_source) {
    using Bytes=std::vector<uint8_t>;
    std::shared_ptr<LiveRenderFrame> retained;
    std::weak_ptr<const Bytes> unrelated;
    const auto expected=picosha2::hash256_hex_string(source.blocks[0].bytes);
    {
        TestLivePlan planner({},false,true);
        auto first=source;first.sequence=600;
        first.command_span=GraphicsCommandSpan{1,600,0x2000,{0xc0,0,0x36,0,0,3,0,0x84}};
        planner.Push(std::move(first));auto prefix=planner.TakePacketPrefix();retained=prefix->batch;
        const auto state=retained->operations.back().Draw()->lazy_device_state;
        if(!state||!retained->lazy_payloads||std::string_view(planner.LazyStateMode())!="enabled"||*state->Owner()!=source.blocks[0].bytes)
            throw std::runtime_error("Lazy prefix lost exact owned state bytes");
#ifndef GOW3_NATIVE_VERIFY_PREFIX_DEPENDENCIES
        if(state->HasDigest())throw std::runtime_error("Normal typed prefix eagerly hashed device state");
        const auto texture=ContentHash(source.blocks.back().data());
        if(*retained->ResourceAt(texture)!=source.blocks.back().bytes||state->HasDigest())
            throw std::runtime_error("Canonical control lookup eagerly hashed unrelated device state");
#endif
        const auto charged=retained->retained_bytes;
        auto second=source;second.sequence=601;second.blocks[0].bytes[0]^=0x12;
        planner.Push(std::move(second));unrelated=second.blocks[0].shared_bytes;
        // Cold observation races with later canonical-mode fallback. Detached
        // banks may fill their own hash caches but never change their entries.
        std::exception_ptr reader_error;
        std::thread reader([&]{try {
            for(unsigned i=0;i<32;++i) {
                const auto map=retained->MaterializeResources();
                if(map.at(expected)!=state->Owner()||retained->operations.back().ToJson().at("device_state")!=expected)
                    throw std::runtime_error("Concurrent lazy export changed canonical state");
            }
        }catch(...){reader_error=std::current_exception();}});
        planner.InvalidateMemory(0x30000,LazyDeviceState::kBytes,source.blocks[0].bytes);
        auto third=source;third.sequence=602;planner.Push(std::move(third));
        auto swap=swap_source;swap.sequence=603;auto complete=planner.Push(std::move(swap));
        reader.join();if(reader_error)std::rethrow_exception(reader_error);
        if(std::string_view(planner.LazyStateMode())!="payload_size_alias"||complete->lazy_payloads||complete->ResourceAt(expected)!=state->Owner()||
           !retained->lazy_payloads||retained->retained_bytes!=charged)
            throw std::runtime_error("Canonical fallback changed detached state ownership/accounting");
    }
    if(!unrelated.expired()||retained->ResourceAt(expected)->size()!=LazyDeviceState::kBytes)
        throw std::runtime_error("Lazy prefix retained an unrelated producer frame");
    {
#ifdef GOW3_NATIVE_VERIFY_PREFIX_DEPENDENCIES
        struct CollisionScope {
            size_t saved=test::live_state_fingerprint_mask;
            CollisionScope(){test::live_state_fingerprint_mask=0;}
            ~CollisionScope(){test::live_state_fingerprint_mask=saved;}
        } collisions;
#endif
        std::vector<std::shared_ptr<LiveRenderFrame>> retained_prefixes;
        std::vector<Json> retained_operations;
        auto compare=[](const LiveRenderFrame& actual,const LiveRenderFrame& expected) {
            if(actual.operations.ToJson()!=expected.operations.ToJson()||actual.retained_bytes!=expected.retained_bytes)
                throw std::runtime_error("Wide-prefix fallback changed canonical operations/accounting");
            for(bool shader:{false,true}) {
                const auto a=actual.MaterializeResources(shader),b=expected.MaterializeResources(shader);
                if(a.size()!=b.size()||a.bucket_count()!=b.bucket_count())
                    throw std::runtime_error("Wide-prefix fallback changed canonical map shape");
                auto ai=a.begin(),bi=b.begin();
                for(;ai!=a.end();++ai,++bi)if(ai->first!=bi->first||*ai->second!=*bi->second)
                    throw std::runtime_error("Wide-prefix fallback changed namespace iteration/bytes");
            }
        };
        {
            TestLivePlan planner({},false,true),canonical;
            auto push=[&](GraphicsBoundaryEvent event) {
                canonical.Push(GraphicsBoundaryEvent(event));planner.Push(std::move(event));
            };
            auto take=[&] {
                auto actual=planner.TakePacketPrefix(),expected=canonical.TakePacketPrefix();
                if(!actual||!expected)throw std::runtime_error("Wide-prefix fixture lost a packet boundary");
                return std::pair{actual->batch,expected->batch};
            };
            auto keep=[&](const std::shared_ptr<LiveRenderFrame>& actual,const std::shared_ptr<LiveRenderFrame>& expected) {
                compare(*actual,*expected);retained_prefixes.push_back(actual);
                retained_operations.push_back(actual->operations.ToJson());
            };
            auto event=source;event.sequence=700;
            event.command_span=GraphicsCommandSpan{1,700,0x2000,{0xc0,0,0x36,0,0,3,0,0x84}};
            push(std::move(event));auto initial=take();keep(initial.first,initial.second);
            for(unsigned i=0;i<40;++i) {
                event=source;event.sequence=701+i;event.blocks[0].bytes[0]^=uint8_t(i+1);
                push(std::move(event));
            }
            // This later state exists in the producer before requesting the
            // earlier wide slice. It must not be hashed by that slice's cold
            // export or retained in its detached resource maps.
            event=source;event.sequence=741;event.blocks[0].bytes[0]^=0x7f;push(std::move(event));
            GraphicsBoundaryEvent commit;commit.span_only=true;
            commit.command_span=GraphicsCommandSpan{2,740,0x2000,{0xc0,0,0x36,0,0,3,0,0x84}};
            push(GraphicsBoundaryEvent(commit));auto wide=take();
            if(std::string_view(planner.LazyStateMode())!="enabled"||wide.first->lazy_payloads||wide.first->prefix_resources||
               wide.first->operations.size()!=40)
                throw std::runtime_error("Large prefix disabled producer lazy state or lost map fallback");
            keep(wide.first,wide.second);
            commit.command_span->generation=3;commit.command_span->api_sequence=741;
            push(std::move(commit));auto narrow=take();
            const auto later_state=narrow.first->operations.back().Draw()->lazy_device_state;
            if(!narrow.first->lazy_payloads||!later_state||std::string_view(planner.LazyStateMode())!="enabled")
                throw std::runtime_error("Wide prefix disabled the following narrow lazy prefix");
#ifndef GOW3_NATIVE_VERIFY_PREFIX_DEPENDENCIES
            if(later_state->HasDigest())throw std::runtime_error("Wide prefix hashed an unrelated later state");
#endif
            keep(narrow.first,narrow.second);
            auto swap=swap_source;swap.sequence=742;
            auto full=planner.Push(GraphicsBoundaryEvent(swap));auto full_expected=canonical.Push(std::move(swap));
            if(!full->lazy_payloads||std::string_view(planner.LazyStateMode())!="enabled")
                throw std::runtime_error("Wide prefix disabled full-frame lazy ownership");
            compare(*full,*full_expected);
            event=source;event.frame=1;event.sequence=743;event.blocks[0].bytes[0]^=0x7e;
            event.command_span=GraphicsCommandSpan{4,743,0x2000,{0xc0,0,0x36,0,0,3,0,0x84}};
            push(std::move(event));auto next_frame=take();
            const auto next_state=next_frame.first->operations.back().Draw()->lazy_device_state;
            if(!next_frame.first->lazy_payloads||!next_state||std::string_view(planner.LazyStateMode())!="enabled")
                throw std::runtime_error("Wide prefix disabled a later frame's lazy identities");
#ifndef GOW3_NATIVE_VERIFY_PREFIX_DEPENDENCIES
            if(next_state->HasDigest())throw std::runtime_error("Later frame state was eagerly hashed after a wide prefix");
#endif
            keep(next_frame.first,next_frame.second);
        }
        for(size_t i=0;i<retained_prefixes.size();++i) {
            const auto& prefix=*retained_prefixes[i];
            if(prefix.operations.ToJson()!=retained_operations[i])
                throw std::runtime_error("Producer teardown changed an earlier wide/narrow prefix");
            size_t retained_bytes=0;
            for(bool shader:{false,true})prefix.ForEachResource(shader,[&](auto key,const auto& owner) {
                if(ContentHash(*owner)!=key)throw std::runtime_error("Detached wide prefix changed canonical payload bytes");
                retained_bytes+=owner->size();
            });
            if(retained_bytes!=prefix.retained_bytes)throw std::runtime_error("Detached wide prefix lost byte accounting");
        }
    }
#ifdef GOW3_NATIVE_VERIFY_PREFIX_DEPENDENCIES
    {
        struct LimitScope {
            size_t saved=test::live_snapshot_bytes_limit;
            LimitScope(){test::live_snapshot_bytes_limit=2*LazyDeviceState::kBytes;}
            ~LimitScope(){test::live_snapshot_bytes_limit=saved;}
        } limit;
        TestLivePlan planner({},false,true);
        auto first=source;first.sequence=760;planner.Push(std::move(first));
        auto second=source;second.sequence=761;second.blocks[0].bytes[0]^=0x57;
        bool rejected=false;
        try {planner.Push(std::move(second));}
        catch(const std::runtime_error& error){rejected=std::string_view(error.what())=="Live frame exceeds its snapshot memory limit";}
        auto swap=swap_source;swap.sequence=762;const auto frame=planner.Push(std::move(swap));
        size_t counted=0;for(bool shader:{false,true})frame->ForEachResource(shader,[&](auto,const auto& bytes){counted+=bytes->size();});
        if(!rejected||counted!=frame->retained_bytes||counted>test::live_snapshot_bytes_limit)
            throw std::runtime_error("Lazy accounting exceeded the pre-hash snapshot limit");
    }
#endif
    {
        auto event=source;event.sequence=780;
        std::erase_if(event.blocks,[](const auto& block){return block.kind==4||block.kind==5||block.kind>=32;});
        auto& header=event.blocks[1].bytes;auto& program=event.blocks[2].bytes;
        program.resize(LazyDeviceState::kBytes-header.size());
        const auto put=[&](size_t at,uint32_t value){for(unsigned i=0;i<4;++i)header.at(at+i)=uint8_t(value>>(24-i*8));};
        put(8,uint32_t(program.size()));put(40,uint32_t(program.size()));
        auto container=header;container.insert(container.end(),program.begin(),program.end());
        event.blocks[0].bytes=container;
        event.command_span=GraphicsCommandSpan{1,780,0x2000,{0xc0,0,0x36,0,0,3,0,0x84}};
        TestLivePlan planner({},false,true);planner.Push(std::move(event));
        const auto prefix=planner.TakePacketPrefix();const auto digest=ContentHash(container);
        if(std::string_view(planner.LazyStateMode())!="shader_size_alias"||!prefix||prefix->batch->lazy_payloads||prefix->batch->ResourceCount(false)!=1||
           prefix->batch->ResourceCount(true)!=1||prefix->batch->retained_bytes!=2*LazyDeviceState::kBytes||
           *prefix->batch->ResourceAt(digest,false)!=container||*prefix->batch->ResourceAt(digest,true)!=container)
            throw std::runtime_error("Lazy state/shader alias lost its distinct canonical namespaces");
    }
    {
        TestLivePlan planner({},false,true);
        auto event=source;event.sequence=800;planner.Push(std::move(event));
        auto swap=swap_source;swap.sequence=801;auto complete=planner.Push(std::move(swap));
        const auto state=complete->operations[0].Draw()->lazy_device_state;
        if(!state)throw std::runtime_error("Lazy full frame lost its typed state");
#ifndef GOW3_NATIVE_VERIFY_PREFIX_DEPENDENCIES
        if(state->HasDigest())throw std::runtime_error("Frame completion eagerly materialized canonical state");
#endif
        size_t bytes=0;for(bool shader:{false,true})complete->ForEachResource(shader,[&](auto key,const auto& owner) {
            if(ContentHash(*owner)!=key)throw std::runtime_error("Lazy export emitted a noncanonical resource identity");
            bytes+=owner->size();
        });
        if(bytes!=complete->retained_bytes||!state->HasDigest())
            throw std::runtime_error("Lazy full bank exported different retained bytes");
    }
}

void CheckUnrelatedStateSizedResources(const GraphicsBoundaryEvent& source,const GraphicsBoundaryEvent& swap_source) {
#ifdef GOW3_NATIVE_VERIFY_PREFIX_DEPENDENCIES
    struct CollisionScope {
        size_t saved=test::live_state_fingerprint_mask;
        CollisionScope(){test::live_state_fingerprint_mask=0;}
        ~CollisionScope(){test::live_state_fingerprint_mask=saved;}
    } collisions;
#endif
    auto different=source.blocks[0].bytes;different[0]^=0x5a;
    const auto different_hash=ContentHash(different);
    for(bool previous:{false,true}) {
        TestLivePlan lazy({},false,true),canonical;
        lazy.InvalidateMemory(0x30000,uint32_t(different.size()),different);
        canonical.InvalidateMemory(0x30000,uint32_t(different.size()),different);
        auto event=source;event.sequence=900;
        lazy.Push(GraphicsBoundaryEvent(event));canonical.Push(GraphicsBoundaryEvent(event));
        if(std::string_view(lazy.LazyStateMode())!="enabled")
            throw std::runtime_error("Unrelated state-sized payload disabled lazy state identities");
        std::shared_ptr<LiveRenderFrame> earlier;
        if(previous) {
            auto swap=swap_source;swap.sequence=901;
            earlier=lazy.Push(GraphicsBoundaryEvent(swap));auto oracle=canonical.Push(GraphicsBoundaryEvent(swap));
            if(earlier->operations.ToJson()!=oracle->operations.ToJson()||earlier->retained_bytes!=oracle->retained_bytes)
                throw std::runtime_error("Unrelated same-sized resource changed first-frame exports");
        }
        // Only actual identity may switch modes; the prefix still includes
        // exactly the old namespaces and previous owner eligibility.
        event=source;event.frame=previous?1:0;event.sequence=902;event.blocks[0].bytes=different;
        lazy.Push(GraphicsBoundaryEvent(event));canonical.Push(GraphicsBoundaryEvent(event));
        if(std::string_view(lazy.LazyStateMode())!="payload_size_alias")
            throw std::runtime_error("Actual current/previous generic state alias was missed");
        auto swap=swap_source;swap.frame=event.frame;swap.sequence=903;
        auto actual=lazy.Push(GraphicsBoundaryEvent(swap)),expected=canonical.Push(GraphicsBoundaryEvent(swap));
        if(actual->operations.ToJson()!=expected->operations.ToJson()||actual->retained_bytes!=expected->retained_bytes)
            throw std::runtime_error("Actual generic alias changed canonical exports/accounting");
        for(bool shader:{false,true}) {
            const auto a=actual->MaterializeResources(shader),b=expected->MaterializeResources(shader);
            if(a.size()!=b.size()||a.bucket_count()!=b.bucket_count())
                throw std::runtime_error("Actual generic alias changed bank shape");
            auto ai=a.begin(),bi=b.begin();
            for(;ai!=a.end();++ai,++bi)if(ai->first!=bi->first||*ai->second!=*bi->second)
                throw std::runtime_error("Actual generic alias changed canonical bank iteration or bytes");
        }
        if(earlier&&actual->ResourceAt(different_hash)!=earlier->ResourceAt(different_hash))
            throw std::runtime_error("Previous generic alias lost eligible immutable owner reuse");
    }
    {
        // Equal-sized shader containers are uncommon but may precede the
        // device state they alias. Preserve both namespaces only on identity.
        auto event=source;event.sequence=920;
        std::erase_if(event.blocks,[](const auto& block){return block.kind==4||block.kind==5||block.kind>=32;});
        auto& header=event.blocks[1].bytes;auto& program=event.blocks[2].bytes;
        program.resize(LazyDeviceState::kBytes-header.size());
        for(auto at:{8u,40u})for(unsigned i=0;i<4;++i)header.at(at+i)=uint8_t(uint32_t(program.size())>>(24-i*8));
        auto container=header;container.insert(container.end(),program.begin(),program.end());
        TestLivePlan lazy({},false,true),canonical;
        lazy.Push(GraphicsBoundaryEvent(event));canonical.Push(GraphicsBoundaryEvent(event));
        if(std::string_view(lazy.LazyStateMode())!="enabled")
            throw std::runtime_error("Unrelated state-sized shader disabled lazy identities");
        auto swap=swap_source;swap.sequence=921;
        const auto earlier=lazy.Push(GraphicsBoundaryEvent(swap));canonical.Push(GraphicsBoundaryEvent(swap));
        event.frame=1;event.sequence=922;event.blocks[0].bytes=container;
        // Replace this frame's shader so only the previous shader index finds
        // the true state alias before any current shader is admitted.
        event.blocks[1].bytes=source.blocks[1].bytes;event.blocks[2].bytes=source.blocks[2].bytes;
        lazy.Push(GraphicsBoundaryEvent(event));canonical.Push(GraphicsBoundaryEvent(event));
        if(std::string_view(lazy.LazyStateMode())!="shader_size_alias")
            throw std::runtime_error("Previous shader-to-state identity was missed");
        swap.frame=1;swap.sequence=923;
        const auto actual=lazy.Push(GraphicsBoundaryEvent(swap)),expected=canonical.Push(GraphicsBoundaryEvent(swap));
        if(actual->operations.ToJson()!=expected->operations.ToJson()||actual->retained_bytes!=expected->retained_bytes)
            throw std::runtime_error("Previous shader-to-state alias changed operations or byte accounting");
        const auto hash=ContentHash(container);
        if(actual->ResourceAt(hash)==earlier->ResourceAt(hash,true))
            throw std::runtime_error("Previous shader owner leaked across payload namespace reuse rules");
    }
}

// The original and candidate producers consume identical immutable events.
// Compare serialized typed operations, independently hashed complete resource
// bytes, and canonical-owner equivalence classes across published generations.
void CheckDirectResourceHandoff(const GraphicsBoundaryEvent& source,const GraphicsBoundaryEvent& swap_source) {
    using Bytes=std::vector<uint8_t>;
    using Owner=std::shared_ptr<const Bytes>;
    using Planner=gow3::renderer::GearsLivePlan;
    size_t comparisons=0,owner_relations=0,prefixes=0,successful_handoffs=0;
#ifdef GOW3_NATIVE_VERIFY_PREFIX_DEPENDENCIES
    struct CollisionScope {
        size_t shader=test::live_shader_fingerprint_mask,state=test::live_state_fingerprint_mask;
        CollisionScope(){test::live_shader_fingerprint_mask=0;test::live_state_fingerprint_mask=0;}
        ~CollisionScope(){test::live_shader_fingerprint_mask=shader;test::live_state_fingerprint_mask=state;}
    } collision_scope;
#endif
    for(bool lazy:{false,true}) {
        std::array<std::vector<Owner>,2> observed;
        std::vector<std::pair<std::shared_ptr<LiveRenderFrame>,std::shared_ptr<LiveRenderFrame>>> retained;
        Owner original=std::make_shared<const Bytes>(source.blocks.back().bytes);
        Owner same=std::make_shared<const Bytes>(*original);
        auto changed_bytes=*original;changed_bytes.at(changed_bytes.size()/2)^=0x80;
        Owner changed=std::make_shared<const Bytes>(std::move(changed_bytes));
        Owner geometry=std::make_shared<const Bytes>(64,0x67);
        std::weak_ptr<const Bytes> original_weak=original,changed_weak=changed,geometry_weak=geometry;
        {
            Planner legacy({},false,lazy,false),candidate({},false,lazy,true);
            const auto compare=[&](const auto& a,const auto& b) {
                if(!a||!b||a->frame!=b->frame||a->retained_bytes!=b->retained_bytes||
                   a->operations.ToJson()!=b->operations.ToJson())
                    throw std::runtime_error("Direct resource handoff changed typed operation JSON or byte charges");
                for(size_t i=0;i<a->operations.size();++i) {
                    const auto* x=a->operations[i].Draw();const auto* y=b->operations[i].Draw();
                    if(bool(x)!=bool(y))throw std::runtime_error("Direct resource handoff changed typed operation representation");
                    if(!x)continue;
                    if(bool(x->lazy_device_state)!=bool(y->lazy_device_state))
                        throw std::runtime_error("Direct resource handoff changed lazy-state representation");
                    const auto refs=[&](const CanonicalResourceRef& p,const CanonicalResourceRef& q,bool shader) {
                        if(p.hash.View()!=q.hash.View()||bool(p.owner)!=bool(q.owner)||
                           (p.owner&&(*p.owner!=*q.owner||a->ResourceAt(p.hash.View(),shader)!=p.owner||
                            b->ResourceAt(q.hash.View(),shader)!=q.owner)))
                            throw std::runtime_error("Direct resource handoff changed immediate typed digest/owner publication");
                    };
                    if(!x->lazy_device_state)refs(x->device_state,y->device_state,false);
                    for(const auto& pair:{std::pair{&x->vertex_shader,&y->vertex_shader},std::pair{&x->pixel_shader,&y->pixel_shader}}) {
                        if(bool(*pair.first)!=bool(*pair.second))throw std::runtime_error("Direct resource handoff changed optional shader binding");
                        if(*pair.first)refs(**pair.first,**pair.second,true);
                    }
                    if(x->geometry.size()!=y->geometry.size()||x->up_geometry.size()!=y->up_geometry.size()||x->textures.size()!=y->textures.size())
                        throw std::runtime_error("Direct resource handoff changed typed resource arity");
                    for(size_t n=0;n<x->geometry.size();++n)refs(x->geometry[n].payload,y->geometry[n].payload,false);
                    for(size_t n=0;n<x->up_geometry.size();++n)refs(x->up_geometry[n].payload,y->up_geometry[n].payload,false);
                    for(size_t n=0;n<x->textures.size();++n) {
                        for(const auto& pair:{std::pair{&x->textures[n].base_payload,&y->textures[n].base_payload},
                                              std::pair{&x->textures[n].mip_payload,&y->textures[n].mip_payload}}) {
                            if(bool(*pair.first)!=bool(*pair.second))throw std::runtime_error("Direct resource handoff changed optional texture payload");
                            if(*pair.first)refs(**pair.first,**pair.second,false);
                        }
                    }
                }
                for(bool shader:{false,true}) {
                    const auto old=a->MaterializeResources(shader),fresh=b->MaterializeResources(shader);
                    if(old.size()!=fresh.size()||a->ResourceCount(shader)!=b->ResourceCount(shader)||
                       old.bucket_count()!=fresh.bucket_count())
                        throw std::runtime_error("Direct resource handoff changed canonical resource layout");
                    auto x=old.begin(),y=fresh.begin();
                    for(;x!=old.end();++x,++y) {
                        if(x->first!=y->first||*x->second!=*y->second||
                           picosha2::hash256_hex_string(*x->second)!=x->first||
                           picosha2::hash256_hex_string(*y->second)!=y->first)
                            throw std::runtime_error("Direct resource handoff changed canonical SHA or exact payload bytes");
                        observed[0].push_back(x->second);observed[1].push_back(y->second);++comparisons;
                    }
                }
            };
            uint64_t sequence=13000;
            for(uint32_t frame=0;frame<3;++frame) {
                for(unsigned i=0;i<8;++i) {
                    auto event=source;event.frame=frame;event.sequence=sequence++;
                    // Changes are in data and otherwise-unused shader header
                    // words, so equality tests retain the original valid ABI.
                    event.blocks[0].bytes.at(0x1a00)^=uint8_t(i>=4?0x40:0);
                    event.blocks[1].bytes.at(16)^=uint8_t((i&2)?0x20:0);
                    event.blocks.back().bytes.clear();
                    event.blocks.back().shared_bytes=i<3?original:(i==3?same:changed);
                    GraphicsBoundaryBlock block{14,0x30000};block.shared_bytes=geometry;event.blocks.push_back(std::move(block));
                    event.command_span=GraphicsCommandSpan{sequence,event.sequence,0x2000,{0xc0,0,0x36,0,0,3,0,0x84}};
                    legacy.Push(GraphicsBoundaryEvent(event));candidate.Push(GraphicsBoundaryEvent(event));
                    auto a=legacy.TakePacketPrefix(),b=candidate.TakePacketPrefix();
                    if(!a||!b||a->first_operation!=b->first_operation||a->operation_end!=b->operation_end||
                       a->span.generation!=b->span.generation||a->span.api_sequence!=b->span.api_sequence||
                       a->span.address!=b->span.address||a->span.bytes!=b->span.bytes)
                        throw std::runtime_error("Direct resource handoff changed immutable command-prefix ownership");
                    compare(a->batch,b->batch);retained.emplace_back(a->batch,b->batch);++prefixes;
                    const auto hash=picosha2::hash256_hex_string(*(i<4?original:changed));
                    if(a->batch->ResourceAt(hash)!=b->batch->ResourceAt(hash)||
                       a->batch->ResourceAt(hash)!=(i<4?original:changed))
                        throw std::runtime_error("Direct resource handoff changed supplied snapshot canonical-owner reuse");
                }
                auto swap=swap_source;swap.frame=frame;swap.sequence=sequence++;
                const auto a=legacy.Push(GraphicsBoundaryEvent(swap)),b=candidate.Push(GraphicsBoundaryEvent(swap));
                compare(a,b);retained.emplace_back(a,b);
            }
            // A validation failure must publish no successful handoff count.
            auto invalid=source;invalid.frame=3;invalid.sequence=sequence++;
            invalid.blocks.erase(invalid.blocks.begin()+1);
            const auto count=candidate.ResourceHandoffStats().handoffs;
            std::array<std::string,2> errors;
            for(unsigned mode=0;mode<2;++mode) {
                try {(mode?candidate:legacy).Push(GraphicsBoundaryEvent(invalid));}
                catch(const std::runtime_error& e){errors[mode]=e.what();}
            }
            if(errors[0].empty()||errors[0]!=errors[1]||candidate.ResourceHandoffStats().handoffs!=count)
                throw std::runtime_error("Direct resource handoff changed validation failure or counted an unpublished operation");
            if(legacy.ResourceHandoffStats().enabled||legacy.ResourceHandoffStats().handoffs||
               !candidate.ResourceHandoffStats().enabled||!count)
                throw std::runtime_error("Direct resource handoff did not execute a positive isolated option path");
            successful_handoffs+=count;
        }
        // All already-published prefixes/frames remain exact after producers die.
        for(const auto& [a,b]:retained)
            if(a->operations.ToJson()!=b->operations.ToJson())
                throw std::runtime_error("Direct resource handoff left a dangling published digest view");
        for(size_t i=0;i<observed[0].size();++i)for(size_t j=0;j<observed[0].size();++j) {
            if((observed[0][i]==observed[0][j])!=(observed[1][i]==observed[1][j]))
                throw std::runtime_error("Direct resource handoff changed deduplication owner equivalence");
            ++owner_relations;
        }
        original.reset();same.reset();changed.reset();geometry.reset();retained.clear();observed={};
        if(!original_weak.expired()||!changed_weak.expired()||!geometry_weak.expired())
            throw std::runtime_error("Direct resource handoff retained an owner after every producer and published consumer died");
    }
    std::cout<<"Direct resource differential: "<<prefixes<<" prefixes, "<<comparisons
             <<" exact SHA/byte comparisons, "<<owner_relations<<" owner relations, "
             <<successful_handoffs<<" successful handoffs; both lazy modes\n";
}

void CheckDeviceStateInterning(const GraphicsBoundaryEvent& source,const GraphicsBoundaryEvent& swap_source) {
    using Bytes=std::vector<uint8_t>;
    using Owner=std::shared_ptr<const Bytes>;
    auto states=std::array<Bytes,4>{source.blocks[0].bytes,source.blocks[0].bytes,
                                  source.blocks[0].bytes,source.blocks[0].bytes};
    states[1][0]^=0x41;states[2][0x1a00]^=0x23;states[3].back()^=0x71;
    std::array<std::string,4> hashes;
    for(size_t i=0;i<states.size();++i)hashes[i]=picosha2::hash256_hex_string(states[i]);
#ifdef GOW3_NATIVE_VERIFY_PREFIX_DEPENDENCIES
    struct CollisionScope {
        size_t saved=test::live_state_fingerprint_mask;
        CollisionScope(){test::live_state_fingerprint_mask=0;}
        ~CollisionScope(){test::live_state_fingerprint_mask=saved;}
    } collision_scope;
#endif
    TestLivePlan planner({},false,test_lazy_states);
    uint32_t frame=0;uint64_t sequence=0;
    auto push=[&](unsigned state,Owner expected={},bool shared=false,bool transferred=false) {
        auto event=source;event.frame=frame;event.sequence=sequence++;
        auto& block=event.blocks[0];block.bytes=states[state];block.shared_bytes.reset();
        Owner supplied;
        if(shared) {supplied=std::make_shared<const Bytes>(std::move(block.bytes));block.shared_bytes=supplied;}
        const auto* original=block.data().data();
        event.command_span=GraphicsCommandSpan{sequence,event.sequence,0x2000,{0xc0,0,0x36,0,0,3,0,0x84}};
        planner.Push(std::move(event));auto prefix=planner.TakePacketPrefix();
        if(event.blocks[0].data().size()!=states[state].size()||
           !std::equal(states[state].begin(),states[state].end(),event.blocks[0].data().begin()))
            throw std::runtime_error("State transfer discarded device bytes required by event diagnostics");
        if(!prefix)throw std::runtime_error("Device state lost its ordered draw prefix");
        const auto owner=prefix->batch->ResourceAt(hashes[state],false);
        if(*owner!=states[state]||prefix->batch->operations.back().ToJson().at("device_state")!=hashes[state]||
           (expected&&owner!=expected)||(transferred&&owner->data()!=original)||(shared&&!expected&&owner!=supplied))
            throw std::runtime_error("Device state changed canonical identity, exact bytes, or immutable ownership");
        return owner;
    };
    auto finish=[&] {
        auto event=swap_source;event.frame=frame++;event.sequence=sequence++;
        auto complete=planner.Push(std::move(event));
        size_t total=0;
        for(const auto& [key,bytes]:complete->MaterializeResources(false))total+=bytes->size();
        for(const auto& [key,bytes]:complete->MaterializeResources(true))total+=bytes->size();
        if(total!=complete->retained_bytes)throw std::runtime_error("Device state changed retained byte accounting");
        return complete;
    };
    const auto a=push(0,{},false,true);push(0,a);
    const auto b=push(1,{},false,true);push(0,a);
    const auto first=finish();
    // Generic payloads must recover the same previous-frame state owner even
    // though the device-state path no longer calculates generic fingerprints.
    planner.InvalidateMemory(0x30000,uint32_t(states[0].size()),states[0]);
    push(1,b);const auto c=push(2,{},true);
    const auto second=finish();
    if(second->MaterializeResources(false).at(hashes[0])!=a)
        throw std::runtime_error("Previous device-state owner was lost to a generic CPU payload");
    planner.InvalidateMemory(0x30000,uint32_t(states[2].size()),states[2]);
    push(0,a);push(3,{},false,true);push(2,c);
    const auto third=finish();
    if(third->MaterializeResources(false).at(hashes[2])!=c||*first->MaterializeResources(false).at(hashes[0])!=states[0])
        throw std::runtime_error("State reuse modified an older frame or lost a shared snapshot");
    finish(); // The intervening frame contains no device state.
    const auto fresh_c=push(2,{},false,true);
    if(fresh_c==c)throw std::runtime_error("Last-state digest cache extended previous-frame owner reuse");
    finish();
    // The opposite alias direction starts as a generic payload in this frame.
    planner.InvalidateMemory(0x30000,uint32_t(states[0].size()),states[0]);
    const auto generic_a=push(0);const auto last=finish();
    if(generic_a==a||last->MaterializeResources(false).at(hashes[0])!=generic_a)
        throw std::runtime_error("Current generic state alias reused an expired cache generation");
    {
        TestLivePlan lifetime({},false,test_lazy_states);
        auto event=source;event.frame=0;lifetime.Push(std::move(event));
        event.blocks.clear(); // The bridge releases its diagnostic event after the sink returns.
        auto swap=swap_source;swap.frame=0;auto complete=lifetime.Push(std::move(swap));
        std::weak_ptr<const Bytes> retired=complete->MaterializeResources(false).at(hashes[0]);complete.reset();
        swap=swap_source;swap.frame=1;lifetime.Push(std::move(swap));
        if(!retired.expired())throw std::runtime_error("Last-state cache retained bytes beyond the previous frame");
    }
    {
        // A repeated physical snapshot uses its identity directly after the
        // first frame, so legacy Intern has no previous fingerprint to find.
        // A state alias of that texture must not gain extra owner reuse.
        TestLivePlan sparse_index({},false,test_lazy_states);
        const auto texture=std::make_shared<const Bytes>(states[0]);
        for(uint32_t i=0;i<2;++i) {
            auto event=source;event.frame=i;event.blocks[0].bytes=states[1];
            event.blocks.back().bytes.clear();event.blocks.back().shared_bytes=texture;
            sparse_index.Push(std::move(event));
            auto swap=swap_source;swap.frame=i;sparse_index.Push(std::move(swap));
        }
        auto event=source;event.frame=2;const auto* owned=event.blocks[0].bytes.data();
        sparse_index.Push(std::move(event));
        auto swap=swap_source;swap.frame=2;const auto complete=sparse_index.Push(std::move(swap));
        if(complete->MaterializeResources(false).at(hashes[0])==texture||complete->MaterializeResources(false).at(hashes[0])->data()!=owned)
            throw std::runtime_error("State alias bypassed legacy previous fingerprint eligibility");
    }
    for(unsigned invalid=0;invalid<3;++invalid) {
        auto event=source;
        std::string expected;
        if(invalid==0) {event.blocks[0].bytes.pop_back();expected="Missing live device state";}
        if(invalid==1) {event.blocks.erase(event.blocks.begin()+1);expected="Missing live vertex shader:";}
        if(invalid==2) {event.blocks[0].bytes[0x483]&=~uint8_t(3);expected="Invalid live texture fetch binding";}
        const auto original_state=event.blocks[0].bytes;
        bool rejected=false;
        try {TestLivePlan malformed({},false,test_lazy_states);malformed.Push(std::move(event));}
        catch(const std::runtime_error& error) {rejected=std::string(error.what()).starts_with(expected);}
        if(!rejected)throw std::runtime_error("Device-state ownership transfer changed draw validation");
        if(event.blocks[0].data().size()!=original_state.size()||
           !std::equal(original_state.begin(),original_state.end(),event.blocks[0].data().begin()))
            throw std::runtime_error("Rejected draw lost its original device-state failure evidence");
    }
}

void CheckBoundaryBlockRecycling(const GraphicsBoundaryEvent& draw,const GraphicsBoundaryEvent& present) {
    NativeBoundaryBlockPool pool;
    std::vector<std::pair<std::shared_ptr<LiveRenderFrame>,std::shared_ptr<LiveRenderFrame>>> held;
    for(bool lazy:{false,true}) {
        GearsLivePlan baseline({},false,lazy),candidate({},false,lazy);
        for(unsigned frame=0;frame<5;++frame) {
            const auto push=[&](const GraphicsBoundaryEvent& original) {
                auto expected=original;expected.frame=frame;
                auto fresh=original;fresh.frame=frame;fresh.blocks.clear();
                pool.Begin(fresh);fresh.blocks.reserve(103);
                for(const auto& source:original.blocks) {
                    auto& block=fresh.blocks.emplace_back();block.kind=source.kind;block.address=source.address;
                    block.deferred_physical_bytes=source.deferred_physical_bytes;
                    if(source.shared_bytes)block.shared_bytes=source.shared_bytes;
                    else {
                        pool.Prepare(block.kind,source.bytes.size(),block.bytes,fresh.blocks.size()-1);
                        block.bytes.assign(source.bytes.begin(),source.bytes.end());
                        pool.Copied(source.bytes.size());
                    }
                }
                const auto a=baseline.Push(std::move(expected)),b=candidate.Push(std::move(fresh));
                // Retired scratch is immediately eligible for overwrite. Keep
                // every published frame alive across this and later reuse.
                pool.Reclaim(fresh);
                if(bool(a)!=bool(b))throw std::runtime_error("Boundary pool changed frame publication");
                if(a)held.emplace_back(a,b);
            };
            for(unsigned repeat=0;repeat<8;++repeat)push(draw);
            push(present);
        }
    }
    const auto stats=pool.statistics();
    if(!stats.outer_reuses||stats.buffer_reuses<100||stats.peak_retained_bytes>pool.kMaximumRetainedBytes)
        throw std::runtime_error("Boundary scratch recycling did not exercise bounded reuse");
    pool.Clear();
    for(const auto& [a,b]:held) {
        if(a->operations.ToJson()!=b->operations.ToJson()||a->retained_bytes!=b->retained_bytes)
            throw std::runtime_error("Recycled boundary scratch changed a retained frame");
        for(bool shader:{false,true}) {
            const auto x=a->MaterializeResources(shader),y=b->MaterializeResources(shader);
            if(x.size()!=y.size())throw std::runtime_error("Boundary recycling lost a resource");
            for(const auto& [key,owner]:x) {
                const auto found=y.find(key);
                if(found==y.end()||*owner!=*found->second||ContentHash(*found->second)!=key)
                    throw std::runtime_error("Retained resource bytes changed after boundary reuse");
            }
        }
    }
    std::cout<<"Boundary pool: "<<stats.buffer_reuses<<" buffer reuses; "<<held.size()<<" retained frames exact after pool destruction\n";
}

void CheckTextureLifetime() {
    auto put=[](std::vector<uint8_t>& bytes,size_t offset,uint32_t value) {
        for(unsigned i=0;i<4;++i)bytes.at(offset+i)=uint8_t(value>>(24-i*8));
    };
    auto shader=[&](bool pixel) {
        std::vector<uint8_t> v(pixel?68:72),p(24);
        put(v,0,pixel?0x102a1100:0x102a1141);put(v,4,v.size());put(v,8,p.size());put(v,24,36);put(v,40,p.size());
        const uint64_t exec=(2ull<<44)|1|(1<<12)|(pixel?1<<16:0);
        put(p,0,uint32_t(exec));put(p,4,uint32_t(exec>>32));put(p,12,pixel?1:0);
        return std::pair{v,p};
    };
    constexpr std::array<uint32_t,6> fetch{2|(1u<<22),0x10006,3|(3<<13),0,0,1<<9};
    GraphicsBoundaryEvent draw;draw.function=0x82AEFF00;draw.phase=1;draw.args={0x83000000,4,0,3,0,0,0,0};
    const auto [vs_v,vs_p]=shader(false);const auto [ps_v,ps_p]=shader(true);
    draw.blocks={{1,draw.args[0],std::vector<uint8_t>(0x3600)},{2,0x83004000,vs_v},{3,0x11000,vs_p},
        {4,0x83005000,ps_v},{5,0x12000,ps_p},{32,0x10000,std::vector<uint8_t>(512,0x22)}};
    for(unsigned i=0;i<6;++i)put(draw.blocks[0].bytes,0x480+i*4,fetch[i]);
    GraphicsBoundaryEvent resolve;resolve.function=0x82AF34A8;resolve.args[0]=draw.args[0];
    resolve.blocks={{1,draw.args[0],draw.blocks[0].bytes},{6,0x83006000,std::vector<uint8_t>(64)},
        {100,0x83007000,std::vector<uint8_t>(64)}};
    put(resolve.blocks[1].bytes,24,32);put(resolve.blocks[1].bytes,36,(3<<18)|(3<<3));
    for(unsigned i=0;i<6;++i)put(resolve.blocks[2].bytes,28+i*4,fetch[i]);
    {
        GraphicsBoundaryEvent present;present.function=0x82AF6B10;
        present.blocks={resolve.blocks[0],resolve.blocks[2]};present.blocks[1].kind=103;
        CheckDirectResourceHandoff(draw,present);
        CheckBoundaryBlockRecycling(draw,present);
        CheckDeviceStateInterning(draw,present);
        CheckLazyStateLifecycle(draw,present);
        CheckUnrelatedStateSizedResources(draw,present);
    }
    {
#ifdef GOW3_NATIVE_VERIFY_PREFIX_DEPENDENCIES
        // All shaders collide in both real planner indexes for this fixture.
        // Current/previous-frame reuse must still compare both exact spans.
        struct FingerprintScope {
            size_t previous=test::live_shader_fingerprint_mask;
            FingerprintScope(){test::live_shader_fingerprint_mask=0;}
            ~FingerprintScope(){test::live_shader_fingerprint_mask=previous;}
        } fingerprints;
        const auto allocations_before=test::live_shader_snapshot_allocations;
#endif
        std::array<std::shared_ptr<LiveRenderFrame>,4> frames;
        std::vector<uint8_t> original_vertex=vs_v;
        original_vertex.insert(original_vertex.end(),vs_p.begin(),vs_p.end());
        const auto original_digest=picosha2::hash256_hex_string(original_vertex);
        {
            TestLivePlan shader_reuse({},false,test_lazy_states);
            for(uint32_t frame=0;frame<frames.size();++frame) {
                auto operation=draw;operation.frame=frame;
                if(frame==2)operation.blocks[1].bytes[16]^=0x40; // Header-only change.
                if(frame==3)operation.blocks[2].bytes.back()^=0x20; // Code-only change.
                auto expected=operation.blocks[1].bytes;
                expected.insert(expected.end(),operation.blocks[2].bytes.begin(),operation.blocks[2].bytes.end());
                const auto digest=picosha2::hash256_hex_string(expected);
                operation.sequence=frame*3;shader_reuse.Push(GraphicsBoundaryEvent(operation));
                ++operation.sequence;shader_reuse.Push(std::move(operation)); // Same-frame hit.
                GraphicsBoundaryEvent present;present.function=0x82AF6B10;present.frame=frame;present.sequence=frame*3+2;
                present.blocks={resolve.blocks[0],resolve.blocks[2]};present.blocks[1].kind=103;
                frames[frame]=shader_reuse.Push(std::move(present));
                if(frames[frame]->MaterializeResources(true).size()!=2||*frames[frame]->MaterializeResources(true).at(digest)!=expected||
                   frames[frame]->operations[0].ToJson()["vertex_shader"]!=digest||frames[frame]->operations[1].ToJson()["vertex_shader"]!=digest)
                    throw std::runtime_error("Segmented shader interning changed canonical bytes or digest under collisions");
                if(frame>=2&&digest==original_digest)
                    throw std::runtime_error("Changed shader segment reused stale content identity");
            }
        }
        if(frames[0]->MaterializeResources(true).at(original_digest)!=frames[1]->MaterializeResources(true).at(original_digest)||
           *frames[0]->MaterializeResources(true).at(original_digest)!=original_vertex)
            throw std::runtime_error("Segmented shader cache lost previous-frame reuse or immutable lifetime");
#ifdef GOW3_NATIVE_VERIFY_PREFIX_DEPENDENCIES
        if(test::live_shader_snapshot_allocations-allocations_before!=4)
            throw std::runtime_error("Segmented shader cache allocated concatenated bytes on a cache hit");
#endif
    }
    {
        // Late packet prefixes retain matches in both namespaces, even when
        // the second namespace acquired the same blob after the original op.
        std::vector<uint8_t> shader_blob=vs_v;
        shader_blob.insert(shader_blob.end(),vs_p.begin(),vs_p.end());
        const auto digest=picosha2::hash256_hex_string(shader_blob);
        std::optional<LivePacketPrefix> retained;
        {
            TestLivePlan aliases({},false,test_lazy_states);
            aliases.InvalidateMemory(0x30000,uint32_t(shader_blob.size()),shader_blob);
            auto first=resolve;first.sequence=100;aliases.Push(std::move(first));
            // Force the source payload map to rehash before the first prefix
            // dereferences its recorded digest. These unrelated writes must
            // not appear in that earlier prefix.
            for(unsigned i=0;i<1024;++i) {
                std::vector<uint8_t> bytes(16);put(bytes,0,i);
                aliases.InvalidateMemory(0x40000+i*16,16,std::move(bytes));
            }
            auto second=draw;second.sequence=101;
            // The shader digest appears as a shader and twice as geometry;
            // each namespace must still count its shared bytes only once.
            second.blocks.push_back({14,0x60000,shader_blob});
            second.blocks.push_back({15,0x61000,shader_blob});
            aliases.Push(std::move(second));
            GraphicsBoundaryEvent commit;commit.span_only=true;
            commit.command_span=GraphicsCommandSpan{1,100,0x2000,{0xc0,0,0x36,0,0,3,0,0x84}};
            aliases.Push(GraphicsBoundaryEvent(commit));retained=aliases.TakePacketPrefix();
            if(!retained||retained->batch->operations.size()!=2||retained->batch->ResourceCount(false)!=1||
               retained->batch->ResourceCount(true)!=1||retained->batch->retained_bytes!=shader_blob.size()*2||
               *retained->batch->ResourceAt(digest,false)!=shader_blob||*retained->batch->ResourceAt(digest,true)!=shader_blob)
                throw std::runtime_error("Late prefix lost cross-namespace aliases or retained unrelated later payloads");
            commit.command_span->generation=2;commit.command_span->api_sequence=101;
            aliases.Push(GraphicsBoundaryEvent(commit));auto tail=aliases.TakePacketPrefix();
            if(!tail||tail->batch->operations.size()!=1025||
               *tail->batch->ResourceAt(digest,false)!=shader_blob||*tail->batch->ResourceAt(digest,true)!=shader_blob)
                throw std::runtime_error("Rehashed operation dependencies lost later payloads or duplicate aliases");
            aliases.Push(std::move(commit));auto empty=aliases.TakePacketPrefix();
            if(!empty||!empty->batch->operations.empty()||(empty->batch->ResourceCount(false)!=0)||
               (empty->batch->ResourceCount(true)!=0)||empty->batch->retained_bytes)
                throw std::runtime_error("Empty packet prefix retained operation dependencies");
            GraphicsBoundaryEvent present;present.function=0x82AF6B10;present.sequence=102;
            present.blocks={resolve.blocks[0],resolve.blocks[2]};present.blocks[1].kind=103;
            aliases.Push(std::move(present));
        }
        if(!retained||*retained->batch->ResourceAt(digest,false)!=shader_blob||
           *retained->batch->ResourceAt(digest,true)!=shader_blob)
            throw std::runtime_error("Producer destruction invalidated cross-namespace prefix owners");
    }
    {
        // Interning occurs before all draw checks finish. Failed construction
        // must not add unused state/shader dependencies to the next operation.
        TestLivePlan failed({},false,test_lazy_states);
        auto invalid=draw;invalid.blocks.erase(invalid.blocks.begin()+1);
        bool rejected=false;try{failed.Push(std::move(invalid));}catch(const std::runtime_error&){rejected=true;}
        if(!rejected)throw std::runtime_error("Invalid draw dependency fixture was accepted");
        auto next=resolve;next.sequence=110;
        next.command_span=GraphicsCommandSpan{1,110,0x2000,{0xc0,0,0x36,0,0,3,0,0x84}};
        failed.Push(std::move(next));const auto prefix=failed.TakePacketPrefix();
        if(!prefix||prefix->batch->operations.size()!=1||prefix->batch->retained_bytes||
           (prefix->batch->ResourceCount(false)!=0)||(prefix->batch->ResourceCount(true)!=0))
            throw std::runtime_error("Failed draw leaked unemitted dependencies into a later prefix");
    }
    {
        // Deferred CPU reads happen only after temporal ownership selection.
        // An accepted prefix must own its bytes after the caller and source go
        // away; later GPU/CPU ownership changes cannot retroactively alter it.
        DeferredTextureSnapshotScope scope;
        size_t captures=0;
        std::vector<uint8_t> cpu_base(512,0x22),cpu_mips(512,0x66);
        auto defer=[&](GraphicsBoundaryEvent event) {
            for(auto& block:event.blocks)if(block.kind>=32&&block.kind<=95&&!block.bytes.empty()) {
                block.deferred_physical_bytes=uint32_t(block.bytes.size());block.bytes.clear();
            }
            event.materialize_texture=scope.Bind([&](uint32_t address,uint32_t count) {
                ++captures;const auto& source=address==0x10000?cpu_base:cpu_mips;
                if(source.size()!=count)throw std::runtime_error("Deferred fixture extent mismatch");
                return std::make_shared<const std::vector<uint8_t>>(source);
            });
            return event;
        };
        TestLivePlan lazy({},true,test_lazy_states);
        auto first=defer(draw);first.sequence=40;
        first.command_span=GraphicsCommandSpan{1,40,0x2000,{0xc0,0,0x36,0,0,3,0,0x84}};
        lazy.Push(std::move(first));auto retained=lazy.TakePacketPrefix();
        if(captures!=1||!retained)throw std::runtime_error("CPU texture was not materialized at its ordered prefix");
        const auto original=retained->batch->operations[0].ToJson()["textures"][0]["base_payload"].get<std::string>();
        std::fill(cpu_base.begin(),cpu_base.end(),0x55);
        lazy.Push(GraphicsBoundaryEvent(resolve));lazy.Push(defer(draw));
        if(captures!=1)throw std::runtime_error("GPU-only texture read its discarded CPU mapping");
        lazy.InvalidateMemory(0x10000,4096,std::vector<uint8_t>(4096,0x55),91);
        lazy.Push(defer(draw));
        if(captures!=2)throw std::runtime_error("CPU invalidation failed to restore deferred texture materialization");
        auto partial=resolve;
        partial.blocks.push_back({98,0x83008000,std::vector<uint8_t>(16)});
        put(partial.blocks.back().bytes,8,1);put(partial.blocks.back().bytes,12,1);
        lazy.Push(std::move(partial));lazy.Push(defer(draw));
        if(captures!=3)throw std::runtime_error("Partial GPU texture skipped required CPU bytes");
        lazy.Push(GraphicsBoundaryEvent(resolve));
        auto mip_draw=draw;
        put(mip_draw.blocks[0].bytes,0x480+16,1<<6);
        put(mip_draw.blocks[0].bytes,0x480+20,0x20000|(1<<9));
        mip_draw.blocks.push_back({33,0x20000,cpu_mips});lazy.Push(defer(std::move(mip_draw)));
        if(captures!=5)throw std::runtime_error("Deferred mip chain lost CPU base/mip ownership");
        GraphicsBoundaryEvent present;present.function=0x82AF6B10;
        present.blocks={resolve.blocks[0],resolve.blocks[2]};present.blocks[1].kind=103;
        const auto frame=lazy.Push(std::move(present));
        if(frame->operations[2].ToJson()["textures"][0]["source"]!="gpu_alias_or_previous_frame"||
           !frame->operations[2].ToJson()["textures"][0]["base_payload"].is_null()||
           frame->operations[4].ToJson()["textures"][0]["source"]!="cpu_snapshot"||
           retained->batch->ResourceAt(original,false)->at(0)!=0x22)
            throw std::runtime_error("Deferred snapshots changed ordered texture provenance or immutable prefix lifetime");
        // Supplying a descriptor without the synchronous capability is an
        // explicit capture failure, never a fabricated empty texture.
        auto missing=defer(draw);missing.materialize_texture={};
        bool rejected=false;try {TestLivePlan invalid({},false,test_lazy_states);invalid.Push(std::move(missing));}
        catch(const std::runtime_error&){rejected=true;}
        if(!rejected)throw std::runtime_error("Deferred texture without a capture capability was accepted");
    }
    for(bool tiled:{false,true})for(uint32_t endian=0;endian<4;++endian) {
        // A one-byte resolve defines its endian-relocated byte only. An
        // adjacent CPU write must not revoke it, whereas a write to that byte
        // must retain the CPU snapshot for the mixed-ownership upload path.
        constexpr uint32_t swaps[]{0,1,3,2};
        const uint32_t owned=0x10000+swaps[endian];
        auto byte_fetch=fetch;
        byte_fetch[0]|=tiled?0x80000000u:0;
        byte_fetch[1]=0x10002|(endian<<6);byte_fetch[2]=0;
        auto byte_draw=draw,byte_resolve=resolve;
        for(unsigned i=0;i<6;++i) {
            put(byte_draw.blocks[0].bytes,0x480+i*4,byte_fetch[i]);
            put(byte_resolve.blocks[2].bytes,28+i*4,byte_fetch[i]);
        }
        std::vector<std::pair<uint32_t,uint32_t>> writes;
        TestLivePlan bytes([&](uint32_t address,uint32_t count){writes.emplace_back(address,count);});
        bytes.Push(std::move(byte_resolve));bytes.Push(GraphicsBoundaryEvent(byte_draw));
        bytes.InvalidateMemory(owned^1u,1,{0x55},101);
        bytes.Push(GraphicsBoundaryEvent(byte_draw));
        bytes.InvalidateMemory(owned,1,{0x66},102);
        byte_draw.blocks.back().bytes.at(swaps[endian])=0x66;
        bytes.Push(std::move(byte_draw));
        GraphicsBoundaryEvent present;present.function=0x82AF6B10;
        present.blocks={resolve.blocks[0],resolve.blocks[2]};present.blocks[1].kind=103;
        const auto frame=bytes.Push(std::move(present));
        const auto before=frame->operations.at(1).ToJson().at("textures").at(0);
        const auto adjacent=frame->operations.at(3).ToJson().at("textures").at(0);
        const auto overwritten=frame->operations.at(5).ToJson().at("textures").at(0);
        if(writes!=std::vector<std::pair<uint32_t,uint32_t>>{{owned,1}}||
           before.at("source")!="gpu_alias_or_previous_frame"||
           adjacent.at("source")!="gpu_alias_or_previous_frame"||
           overwritten.at("source")!="cpu_snapshot"||overwritten.at("base_payload").is_null()||
           frame->operations.at(4).ToJson().at("coherence_epoch")!=102)
            throw std::runtime_error("One-byte resolve ownership lost its exact endian footprint");
    }
    {
        std::vector<std::pair<uint32_t,uint32_t>> writes;
        TestLivePlan rectangles([&](uint32_t address,uint32_t bytes){writes.emplace_back(address,bytes);});
        auto partial=resolve;
        partial.blocks.push_back({98,0x83008000,std::vector<uint8_t>(16)});
        partial.blocks.push_back({101,0x83008100,std::vector<uint8_t>(8)});
        put(partial.blocks[3].bytes,8,1);put(partial.blocks[3].bytes,12,1);
        // Same image, different one-pixel rectangles, then reuse the first.
        // Linear RGBA8 pitch is 32 pixels. Cache reuse must never widen or
        // relocate the byte range declared GPU-owned.
        for(uint32_t coordinate:{0u,3u,0u}) {
            put(partial.blocks[4].bytes,0,coordinate);put(partial.blocks[4].bytes,4,coordinate);
            rectangles.Push(GraphicsBoundaryEvent(partial));
        }
        const std::vector<std::pair<uint32_t,uint32_t>> expected{{0x10000,4},{0x1018c,4},{0x10000,4}};
        if(writes!=expected)throw std::runtime_error("Cached resolve footprint changed exact rectangle ownership");
    }
    {
        TestLivePlan packets({},true,test_lazy_states);
        auto operation=draw;operation.sequence=40;
        operation.command_span=GraphicsCommandSpan{1,40,0x2000,{0xc0,0,0x36,0,0,3,0,0x84}};
        packets.Push(std::move(operation));auto first=packets.TakePacketPrefix();
        if(!first||first->first_operation!=0||first->operation_end!=1||first->span.generation!=1)
            throw std::runtime_error("Draw chunk packet prefix was not emitted");
        auto copied=resolve;copied.sequence=41;packets.Push(std::move(copied));
        GraphicsBoundaryEvent commit;commit.frame=0;commit.span_only=true;
        commit.command_span=GraphicsCommandSpan{2,41,0x3000,{0xc0,0,0x36,0,0,3,0,0x84}};
        packets.Push(GraphicsBoundaryEvent(commit));auto next=packets.TakePacketPrefix();
        if(!next||next->first_operation!=1||next->operation_end!=2||next->batch->operations[0].ToJson().at("type")!="resolve")
            throw std::runtime_error("Resolve commit lost its previously captured operation");
        commit.command_span->generation=3;commit.command_span->address=0x4000;
        packets.Push(std::move(commit));auto tail=packets.TakePacketPrefix();
        if(!tail||tail->first_operation!=2||tail->operation_end!=2||!tail->batch->operations.empty())
            throw std::runtime_error("A second resolve packet repeated its native operation");
        const auto digest=first->batch->operations[0].ToJson().at("textures")[0].at("base_payload").get<std::string>();
        if(first->batch->ResourceAt(digest,false)->at(0)!=0x22)
            throw std::runtime_error("A later resolve changed the immutable draw packet input");
        GraphicsBoundaryEvent unmatched;unmatched.span_only=true;
        unmatched.command_span=GraphicsCommandSpan{4,999,0x5000,{0xc0,0,0x36,0,0,3,0,0x84}};
        bool rejected=false;try{packets.Push(std::move(unmatched));}catch(const std::runtime_error&){rejected=true;}
        if(!rejected)throw std::runtime_error("Unowned command commit was accepted");
    }
    {
        // One GPU texel cannot displace the CPU bytes of an entire image.
        // A later CPU overwrite also must not restore GPU-only classification.
        TestLivePlan mixed({},false,test_lazy_states);
        auto partial=resolve;
        partial.blocks.push_back({98,0x83008000,std::vector<uint8_t>(16)});
        put(partial.blocks.back().bytes,8,1);put(partial.blocks.back().bytes,12,1);
        mixed.Push(std::move(partial));mixed.Push(GraphicsBoundaryEvent(draw));
        mixed.Push(GraphicsBoundaryEvent(resolve));
        mixed.InvalidateMemory(0x10000,4,std::vector<uint8_t>(4,0x55),77);
        auto changed=draw;std::fill_n(changed.blocks.back().bytes.begin(),4,0x55);
        mixed.Push(std::move(changed));
        GraphicsBoundaryEvent present;present.function=0x82AF6B10;
        present.blocks={resolve.blocks[0],resolve.blocks[2]};present.blocks[1].kind=103;
        const auto frame=mixed.Push(std::move(present));
        const auto before=frame->operations.at(1).ToJson().at("textures").at(0);
        const auto after=frame->operations.at(4).ToJson().at("textures").at(0);
        if(before.at("source")!="cpu_snapshot"||after.at("source")!="cpu_snapshot"||
           before.at("base_payload").is_null()||after.at("base_payload").is_null()||
           before.at("base_payload")==after.at("base_payload")||
           frame->operations.at(3).ToJson().at("coherence_epoch")!=77)
            throw std::runtime_error("Partial GPU texture ownership lost CPU bytes or CPU-write ordering");
    }
    {
        // CPU base and a GPU-produced mip retain both immutable regions for
        // exact overlay. Mip-page reuse invalidates only that GPU ownership.
        TestLivePlan mixed({},false,test_lazy_states);
        auto mip_resolve=resolve;
        put(mip_resolve.blocks[2].bytes,32,0x20006);
        put(mip_resolve.blocks[2].bytes,36,1|(1<<13));
        mixed.Push(GraphicsBoundaryEvent(mip_resolve));
        auto mip_draw=draw;
        put(mip_draw.blocks[0].bytes,0x480+16,1<<6);
        put(mip_draw.blocks[0].bytes,0x480+20,0x20000|(1<<9));
        mip_draw.blocks.push_back({33,0x20000,std::vector<uint8_t>(512,0x66)});
        mixed.Push(GraphicsBoundaryEvent(mip_draw));
        // Even if the base becomes fully GPU-owned, the sampled mip chain
        // still uses the mixed-payload path supported by the executor.
        mixed.Push(GraphicsBoundaryEvent(resolve));mixed.Push(GraphicsBoundaryEvent(mip_draw));
        mixed.InvalidateMemory(0x20000,4096,std::vector<uint8_t>(4096,0x77),88);
        mip_draw.blocks.back().bytes.assign(512,0x77);mixed.Push(std::move(mip_draw));
        GraphicsBoundaryEvent present;present.function=0x82AF6B10;
        present.blocks={resolve.blocks[0],resolve.blocks[2]};present.blocks[1].kind=103;
        const auto frame=mixed.Push(std::move(present));
        for(unsigned index:{1u,3u,5u}) {
            const auto texture=frame->operations.at(index).ToJson().at("textures").at(0);
            if(texture.at("source")!="cpu_snapshot"||texture.at("base_payload").is_null()||texture.at("mip_payload").is_null())
                throw std::runtime_error("GPU mip ownership discarded required CPU base/mip payloads");
        }
        if(frame->operations.at(1).ToJson().at("textures").at(0).at("mip_payload")==
           frame->operations.at(5).ToJson().at("textures").at(0).at("mip_payload")||
           frame->operations.at(4).ToJson().at("coherence_epoch")!=88)
            throw std::runtime_error("Mip CPU overwrite reused a stale immutable payload or epoch");
    }
    TestLivePlan planner({},false,test_lazy_states);planner.Push(GraphicsBoundaryEvent(draw));planner.Push(GraphicsBoundaryEvent(resolve));
    // A stale CPU mapping of a GPU-owned image must neither become a texture
    // input nor consume the immutable snapshot budget.
    auto gpu_draw=draw;gpu_draw.blocks.back().bytes.assign(512,0x44);
    const auto unused_gpu_digest=picosha2::hash256_hex_string(gpu_draw.blocks.back().bytes);
    planner.Push(std::move(gpu_draw));
    planner.InvalidateMemory(0x10000,4096,std::vector<uint8_t>(4096,0x33));
    draw.blocks.back().bytes.assign(512,0x33);planner.Push(std::move(draw));
    put(resolve.blocks[2].bytes,40,0x60a<<1);
    planner.Push(GraphicsBoundaryEvent(resolve));
    GraphicsBoundaryEvent present;present.function=0x82AF6B10;
    present.blocks={resolve.blocks[0],resolve.blocks[2]};present.blocks[1].kind=103;
    const auto frame=planner.Push(std::move(present));
    if(!frame||frame->operations.size()!=7||
        frame->operations.at(0).ToJson().at("textures").at(0).at("source")!="cpu_snapshot"||
        frame->operations.at(2).ToJson().at("textures").at(0).at("source")!="gpu_alias_or_previous_frame"||
        frame->operations.at(4).ToJson().at("textures").at(0).at("source")!="cpu_snapshot"||
        !frame->operations.at(2).ToJson().at("textures").at(0).at("base_payload").is_null())
        throw std::runtime_error("Texture ownership was changed by a later GPU resolve or CPU overwrite");
    if(frame->operations.at(1).ToJson().at("copy_dest_swap")!=false||frame->operations.at(5).ToJson().at("copy_dest_swap")!=true)
        throw std::runtime_error("Live resolve destination swap did not follow captured texture header");
    if(frame->MaterializeResources(false).contains(unused_gpu_digest))
        throw std::runtime_error("Unused CPU bytes of a GPU-owned image consumed snapshot storage");
}
}
int main(int argc,char** argv) {
    try {
        test_fast_fingerprints=std::getenv("GOW3_TEST_FAST_FINGERPRINTS")!=nullptr;
        std::cout<<"Fast snapshot fingerprint fixture mode: "<<test_fast_fingerprints<<"\n";
#ifdef GOW3_NATIVE_VERIFY_PREFIX_DEPENDENCIES
        if(std::getenv("GOW3_TEST_COLLIDE_PAYLOAD_FINGERPRINTS"))test::live_payload_fingerprint_mask=0;
#endif
        test_direct_refs=std::getenv("GOW3_TEST_DIRECT_RESOURCE_REFS")!=nullptr;
        std::cout<<"Direct canonical resource handoff fixture mode: "<<test_direct_refs<<"\n";
        test_lazy_states=std::getenv("GOW3_TEST_LAZY_STATES")!=nullptr||
            (argc>=4&&std::string_view(argv[3])=="--lazy-device-states");
        CheckImmutableOperations();
        CheckDeferredSnapshotScope();
        CheckQueryABI();
        CheckThinLiveMetadata();
        CheckTextureLifetime();
        if(argc==3&&std::string(argv[1])=="--memory-copy-prefix") {
            CheckMemoryCopyPrefixes(argv[2]);
            CheckChunkMemoryCopyBlock(argv[2]);
            std::cout<<"Memory-copy CPU/GPU and late diagnostic-hash prefix dependencies passed\n";
#ifdef GOW3_NATIVE_VERIFY_PREFIX_DEPENDENCIES
            std::cout<<"Legacy prefix dependency oracle: "<<test::live_prefix_dependency_checks<<" exact comparisons passed\n";
        std::cout<<"Prefix storage: "<<test::live_flat_prefixes<<" flat, "<<test::live_map_prefixes<<" legacy-map fallback\n";
#endif
            return 0;
        }
        if(argc==2&&std::string(argv[1])=="--queries") {
            std::cout<<"Live query ABI, ordered immutable prefixes/report generations, CPU write/free ordering, and temporal texture ownership passed\n";
#ifdef GOW3_NATIVE_VERIFY_PREFIX_DEPENDENCIES
            std::cout<<"Legacy prefix dependency oracle: "<<test::live_prefix_dependency_checks<<" exact comparisons passed\n";
        std::cout<<"Prefix storage: "<<test::live_flat_prefixes<<" flat, "<<test::live_map_prefixes<<" legacy-map fallback\n";
#endif
            return 0;
        }
        const bool write_exact=argc>=5&&std::string(argv[argc-2])=="--write-exact-plan";
        const int input_argc=argc-(write_exact?2:0);
        std::ofstream exact_plan;
        if(write_exact) {
            exact_plan.open(argv[argc-1],std::ios::binary);
            if(!exact_plan)throw std::runtime_error("Cannot open exact live plan output");
        }
        const bool thin_metadata=input_argc==4&&std::string(argv[3])=="--thin-live-metadata";
        const bool deferred=thin_metadata||(input_argc==4&&(std::string(argv[3])=="--deferred-snapshots"||std::string(argv[3])=="--lazy-device-states"));
        const bool shared=deferred||(input_argc==4&&std::string(argv[3])=="--shared-snapshots");
        const bool selective=shared||(input_argc==4&&std::string(argv[3])=="--selective-snapshots");
        if(input_argc!=3&&!selective)throw std::runtime_error("usage: live-plan-test capture.g3d scene-plan.json [--selective-snapshots|--shared-snapshots|--deferred-snapshots|--thin-live-metadata] [--write-exact-plan output.jsonl]");
        const auto bytes=Read(argv[1]);ImmutableSnapshots snapshots;
        ShaderInputCache thin_inputs;LiveMetadataCaptureStatistics thin_stats;
        if(LE(bytes,8)!=6)throw std::runtime_error("Version-6 Present resource required");
        std::ifstream source(argv[2]);Json expected;source>>expected;
        TestLivePlan planner({},false,test_lazy_states);Json operations=Json::array();
        std::map<uint32_t,std::pair<size_t,size_t>> references;
        size_t at=16,event_index=0,frames=0,largest_frame=0,deferred_bytes=0,materialized_bytes=0;
        while(at<bytes.size()) {
            const auto size=LE(bytes,at),function=LE(bytes,at+4);
            if(!function)break;
            if(size<60||at+size>bytes.size())throw std::runtime_error("Invalid event size");
            GraphicsBoundaryEvent event;event.sequence=event_index++;event.function=function;
            event.phase=LE(bytes,at+8);event.frame=LE(bytes,at+12);event.lr=LE(bytes,at+16);
            for(unsigned i=0;i<8;++i)event.args[i]=LE(bytes,at+20+i*4);
            event.result=LE(bytes,at+52);
            size_t cursor=at+60;
            for(unsigned i=0;i<LE(bytes,at+56);++i) {
                GraphicsBoundaryBlock block;block.kind=LE(bytes,cursor);block.address=LE(bytes,cursor+4);
                const auto length=LE(bytes,cursor+8);cursor+=12;size_t start=cursor,count=length;
                if(length&0x80000000u) {const auto previous=references.at(length&0x7fffffffu);start=previous.first;count=previous.second;}
                else {if(length)references.emplace(uint32_t(cursor),std::pair{cursor,size_t(length)});cursor+=length;}
                if(start+count>bytes.size())throw std::runtime_error("Capture payload out of bounds");
                block.bytes.assign(bytes.begin()+start,bytes.begin()+start+count);event.blocks.push_back(std::move(block));
            }
            if(cursor!=at+size)throw std::runtime_error("Event block extent mismatch");
            if(selective)if(const auto required=SnapshotShaderInputs(event.blocks))
                for(auto& block:event.blocks) {
                    if(block.kind>=14&&block.kind<=29&&!required->first.contains(95-(block.kind-14)))block.bytes.clear();
                    if(block.kind>=32&&block.kind<=95&&!required->second.contains((block.kind-32)/2))block.bytes.clear();
                }
            if(thin_metadata) {
                const auto required=thin_inputs.Get(event.blocks);
                bool header=false,code=false;
                for(const auto& block:event.blocks) {
                    if(block.kind==2)header=!block.data().empty();
                    if(block.kind==3)code=!block.data().empty();
                }
                const bool draw=(event.phase==1&&(event.function==0x82AEFF00||event.function==0x82AF0318))||
                    (event.phase==2&&(event.function==0x82AEF500||event.function==0x82AEF9C0));
                const bool live=draw||(event.phase==0&&(event.function==0x82AF6B10||
                    event.function==0x82AF16D8||event.function==0x82AF34A8||event.function==0x82AF0790));
                const LiveMetadataCapturePolicy policy{true,live,false,draw&&required.has_value()&&header&&code};
                for(auto& block:event.blocks)if(policy.Omit(block.kind)) {
                    thin_stats.Record(block.kind,uint32_t(block.bytes.size()),!block.bytes.empty());
                    block.bytes.clear();
                }
            }
            DeferredTextureSnapshotScope snapshot_scope;
            std::map<std::pair<uint32_t,uint32_t>,std::shared_ptr<const std::vector<uint8_t>>> texture_sources;
            if(deferred) {
                for(auto& block:event.blocks)if(block.kind>=32&&block.kind<=95&&!block.bytes.empty()) {
                    const auto count=uint32_t(block.bytes.size());deferred_bytes+=count;
                    const auto source=std::make_shared<const std::vector<uint8_t>>(std::move(block.bytes));
                    const auto [it,inserted]=texture_sources.emplace(std::pair{block.address,count},source);
                    if(!inserted&&*it->second!=*source)throw std::runtime_error("Captured texture aliases disagree within an event");
                    block.deferred_physical_bytes=count;
                }
                event.materialize_texture=snapshot_scope.Bind([&](uint32_t address,uint32_t count) {
                    materialized_bytes+=count;const auto source=texture_sources.at({address,count});
                    return count>=4096?snapshots.Get(address,*source):source;
                });
            }
            if(shared)for(auto& block:event.blocks)if(block.kind>=14&&block.kind<=95&&block.bytes.size()>=4096) {
                block.shared_bytes=snapshots.Get(block.address,block.bytes);block.bytes.clear();
            }
            if(auto frame=planner.Push(std::move(event))) {
                size_t retained=0;
                for(const auto& [digest,payload]:frame->MaterializeResources(false))retained+=payload->size();
                for(const auto& [digest,shader]:frame->MaterializeResources(true))retained+=shader->size();
                if(retained!=frame->retained_bytes)throw std::runtime_error("Snapshot reuse double-counted retained payloads");
                if(write_exact) {
                    // Export before normalization or initial-history exclusions.
                    // Rehash actual immutable bytes independently of map keys.
                    Json exact={{"frame",frame->frame},{"retained_bytes",frame->retained_bytes},
                        {"operations",frame->operations.ToJson()},{"payloads",Json::object()},{"shaders",Json::object()}};
                    for(const auto& [key,payload]:frame->MaterializeResources(false))
                        exact["payloads"][key]={{"bytes",payload->size()},{"sha256",ContentHash(*payload)}};
                    for(const auto& [key,shader]:frame->MaterializeResources(true))
                        exact["shaders"][key]={{"bytes",shader->size()},{"sha256",ContentHash(*shader)}};
                    exact_plan<<exact.dump()<<'\n';
                    if(!exact_plan)throw std::runtime_error("Cannot write exact live plan output");
                }
                ++frames;largest_frame=std::max(largest_frame,frame->retained_bytes);
                for(const auto& op:frame->operations) {
                    if(op.Kind()=="draw"&&!op.Draw())throw std::runtime_error("Live ordinary draw unexpectedly retained JSON");
                    operations.push_back(op.ToJson());
                }
            }
            at+=size;
        }
        if(operations.size()!=expected["operations"].size())throw std::runtime_error("Operation count mismatch");
        size_t unknown_initial_bindings=0;
        for(size_t i=0;i<operations.size();++i) {
            auto want=expected["operations"][i];Normalize(want,selective);Normalize(operations[i],selective);
            // These captures begin mid-game and do not include the ownership
            // history from guest boot. The offline plan also uses future
            // producers. Compare the API/layout and same-provenance payloads;
            // explicitly exclude differing temporal provenance here. Dedicated
            // ordered fixtures above validate the live ownership contract.
            if(want.at("type")=="draw"&&want.at("textures").size()==operations[i].at("textures").size())
                for(size_t slot=0;slot<want.at("textures").size();++slot) {
                    auto& a=want["textures"][slot];auto& b=operations[i]["textures"][slot];
                    if(a.at("source")==b.at("source"))continue;
                    ++unknown_initial_bindings;
                    for(auto* binding:{&a,&b}) {
                        binding->erase("source");binding->erase("base_payload");binding->erase("mip_payload");
                    }
                }
            if(want!=operations[i]) {
                std::cerr<<"Operation "<<i<<" event "<<operations[i]["event"]<<" differs\n"
                         <<Json::diff(want,operations[i]).dump(2)<<'\n';return 1;
            }
        }
        std::cout<<"Live planner matched API/layout for "<<operations.size()<<" operations in "<<frames
                 <<" captured frames; excluded "<<unknown_initial_bindings
                 <<" texture provenance differences without initial ownership; peak immutable payload bytes "<<largest_frame<<'\n';
        if(thin_metadata)std::cout<<"Thin live metadata captured-plan: "<<thin_stats.readable_copies_avoided
            <<" readable object copies / "<<thin_stats.copy_bytes_avoided<<" bytes omitted\n";
        if(deferred)std::cout<<"Deferred texture snapshots: "<<deferred_bytes<<" candidate bytes, "
            <<materialized_bytes<<" materialized, "<<deferred_bytes-materialized_bytes<<" skipped\n";
#ifdef GOW3_NATIVE_VERIFY_PREFIX_DEPENDENCIES
        std::cout<<"Legacy prefix dependency oracle: "<<test::live_prefix_dependency_checks<<" exact comparisons passed\n";
        std::cout<<"Prefix storage: "<<test::live_flat_prefixes<<" flat, "<<test::live_map_prefixes<<" legacy-map fallback\n";
#endif
        return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
