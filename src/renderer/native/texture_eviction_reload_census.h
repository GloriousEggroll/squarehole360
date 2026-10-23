#pragma once

#include "dormant_cache.h"
#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <initializer_list>
#include <string_view>

namespace gow3::renderer {

// Diagnostic identity only: every field used by CpuImageLess is represented
// exactly. Noncanonical digest strings are counted, never truncated or hashed.
struct NativeTextureCensusIdentity {
    std::array<uint32_t,6> fetch{};
    uint64_t version=0;
    std::array<char,64> base{},mips{};
    bool has_base=false,has_mips=false;
    bool operator==(const NativeTextureCensusIdentity&) const = default;
};
template<class Key> bool NativeTextureCensusIdentityOf(const Key& key,
        NativeTextureCensusIdentity& out) noexcept {
    out={};out.fetch=key.fetch;out.version=key.version;
    const auto copy=[](const auto& value,std::array<char,64>& target,bool& present) {
        present=bool(value);if(!present)return true;
        const std::string_view text(*value);if(text.size()!=target.size())return false;
        for(std::size_t i=0;i<text.size();++i) {
            if(!((text[i]>='0'&&text[i]<='9')||(text[i]>='a'&&text[i]<='f')))return false;
            target[i]=text[i];
        }
        return true;
    };
    return copy(key.base,out.base,out.has_base)&&copy(key.mips,out.mips,out.has_mips);
}

struct NativeTextureCensusStages {
    double payload_ms=0,overlay_ms=0,decode_ms=0,upload_ms=0;
};
struct NativeTextureCensusCounters {
    uint64_t creations=0,recent_reloads=0,not_recent_or_untrackable=0;
    uint64_t untrackable_creations=0,evictions=0,untrackable_evictions=0;
    uint64_t entry_budget_evictions=0,byte_budget_evictions=0,both_budget_evictions=0;
    uint64_t disabled_budget_evictions=0,age_evictions=0,binding_evictions=0,wrap_evictions=0;
    uint64_t reload_entry_budget=0,reload_byte_budget=0,reload_both_budget=0;
    uint64_t reload_disabled_budget=0,reload_age=0,reload_binding=0,reload_wrap=0;
    uint64_t created_allocation_bytes=0,evicted_allocation_bytes=0;
    double creation_ms=0,reload_ms=0,flush_pending_ms=0,eviction_ms=0;
    double payload_ms=0,overlay_ms=0,decode_ms=0,upload_ms=0;
    double maximum_creation_ms=0,maximum_reload_ms=0,maximum_eviction_ms=0;
};

class NativeTextureEvictionReloadCensus {
public:
    using Clock=std::chrono::steady_clock;
    static constexpr std::size_t kMaximumTombstones=512,kMaximumFrames=128;
    struct Frame {bool valid=false;uint32_t frame=0;uint64_t completed_frame_epoch=0;NativeTextureCensusCounters counters;};
    struct Tombstone {
        bool valid=false;NativeTextureCensusIdentity identity;
        uint64_t serial=0;uint32_t frame=0;
        DormantCacheEvictionKind kind=DormantCacheEvictionKind::Age;
        bool entry_pressure=false,byte_pressure=false;
    };
    struct Creation {
        bool valid=false,identity_valid=false,recent=false;
        NativeTextureCensusIdentity identity;
        uint64_t tombstone_serial=0,completed_frame_epoch=0;uint32_t frame=0;
        DormantCacheEvictionKind kind=DormantCacheEvictionKind::Age;
        bool entry_pressure=false,byte_pressure=false;
        Clock::time_point began;NativeTextureCensusStages before;
        double flush_pending_ms=0;
    };
    struct Eviction {
        bool valid=false,identity_valid=false;
        NativeTextureCensusIdentity identity;
        uint32_t frame=0;uint64_t allocation_bytes=0,completed_frame_epoch=0;
        DormantCacheEvictionKind kind=DormantCacheEvictionKind::Age;
        bool entry_pressure=false,byte_pressure=false;
        Clock::time_point began;
    };
    struct Observer {
        static constexpr bool kEnabled=true;
        NativeTextureEvictionReloadCensus* census;
        uint32_t frame;
        bool frame_valid=true;
        template<class Key,class Entry> Eviction Begin(const Key& key,const Entry& entry,
                DormantCacheEvictionKind kind,bool entry_pressure,bool byte_pressure) const noexcept {
            return frame_valid?census->BeginEviction(key,entry.residency.allocation_bytes,frame,kind,entry_pressure,byte_pressure):Eviction{};
        }
        void End(const Eviction& eviction) const noexcept {census->EndEviction(eviction);}
    };
    NativeTextureCensusCounters total;
    std::array<Frame,kMaximumFrames> frames{};
    uint64_t tombstones_overwritten=0,metadata_failures=0;
    bool overflowed=false;

