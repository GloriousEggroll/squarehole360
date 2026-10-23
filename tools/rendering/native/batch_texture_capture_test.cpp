#include "renderer/native/immutable_snapshots.h"
#include "renderer/native/deferred_texture_snapshots.h"
#include "renderer/native/live_plan.h"
#include "renderer/native/shader_bindings.h"
#include "renderer/native/content_hash.h"
#include <rex/system/physical_snapshot.h>
#include <fstream>
#include <iostream>
#include <map>
#include <string_view>
using namespace gow3::renderer;
using Json=nlohmann::json;
namespace {
using Bytes=std::vector<uint8_t>;using Key=std::pair<uint32_t,uint32_t>;
size_t checks=0;
void Require(bool condition,const char* message){++checks;if(!condition)throw std::runtime_error(message);}
uint32_t LE(std::span<const uint8_t> data,size_t at){if(at>data.size()||data.size()-at<4)throw std::runtime_error("Truncated capture word");return uint32_t(data[at])|uint32_t(data[at+1])<<8|uint32_t(data[at+2])<<16|uint32_t(data[at+3])<<24;}
Bytes Read(const char* path){std::ifstream f(path,std::ios::binary|std::ios::ate);if(!f)throw std::runtime_error("Cannot open capture");const auto length=f.tellg();if(length<=0||uint64_t(length)>256u*1024*1024)throw std::runtime_error("Capture extent outside bound");Bytes out(static_cast<size_t>(length));f.seekg(0);f.read(reinterpret_cast<char*>(out.data()),std::streamsize(out.size()));if(!f)throw std::runtime_error("Cannot read capture");return out;}
struct Input {
    GraphicsBoundaryEvent event;
    std::map<Key,std::span<const uint8_t>> physical;
};
Input Decode(std::span<const uint8_t> data,size_t& at,uint64_t sequence,std::map<uint32_t,std::pair<size_t,size_t>>& references) {
    const auto length=LE(data,at),function=LE(data,at+4);
    Require(length>=60&&at<=data.size()&&length<=data.size()-at&&function,"Invalid capture event extent");
    Input input;auto& event=input.event;event.sequence=sequence;event.function=function;event.phase=LE(data,at+8);event.frame=LE(data,at+12);event.lr=LE(data,at+16);event.result=LE(data,at+52);
    for(unsigned i=0;i<8;++i)event.args[i]=LE(data,at+20+4*i);
    const auto blocks=LE(data,at+56);Require(blocks<=105,"Too many capture block descriptors");
    size_t cursor=at+60;
    for(uint32_t i=0;i<blocks;++i) {
        Require(cursor<=at+length&&at+length-cursor>=12,"Truncated capture block");
        GraphicsBoundaryBlock block;block.kind=LE(data,cursor);block.address=LE(data,cursor+4);const auto encoded=LE(data,cursor+8);cursor+=12;size_t start=cursor,count=encoded;
        if(encoded&0x80000000u){const auto found=references.find(encoded&0x7fffffffu);Require(found!=references.end(),"Capture reference has no earlier payload");start=found->second.first;count=found->second.second;}
        else {Require(count<=at+length-cursor,"Capture inline payload exceeds event");if(count)references.emplace(uint32_t(cursor),std::pair{cursor,count});cursor+=count;}
        Require(start<=data.size()&&count<=data.size()-start,"Capture payload outside file");
        const auto source=data.subspan(start,count);block.bytes.assign(source.begin(),source.end());
        if(block.kind>=32&&block.kind<=95&&count) {
            Require(count<=8u*1024*1024&&uint64_t(block.address)+count<=0x20000000ull,"Captured texture exceeds original physical bound");
        }
        if(block.kind>=14&&block.kind<=95&&count) {
            const auto [found,inserted]=input.physical.emplace(Key{block.address,uint32_t(count)},source);
            Require(inserted||std::equal(source.begin(),source.end(),found->second.begin()),"Captured duplicate physical descriptors disagree");
        }
        event.blocks.push_back(std::move(block));
    }
    Require(cursor==at+length,"Capture block extent mismatch");at+=length;return input;
}
// This is an independent fixture provider, NOT the SDK protection adapter.
// Every changed captured view conservatively changes all rounded page versions.
// The immutable capture backing supplies the exact view bytes. Alias protection,
// concurrency and OS/Memory lifetime are covered by separate actual SDK tests.
struct Provider {
    struct Token {std::shared_ptr<const uint8_t> identity;uint32_t address=0,size=0;std::vector<uint64_t> versions;std::shared_ptr<const Bytes> owner;};
    std::shared_ptr<const uint8_t> identity=std::make_shared<const uint8_t>(0);
    std::map<Key,std::span<const uint8_t>> previous,current;
    std::map<uint32_t,uint64_t> pages;
    uint64_t scalar_calls=0,batch_calls=0,validations=0,captures=0,changes=0,refusals=0;
    void Install(const std::map<Key,std::span<const uint8_t>>& sources) {
        current=sources;
        for(const auto& [key,bytes]:sources) {
            const auto old=previous.find(key);
            if(old==previous.end()||!std::equal(bytes.begin(),bytes.end(),old->second.begin())) {
                ++changes;for(uint32_t p=key.first/4096;p<uint32_t((uint64_t(key.first)+key.second+4095)/4096);++p)++pages[p];
            }
            previous[key]=bytes; // Span is into immutable, authenticated input; no payload-owning cohort.
        }
    }
    ImmutableSnapshots::Certified Certify(uint32_t address,size_t size,const std::shared_ptr<const void>& old,bool validate) {
        using Tracker=rex::memory::PhysicalSnapshotTracker;
        if(!size||size>Tracker::kMaxSnapshot||address<Tracker::kFirst||uint64_t(address)+size>Tracker::kEnd){++refusals;return {};}
        const auto previous_token=std::static_pointer_cast<const Token>(old);const auto first=address/4096,end=uint32_t((uint64_t(address)+size+4095)/4096);
        bool valid=previous_token&&previous_token->identity==identity&&previous_token->address==address&&previous_token->size==size&&previous_token->versions.size()==end-first;
        if(valid)for(auto p=first;p<end;++p)valid&=previous_token->versions[p-first]==pages[p];
        if(validate)++validations;
        if(valid)return {previous_token->owner,previous_token};
        if(validate){++refusals;return {};}
        const auto source=current.at({address,uint32_t(size)});
        auto bytes=std::make_shared<const Bytes>(source.begin(),source.end());
        if(previous_token&&previous_token->owner->size()==size&&*previous_token->owner==*bytes)bytes=previous_token->owner;
        auto token=std::make_shared<Token>();token->identity=identity;token->address=address;token->size=uint32_t(size);token->owner=std::move(bytes);
        for(auto p=first;p<end;++p)token->versions.push_back(pages[p]);++captures;return {token->owner,token};
    }
    void Configure(ImmutableSnapshots& cache,bool batch) {
        cache.ConfigureMetadataLru(true);
        cache.ConfigureStableCertifiedProvider([this](uint32_t a,size_t n,const auto& old){++scalar_calls;return Certify(a,n,old,false);},
            [this](uint32_t a,size_t n,const auto& old){++scalar_calls;return Certify(a,n,old,true);},262144);
        if(batch)cache.ConfigureBatchCertifiedProvider([this](auto requests){++batch_calls;std::vector<ImmutableSnapshots::Certified> out;for(const auto& r:requests)out.push_back(Certify(r.address,r.size,r.previous,r.validate_only));return out;});
    }
};
Json Snapshot(const LiveRenderFrame& frame,bool rehash) {
    Json out={{"frame",frame.frame},{"retained_bytes",frame.retained_bytes},{"operations",frame.operations.ToJson()},{"payloads",Json::object()},{"shaders",Json::object()}};
    size_t retained=0;
    for(bool shader:{false,true})frame.ForEachResource(shader,[&](auto hash,const auto& owner){
        Require(bool(owner),"Canonical bank lacks immutable owner");retained+=owner->size();
        if(rehash)Require(ContentHash(*owner)==hash,"Canonical digest differs from exact payload bytes");
        out[shader?"shaders":"payloads"][std::string(hash)]={{"bytes",owner->size()},{"sha256",std::string(hash)}};
    });
    Require(retained==frame.retained_bytes,"Canonical bank retained-byte accounting differs");return out;
}
void Equal(const LiveRenderFrame& a,const LiveRenderFrame& b,bool rehash) {
    Require(Snapshot(a,rehash)==Snapshot(b,rehash),"Complete pre-normalization typed JSON/bank identities differ");
    for(bool shader:{false,true}) {
        const auto x=a.MaterializeResources(shader),y=b.MaterializeResources(shader);Require(x.size()==y.size(),"Canonical bank cardinality differs");
        for(const auto& [hash,bytes]:x)Require(y.contains(hash)&&*bytes==*y.at(hash),"Original-sized canonical payload bytes differ");
    }
}
struct Side {
    Provider provider;ImmutableSnapshots cache;std::unique_ptr<GearsLivePlan> planner;bool batch;
    uint64_t deferred_views=0,required_views=0,required_bytes=0,batch_callbacks=0;
    Side(bool lazy,bool direct,bool is_batch):planner(std::make_unique<GearsLivePlan>(std::function<void(uint32_t,uint32_t)>{},false,lazy,direct)),batch(is_batch){provider.Configure(cache,batch);}
    GraphicsBoundaryEvent Prepare(const Input& input,const DeferredTextureSnapshotScope& scope) {
        provider.Install(input.physical);auto event=input.event;
        // Match the existing selective live capture route. Base/mip views not
        // selected by shaders and unused geometry never enter the producer.
        if(const auto required=SnapshotShaderInputs(event.blocks))for(auto& block:event.blocks) {
            if(block.kind>=14&&block.kind<=29&&!required->first.contains(95-(block.kind-14)))block.bytes.clear();
            if(block.kind>=32&&block.kind<=95&&!required->second.contains((block.kind-32)/2))block.bytes.clear();
        }
        // Retain the original scalar physical geometry snapshots in BOTH arms.
        // The candidate only changes deferred required CPU texture handling.
        for(auto& block:event.blocks)if(block.kind>=14&&block.kind<=31&&block.bytes.size()>=4096) {
            block.shared_bytes=cache.Get(block.address,block.bytes);block.bytes.clear();
        }
        for(auto& block:event.blocks)if(block.kind>=32&&block.kind<=95&&!block.bytes.empty()) {
            ++deferred_views;block.deferred_physical_bytes=uint32_t(block.bytes.size());block.bytes.clear();
        }
        auto read=[this](uint32_t address,uint32_t size){++required_views;required_bytes+=size;const auto source=provider.current.at({address,size});return size>=4096?cache.Get(address,source):std::make_shared<const Bytes>(source.begin(),source.end());};
        event.materialize_texture=scope.Bind(read);
        if(batch)event.materialize_textures=scope.BindBatch([this](auto requests){
            ++batch_callbacks;std::vector<DeferredTextureSnapshotScope::Bytes> out(requests.size());std::array<ImmutableSnapshots::View,64> views{};std::array<size_t,64> indices{};size_t count=0;
            for(size_t i=0;i<requests.size();++i){++required_views;required_bytes+=requests[i].bytes;const auto source=provider.current.at({requests[i].address,requests[i].bytes});if(requests[i].bytes<4096)out[i]=std::make_shared<const Bytes>(source.begin(),source.end());else{views[count]={requests[i].address,source};indices[count++]=i;}}
            if(count){const auto owners=cache.GetBatch(std::span<const ImmutableSnapshots::View>(views).first(count));for(size_t i=0;i<count;++i)out[indices[i]]=owners[i];}return out;
        });return event;
    }
};
Json Run(std::span<const uint8_t> data,bool lazy,bool direct,const std::string& output_prefix) {
    Side scalar(lazy,direct,false),batch(lazy,direct,true);size_t at=16,events=0,frames=0,operations=0,draws=0,prefixes=0;std::map<uint32_t,std::pair<size_t,size_t>> references;
    std::vector<std::pair<std::shared_ptr<LiveRenderFrame>,std::shared_ptr<LiveRenderFrame>>> retained;
    std::ofstream off(output_prefix+"-off.jsonl"),on(output_prefix+"-on.jsonl");Require(bool(off)&&bool(on),"Cannot open exact plan outputs");
    while(at<data.size()) {
        if(!LE(data,at+4)){Require(data.size()-at==16&&LE(data,at)==16,"Invalid capture footer");break;}
        auto input=Decode(data,at,events++,references);DeferredTextureSnapshotScope scope;
        auto a=scalar.Prepare(input,scope),b=batch.Prepare(input,scope);
        // The .g3d API capture contains no owned PM4 command spans. Synthetic
        // fixed spans at these authenticated API boundaries exercise immutable
        // prefix ownership; they are NOT claimed as recovered actual PM4 bytes.
        const GraphicsCommandSpan span{events,input.event.sequence,0x2000,{0xc0,0,0x36,0,0,3,0,0x84}};
        a.command_span=span;b.command_span=span;
        auto first=scalar.planner->Push(std::move(a)),second=batch.planner->Push(std::move(b));
        auto ap=scalar.planner->TakePacketPrefix(),bp=batch.planner->TakePacketPrefix();Require(bool(ap)==bool(bp),"Immutable prefix admission differs");
        if(ap){++prefixes;Require(ap->span.bytes==bp->span.bytes&&ap->span.generation==bp->span.generation&&ap->first_operation==bp->first_operation&&ap->operation_end==bp->operation_end,"Immutable command prefix identity/frontier differs");Equal(*ap->batch,*bp->batch,false);if(retained.empty()||input.event.function==0x82AF6B10)retained.emplace_back(ap->batch,bp->batch);}
        Require(bool(first)==bool(second),"Complete frame admission differs");
        if(first){++frames;Equal(*first,*second,true);const auto x=Snapshot(*first,false),y=Snapshot(*second,false);const auto off_line=x.dump(),on_line=y.dump();Require(off_line==on_line,"Exact JSONL serialization differs");off<<off_line<<'\n';on<<on_line<<'\n';Require(bool(off)&&bool(on),"Exact JSONL write failed");operations+=first->operations.size();for(const auto& op:first->operations)if(op.Draw())++draws;retained.emplace_back(first,second);}
    }
    Require(frames==2&&operations==3462&&draws==3330&&prefixes>3000,"Authenticated room capture shape/positive prefixes differs");
    Require(scalar.required_views==batch.required_views&&scalar.required_bytes==batch.required_bytes,"CPU-only required snapshot conservation differs");
    const auto stats=batch.cache.batch_stats();Require(stats.enabled&&stats.calls&&stats.provider_calls&&stats.provider_views&&batch.provider.captures&&batch.provider.validations,"Captured batch certificate route was vacuous");
    // Destroy planners and release all cache/provider payload state before
    // independently re-reading retained original-sized owners. Input is alive;
    // frame resources themselves never contain those borrowed source spans.
    scalar.planner.reset();batch.planner.reset();scalar.cache.ConfigureCertifiedProvider({});batch.cache.ConfigureCertifiedProvider({});scalar.provider.current.clear();batch.provider.current.clear();scalar.provider.previous.clear();batch.provider.previous.clear();scalar.provider.identity.reset();batch.provider.identity.reset();scalar.provider.pages.clear();batch.provider.pages.clear();
    for(const auto& [a,b]:retained)Equal(*a,*b,true);
    return {{"lazy",lazy},{"direct",direct},{"events",events},{"frames",frames},{"operations",operations},{"draws",draws},{"prefixes",prefixes},{"retained_owner_checks",retained.size()},{"required_views",batch.required_views},{"required_bytes",batch.required_bytes},{"batch_callbacks",batch.batch_callbacks},{"batch_calls",stats.calls},{"provider_calls",stats.provider_calls},{"provider_views",stats.provider_views},{"cohort_bytes",stats.cohort_bytes},{"scalar_fallback_calls",stats.scalar_fallback_calls},{"batch_model_captures",batch.provider.captures},{"batch_model_validations",batch.provider.validations},{"model_page_changes",batch.provider.changes},{"pre_normalization_jsonl_byte_identical",true},{"actual_PM4_bytes",false},{"actual_SDK_alias_protection",false}};
}
}
int main(int argc,char** argv)try {
    if(argc!=4)throw std::runtime_error("usage: batch-texture-capture-test api.g3d expected-capture-sha256 output-prefix");
    const auto data=Read(argv[1]);Require(data.size()==156151486&&std::string_view(reinterpret_cast<const char*>(data.data()),8)=="G3D3D001"&&LE(data,8)==6&&LE(data,12)==0x3600,"Captured room file/version/device extent differs");
    const auto digest=ContentHash(data);Require(digest==argv[2]&&digest.size()==64,"Capture source SHA256 mismatch");
    Json result={{"scope","Authenticated two-frame producer/byte/owner differential; software per-view certificate model, separate actual SDK alias tests required; no Vulkan/performance proof"},{"capture_path",argv[1]},{"capture_sha256",digest},{"capture_bytes",data.size()},{"cases",Json::array()}};
    for(bool lazy:{false,true})for(bool direct:{false,true})result["cases"].push_back(Run(data,lazy,direct,std::string(argv[3])+"-lazy"+std::to_string(lazy)+"-direct"+std::to_string(direct)));
    result["checks"]=checks;result["passed"]=true;std::ofstream output(std::string(argv[3])+"-result.json");output<<result.dump(2)<<'\n';Require(bool(output),"Cannot write full capture result");std::cout<<result.dump(2)<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
