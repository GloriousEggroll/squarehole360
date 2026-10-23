#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace gow3::renderer {
using LiveResourceBytes=std::shared_ptr<const std::vector<uint8_t>>;
struct LiveResourceHash {
    using is_transparent=void;
    // Keys are SHA-256 hex digests, already uniformly distributed, so their
    // first 16 characters (64 bits of entropy) need only a cheap mix. This
    // hasher counts as "fast" to libstdc++, which then recomputes it on every
    // rehash and erase instead of caching it, so it must stay a few cycles.
    // Any other key keeps the standard string hash.
    size_t operator()(std::string_view value)const noexcept{
        if(value.size()!=64)return std::hash<std::string_view>{}(value);
        uint64_t first,second;
        std::memcpy(&first,value.data(),sizeof(first));
        std::memcpy(&second,value.data()+sizeof(first),sizeof(second));
        const uint64_t mixed=(first^(second>>1|second<<63))*0x9E3779B97F4A7C15ull;
        return size_t(mixed^(mixed>>29));
    }
};
using LiveResourceMap=std::unordered_map<std::string,LiveResourceBytes,LiveResourceHash,std::equal_to<>>;

// Keys borrow immutable operation digest storage. The containing frame must own
// those operations until after this object dies; byte owners are independent.
// The full producer bank stays mutable and never becomes a consumer lookup.
class LivePrefixResources {
    struct Entry {std::string_view digest;LiveResourceBytes bytes;size_t encounter;};
    using Table=std::vector<Entry>;
    Table payloads_,shaders_;
    size_t reserve_hint_=0,limit_=kMaximumDependencies;
    bool sealed_=false;
    const Table& Values(bool shader)const{return shader?shaders_:payloads_;}
public:
    // Larger prefixes keep the old map path. This limits scratch even when a
    // long query interval repeats the same few resources thousands of times.
    static constexpr size_t kMaximumDependencies=128;
    // Direct-execution batches: storage is sorted once and binary-searched, so
    // a wider bound only costs scratch, and avoids the canonical map fallback.
    static constexpr size_t kMaximumDirectDependencies=1024;
    explicit LivePrefixResources(size_t dependencies=0,size_t limit=kMaximumDependencies)
        :reserve_hint_(dependencies),limit_(limit) {
        if(limit>kMaximumDirectDependencies||dependencies>limit)throw std::invalid_argument("Flat prefix exceeds its dependency bound");
    }
    void Append(std::string_view digest,LiveResourceBytes bytes,bool shader) {
        if(sealed_)throw std::logic_error("Cannot append to a sealed native prefix");
        if(!bytes)throw std::invalid_argument("Null native prefix resource");
        auto& values=shader?shaders_:payloads_;
        if(values.size()>=limit_)throw std::runtime_error("Flat prefix exceeds its dependency bound");
        if(values.empty())values.reserve(reserve_hint_);
        values.push_back({digest,std::move(bytes),values.size()});
    }
    size_t Seal() {
        if(sealed_)throw std::logic_error("Native prefix was already sealed");
        size_t retained=0;
        for(auto* values:{&payloads_,&shaders_}) {
            std::sort(values->begin(),values->end(),[](const auto& a,const auto& b) {
                const auto order=a.digest.compare(b.digest);
                return order?order<0:a.encounter<b.encounter;
            });
            const auto last=std::unique(values->begin(),values->end(),[](const auto& a,const auto& b) {
                if(a.digest!=b.digest)return false;
                if(a.bytes!=b.bytes)throw std::logic_error("Native prefix resource owner changed during publication");
                return true;
            });
            values->erase(last,values->end());
            for(const auto& entry:*values)retained+=entry.bytes->size();
        }
        sealed_=true;return retained;
    }
    const LiveResourceBytes* Find(std::string_view digest,bool shader)const {
        if(!sealed_)throw std::logic_error("Native prefix lookup before publication");
        const auto& values=Values(shader);
        const auto found=std::lower_bound(values.begin(),values.end(),digest,
            [](const auto& entry,std::string_view key){return entry.digest<key;});
        return found!=values.end()&&found->digest==digest?&found->bytes:nullptr;
    }
    size_t size(bool shader)const{return Values(shader).size();}
    template<class Visit>void ForEach(bool shader,Visit&& visit)const {
        if(!sealed_)throw std::logic_error("Native prefix enumeration before publication");
        for(const auto& entry:Values(shader))visit(entry.digest,entry.bytes);
    }
    // Diagnostics used the original unordered_map traversal, including for a
    // capped export. Rebuild only on that cold path with identical first-insert
    // order and default bucket growth; no reserve may change its iteration.
    LiveResourceMap Materialize(bool shader)const {
        if(!sealed_)throw std::logic_error("Native prefix export before publication");
        const auto& values=Values(shader);
        std::vector<const Entry*> encounter;encounter.reserve(values.size());
        for(const auto& entry:values)encounter.push_back(&entry);
        std::sort(encounter.begin(),encounter.end(),[](auto* a,auto* b){return a->encounter<b->encounter;});
        LiveResourceMap result;
        for(const auto* entry:encounter)result.emplace(std::string(entry->digest),entry->bytes);
        return result;
    }
};
} // namespace gow3::renderer
