#pragma once
#include <nlohmann/json.hpp>
#include <array>
#include <cstddef>
#include <concepts>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

namespace gow3::renderer {
// Exact canonical Identity() fields, in their serialized field order, followed
// by sign interpretation. This only accelerates lookup; the original ordered
// string-keyed map remains authoritative for ownership and retirement.
using CanonicalSampledAlias=std::array<uint32_t,11>;
inline std::optional<CanonicalSampledAlias> TryCanonicalSampledAlias(
    const nlohmann::json& identity,uint32_t sign_bits) {
    if(!identity.is_object()||identity.size()!=10)return {};
    CanonicalSampledAlias result{};result[10]=sign_bits;
    constexpr std::array<const char*,10> names{
        "base","dimension","endian","format","height","layers","packed","pitch","tiled","width"};
    for(size_t i=0;i<names.size();++i) {
        const auto found=identity.find(names[i]);if(found==identity.end())return {};
        const auto& value=*found;
        if(i==0) {
            if(!value.is_string())return {};
            const auto& base=value.get_ref<const std::string&>();if(base.size()!=8)return {};
            uint32_t address=0;
            for(char c:base) {
                unsigned digit;
                if(c>='0'&&c<='9')digit=unsigned(c-'0');
                else if(c>='a'&&c<='f')digit=unsigned(c-'a')+10;
                else return {};
                address=(address<<4)|digit;
            }
            result[i]=address;
        } else if(i==6||i==8) {
            if(!value.is_boolean())return {};
            result[i]=value.get<bool>();
        } else if(value.is_number_unsigned()) {
            const auto number=value.get<uint64_t>();
            if(number>std::numeric_limits<uint32_t>::max())return {};
            result[i]=uint32_t(number);
        } else if(value.is_number_integer()) {
            const auto number=value.get<int64_t>();
            if(number<0||uint64_t(number)>std::numeric_limits<uint32_t>::max())return {};
            result[i]=uint32_t(number);
        } else return {};
    }
    return result;
}
struct CanonicalSampledAliasHash {
    size_t operator()(const CanonicalSampledAlias& key) const {
        size_t hash=0;
        for(uint32_t word:key)hash^=size_t(word)+size_t(0x9e3779b9u)+(hash<<6)+(hash>>2);
        return hash;
    }
};

// PrimaryMap must keep iterators stable on insertion (the scene uses std::map).
// Clear BEFORE erasing from that map, including at every EndFrame retirement.
// The helper owns no GPU resources and no references to operation JSON.
template<class PrimaryMap,class Hash=CanonicalSampledAliasHash>
class SampledAliasIndex {
public:
    static constexpr size_t kMaxEntries=512;
    struct Statistics {size_t hits=0,canonical_misses=0,fallbacks=0,capacity_clears=0;};
    explicit SampledAliasIndex(PrimaryMap& primary):primary_(primary) {}
    SampledAliasIndex(const SampledAliasIndex&)=delete;
    SampledAliasIndex& operator=(const SampledAliasIndex&)=delete;
    typename PrimaryMap::iterator FindOrCreate(const nlohmann::json& identity,uint32_t sign_bits) {
        const auto canonical=TryCanonicalSampledAlias(identity,sign_bits);
        if(canonical) {
            const auto found=entries_.find(*canonical);
            if(found!=entries_.end()){++statistics_.hits;return found->second;}
            ++statistics_.canonical_misses;
        } else ++statistics_.fallbacks;
        // Keep the exact existing key, including extra fields, numeric spelling
        // and noncanonical base strings. Canonical misses enter this map too.
        auto key=nlohmann::json::array({identity,sign_bits}).dump();
        const auto stored=primary_.try_emplace(std::move(key)).first;
        if(canonical) {
            if(entries_.size()==kMaxEntries){entries_.clear();++statistics_.capacity_clears;}
            entries_.emplace(*canonical,stored);
        }
        return stored;
    }
    // Typed live input has already captured the exact canonical fields. Build
    // the legacy identity JSON only on a lookup miss, keeping the authoritative
    // ordered key identical to the offline path.
    template<std::invocable SerializeIdentity>
    typename PrimaryMap::iterator FindOrCreate(const CanonicalSampledAlias& canonical,
                                               SerializeIdentity&& serialize_identity) {
        const auto found=entries_.find(canonical);
        if(found!=entries_.end()){++statistics_.hits;return found->second;}
        ++statistics_.canonical_misses;
        auto key=nlohmann::json::array({serialize_identity(),canonical[10]}).dump();
        const auto stored=primary_.try_emplace(std::move(key)).first;
        if(entries_.size()==kMaxEntries){entries_.clear();++statistics_.capacity_clears;}
        entries_.emplace(canonical,stored);
        return stored;
    }
    void Clear(){entries_.clear();}
    // Drop only entries whose primary element is about to be erased. Survivors
    // keep valid std::map iterators, so a typed hit skips identity JSON + dump.
    template<class Keep>void Retain(Keep&& keep) {
        std::erase_if(entries_,[&](const auto& entry){return !keep(entry.second->first);});
    }
    size_t size() const{return entries_.size();}
    const Statistics& statistics() const{return statistics_;}
private:
    PrimaryMap& primary_;
    std::unordered_map<CanonicalSampledAlias,typename PrimaryMap::iterator,Hash> entries_;
    Statistics statistics_;
};
}
