#pragma once
#include "bytes_equal.h"
#include "parallel_snapshot_compare.h"
#include <array>
#include <algorithm>
#include <utility>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <list>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace gow3::renderer {
// The guest address is a lookup hint only. Every use either compares all bytes
// or obtains an owner-bound certificate from the optional SDK provider. Ordinary
// invalidation notifications and address/epoch hints never skip comparisons.
// Eviction releases only this cache's reference, never an in-flight snapshot.
// Serialized by the graphics bridge lock; no platform memory API is required.
class ImmutableSnapshots {
public:
    using Bytes=std::vector<uint8_t>;
    struct Certified {std::shared_ptr<const Bytes> bytes;std::shared_ptr<const void> token;};
    using CertifiedProvider=std::function<Certified(uint32_t,size_t,const std::shared_ptr<const void>&)>;
    struct BatchRequest {
        uint32_t address=0;size_t size=0;std::shared_ptr<const void> previous;
        bool validate_only=true;
    };
    using BatchProvider=std::function<std::vector<Certified>(std::span<const BatchRequest>)>;
    struct View {uint32_t address=0;std::span<const uint8_t> data;};
    struct WarmRange {uint32_t address=0;size_t size=0;};
    struct WarmBatchStats {
        uint64_t attempts=0,accepted=0,accepted_views=0,accepted_bytes=0;
        uint64_t provider_calls=0,provider_errors=0,provider_refusals=0;
    };
    struct BatchStats {
        uint64_t calls=0,views=0,unique_views=0,provider_calls=0,provider_views=0;
        uint64_t cohort_bytes=0,scalar_fallback_calls=0,provider_errors=0;
        bool enabled=false;
    };
    struct CertificateStats {
        uint64_t attempts=0,accepted=0,fallback=0;
        uint64_t minimum_size_bypasses=0,minimum_size_bypassed_bytes=0;
    };
    struct StableCertificateStats {
        uint64_t exact_hits=0,exact_changes=0,exact_fallbacks=0,exact_fallback_bytes=0;
        uint64_t validation_attempts=0,safe_hits=0,safe_hit_bytes=0,invalidations=0,owner_refusals=0;
        uint64_t arm_attempts=0,arms=0,arm_refusals=0,provider_errors=0,cooldown_bypasses=0;
        uint64_t below_minimum_attempts=0,below_minimum_hits=0,below_minimum_hit_bytes=0,below_minimum_refusals=0;
        bool enabled=false;
    };
    static constexpr uint32_t kStableCertificateInitialHits=32;
    static constexpr uint32_t kStableCertificateMaximumHits=4096;
    struct CacheStats {
        uint64_t budget_evictions=0,metadata_clears=0,metadata_clear_entries=0,metadata_lru_evictions=0;
        size_t entries=0,peak_entries=0,retained_bytes=0;
        bool metadata_lru=false;
    };
    struct MetadataEviction {
        uint32_t address=0;
        size_t requested_bytes=0,entries=0,retained_bytes=0;
        bool lru=false;
    };
    // Called only before an actual metadata-policy eviction, under the existing
    // bridge serialization. The observer must not reenter this cache. It may
    // copy this POD for diagnostics; it cannot retain internal cache resources.
    using MetadataEvictionObserver=void(*)(const MetadataEviction&) noexcept;
    void ConfigureMetadataEvictionObserver(MetadataEvictionObserver observer)noexcept{metadata_observer_=observer;}
    void ConfigureMetadataLru(bool enabled)noexcept{metadata_lru_=enabled;}
    // The minimum remains a cold-arming policy. Optional reuse below it still
    // requires fresh validation of the exact active token and byte owner.
    void ConfigureActiveBelowMinimum(bool enabled)noexcept{active_below_minimum_=enabled;}
    CacheStats cache_stats()const noexcept {
        auto out=cache_stats_;out.entries=entries_.size();out.retained_bytes=retained_;
        out.metadata_lru=metadata_lru_;return out;
    }
    void ConfigureCertifiedProvider(CertifiedProvider provider,size_t minimum_bytes=0){
        // Opaque tokens belong to exactly one provider; never offer tokens from
        // a previous Memory instance or adapter to a replacement provider.
        Clear();batch_provider_={};stable_validator_={};provider_=std::move(provider);certified_minimum_bytes_=minimum_bytes;
    }
    // Both providers must certify all aliases. The validator may only reuse the
    // exact previous token; it must never capture/re-arm an invalid token.
    void ConfigureStableCertifiedProvider(CertifiedProvider capture,CertifiedProvider validate,size_t minimum_bytes=0){
        Clear();batch_provider_={};provider_=std::move(capture);stable_validator_=std::move(validate);
        certified_minimum_bytes_=minimum_bytes;stable_stats_={};
    }
    // Only the live CPU-texture caller uses this optional provider. Replacing
    // it revokes all prior per-view tokens; no batch owns aggregate payloads.
    void ConfigureBatchCertifiedProvider(BatchProvider provider) {
        Clear();batch_provider_=std::move(provider);batch_stats_={};warm_batch_stats_={};
    }
    WarmBatchStats warm_batch_stats()const noexcept{return warm_batch_stats_;}
    BatchStats batch_stats()const noexcept {auto out=batch_stats_;out.enabled=bool(batch_provider_&&provider_&&stable_validator_);return out;}
    CertificateStats certificate_stats()const noexcept{return certificate_stats_;}
    StableCertificateStats stable_certificate_stats()const noexcept {
        auto out=stable_stats_;out.enabled=bool(provider_&&stable_validator_);return out;
    }
    explicit ImmutableSnapshots(size_t limit=128u*1024*1024,unsigned comparison_helpers=0):limit_(limit) {
        ConfigureComparisonHelpers(comparison_helpers);
    }
    // Configure before capture starts (or under its existing serialization).
    // Optional thread startup failure keeps the scalar/partial-pool path.
    void ConfigureComparisonHelpers(unsigned helpers) {
        // Drain/release the old optional pool first so reconfiguration never
        // overlaps two hardware-bounded sets of borrowed-span workers.
        comparison_.reset();
        if(helpers)comparison_=std::make_unique<ParallelSnapshotCompare>(helpers);
    }
    unsigned comparison_helpers()const noexcept{return comparison_?comparison_->helpers():0;}
    std::shared_ptr<const Bytes> Get(uint32_t address,std::span<const uint8_t> data) {
        if(provider_&&stable_validator_)return GetStable(address,data);
        const uint64_t key=uint64_t(address)<<32|data.size();
        auto it=entries_.find(key);
        Certified certified;
        // Below the configured minimum, retain the original exact comparison
        // and copy path. A size gate supplies no byte-identity proof.
        if(provider_&&data.size()<certified_minimum_bytes_) {
            ++certificate_stats_.minimum_size_bypasses;
            certificate_stats_.minimum_size_bypassed_bytes+=data.size();
        } else if(provider_) {
            ++certificate_stats_.attempts;
            try {certified=provider_(address,data.size(),it==entries_.end()?nullptr:it->second.certificate);}
            catch(...) {} // An optional certificate failure preserves exact fallback.
            if(certified.bytes&&certified.token&&certified.bytes->size()==data.size())++certificate_stats_.accepted;
            else {certified={};++certificate_stats_.fallback;}
        }
        if(it!=entries_.end()) {
            if(certified.bytes==it->second.bytes || (!certified.bytes&&it->second.bytes->size()==data.size()&&
               (!data.size()||(comparison_?comparison_->Equal(*it->second.bytes,data):
                  BytesEqual(it->second.bytes->data(),data.data(),data.size()))))) {
                if(certified.bytes)it->second.certificate=std::move(certified.token);
                order_.splice(order_.end(),order_,it->second.age);
                return it->second.bytes;
            }
            retained_-=it->second.bytes->size();order_.erase(it->second.age);entries_.erase(it);
        }
        auto bytes=certified.bytes?std::move(certified.bytes):std::make_shared<const Bytes>(data.begin(),data.end());
        if(data.size()>limit_)return bytes;
        while(retained_+data.size()>limit_&&!order_.empty()) {
            const auto oldest=entries_.find(order_.front());retained_-=oldest->second.bytes->size();
            entries_.erase(oldest);order_.pop_front();++cache_stats_.budget_evictions;
        }
        // Bound metadata as well as bytes for tiny or empty snapshots.
        if(entries_.size()>=4096) {
            if(metadata_observer_)metadata_observer_({address,data.size(),entries_.size(),retained_,metadata_lru_});
            if(metadata_lru_) {
                // Retire only this cache's oldest owner. Immutable users may
                // retain it independently; the exact bytes/token contract and
                // both existing bounds remain unchanged.
                const auto oldest=entries_.find(order_.front());
                retained_-=oldest->second.bytes->size();entries_.erase(oldest);order_.pop_front();
                ++cache_stats_.metadata_lru_evictions;
            } else {
                ++cache_stats_.metadata_clears;cache_stats_.metadata_clear_entries+=entries_.size();Clear();
            }
        }
        order_.push_back(key);entries_.emplace(key,Entry{bytes,std::prev(order_.end()),std::move(certified.token)});
        retained_+=data.size();cache_stats_.peak_entries=std::max(cache_stats_.peak_entries,entries_.size());return bytes;
    }
    // Each use obtains fresh SDK validation. Neither a successful batch nor
    // an ordinary write/observation epoch is remembered as byte readiness.
    std::vector<std::shared_ptr<const Bytes>> GetBatch(std::span<const View> views) {
        ++batch_stats_.calls;batch_stats_.views+=views.size();
        auto scalar=[&] {
            ++batch_stats_.scalar_fallback_calls;
            std::vector<std::shared_ptr<const Bytes>> out;out.reserve(views.size());
            for(const auto& view:views)out.push_back(Get(view.address,view.data));return out;
        };
        if(!batch_provider_||!provider_||!stable_validator_||views.empty()||views.size()>64)return scalar();
        struct Unique {View view;BatchDecision decision;};
        std::array<Unique,64> unique_storage{};size_t unique_count=0;
        std::array<size_t,64> mapping{};size_t view_index=0;
        uint64_t total=0;
        for(const auto& view:views) {
            if(view.data.empty()||view.data.size()>8u*1024*1024||
               uint64_t(view.address)+view.data.size()>0x20000000ull)return scalar();
            size_t index=0;
            for(;index<unique_count;++index)
                if(unique_storage[index].view.address==view.address&&unique_storage[index].view.data.size()==view.data.size())break;
            if(index==unique_count) {
                total+=view.data.size();if(total>8u*1024*1024)return scalar();
                unique_storage[unique_count++]={view,{}};
            }
            if(unique_storage[index].view.data.data()!=view.data.data())return scalar();
            mapping[view_index++]=index;
        }
        const auto unique=std::span<Unique>(unique_storage).first(unique_count);
        std::array<std::pair<uint64_t,uint64_t>,64> range_storage{};
        for(size_t i=0;i<unique.size();++i)range_storage[i]={unique[i].view.address,uint64_t(unique[i].view.address)+unique[i].view.data.size()};
        const auto ranges=std::span<std::pair<uint64_t,uint64_t>>(range_storage).first(unique_count);
        std::sort(ranges.begin(),ranges.end());
        uint64_t union_bytes=0,lo=ranges.front().first,hi=ranges.front().second;
        for(size_t i=1;i<ranges.size();++i) {
            if(ranges[i].first<=hi)hi=std::max(hi,ranges[i].second);
            else {union_bytes+=hi-lo;lo=ranges[i].first;hi=ranges[i].second;}
        }
        union_bytes+=hi-lo;
        const bool cohort=union_bytes>=certified_minimum_bytes_;
        batch_stats_.unique_views+=unique.size();if(cohort)batch_stats_.cohort_bytes+=union_bytes;
        std::array<BatchRequest,64> request_storage{};size_t request_count=0;
        for(auto& item:unique) {
            auto& decision=item.decision;const auto& view=item.view;
            decision.eligible=view.data.size()>=certified_minimum_bytes_||(cohort&&view.data.size()>=4096);
            const uint64_t key=uint64_t(view.address)<<32|view.data.size();const auto it=entries_.find(key);
            if(it==entries_.end())continue;
            decision.stability=it->second.stability;decision.old_bytes=it->second.bytes;decision.old_token=it->second.certificate;
            if(active_below_minimum_&&!decision.eligible&&decision.stability.active) {
                // A previously eligible cohort may leave an active small view.
                // Scalar ValidateOnly avoids allocating a one-view SDK batch;
                // failure cannot admit this below-floor entry for cold capture.
                decision.mode=1;decision.scalar_validation=true;
            } else if(decision.eligible&&(decision.stability.active||decision.stability.exact_hits>=decision.stability.required_hits)) {
                decision.mode=decision.stability.active?1:2;decision.request_index=request_count;
                request_storage[request_count++]={view.address,view.data.size(),decision.old_token,decision.mode==1};
            }
        }
        const auto requests=std::span<const BatchRequest>(request_storage).first(request_count);
        // Allocate final output before provider admission. An allocation failure
        // here has changed no tokens/protections and uses the original path.
        std::vector<std::shared_ptr<const Bytes>> out;
        try {out.resize(views.size());}catch(const std::bad_alloc&){return scalar();}
        std::vector<Certified> certified;
        if(!requests.empty()) {
            ++batch_stats_.provider_calls;batch_stats_.provider_views+=requests.size();
            try {certified=batch_provider_(requests);}
            catch(...) {++batch_stats_.provider_errors;}
            // A truncated/unmappable response certifies nothing. Never guess
            // an output index or fall through to scalar recapture after failure.
            if(certified.size()!=requests.size())certified.clear();
        }
        std::array<std::shared_ptr<const Bytes>,64> owners{};
        for(size_t i=0;i<unique.size();++i) {
            auto& item=unique[i];
            if(item.decision.scalar_validation) {
                try {item.decision.certified=stable_validator_(item.view.address,item.view.data.size(),item.decision.old_token);}
                catch(...) {++stable_stats_.provider_errors;}
            } else if(item.decision.mode&&!certified.empty())item.decision.certified=std::move(certified[item.decision.request_index]);
            owners[i]=GetStable(item.view.address,item.view.data,&item.decision);
        }
        for(size_t i=0;i<views.size();++i)out[i]=owners[mapping[i]];return out;
    }
    // Optional all-or-nothing warm path for bounded physical descriptors. A
    // provider accepted here MUST certify current canonical readability plus
    // all aliases, as well as the exact previous byte/token owners. It receives
    // ValidateOnly requests, never a guest pointer or permission to cold-arm.
    // Failure changes no cache owner, LRU, stability or ordinary batch counters:
    // the caller must execute its original readability/filter/capture path.
    // Provider-side diagnostic hits from a refused mixed batch are observable.
    std::optional<std::vector<std::shared_ptr<const Bytes>>> TryReuseReadableBatch(
        std::span<const WarmRange> views) {
        ++warm_batch_stats_.attempts;
        if(!batch_provider_||!provider_||!stable_validator_||views.empty()||views.size()>64)return {};
        struct Unique {WarmRange view;std::shared_ptr<const Bytes> bytes;std::shared_ptr<const void> token;};
        std::array<Unique,64> storage{};std::array<size_t,64> mapping{};
        size_t count=0;uint64_t total=0,requested_bytes=0;
        for(size_t i=0;i<views.size();++i) {
            const auto& view=views[i];
            if(view.size<4096||view.size>8u*1024*1024||uint64_t(view.address)+view.size>0x20000000ull)return {};
            requested_bytes+=view.size;
            size_t j=0;
            for(;j<count;++j)if(storage[j].view.address==view.address&&storage[j].view.size==view.size)break;
            if(j==count) {
                total+=view.size;if(total>8u*1024*1024)return {};
                const auto it=entries_.find(uint64_t(view.address)<<32|view.size);
                if(it==entries_.end()||!it->second.stability.active||!it->second.certificate||
                   !it->second.bytes||it->second.bytes->size()!=view.size)return {};
                storage[count++]={view,it->second.bytes,it->second.certificate};
            }
            mapping[i]=j;
        }
        // Exactly the original union floor: duplicates and overlapping ranges
        // cannot create eligibility. Every member must independently validate
        // before this union can stand in for the original readable-view set.
        std::array<std::pair<uint64_t,uint64_t>,64> ranges{};
        for(size_t i=0;i<count;++i)ranges[i]={storage[i].view.address,uint64_t(storage[i].view.address)+storage[i].view.size};
        std::sort(ranges.begin(),ranges.begin()+count);
        uint64_t union_bytes=0,lo=ranges[0].first,hi=ranges[0].second;
        for(size_t i=1;i<count;++i) {
            if(ranges[i].first<=hi)hi=std::max(hi,ranges[i].second);
            else {union_bytes+=hi-lo;lo=ranges[i].first;hi=ranges[i].second;}
        }
        union_bytes+=hi-lo;
        const bool cohort=union_bytes>=certified_minimum_bytes_;
        std::array<BatchRequest,64> requests{};
        for(size_t i=0;i<count;++i) {
            const auto& item=storage[i];
            if(item.view.size<certified_minimum_bytes_&&!cohort)return {};
            requests[i]={item.view.address,item.view.size,item.token,true};
        }
        std::vector<std::shared_ptr<const Bytes>> out;
        std::vector<Certified> certified;
        try {
            out.resize(views.size()); // Allocate before provider admission.
            ++warm_batch_stats_.provider_calls;
            certified=batch_provider_(std::span<const BatchRequest>(requests).first(count));
        } catch(...) {++warm_batch_stats_.provider_errors;return {};}
        if(certified.size()!=count){++warm_batch_stats_.provider_refusals;return {};}
        std::array<Entry*,64> current{};
        for(size_t i=0;i<count;++i) {
            const auto& item=storage[i];
            const auto it=entries_.find(uint64_t(item.view.address)<<32|item.view.size);
            if(certified[i].bytes!=item.bytes||certified[i].token!=item.token||
               it==entries_.end()||it->second.bytes!=item.bytes||it->second.certificate!=item.token||
               !it->second.stability.active) {++warm_batch_stats_.provider_refusals;return {};}
            current[i]=&it->second;
        }
        // All decisions are complete. No allocation/callback occurs while
        // committing exactly the ordinary successful GetBatch counters/LRU.
        ++batch_stats_.calls;batch_stats_.views+=views.size();batch_stats_.unique_views+=count;
        if(cohort)batch_stats_.cohort_bytes+=union_bytes;
        ++batch_stats_.provider_calls;batch_stats_.provider_views+=count;
        certificate_stats_.attempts+=count;certificate_stats_.accepted+=count;
        stable_stats_.validation_attempts+=count;stable_stats_.safe_hits+=count;
        for(size_t i=0;i<count;++i) {
            stable_stats_.safe_hit_bytes+=storage[i].view.size;
            order_.splice(order_.end(),order_,current[i]->age);
        }
        for(size_t i=0;i<views.size();++i)out[i]=storage[mapping[i]].bytes;
        ++warm_batch_stats_.accepted;warm_batch_stats_.accepted_views+=views.size();
        warm_batch_stats_.accepted_bytes+=requested_bytes;
        return out;
    }
    // True when this exact owner holds an active certificate, i.e. its pages
    // were validated or armed by the provider and every later write faults.
    bool CertifiedActive(uint32_t address,size_t size,const std::shared_ptr<const Bytes>& owner)const {
        const auto it=entries_.find(uint64_t(address)<<32|size);
        return it!=entries_.end()&&owner&&it->second.bytes==owner&&it->second.certificate&&it->second.stability.active;
    }
    void Clear(){entries_.clear();order_.clear();retained_=0;}
    size_t retained_bytes()const{return retained_;}
private:
    struct Stability {
        uint32_t exact_hits=0,required_hits=kStableCertificateInitialHits;
        bool active=false;
        void BackOff()noexcept {
            exact_hits=0;active=false;
            required_hits=std::min(kStableCertificateMaximumHits,required_hits*2);
        }
    };
    struct Entry {std::shared_ptr<const Bytes> bytes;std::list<uint64_t>::iterator age;std::shared_ptr<const void> certificate;Stability stability{};};
    struct BatchDecision {
        Stability stability;bool eligible=false,scalar_validation=false;unsigned mode=0;size_t request_index=0;
        std::shared_ptr<const Bytes> old_bytes;std::shared_ptr<const void> old_token;Certified certified;
    };
    std::shared_ptr<const Bytes> GetStable(uint32_t address,std::span<const uint8_t> data,const BatchDecision* batch=nullptr) {
        const uint64_t key=uint64_t(address)<<32|data.size();
        auto it=entries_.find(key);Certified certified;Stability stability;
        bool may_arm=true,backed_off=false;
        const bool eligible=batch?batch->eligible:data.size()>=certified_minimum_bytes_;
        const bool below_minimum=active_below_minimum_&&!eligible&&
            (batch?batch->scalar_validation:it!=entries_.end()&&it->second.stability.active);
        if(!eligible&&!below_minimum) {
            ++certificate_stats_.minimum_size_bypasses;
            certificate_stats_.minimum_size_bypassed_bytes+=data.size();
        }
        if(batch) {
            stability=batch->stability;certified=batch->certified;
            if(batch->mode==1) {
                ++certificate_stats_.attempts;++stable_stats_.validation_attempts;
                if(below_minimum)++stable_stats_.below_minimum_attempts;
                if(certified.token&&certified.token==batch->old_token&&certified.bytes&&
                   certified.bytes==batch->old_bytes&&certified.bytes->size()==data.size()) {
                    ++certificate_stats_.accepted;++stable_stats_.safe_hits;stable_stats_.safe_hit_bytes+=data.size();
                    if(below_minimum){++stable_stats_.below_minimum_hits;stable_stats_.below_minimum_hit_bytes+=data.size();}
                } else {
                    if(below_minimum)++stable_stats_.below_minimum_refusals;
                    if(certified.token||certified.bytes)++stable_stats_.owner_refusals;
                    certified={};++certificate_stats_.fallback;++stable_stats_.invalidations;
                    stability.BackOff();backed_off=true;may_arm=false;
                }
            } else if(batch->mode==2) {
                ++certificate_stats_.attempts;++stable_stats_.arm_attempts;
                if(certified.token&&certified.bytes&&certified.bytes->size()==data.size()) {
                    ++certificate_stats_.accepted;++stable_stats_.arms;stability.active=true;
                } else {
                    certified={};++certificate_stats_.fallback;++stable_stats_.arm_refusals;
                    stability.BackOff();backed_off=true;
                }
            } else if(eligible)++stable_stats_.cooldown_bypasses;
        }
        if(it!=entries_.end()) {
            if(!batch) {
            stability=it->second.stability;
            if((eligible||below_minimum)&&stability.active) {
                ++certificate_stats_.attempts;++stable_stats_.validation_attempts;
                if(below_minimum)++stable_stats_.below_minimum_attempts;
                try {certified=stable_validator_(address,data.size(),it->second.certificate);}
                catch(...) {++stable_stats_.provider_errors;}
                // A different token/byte owner is not reuse-only validation.
                // Fail closed even if a misconfigured adapter tries to recapture.
                if(certified.token&&certified.token==it->second.certificate&&
                   certified.bytes&&certified.bytes==it->second.bytes&&certified.bytes->size()==data.size()) {
                    ++certificate_stats_.accepted;++stable_stats_.safe_hits;
                    stable_stats_.safe_hit_bytes+=data.size();
                    if(below_minimum){++stable_stats_.below_minimum_hits;stable_stats_.below_minimum_hit_bytes+=data.size();}
                    order_.splice(order_.end(),order_,it->second.age);return it->second.bytes;
                }
                if(below_minimum)++stable_stats_.below_minimum_refusals;
                if(certified.token||certified.bytes)++stable_stats_.owner_refusals;
                certified={};++certificate_stats_.fallback;++stable_stats_.invalidations;
                stability.BackOff();may_arm=false;backed_off=true;
                // Retain the old immutable token only for a future fresh capture's
                // exact owner comparison. It cannot skip any warming read.
                it->second.stability=stability;
            }
            if(eligible&&may_arm&&stability.exact_hits>=stability.required_hits) {
                ++certificate_stats_.attempts;++stable_stats_.arm_attempts;
                try {certified=provider_(address,data.size(),it->second.certificate);}
                catch(...) {++stable_stats_.provider_errors;}
                if(certified.token&&certified.bytes&&certified.bytes->size()==data.size()) {
                    ++certificate_stats_.accepted;++stable_stats_.arms;stability.active=true;
                } else {
                    certified={};++certificate_stats_.fallback;++stable_stats_.arm_refusals;
                    stability.BackOff();backed_off=true;it->second.stability=stability;
                }
            } else if(eligible&&!stability.active)++stable_stats_.cooldown_bypasses;
            }
            if(certified.bytes) {
                if(certified.bytes==it->second.bytes) {
                    it->second.certificate=std::move(certified.token);it->second.stability=stability;
                    order_.splice(order_.end(),order_,it->second.age);return it->second.bytes;
                }
                // The fresh certificate is authoritative for its own immutable
                // bytes. Owner replacement preserves this key's accumulated
                // cooldown; no independent pointer/address hint supplies proof.
                stability.exact_hits=0;
            } else {
                ++stable_stats_.exact_fallbacks;stable_stats_.exact_fallback_bytes+=data.size();
                if(it->second.bytes->size()==data.size()&&(!data.size()||
                   (comparison_?comparison_->Equal(*it->second.bytes,data):
                    BytesEqual(it->second.bytes->data(),data.data(),data.size())))) {
                    ++stable_stats_.exact_hits;
                    if(eligible)stability.exact_hits=std::min(stability.required_hits,stability.exact_hits+1);
                    it->second.stability=stability;
                    order_.splice(order_.end(),order_,it->second.age);return it->second.bytes;
                }
                ++stable_stats_.exact_changes;
                if(!backed_off)stability.BackOff();
                else stability.exact_hits=0;
            }
            retained_-=it->second.bytes->size();order_.erase(it->second.age);entries_.erase(it);
        } else if(!certified.bytes) {
            ++stable_stats_.exact_fallbacks;stable_stats_.exact_fallback_bytes+=data.size();
            if(eligible&&!batch)++stable_stats_.cooldown_bypasses;
        }
        auto bytes=certified.bytes?std::move(certified.bytes):std::make_shared<const Bytes>(data.begin(),data.end());
        if(data.size()>limit_)return bytes;
        while(retained_+data.size()>limit_&&!order_.empty()) {
            const auto oldest=entries_.find(order_.front());retained_-=oldest->second.bytes->size();
            entries_.erase(oldest);order_.pop_front();++cache_stats_.budget_evictions;
        }
        if(entries_.size()>=4096) {
            if(metadata_observer_)metadata_observer_({address,data.size(),entries_.size(),retained_,metadata_lru_});
            if(metadata_lru_) {
                const auto oldest=entries_.find(order_.front());
                retained_-=oldest->second.bytes->size();entries_.erase(oldest);order_.pop_front();
                ++cache_stats_.metadata_lru_evictions;
            } else {
                ++cache_stats_.metadata_clears;cache_stats_.metadata_clear_entries+=entries_.size();Clear();
            }
        }
        order_.push_back(key);entries_.emplace(key,Entry{bytes,std::prev(order_.end()),std::move(certified.token),stability});
        retained_+=data.size();cache_stats_.peak_entries=std::max(cache_stats_.peak_entries,entries_.size());return bytes;
    }
    BatchProvider batch_provider_;BatchStats batch_stats_{};
    WarmBatchStats warm_batch_stats_{};
    CertifiedProvider provider_;CertificateStats certificate_stats_{};
    CertifiedProvider stable_validator_;StableCertificateStats stable_stats_{};
    size_t certified_minimum_bytes_=0;
    bool active_below_minimum_=false;
    CacheStats cache_stats_{};bool metadata_lru_=false;
    MetadataEvictionObserver metadata_observer_=nullptr;
    size_t limit_,retained_=0;
    std::list<uint64_t> order_;
    std::unordered_map<uint64_t,Entry> entries_;
    // Declared last: helper shutdown precedes release of cached byte owners.
    std::unique_ptr<ParallelSnapshotCompare> comparison_;
};
}
