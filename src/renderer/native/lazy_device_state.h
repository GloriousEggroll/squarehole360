#pragma once
#include "live_prefix_resources.h"
#include <array>
#include <atomic>
#include <mutex>
#include <span>
#include <unordered_map>
#include <unordered_set>

namespace gow3::renderer {
// Rendering consumes the immutable captured bytes. A canonical identity is
// computed only when a diagnostic/legacy consumer actually asks for it.
class LazyDeviceState {
    LiveResourceBytes bytes_;
    mutable std::once_flag once_;
    mutable std::string digest_;
    mutable std::atomic<bool> materialized_{false};
public:
    static constexpr size_t kBytes=0x3600;
    explicit LazyDeviceState(LiveResourceBytes bytes):bytes_(std::move(bytes)) {
        if(!bytes_||bytes_->size()!=kBytes)throw std::invalid_argument("Invalid lazy device state extent");
    }
    const LiveResourceBytes& Owner()const noexcept{return bytes_;}
    std::string_view Digest()const;
    bool HasDigest()const noexcept{return materialized_.load(std::memory_order_acquire);}
};
using LazyDeviceStateRef=std::shared_ptr<const LazyDeviceState>;

// Device-state copies are made on the guest render thread for every draw and
// released by the GPU Commands thread once the frame retires. Recycling their
// buffers keeps ~14 KB per draw out of the allocator's cross-thread path.
class DeviceStateBufferPool {
    static constexpr size_t kLimit=1024;
    struct Free {std::mutex mutex;std::vector<std::vector<uint8_t>> buffers;};
    // Leaked: a frame may be released after static destruction at exit.
    static Free& Pool(){static Free* pool=new Free;return *pool;}
public:
    // Leaves `out` untouched when no buffer is available.
    static void Take(std::vector<uint8_t>& out) {
        auto& pool=Pool();std::lock_guard lock(pool.mutex);
        if(pool.buffers.empty())return;
        out=std::move(pool.buffers.back());pool.buffers.pop_back();
    }
    static LiveResourceBytes Adopt(std::vector<uint8_t>&& bytes) {
        return LiveResourceBytes(new std::vector<uint8_t>(std::move(bytes)),[](const std::vector<uint8_t>* owned) {
            std::unique_ptr<std::vector<uint8_t>> released(const_cast<std::vector<uint8_t>*>(owned));
            if(released->capacity()!=LazyDeviceState::kBytes)return;
            auto& pool=Pool();std::lock_guard lock(pool.mutex);
            if(pool.buffers.size()<kLimit)pool.buffers.push_back(std::move(*released));
        });
    }
};

// The producer records original first-insertion order without hashing states.
// Published frames/prefixes seal their own table; no consumer references the
// mutable producer bank. Cold materialization recreates the old unordered_map
// growth and traversal without changing retained-byte accounting.
class LazyPayloadTable {
    struct Entry {
        std::array<char,64> digest{};
        LiveResourceBytes bytes;
        LazyDeviceStateRef state;
        std::string_view Key()const{return state?state->Digest():std::string_view(digest.data(),digest.size());}
    };
    std::vector<Entry> entries_;
    // Duplicate and lookup indexes; entry order (and thus every published
    // enumeration) is unchanged. Eager digests index by entry position because
    // entries move as the vector grows.
    std::unordered_multimap<size_t,size_t> eager_index_;
    std::unordered_set<const LazyDeviceState*> state_index_;
    size_t retained_=0;
    bool sealed_=false,prefix_=false;
    mutable std::once_flag canonical_once_;
    mutable LiveResourceMap canonical_;
    void CheckMutable()const {if(sealed_)throw std::logic_error("Cannot change a published lazy payload bank");}
public:
    explicit LazyPayloadTable(size_t reserve=0,bool prefix=false):prefix_(prefix){entries_.reserve(reserve);}
    void Append(std::string_view digest,LiveResourceBytes bytes,bool unique=false) {
        CheckMutable();
        if(digest.size()!=64||!bytes)throw std::invalid_argument("Invalid lazy payload entry");
        const size_t hash=LiveResourceHash{}(digest);
        if(!unique) {
            const auto [first,last]=eager_index_.equal_range(hash);
            for(auto it=first;it!=last;++it) {
                const auto& e=entries_[it->second];
                if(e.Key()!=digest)continue;
                if(e.bytes!=bytes)throw std::logic_error("Lazy prefix owner changed during publication");
                return;
            }
        }
        Entry e;std::copy(digest.begin(),digest.end(),e.digest.begin());e.bytes=std::move(bytes);
        entries_.push_back(std::move(e));retained_+=entries_.back().bytes->size();
        eager_index_.emplace(hash,entries_.size()-1);
    }
    void Append(LazyDeviceStateRef state,bool unique=false) {
        CheckMutable();if(!state)throw std::invalid_argument("Null lazy device state");
        if(!state_index_.insert(state.get()).second&&!unique)return;
        entries_.push_back({{},state->Owner(),std::move(state)});retained_+=LazyDeviceState::kBytes;
    }
    void Seal(){CheckMutable();sealed_=true;}
    size_t size()const noexcept{return entries_.size();}
    size_t retained_bytes()const noexcept{return retained_;}
    const LiveResourceMap& Canonical()const {
        if(!sealed_)throw std::logic_error("Lazy payload lookup before publication");
        std::call_once(canonical_once_,[&] {
            LiveResourceMap result;
            for(const auto& e:entries_)result.emplace(std::string(e.Key()),e.bytes);
            canonical_=std::move(result);
        });
        return canonical_;
    }
    const LiveResourceBytes* Find(std::string_view key)const {
        if(!sealed_)throw std::logic_error("Lazy payload lookup before publication");
        // Control operations retain canonical eager payload keys. Looking up
        // one must not force identities for unrelated typed draw states.
        if(key.size()==64) {
            const auto [first,last]=eager_index_.equal_range(LiveResourceHash{}(key));
            for(auto it=first;it!=last;++it)if(entries_[it->second].Key()==key)return &entries_[it->second].bytes;
        }
        const auto& values=Canonical();const auto found=values.find(key);
        return found==values.end()?nullptr:&found->second;
    }
    template<class Visit>void ForEach(Visit&& visit)const {
        const auto& values=Canonical();
        if(!prefix_) {for(const auto& [key,bytes]:values)visit(key,bytes);return;}
        // Preserve the existing flat-prefix lexicographic enumeration contract.
        std::vector<const LiveResourceMap::value_type*> sorted;sorted.reserve(values.size());
        for(const auto& entry:values)sorted.push_back(&entry);
        std::sort(sorted.begin(),sorted.end(),[](auto* a,auto* b){return a->first<b->first;});
        for(const auto* entry:sorted)visit(entry->first,entry->second);
    }
    // Used only by the independent producer oracle while a frame is mutable.
    // Taking a diagnostic snapshot never seals or mutates the source table.
    LiveResourceMap DiagnosticSnapshot()const {
        LiveResourceMap result;for(const auto& e:entries_)result.emplace(std::string(e.Key()),e.bytes);return result;
    }
};
} // namespace gow3::renderer
