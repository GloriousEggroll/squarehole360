#pragma once
#include "renderer/native/shader_bindings.h"
#include "renderer/native/shader_byte_segments.h"
#include "renderer/native/shader_requirement_masks.h"
#include <optional>
#include <unordered_map>

namespace gow3::renderer {
namespace detail {
struct ShaderInputFingerprint {
    size_t operator()(const ShaderByteSegments& bytes) const {return bytes.Fingerprint();}
};
}
// Used only while the bridge's capture mutex is held. Entries own complete
// immutable shader bytes; neither captured blocks nor guest pointers escape.
// Fingerprints select candidates, but every hit compares both exact spans.
template<class Fingerprint=detail::ShaderInputFingerprint>
class BasicShaderInputCache {
public:
    static constexpr size_t kMaxEntries=256,kMaxRetainedBytes=8u*1024*1024;
    struct Statistics {size_t hits=0,decodes=0,evictions=0,uncached=0,unsupported=0,unsupported_hits=0;};
    explicit BasicShaderInputCache(size_t max_entries=kMaxEntries,size_t max_bytes=kMaxRetainedBytes)
        :max_entries_(max_entries),max_bytes_(max_bytes) {
        if(max_entries>kMaxEntries||max_bytes>kMaxRetainedBytes)
            throw std::invalid_argument("Shader input cache exceeds its fixed bounds");
    }
    std::optional<ShaderRequirementMasks> Get(std::span<const GraphicsBoundaryBlock> captured) {
        // Match SnapshotShaderInputs exactly: the final block of each kind
        // wins; absent/empty VS is unknown, absent/empty PS is optional.
        std::array<const std::vector<uint8_t>*,4> blocks{};
        for(const auto& block:captured)
            if(block.kind>=2&&block.kind<=5)blocks[block.kind-2]=&block.bytes;
        ShaderRequirementMasks required;
        try {
            for(unsigned stage=0;stage<2;++stage) {
                const auto* header=blocks[stage*2];const auto* code=blocks[stage*2+1];
                if(!header||!code||header->empty()) {
                    if(stage)continue;
                    return {};
                }
                // Unsupported inputs must still select full diagnostic capture.
                // The planner decides whether to reject them or use a fallback.
                const auto stage_required=Stage(*header,*code);
                if(!stage_required)return {};
                required.Merge(*stage_required);
            }
        } catch(const std::exception&) {
            return {};
        }
        return required;
    }
    void Clear() {entries_.clear();retained_bytes_=0;statistics_={};}
    // Startup/reset only, under the same bridge mutex as Get. Never retain
    // buckets built using a different fingerprint policy.
    void ConfigureFingerprint(Fingerprint fingerprint) {
        Clear();fingerprint_=std::move(fingerprint);
    }
    size_t size() const {return entries_.size();}
    size_t retained_bytes() const {return retained_bytes_;}
    const Statistics& statistics() const {return statistics_;}
private:
    // supported=false caches an analysis failure: a shader the analysis
    // rejects (an unlinked memcpy kernel, issued thousands of times in a
    // texture streaming burst) must not throw and unwind on every draw.
    struct Entry {std::shared_ptr<const std::vector<uint8_t>> bytes;ShaderRequirementMasks required;bool supported=true;};
    std::unordered_multimap<size_t,Entry> entries_;
    size_t retained_bytes_=0,max_entries_,max_bytes_;
    Statistics statistics_;
    Fingerprint fingerprint_{};
    std::optional<ShaderRequirementMasks> Stage(std::span<const uint8_t> header,std::span<const uint8_t> code) {
        const ShaderByteSegments bytes(header,code);
        const auto fingerprint=fingerprint_(bytes);
        const auto [first,last]=entries_.equal_range(fingerprint);
        for(auto it=first;it!=last;++it)if(bytes.Matches(*it->second.bytes)) {
            if(!it->second.supported){++statistics_.unsupported_hits;return std::nullopt;}
            ++statistics_.hits;return it->second.required;
        }
        const auto snapshot=bytes.Snapshot();++statistics_.decodes;
        std::optional<ShaderRequirementMasks> required;
        try {required.emplace(ShaderFetchRequirements(*snapshot));}
        catch(const std::exception&) {++statistics_.unsupported;}
        if(!max_entries_||bytes.size()>max_bytes_) {++statistics_.uncached;return required;}
        if(entries_.size()>=max_entries_||retained_bytes_>max_bytes_-bytes.size()) {
            statistics_.evictions+=entries_.size();entries_.clear();retained_bytes_=0;
        }
        entries_.emplace(fingerprint,Entry{snapshot,required.value_or(ShaderRequirementMasks{}),bool(required)});
        retained_bytes_+=bytes.size();
        return required;
    }
};
using ShaderInputCache=BasicShaderInputCache<>;
}
