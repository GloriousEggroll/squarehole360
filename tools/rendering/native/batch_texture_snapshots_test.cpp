#include "renderer/native/immutable_snapshots.h"
#include "renderer/native/deferred_texture_snapshots.h"
#include "renderer/native/live_plan.h"
#include <array>
#include <iostream>
#include <map>
#include <set>
#include <thread>
using namespace gow3::renderer;
namespace {
size_t checks=0;
void Require(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
template<class F>void Refuses(F&& f){bool refused=false;try{f();}catch(const std::runtime_error&){refused=true;}Require(refused,"Expected explicit refusal");}
struct Model {
    using Cache=ImmutableSnapshots;using Bytes=Cache::Bytes;
    struct Token {uint32_t address;size_t size;uint64_t epoch;std::vector<uint64_t> generations;std::shared_ptr<const Bytes> bytes;};
    struct Call {uint32_t address;size_t size;bool validation;};
    std::map<uint32_t,Bytes> memory;std::map<uint32_t,uint64_t> generations;
    std::set<uint32_t> unreadable;
    std::vector<Call> last;uint64_t epoch=1,arms=0,validations=0,scalar_calls=0,batch_calls=0;
    bool throwing=false,truncated=false,wrong_owner=false,wrong_token=false,scalar_throwing=false;
    Bytes& Add(uint32_t address,size_t bytes,uint8_t fill){auto& out=memory[address];out.assign(bytes,fill);return out;}
    void Write(uint32_t address,size_t at,uint8_t value){memory.at(address).at(at)=value;++generations[(address+uint32_t(at))/4096];}
    Cache::Certified Certify(uint32_t address,size_t size,const std::shared_ptr<const void>& previous,bool validate) {
        const auto old=std::static_pointer_cast<const Token>(previous);
        const auto first=address/4096,end=uint32_t((uint64_t(address)+size+4095)/4096);
        bool valid=old&&!unreadable.contains(address)&&old->address==address&&old->size==size&&old->epoch==epoch&&old->generations.size()==end-first;
        if(valid)for(auto p=first;p<end;++p)valid&=old->generations[p-first]==generations[p];
        if(validate)++validations;
        if(valid) {
            if(wrong_owner)return {std::make_shared<const Bytes>(*old->bytes),old};
            if(wrong_token)return {old->bytes,std::make_shared<Token>(*old)};
            return {old->bytes,old};
        }
        if(validate||unreadable.contains(address))return {};
        ++arms;
        auto bytes=std::make_shared<const Bytes>(memory.at(address));
        if(old&&*old->bytes==*bytes)bytes=old->bytes;
        auto token=std::make_shared<Token>();token->address=address;token->size=size;token->epoch=epoch;token->bytes=bytes;
        for(auto p=first;p<end;++p)token->generations.push_back(generations[p]);
        return {bytes,token};
    }
    void Configure(Cache& cache,bool batch,size_t minimum=262144) {
        cache.ConfigureStableCertifiedProvider([this](uint32_t a,size_t n,const auto& old){++scalar_calls;return Certify(a,n,old,false);},
            [this](uint32_t a,size_t n,const auto& old){++scalar_calls;if(scalar_throwing)throw std::runtime_error("Optional scalar validator fault");return Certify(a,n,old,true);},minimum);
        if(batch)cache.ConfigureBatchCertifiedProvider([this](std::span<const Cache::BatchRequest> requests){
            ++batch_calls;last.clear();for(const auto& r:requests)last.push_back({r.address,r.size,r.validate_only});
            if(throwing)throw std::runtime_error("Optional provider fault");
            std::vector<Cache::Certified> out;for(const auto& r:requests)out.push_back(Certify(r.address,r.size,r.previous,r.validate_only));
            if(truncated&&!out.empty())out.pop_back();return out;
        });
    }
};
void CheckCache() {
    constexpr uint32_t a=0x1000000,b=0x1100000;Model model;auto& first=model.Add(a,131072,0x37);auto& second=model.Add(b,131072,0x51);
    ImmutableSnapshots cache;model.Configure(cache,true);
    const std::array<ImmutableSnapshots::View,3> views{{{a,first},{b,second},{a,first}}};
    std::vector<std::shared_ptr<const Model::Bytes>> out;
    for(unsigned i=0;i<34;++i) {
        out=cache.GetBatch(views);Require(*out[0]==first&&*out[1]==second&&out[0]==out[2],"Batch changed bytes/duplicate owner");
    }
    Require(model.batch_calls==1&&model.arms==2&&model.scalar_calls==0,"Per-view equality streak did not arm in one batch");
    const auto original=out;out=cache.GetBatch(views);
    Require(out==original&&model.last.size()==2&&model.last[0].validation&&model.last[1].validation,"Warm batch was not fresh exact-token validation");
    const auto armed=model.arms;model.Write(a,131071,0x89);out=cache.GetBatch(views);
    Require(model.arms==armed&&*out[0]==first&&out[1]==original[1]&&out[0]!=original[0],"Invalid active member rearmed or changed valid neighbor");
    Require(original[0]->back()==0x37,"Later guest write changed published immutable bytes");
    for(const auto& request:model.last)Require(request.validation,"Invalid active token converted to Capture");
    Require(cache.stable_certificate_stats().invalidations==1,"Active invalidation count not exact");
    // A smaller later request set supplies no readiness memo. It compares all
    // bytes, even if a token was accepted during the previous observation.
    const std::array<ImmutableSnapshots::View,1> small{{{b,second}}};const auto calls=model.batch_calls;
    model.Write(b,65537,0x44);auto changed=cache.GetBatch(small);
    Require(model.batch_calls==calls&&*changed[0]==second&&changed[0]!=original[1],"Small set borrowed a previous batch readiness stamp");
    // Cooldown belongs to each view, not the material's request-set identity.
    for(unsigned i=0;i<66;++i)out=cache.GetBatch(views);
    Require(model.arms>armed&&cache.stable_certificate_stats().arms>=4,"Per-view cooldown never rearmed after bounded exact reuse");
    model.wrong_owner=true;const auto refusal=cache.stable_certificate_stats().owner_refusals;out=cache.GetBatch(views);
    Require(*out[0]==first&&*out[1]==second&&cache.stable_certificate_stats().owner_refusals>refusal,"Wrong-owner validation was trusted");
    model.wrong_owner=false;
    for(unsigned i=0;i<130;++i)out=cache.GetBatch(views);
    model.throwing=true;model.Write(a,0,0xA7);const auto before=model.arms;out=cache.GetBatch(views);
    Require(*out[0]==first&&*out[1]==second&&model.arms==before&&cache.batch_stats().provider_errors==1,"Provider throw did not preserve exact fallback/no rearm");
    model.throwing=false;
    for(unsigned i=0;i<258;++i)out=cache.GetBatch(views);
    model.truncated=true;model.Write(b,4095,0xBC);out=cache.GetBatch(views);
    Require(*out[0]==first&&*out[1]==second,"Truncated result reused mismapped owners");model.truncated=false;
    const auto old=out[0];std::weak_ptr<const Model::Bytes> weak=old;
    ++model.epoch;model.Configure(cache,true);
    Require(cache.cache_stats().entries==0&&*old==first,"Provider replacement revoked a published owner");
    // No hidden cohort owns old entries after byte-budget eviction or Clear.
    out.clear();changed.clear();cache.Clear();Require(!weak.expired(),"Caller owner was released by cache clear");
    Model off;auto& one=off.Add(a,131072,0x12);auto& two=off.Add(b,131072,0x13);ImmutableSnapshots scalar;off.Configure(scalar,false);
    const std::array<ImmutableSnapshots::View,2> pair{{{a,one},{b,two}}};
    for(unsigned i=0;i<80;++i){const auto result=scalar.GetBatch(pair);Require(*result[0]==one&&*result[1]==two,"OFF scalar bytes changed");}
    Require(off.arms==0&&off.scalar_calls==0&&scalar.stable_certificate_stats().arms==0&&scalar.certificate_stats().minimum_size_bypasses==160,"OFF lowered scalar floor");
    Model duplicate;auto& only=duplicate.Add(a,131072,0xE2);ImmutableSnapshots dedup;duplicate.Configure(dedup,true);
    const std::array<ImmutableSnapshots::View,3> repeats{{{a,only},{a,only},{a,only}}};
    for(unsigned i=0;i<80;++i)dedup.GetBatch(repeats);
    Require(duplicate.arms==0&&duplicate.batch_calls==0&&dedup.batch_stats().cohort_bytes==0,"Duplicate descriptors inflated cohort floor");
    // Independent original-sized owners and original byte/metadata bounds.
    ImmutableSnapshots limited(131072);Model limited_model;auto& x=limited_model.Add(a,131072,1);auto& y=limited_model.Add(b,131072,2);limited_model.Configure(limited,true);
    const std::array<ImmutableSnapshots::View,2> pressure{{{a,x},{b,y}}};auto owners=limited.GetBatch(pressure);
    std::weak_ptr<const Model::Bytes> retired=owners[0];Require(limited.retained_bytes()==131072&&limited.cache_stats().entries==1,"Batch escaped original byte budget");
    limited.Clear();Require(!retired.expired(),"Eviction released caller owner");owners.clear();Require(retired.expired(),"Persistent cohort retained an evicted member");
    ImmutableSnapshots metadata;metadata.ConfigureMetadataLru(true);std::vector<uint8_t> tiny(1,0x42);
    for(uint32_t i=0;i<4097;++i)metadata.Get(0x2000000+i,tiny);
    Require(metadata.cache_stats().entries==4096&&metadata.cache_stats().metadata_lru_evictions==1,"Metadata bound changed");
    std::vector<ImmutableSnapshots::View> over(65,{a,only});const auto n=dedup.batch_stats().scalar_fallback_calls;dedup.GetBatch(over);
    Require(dedup.batch_stats().scalar_fallback_calls==n+1,"Oversized view count was admitted");
    // Sum bound is deliberately stricter than union; overlap never hides
    // temporary owner/copy/metadata costs from the per-call bound.
    std::vector<uint8_t> huge(4194305,0x67);const std::array<ImmutableSnapshots::View,2> overbytes{{{0x3000000,huge},{0x3100000,huge}}};
    const auto fallback=dedup.batch_stats().scalar_fallback_calls;dedup.GetBatch(overbytes);
    Require(dedup.batch_stats().scalar_fallback_calls==fallback+1,"Summed byte bound was bypassed by overlapping views");
}
void CheckWarmReadability() {
    using Range=ImmutableSnapshots::WarmRange;
    constexpr uint32_t a=0x1000000,b=0x1100000;
    Model model;auto& first=model.Add(a,131072,0x31);auto& second=model.Add(b,131072,0x52);
    ImmutableSnapshots cache;model.Configure(cache,true);
    const std::array<ImmutableSnapshots::View,3> views{{{a,first},{b,second},{a,first}}};
    const std::array<Range,3> ranges{{{a,131072},{b,131072},{a,131072}}};
    Require(!cache.TryReuseReadableBatch(ranges),"Cold ranges accepted as readable without byte capture");
    std::vector<std::shared_ptr<const Model::Bytes>> original;
    for(unsigned i=0;i<34;++i)original=cache.GetBatch(views);
    const auto arms=model.arms,provider_calls=model.batch_calls;
    const auto before=cache.batch_stats();const auto stable=cache.stable_certificate_stats();
    auto hit=cache.TryReuseReadableBatch(ranges);
    Require(hit&&*hit==original,"Warm readable batch changed exact owners or duplicates");
    Require(model.arms==arms&&model.batch_calls==provider_calls+1&&model.last.size()==2,
            "Warm readable batch cold-armed or failed to deduplicate validation");
    for(const auto& request:model.last)Require(request.validation,"Warm readability request allowed capture");
    const auto after=cache.batch_stats();const auto after_stable=cache.stable_certificate_stats();
    Require(after.calls==before.calls+1&&after.views==before.views+3&&after.unique_views==before.unique_views+2&&
            after.provider_calls==before.provider_calls+1&&after.provider_views==before.provider_views+2&&
            after.cohort_bytes==before.cohort_bytes+262144,"Warm success changed ordinary batch accounting");
    Require(after_stable.validation_attempts==stable.validation_attempts+2&&
            after_stable.safe_hits==stable.safe_hits+2&&after_stable.safe_hit_bytes==stable.safe_hit_bytes+262144,
            "Warm success lost ordinary certificate validation accounting");
    Require(cache.warm_batch_stats().accepted==1&&cache.warm_batch_stats().accepted_views==3&&
            cache.warm_batch_stats().accepted_bytes==393216,"Clock-free warm admission counters wrong");
    const auto calls=model.batch_calls;
    const std::array<Range,1> small{{{b,131072}}},tiny{{{a,4095}}},different{{{a,131071}}};
    const std::array<Range,2> repeats{{{a,131072},{a,131072}}};
    Require(!cache.TryReuseReadableBatch(small)&&!cache.TryReuseReadableBatch(tiny)&&
            !cache.TryReuseReadableBatch(different)&&!cache.TryReuseReadableBatch(repeats),
            "Size/identity/duplicate ranges weakened the original admission floor");
    const std::array<Range,1> overflow{{{0x1fffffff,4096}}},oversized{{{a,8388609}}};
    std::array<Range,65> too_many{};
    Require(!cache.TryReuseReadableBatch({})&&!cache.TryReuseReadableBatch(overflow)&&
            !cache.TryReuseReadableBatch(oversized)&&!cache.TryReuseReadableBatch(too_many)&&
            model.batch_calls==calls,"Invalid/partial metadata reached optional provider");
    for(unsigned failure=0;failure<4;++failure) {
        model.throwing=failure==0;model.truncated=failure==1;model.wrong_owner=failure==2;model.wrong_token=failure==3;
        const auto old_batch=cache.batch_stats();const auto old_stable=cache.stable_certificate_stats();
        Require(!cache.TryReuseReadableBatch(ranges),"Partial/throw/wrong-owner warm batch accepted");
        Require(cache.batch_stats().calls==old_batch.calls&&
                cache.stable_certificate_stats().validation_attempts==old_stable.validation_attempts&&
                cache.cache_stats().entries==2&&cache.retained_bytes()==262144&&model.arms==arms,
                "Refused warm batch changed cache/stability/admission state");
    }
    model.throwing=model.truncated=model.wrong_owner=model.wrong_token=false;
    Require(cache.TryReuseReadableBatch(ranges)==hit,"Failure released valid previous owners");
    // Simulate unmap/permission invalidation: a successful token must certify
    // readability. The unchanged slow path filters before computing its union.
    model.unreadable.insert(a);++model.generations[a/4096];
    Require(!cache.TryReuseReadableBatch(ranges),"Unreadable token accepted");
    std::vector<std::shared_ptr<const Model::Bytes>> filtered(ranges.size());
    std::vector<ImmutableSnapshots::View> readable;std::vector<size_t> indices;
    for(size_t i=0;i<ranges.size();++i)if(!model.unreadable.contains(ranges[i].address)) {
        readable.push_back({ranges[i].address,model.memory.at(ranges[i].address)});indices.push_back(i);
    }
    const auto filtered_calls=model.batch_calls;
    auto owners=cache.GetBatch(readable);
    for(size_t i=0;i<owners.size();++i)filtered[indices[i]]=std::move(owners[i]);
    Require(!filtered[0]&&filtered[1]==original[1]&&!filtered[2]&&model.batch_calls==filtered_calls&&model.arms==arms,
            "Unreadable descriptor enlarged fallback cohort or became fabricated data");
    model.unreadable.clear();model.Write(a,65537,0x98);
    Require(!cache.TryReuseReadableBatch(ranges),"Write generation change kept warm certificate");
    auto changed=cache.GetBatch(views);
    Require(changed[0]!=original[0]&&*changed[0]==first&&changed[1]==original[1]&&
            original[0]->at(65537)==0x31&&model.arms==arms,"Write fallback changed held owners or rearmed immediately");
    // Different view starts alias the same pages. A write generation touching
    // their overlap invalidates the batch; union, not summed sizes, sets floor.
    Model alias;auto& x=alias.Add(a,131072,1);auto& y=alias.Add(a+65536,131072,2);auto& z=alias.Add(b,131072,3);
    ImmutableSnapshots overlapping;alias.Configure(overlapping,true);
    const std::array<ImmutableSnapshots::View,3> av{{{a,x},{a+65536,y},{b,z}}};
    for(unsigned i=0;i<34;++i)overlapping.GetBatch(av);
    const std::array<Range,2> ar{{{a,131072},{a+65536,131072}}};
    Require(!overlapping.TryReuseReadableBatch(ar),"Overlapping ranges inflated cohort floor");
    const std::array<Range,3> all{{{a,131072},{a+65536,131072},{b,131072}}};
    Require(bool(overlapping.TryReuseReadableBatch(all)),"Valid overlapping full cohort refused");
    alias.Write(a,65536,4);
    Require(!overlapping.TryReuseReadableBatch(all),"Alias page mutation was missed");
    auto retained=changed[1];cache.Clear();
    Require(!cache.TryReuseReadableBatch(ranges)&&*retained==second,"Cache clear admitted stale token or released published bytes");
}

void CheckActiveBelowMinimum() {
    constexpr uint32_t a=0x1000000,b=0x1100000;
    using View=ImmutableSnapshots::View;
    for(size_t size:{4096u,8192u,16384u,32768u,65536u})for(bool batch:{false,true}) {
        Model model;auto& first=model.Add(a,size,0x31);auto& second=model.Add(b,size,0x52);
        ImmutableSnapshots cache;model.Configure(cache,true,size*2);
        const std::array<View,2> cohort{{{a,first},{b,second}}};
        const std::array<View,3> small{{{a,first},{a,first},{a,first}}};
        std::vector<std::shared_ptr<const Model::Bytes>> original;
        for(unsigned i=0;i<34;++i)original=cache.GetBatch(cohort);
        Require(model.arms==2,"Cold cohort did not arm two exact members");
        // OFF must retain the original comparison path even with an active token.
        const auto armed=model.arms,scalar_calls=model.scalar_calls,batch_calls=model.batch_calls;
        auto use=[&]{return batch?cache.GetBatch(small)[0]:cache.Get(a,first);};
        Require(use()==original[0]&&model.scalar_calls==scalar_calls&&model.batch_calls==batch_calls,
                "Default OFF made a below-floor provider request");
        Require(cache.stable_certificate_stats().below_minimum_attempts==0,"OFF changed optional counters");
        cache.ConfigureActiveBelowMinimum(true);
        const auto before=cache.stable_certificate_stats();
        for(unsigned i=0;i<8;++i)Require(use()==original[0],"Fresh validation replaced active exact owner");
        const auto after=cache.stable_certificate_stats();
        Require(model.arms==armed&&model.batch_calls==batch_calls&&model.scalar_calls==scalar_calls+8,
                "Below-floor validation rearmed, batched, or repeated duplicate requests");
        Require(after.below_minimum_attempts==8&&after.below_minimum_hits==8&&
                after.below_minimum_hit_bytes==8*size&&after.below_minimum_refusals==0&&
                after.exact_fallbacks==before.exact_fallbacks&&after.validation_attempts==before.validation_attempts+8,
                "Fresh below-floor validation did not bypass comparison exactly once per unique owner");
        // Warm descriptor admission deliberately retains the original cohort floor.
        const std::array<ImmutableSnapshots::WarmRange,1> range{{{a,size}}};
        Require(!cache.TryReuseReadableBatch(range)&&model.batch_calls==batch_calls,
                "Below-floor option widened readability/cohort admission");
        // A generation change refuses the old token; it cannot recapture on the
        // same call or build a cold eligibility streak through small requests.
        model.Write(a,size-1,0xA1);auto changed=use();
        Require(changed!=original[0]&&*changed==first&&original[0]->back()==0x31&&model.arms==armed,
                "Invalidation reused changed bytes, rearmed, or mutated held owners");
        const auto invalid=cache.stable_certificate_stats();const auto calls=model.scalar_calls;
        Require(invalid.below_minimum_attempts==9&&invalid.below_minimum_refusals==1&&invalid.invalidations==1,
                "Below-floor invalidation counters not exact");
        for(unsigned i=0;i<128;++i)Require(use()==changed,"Below-floor cooldown changed equal bytes");
        Require(model.arms==armed&&model.scalar_calls==calls&&cache.stable_certificate_stats().below_minimum_attempts==9,
                "Inactive below-floor entry validated or warmed into cold capture");
        for(unsigned i=0;i<64;++i)cache.GetBatch(cohort);
        Require(model.arms==armed,"Below-floor refusals reduced original doubled cooldown");
        const auto rearmed=cache.GetBatch(cohort);Require(model.arms==armed+1,"Eligible exact streak did not rearm after original cooldown");
        cache.ConfigureActiveBelowMinimum(false);const auto off_calls=model.scalar_calls;
        Require(use()==rearmed[0]&&model.scalar_calls==off_calls,"Disabling reuse did not restore exact path");
        auto retained=changed;cache.Clear();
        Require(*retained==first&&cache.cache_stats().entries==0,"Clear mutated published bytes or retained tokens");
        for(unsigned i=0;i<80;++i)use();
        Require(model.arms==armed+1&&model.scalar_calls==off_calls,"Cold sub-floor data acquired a certificate");
    }
    // Refusal on unchanged bytes must retain the immutable owner, back off once,
    // and preserve the original cold admission streak. Cover scalar and batch
    // entry points with wrong owners/tokens, provider exceptions, and no access.
    for(bool batch:{false,true})for(unsigned failure=0;failure<4;++failure) {
        Model model;auto& first=model.Add(a,16384,0x73);auto& second=model.Add(b,16384,0x26);
        ImmutableSnapshots cache;model.Configure(cache,true,32768);cache.ConfigureActiveBelowMinimum(true);
        const std::array<View,2> cohort{{{a,first},{b,second}}};const std::array<View,1> small{{{a,first}}};
        std::vector<std::shared_ptr<const Model::Bytes>> original;
        for(unsigned i=0;i<34;++i)original=cache.GetBatch(cohort);
        const auto arms=model.arms,calls=model.scalar_calls;
        model.wrong_owner=failure==0;model.wrong_token=failure==1;model.scalar_throwing=failure==2;
        if(failure==3)model.unreadable.insert(a);
        auto use=[&]{return batch?cache.GetBatch(small)[0]:cache.Get(a,first);};
        Require(use()==original[0]&&model.arms==arms&&model.scalar_calls==calls+1,
                "Refused validation changed exact equal owner or cold armed");
        const auto stats=cache.stable_certificate_stats();
        Require(stats.below_minimum_attempts==1&&stats.below_minimum_hits==0&&stats.below_minimum_refusals==1&&
                stats.invalidations==1&&stats.provider_errors==(failure==2?1u:0u)&&
                stats.owner_refusals==(failure<2?1u:0u),"Refusal/provider-error accounting wrong");
        model.wrong_owner=model.wrong_token=model.scalar_throwing=false;model.unreadable.clear();
        for(unsigned i=0;i<128;++i)Require(use()==original[0],"Refusal cooldown changed retained owner");
        Require(model.arms==arms&&model.scalar_calls==calls+1,"Below-floor equal fallback advanced cold eligibility");
        for(unsigned i=0;i<64;++i)cache.GetBatch(cohort);
        Require(model.arms==arms,"Unchanged refusal advanced or shortened cold cooldown");
        cache.GetBatch(cohort);Require(cache.stable_certificate_stats().arm_attempts==3&&cache.stable_certificate_stats().arms==3,"Unchanged refusal never regained eligible certificate");
    }
    // Overlapping/duplicate cold ranges still cannot create cohort eligibility.
    Model overlap;auto& one=overlap.Add(a,16384,1);auto& two=overlap.Add(a+8192,16384,2);
    ImmutableSnapshots cold;overlap.Configure(cold,true,32768);cold.ConfigureActiveBelowMinimum(true);
    const std::array<View,3> views{{{a,one},{a+8192,two},{a,one}}};
    for(unsigned i=0;i<100;++i)cold.GetBatch(views);
    Require(overlap.arms==0&&overlap.scalar_calls==0&&overlap.batch_calls==0&&
            cold.stable_certificate_stats().below_minimum_attempts==0,"ON changed cold union floor or duplicate admission");
}

void CheckScope() {
    using Scope=DeferredTextureSnapshotScope;
    auto descriptor=[](uint32_t kind,uint32_t address){GraphicsBoundaryBlock b;b.kind=kind;b.address=address;b.deferred_physical_bytes=4096;return b;};
    std::array<GraphicsBoundaryBlock,2> blocks{descriptor(32,0x1000000),descriptor(33,0x1200000)};
    std::array<GraphicsBoundaryBlock*,2> pointers{&blocks[0],&blocks[1]};size_t calls=0;
    std::function<void(std::span<GraphicsBoundaryBlock* const>)> capture;
    {
        Scope scope;capture=scope.BindBatch([&](std::span<const Scope::Request> requests){
            ++calls;std::vector<Scope::Bytes> out;for(const auto& r:requests)out.push_back(std::make_shared<const std::vector<uint8_t>>(r.bytes,uint8_t(r.address>>16)));return out;
        });
        bool refused=false;std::thread thread([&]{try{capture(pointers);}catch(const std::runtime_error&){refused=true;}});thread.join();
        Require(refused&&calls==0&&blocks[0].deferred_physical_bytes==4096,"Batch escaped caller thread");
        auto partial=scope.BindBatch([](auto requests){return std::vector<Scope::Bytes>{std::make_shared<const std::vector<uint8_t>>(requests[0].bytes),std::make_shared<const std::vector<uint8_t>>(requests[1].bytes-1)};});
        Refuses([&]{partial(pointers);});Require(!blocks[0].shared_bytes&&!blocks[1].shared_bytes&&blocks[0].deferred_physical_bytes==4096,"Partial result mutated earlier descriptor");
        auto short_result=scope.BindBatch([](auto){return std::vector<Scope::Bytes>{};});Refuses([&]{short_result(pointers);});
        const std::array<GraphicsBoundaryBlock*,2> duplicate{pointers[0],pointers[0]};Refuses([&]{capture(duplicate);});
        blocks[1].kind=14;Refuses([&]{capture(pointers);});blocks[1].kind=33;
        auto absent=scope.BindBatch([](auto requests){return std::vector<Scope::Bytes>(requests.size());});absent(pointers);
        Require(blocks[0].data().empty()&&blocks[1].data().empty()&&!blocks[0].deferred_physical_bytes,"Unreadable view fabricated bytes");
        blocks={descriptor(32,0x1000000),descriptor(33,0x1200000)};capture(pointers);
        Require(calls==1&&blocks[0].data().size()==4096&&blocks[1].data().size()==4096,"Batch lost original extents");
    }
    blocks={descriptor(32,0x1000000),descriptor(33,0x1200000)};Refuses([&]{capture(pointers);});Require(calls==1,"Batch escaped caller lifetime");
}
void Put(std::vector<uint8_t>& bytes,size_t at,uint32_t v){for(unsigned i=0;i<4;++i)bytes.at(at+i)=uint8_t(v>>(24-i*8));}
void CompareFrames(const LiveRenderFrame& a,const LiveRenderFrame& b) {
    Require(a.operations.ToJson()==b.operations.ToJson()&&a.retained_bytes==b.retained_bytes,"Batch changed typed operation/JSON/accounting");
    for(bool shader:{false,true}) {
        const auto x=a.MaterializeResources(shader),y=b.MaterializeResources(shader);Require(x.size()==y.size(),"Batch changed canonical bank cardinality");
        for(const auto& [hash,bytes]:x){Require(y.contains(hash),"Batch changed canonical hash");Require(*bytes==*y.at(hash),"Batch changed canonical payload bytes");}
    }
}
void CheckPlanner() {
    constexpr std::array<uint32_t,6> fetch{2|(1u<<22),0x10006,3|(3<<13),0,0,1<<9};
    auto shader=[](bool pixel){std::vector<uint8_t> v(pixel?68:72),p(24);Put(v,0,pixel?0x102a1100:0x102a1141);Put(v,4,uint32_t(v.size()));Put(v,8,uint32_t(p.size()));Put(v,24,36);Put(v,40,uint32_t(p.size()));const uint64_t exec=(2ull<<44)|1|(1<<12)|(pixel?1<<16:0);Put(p,0,uint32_t(exec));Put(p,4,uint32_t(exec>>32));Put(p,12,pixel?1:0);return std::pair{v,p};};
    const auto [vv,vp]=shader(false);const auto [pv,pp]=shader(true);
    GraphicsBoundaryEvent draw;draw.function=0x82AEFF00;draw.phase=1;draw.args={0x83000000,4,0,3};draw.blocks={{1,draw.args[0],std::vector<uint8_t>(0x3600)},{2,0x83004000,vv},{3,0x11000,vp},{4,0x83005000,pv},{5,0x12000,pp},{32,0x10000,std::vector<uint8_t>(512,0x22)}};
    for(unsigned i=0;i<6;++i)Put(draw.blocks[0].bytes,0x480+i*4,fetch[i]);
    GraphicsBoundaryEvent resolve;resolve.function=0x82AF34A8;resolve.args[0]=draw.args[0];resolve.blocks={{1,draw.args[0],draw.blocks[0].bytes},{6,0x83006000,std::vector<uint8_t>(64)},{100,0x83007000,std::vector<uint8_t>(64)}};Put(resolve.blocks[1].bytes,24,32);Put(resolve.blocks[1].bytes,36,(3<<18)|(3<<3));for(unsigned i=0;i<6;++i)Put(resolve.blocks[2].bytes,28+i*4,fetch[i]);
    for(bool lazy:{false,true})for(bool direct:{false,true}) {
        GearsLivePlan old({},true,lazy,direct),candidate({},true,lazy,direct);DeferredTextureSnapshotScope scope;
        std::vector<uint8_t> base(512,0x22),mips(512,0x66);size_t single_reads=0,batch_reads=0,batches=0;uint64_t sequence=0,generation=0;
        std::optional<LivePacketPrefix> original_prefix;
        auto deferred=[&](GraphicsBoundaryEvent event,bool batch) {
            for(auto& block:event.blocks)if(block.kind>=32&&block.kind<=95&&!block.bytes.empty()){block.deferred_physical_bytes=uint32_t(block.bytes.size());block.bytes.clear();}
            // An unused slot is deliberately unreadable. Reaching the provider
            // for it is a test failure, rather than an invented empty texture.
            GraphicsBoundaryBlock unused;unused.kind=34;unused.address=0xDEAD000;unused.deferred_physical_bytes=4096;event.blocks.push_back(std::move(unused));
            auto read=[&](uint32_t address,uint32_t count){Require(address==0x10000||address==0x20000,"Unused/GPU-only descriptor reached CPU reader");const auto& source=address==0x10000?base:mips;Require(source.size()==count,"Planner changed base/mip extent");return std::make_shared<const std::vector<uint8_t>>(source);};
            event.materialize_texture=scope.Bind([&,read](uint32_t a,uint32_t n){++single_reads;return read(a,n);});
            if(batch)event.materialize_textures=scope.BindBatch([&,read](auto requests){++batches;std::vector<DeferredTextureSnapshotScope::Bytes> out;for(const auto& r:requests){++batch_reads;out.push_back(read(r.address,r.bytes));}return out;});return event;
        };
        auto push=[&](GraphicsBoundaryEvent event,bool is_draw){event.sequence=sequence++;event.command_span=GraphicsCommandSpan{++generation,event.sequence,0x2000,{0xc0,0,0x36,0,0,3,0,0x84}};if(is_draw){old.Push(deferred(event,false));candidate.Push(deferred(std::move(event),true));}else{old.Push(GraphicsBoundaryEvent(event));candidate.Push(std::move(event));}auto a=old.TakePacketPrefix(),b=candidate.TakePacketPrefix();Require(a&&b,"Missing immutable command prefix");CompareFrames(*a->batch,*b->batch);if(!original_prefix)original_prefix=a;};
        push(draw,true);Require(single_reads==1&&batch_reads==1,"CPU texture capture counts wrong");
        base.assign(512,0x55);push(resolve,false);const auto prior=batch_reads;push(draw,true);Require(batch_reads==prior&&single_reads==1,"Fully GPU-owned texture was read");
        old.InvalidateMemory(0x10000,4096,std::vector<uint8_t>(4096,0x55),91);candidate.InvalidateMemory(0x10000,4096,std::vector<uint8_t>(4096,0x55),91);push(draw,true);Require(batch_reads==prior+1,"Pending CPU write did not revoke GPU-only classification");
        auto partial=resolve;partial.blocks.push_back({98,0x83008000,std::vector<uint8_t>(16)});Put(partial.blocks.back().bytes,8,1);Put(partial.blocks.back().bytes,12,1);push(std::move(partial),false);push(draw,true);
        push(resolve,false);auto mip=draw;Put(mip.blocks[0].bytes,0x480+16,1<<6);Put(mip.blocks[0].bytes,0x480+20,0x20000|(1<<9));mip.blocks.push_back({33,0x20000,mips});const auto before=batch_reads;push(mip,true);Require(batch_reads==before+2,"Mixed GPU/CPU mip chain lost required snapshots");
        GraphicsBoundaryEvent present;present.function=0x82AF6B10;present.sequence=sequence++;present.blocks={resolve.blocks[0],resolve.blocks[2]};present.blocks[1].kind=103;auto a=old.Push(GraphicsBoundaryEvent(present)),b=candidate.Push(std::move(present));Require(a&&b,"Missing complete frame");CompareFrames(*a,*b);Require(batches>0&&batch_reads==single_reads,"Batch grouping lost required view conservation");
        const auto first=original_prefix->batch->operations[0].Draw();Require(first&&first->textures[0].base_payload,"First prefix lost texture payload");Require(first->textures[0].base_payload->owner->front()==0x22,"Later mutation changed original prefix owner");
    }
}
}
int main(){try{CheckCache();CheckWarmReadability();CheckActiveBelowMinimum();CheckScope();CheckPlanner();std::cout<<"Batch CPU texture snapshots: "<<checks<<" checks; per-view bytes/owners, backoff, bounds, scope and typed/JSON prefix oracle passed\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
