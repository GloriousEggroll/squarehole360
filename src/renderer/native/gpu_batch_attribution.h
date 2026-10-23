#pragma once
#include "gpu_timestamp.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <vector>

namespace gow3::renderer {
// Diagnostic identity only. It never participates in command admission,
// resource lifetime, guest visibility, or query publication.
enum class NativeGpuWorkKind : uint8_t { Unknown, Draw, Clear, Resolve, AliasLoad, RetainedPhaseRestore, RetainedPhaseExport, SurfaceConversion };
struct NativeGpuWorkTag {
    uint64_t event=0;
    uint32_t frame=UINT32_MAX,pair=0,depth_control=0,color_mask=0,blend_mask=0;
    uint32_t width=0,height=0,indices=0;
    NativeGpuWorkKind kind=NativeGpuWorkKind::Unknown;
    bool discarded=false,queried=false;
    // Physical-list diagnostic only; never used for render admission.
    uint32_t vertex_variant=0,pixel_variant=0,cohort=0;
    bool cohort_known=false,fsi_state_specialized=false;
};
struct NativeGpuBatchComponent {
    NativeGpuWorkTag tag;
    uint32_t lists=0;
    uint64_t indices=0;
    static auto Key(const NativeGpuWorkTag& tag) noexcept {
        return std::tuple(tag.kind,tag.pair,tag.depth_control,tag.color_mask,tag.blend_mask,
                          tag.width,tag.height,tag.discarded,tag.queried);
    }
};
struct NativeGpuBatchSample {
    static constexpr size_t kComponents=16;
    uint64_t submission=0,first_event=UINT64_MAX,last_event=0,retired_steady_ns=0;
    uint32_t first_frame=UINT32_MAX,last_frame=0,lists=0,draws=0,transfers=0;
    uint32_t untagged=0,component_count=0,overflow_lists=0;
    bool frame_known=true;
    std::optional<NativeGpuTimestampInterval> interval;
    double host_wait_ms=0,submit_ms=0;
    std::array<NativeGpuBatchComponent,kComponents> components{};
    void Add(const NativeGpuWorkTag& tag,bool draw) noexcept {
        ++lists;if(draw)++draws;else ++transfers;
        first_event=std::min(first_event,tag.event);last_event=std::max(last_event,tag.event);
        if(tag.frame==UINT32_MAX)frame_known=false;
        else {first_frame=std::min(first_frame,tag.frame);last_frame=std::max(last_frame,tag.frame);}
        if(tag.kind==NativeGpuWorkKind::Unknown||(draw&&!tag.pair))++untagged;
        for(uint32_t i=0;i<component_count;++i)if(NativeGpuBatchComponent::Key(components[i].tag)==NativeGpuBatchComponent::Key(tag)) {
            ++components[i].lists;components[i].indices+=tag.indices;return;
        }
        if(component_count==components.size()){++overflow_lists;return;}
        components[component_count++]={tag,1,tag.indices};
    }
    bool Homogeneous() const noexcept {
        return lists&&component_count==1&&!overflow_lists&&!untagged&&frame_known&&first_frame==last_frame;
    }
};
class NativeGpuBatchAttribution {
public:
    static constexpr size_t kPairs=2048,kRecent=256,kSlowest=32;
    struct Pair {
        std::string vertex,pixel; // Empty pixel means the built-in depth module.
        uint64_t draw_lists=0,participating_batches=0,homogeneous_batches=0,homogeneous_intervals=0;
        double homogeneous_inclusive_ms=0;
    };
    struct Category {
        uint64_t batches=0,intervals=0,lists=0;
        double inclusive_ms=0;
    };
    struct Totals {
        uint64_t batches=0,intervals=0,unavailable=0,homogeneous=0,mixed=0,untagged=0;
        uint64_t component_overflows=0,pair_overflows=0,pair_failures=0,evicted_recent=0;
        double homogeneous_inclusive_ms=0,mixed_inclusive_ms=0;
    };
    NativeGpuBatchAttribution(){pairs_.reserve(kPairs);}
    // Called once per successful pipeline entry, not once per draw. Own all
    // strings; expired operation or cache owners cannot dangle in a report.
    uint32_t InternPair(std::string_view vertex,std::optional<std::string_view> pixel) noexcept {
        try {
            if(vertex.size()!=64||(pixel&&pixel->size()!=64)){++totals_.pair_failures;return 0;}
            const auto p=pixel.value_or(std::string_view{});
            for(size_t i=0;i<pairs_.size();++i)
                if(pairs_[i].vertex==vertex&&pairs_[i].pixel==p)return uint32_t(i+1);
            if(pairs_.size()==kPairs){++totals_.pair_overflows;return 0;}
            Pair next;next.vertex=vertex;next.pixel=p;pairs_.push_back(std::move(next));
            return uint32_t(pairs_.size());
        } catch(...) {++totals_.pair_failures;return 0;}
    }
    // Only the caller's already-fenced prefix may reach this method. No
    // allocations, callbacks, waits or resource references occur here.
    void Record(const NativeGpuBatchSample& sample) noexcept {
        ++totals_.batches;
        if(sample.interval)++totals_.intervals;else ++totals_.unavailable;
        if(sample.Homogeneous()) {
            ++totals_.homogeneous;
            if(sample.interval)totals_.homogeneous_inclusive_ms+=sample.interval->milliseconds;
            const auto kind=size_t(sample.components[0].tag.kind);
            if(kind<categories_.size()) {
                auto& category=categories_[kind];++category.batches;category.lists+=sample.lists;
                if(sample.interval){++category.intervals;category.inclusive_ms+=sample.interval->milliseconds;}
            }
        } else {
            ++totals_.mixed;
            if(sample.interval)totals_.mixed_inclusive_ms+=sample.interval->milliseconds;
        }
        totals_.untagged+=sample.untagged;totals_.component_overflows+=sample.overflow_lists;
        for(uint32_t i=0;i<sample.component_count;++i) {
            const auto& c=sample.components[i];
            if(c.tag.kind!=NativeGpuWorkKind::Draw||!c.tag.pair||c.tag.pair>pairs_.size())continue;
            auto& pair=pairs_[c.tag.pair-1];pair.draw_lists+=c.lists;
            bool first=true;
            for(uint32_t j=0;j<i;++j)if(sample.components[j].tag.kind==NativeGpuWorkKind::Draw&&sample.components[j].tag.pair==c.tag.pair)first=false;
            if(first)++pair.participating_batches;
            if(sample.Homogeneous()&&!c.tag.discarded) {
                ++pair.homogeneous_batches;
                if(sample.interval){++pair.homogeneous_intervals;pair.homogeneous_inclusive_ms+=sample.interval->milliseconds;}
            }
        }
        recent_[recent_next_]=sample;recent_next_=(recent_next_+1)%kRecent;
        if(recent_count_<kRecent)++recent_count_;else ++totals_.evicted_recent;
        if(!sample.interval)return;
        size_t index=slow_count_;
        if(slow_count_<kSlowest)++slow_count_;
        else {
            index=0;
            for(size_t i=1;i<kSlowest;++i)if(slowest_[i].interval->milliseconds<slowest_[index].interval->milliseconds)index=i;
            if(sample.interval->milliseconds<=slowest_[index].interval->milliseconds)return;
        }
        slowest_[index]=sample;
    }
    const auto& totals() const noexcept{return totals_;}
    const auto& pairs() const noexcept{return pairs_;}
    const auto& categories() const noexcept{return categories_;}
    size_t recent_count() const noexcept{return recent_count_;}
    const NativeGpuBatchSample& recent(size_t index) const {
        if(index>=recent_count_)throw std::out_of_range("GPU batch sample index");
        return recent_[(recent_next_+kRecent-recent_count_+index)%kRecent];
    }
    std::span<const NativeGpuBatchSample> slowest() const noexcept{return {slowest_.data(),slow_count_};}
private:
    std::vector<Pair> pairs_;
    Totals totals_;
    std::array<Category,8> categories_{};
    std::array<NativeGpuBatchSample,kRecent> recent_{};
    std::array<NativeGpuBatchSample,kSlowest> slowest_{};
    size_t recent_next_=0,recent_count_=0,slow_count_=0;
};
} // namespace gow3::renderer