    template<class Key> Creation BeginCreation(const Key& key,uint32_t frame,
            NativeTextureCensusStages stages) noexcept {
        Creation result;result.valid=true;result.frame=frame;result.before=stages;result.completed_frame_epoch=completed_frame_epoch_;
        result.identity_valid=NativeTextureCensusIdentityOf(key,result.identity);
        if(result.identity_valid)for(const auto& entry:tombstones_)
            if(entry.valid&&entry.identity==result.identity&&entry.serial>result.tombstone_serial) {
                result.recent=true;result.tombstone_serial=entry.serial;result.kind=entry.kind;
                result.entry_pressure=entry.entry_pressure;result.byte_pressure=entry.byte_pressure;
            }
        // Copies/searches are outside the timed creation body.
        result.began=Clock::now();return result;
    }
    void EndCreation(const Creation& creation,uint64_t allocation_bytes,
            NativeTextureCensusStages after) noexcept {
        if(!creation.valid)return;
        const auto elapsed=Milliseconds(creation.began,Clock::now());
        auto& frame=FrameFor(creation.frame,creation.completed_frame_epoch);
        for(auto* counters:{&total,&frame.counters}) {
            Add(counters->creations);Add(counters->created_allocation_bytes,allocation_bytes);
            if(creation.recent) {
                Add(counters->recent_reloads);counters->reload_ms+=elapsed;
                counters->maximum_reload_ms=std::max(counters->maximum_reload_ms,elapsed);
                CountKind(*counters,creation.kind,creation.entry_pressure,creation.byte_pressure,true);
            } else Add(counters->not_recent_or_untrackable);
            if(!creation.identity_valid)Add(counters->untrackable_creations);
            counters->creation_ms+=elapsed;counters->flush_pending_ms+=creation.flush_pending_ms;
            counters->maximum_creation_ms=std::max(counters->maximum_creation_ms,elapsed);
            counters->payload_ms+=after.payload_ms-creation.before.payload_ms;
            counters->overlay_ms+=after.overlay_ms-creation.before.overlay_ms;
            counters->decode_ms+=after.decode_ms-creation.before.decode_ms;
            counters->upload_ms+=after.upload_ms-creation.before.upload_ms;
        }
        if(creation.recent)for(auto& entry:tombstones_)
            if(entry.valid&&entry.serial==creation.tombstone_serial)entry.valid=false;
    }
    template<class Key> Eviction BeginEviction(const Key& key,uint64_t bytes,uint32_t frame,
            DormantCacheEvictionKind kind,bool entry_pressure,bool byte_pressure) noexcept {
        Eviction result;result.valid=true;result.frame=frame;result.allocation_bytes=bytes;
        result.kind=kind;result.entry_pressure=entry_pressure;result.byte_pressure=byte_pressure;result.completed_frame_epoch=completed_frame_epoch_;
        result.identity_valid=NativeTextureCensusIdentityOf(key,result.identity);
        result.began=Clock::now();return result;
    }
    void EndEviction(const Eviction& eviction) noexcept {
        if(!eviction.valid)return;
        const auto elapsed=Milliseconds(eviction.began,Clock::now());auto& frame=FrameFor(eviction.frame,eviction.completed_frame_epoch);
        for(auto* counters:{&total,&frame.counters}) {
            Add(counters->evictions);Add(counters->evicted_allocation_bytes,eviction.allocation_bytes);
            if(!eviction.identity_valid)Add(counters->untrackable_evictions);
            CountKind(*counters,eviction.kind,eviction.entry_pressure,eviction.byte_pressure,false);
            counters->eviction_ms+=elapsed;counters->maximum_eviction_ms=std::max(counters->maximum_eviction_ms,elapsed);
        }
        if(!eviction.identity_valid)return;
        if(next_serial_==UINT64_MAX){overflowed=true;return;}
        auto& entry=tombstones_[next_tombstone_];
        if(entry.valid)Add(tombstones_overwritten);
        entry={true,eviction.identity,++next_serial_,eviction.frame,eviction.kind,
            eviction.entry_pressure,eviction.byte_pressure};
        next_tombstone_=(next_tombstone_+1)%tombstones_.size();
    }
    static double Milliseconds(Clock::time_point from,Clock::time_point to) noexcept {
        return std::chrono::duration<double,std::milli>(to-from).count();
    }
    void MetadataFailure() noexcept {Add(metadata_failures);}
    void EndFrame() noexcept {Add(completed_frame_epoch_);}
    uint64_t completed_frame_epoch() const noexcept {return completed_frame_epoch_;}
private:
    std::array<Tombstone,kMaximumTombstones> tombstones_{};
    std::size_t next_tombstone_=0,next_frame_=0;uint64_t next_serial_=0,completed_frame_epoch_=0;
    void Add(uint64_t& value,uint64_t amount=1) noexcept {
        if(amount>UINT64_MAX-value){value=UINT64_MAX;overflowed=true;}else value+=amount;
    }
    Frame& FrameFor(uint32_t frame,uint64_t epoch) noexcept {
        for(auto& entry:frames)if(entry.valid&&entry.frame==frame&&entry.completed_frame_epoch==epoch)return entry;
        auto& entry=frames[next_frame_];entry={true,frame,epoch,{}};next_frame_=(next_frame_+1)%frames.size();return entry;
    }
    void CountKind(NativeTextureCensusCounters& counters,DormantCacheEvictionKind kind,
            bool entry_pressure,bool byte_pressure,bool reload) noexcept {
        switch(kind) {
        case DormantCacheEvictionKind::Budget:
            if(entry_pressure&&byte_pressure)Add(reload?counters.reload_both_budget:counters.both_budget_evictions);
            else if(entry_pressure)Add(reload?counters.reload_entry_budget:counters.entry_budget_evictions);
            else if(byte_pressure)Add(reload?counters.reload_byte_budget:counters.byte_budget_evictions);
            else MetadataFailure();
            break;
        case DormantCacheEvictionKind::DisabledBudget:Add(reload?counters.reload_disabled_budget:counters.disabled_budget_evictions);break;
        case DormantCacheEvictionKind::Age:Add(reload?counters.reload_age:counters.age_evictions);break;
        case DormantCacheEvictionKind::Bindings:Add(reload?counters.reload_binding:counters.binding_evictions);break;
        case DormantCacheEvictionKind::Wrap:Add(reload?counters.reload_wrap:counters.wrap_evictions);break;
        }
    }
};
} // namespace gow3::renderer
