#pragma once
#include "gpu_batch_attribution.h"
#include <nlohmann/json.hpp>
namespace gow3::renderer {
inline const char* NativeGpuWorkKindName(NativeGpuWorkKind kind) {
    switch(kind) {
        case NativeGpuWorkKind::Draw:return "draw";
        case NativeGpuWorkKind::Clear:return "clear";
        case NativeGpuWorkKind::Resolve:return "resolve";
        case NativeGpuWorkKind::AliasLoad:return "alias_load";
        case NativeGpuWorkKind::SurfaceConversion:return "surface_conversion";
        case NativeGpuWorkKind::RetainedPhaseRestore:return "retained_phase_restore";
        case NativeGpuWorkKind::RetainedPhaseExport:return "retained_phase_export";
        default:return "unknown";
    }
}
inline nlohmann::json ReportNativeGpuBatchSample(const NativeGpuBatchSample& sample) {
    using Json=nlohmann::json;
    Json result={{"submission",sample.submission},{"first_api_frame",sample.first_frame},
        {"last_api_frame",sample.last_frame},{"frame_known",sample.frame_known},
        {"first_event",sample.first_event},{"last_event",sample.last_event},
        {"retired_steady_ns",sample.retired_steady_ns},{"draw_lists",sample.draws},
        {"transfer_lists",sample.transfers},{"homogeneous",sample.Homogeneous()},
        {"mixed_or_unknown",!sample.Homogeneous()},{"untagged_lists",sample.untagged},
        {"composition_overflow_lists",sample.overflow_lists},{"submit_ms",sample.submit_ms},
        {"host_wait_ms",sample.host_wait_ms},{"inclusive_gpu_ms",nullptr},
        {"composition",Json::array()}};
    if(sample.interval) {
        result["inclusive_gpu_ms"]=sample.interval->milliseconds;
        result["begin_tick"]=sample.interval->begin_tick;
        result["end_tick"]=sample.interval->end_tick;
    }
    for(uint32_t i=0;i<sample.component_count;++i) {
        const auto& c=sample.components[i];const auto& t=c.tag;
        result["composition"].push_back({{"kind",NativeGpuWorkKindName(t.kind)},
            {"shader_pair",t.pair},{"lists",c.lists},{"indices",c.indices},
            {"discarded",t.discarded},{"queried",t.queried},{"depth_control",t.depth_control},
            {"color_mask",t.color_mask},{"blend_mask",t.blend_mask},
            {"width",t.width},{"height",t.height}});
    }
    return result;
}
inline nlohmann::json ReportNativeGpuBatchAttribution(const NativeGpuBatchAttribution& profile,bool details) {
    using Json=nlohmann::json;
    const auto& t=profile.totals();
    Json result={{"enabled",true},{"scope","retired unified-queue submissions only"},
        {"semantics","Inclusive batch interval, possibly overlapping adjacent batches. Mixed intervals are never divided or charged to a shader. Discarded lists are excluded from shader rankings. Direct synchronous submissions are not covered."},
        {"batches",t.batches},{"intervals",t.intervals},{"unavailable",t.unavailable},
        {"homogeneous_batches",t.homogeneous},{"mixed_or_unknown_batches",t.mixed},
        {"homogeneous_inclusive_ms",t.homogeneous_inclusive_ms},
        {"mixed_or_unknown_inclusive_ms",t.mixed_inclusive_ms},{"untagged_lists",t.untagged},
        {"component_overflow_lists",t.component_overflows},{"shader_pair_overflows",t.pair_overflows},
        {"shader_pair_failures",t.pair_failures},{"recent_evictions",t.evicted_recent},
        {"maximum_shader_pairs",NativeGpuBatchAttribution::kPairs},
        {"maximum_recent_batches",NativeGpuBatchAttribution::kRecent},
        {"maximum_components",NativeGpuBatchSample::kComponents},{"top_homogeneous_shader_pairs",Json::array()}};
    result["homogeneous_categories"]=Json::array();
    for(size_t i=0;i<profile.categories().size();++i) {
        const auto& c=profile.categories()[i];
        result["homogeneous_categories"].push_back({{"kind",NativeGpuWorkKindName(NativeGpuWorkKind(i))},
            {"batches",c.batches},{"intervals",c.intervals},{"lists",c.lists},{"inclusive_ms",c.inclusive_ms}});
    }
    std::array<uint32_t,NativeGpuBatchAttribution::kPairs> order{};
    for(size_t i=0;i<profile.pairs().size();++i)order[i]=uint32_t(i);
    std::sort(order.begin(),order.begin()+profile.pairs().size(),[&](uint32_t a,uint32_t b) {
        const auto& x=profile.pairs()[a];const auto& y=profile.pairs()[b];
        if(x.homogeneous_inclusive_ms!=y.homogeneous_inclusive_ms)return x.homogeneous_inclusive_ms>y.homogeneous_inclusive_ms;
        return a<b;
    });
    const auto pair_json=[&](uint32_t i) {
        const auto& p=profile.pairs()[i];
        return Json{{"id",i+1},{"vertex_shader",p.vertex},{"pixel_shader",p.pixel.empty()?Json(nullptr):Json(p.pixel)},
            {"draw_lists",p.draw_lists},{"participating_batches",p.participating_batches},
            {"homogeneous_batches",p.homogeneous_batches},{"homogeneous_intervals",p.homogeneous_intervals},{"homogeneous_inclusive_ms",p.homogeneous_inclusive_ms}};
    };
    for(size_t i=0;i<std::min(size_t(16),profile.pairs().size());++i)
        result["top_homogeneous_shader_pairs"].push_back(pair_json(order[i]));
    // A bounded recent window keeps loading totals from hiding a later camera
    // view. Observation is cold; never reset or mutate retirement counters.
    struct RecentPair {uint64_t batches=0,intervals=0;double milliseconds=0;};
    std::array<RecentPair,NativeGpuBatchAttribution::kPairs> recent_pairs{};
    uint32_t first_frame=UINT32_MAX,last_frame=0;uint64_t homogeneous=0,mixed=0;
    double homogeneous_ms=0,mixed_ms=0;
    for(size_t i=0;i<profile.recent_count();++i) {
        const auto& sample=profile.recent(i);
        if(sample.frame_known){first_frame=std::min(first_frame,sample.first_frame);last_frame=std::max(last_frame,sample.last_frame);}
        if(!sample.Homogeneous()){++mixed;if(sample.interval)mixed_ms+=sample.interval->milliseconds;continue;}
        ++homogeneous;if(sample.interval)homogeneous_ms+=sample.interval->milliseconds;
        const auto& tag=sample.components[0].tag;
        if(tag.kind==NativeGpuWorkKind::Draw&&!tag.discarded&&tag.pair&&tag.pair<=profile.pairs().size()) {
            auto& pair=recent_pairs[tag.pair-1];++pair.batches;
            if(sample.interval){++pair.intervals;pair.milliseconds+=sample.interval->milliseconds;}
        }
    }
    std::sort(order.begin(),order.begin()+profile.pairs().size(),[&](uint32_t a,uint32_t b) {
        if(recent_pairs[a].milliseconds!=recent_pairs[b].milliseconds)return recent_pairs[a].milliseconds>recent_pairs[b].milliseconds;
        if(recent_pairs[a].batches!=recent_pairs[b].batches)return recent_pairs[a].batches>recent_pairs[b].batches;
        return a<b;
    });
    Json recent={{"batches",profile.recent_count()},{"first_api_frame",first_frame},{"last_api_frame",last_frame},
        {"homogeneous_batches",homogeneous},{"mixed_or_unknown_batches",mixed},
        {"homogeneous_inclusive_ms",homogeneous_ms},{"mixed_or_unknown_inclusive_ms",mixed_ms},
        {"top_homogeneous_shader_pairs",Json::array()}};
    for(size_t i=0;i<std::min(size_t(16),profile.pairs().size());++i) {
        const auto id=order[i];const auto& pair=recent_pairs[id];if(!pair.batches)continue;
        const auto& identity=profile.pairs()[id];
        recent["top_homogeneous_shader_pairs"].push_back({{"id",id+1},{"vertex_shader",identity.vertex},
            {"pixel_shader",identity.pixel.empty()?Json(nullptr):Json(identity.pixel)},
            {"homogeneous_batches",pair.batches},{"homogeneous_intervals",pair.intervals},
            {"homogeneous_inclusive_ms",pair.milliseconds}});
    }
    result["recent_window"]=std::move(recent);
    if(details) {
        result["shader_pairs"]=Json::array();
        for(size_t i=0;i<profile.pairs().size();++i)result["shader_pairs"].push_back(pair_json(uint32_t(i)));
        result["recent_retired_batches"]=Json::array();
        for(size_t i=0;i<profile.recent_count();++i)result["recent_retired_batches"].push_back(ReportNativeGpuBatchSample(profile.recent(i)));
        result["slowest_retired_batches"]=Json::array();
        for(const auto& sample:profile.slowest())result["slowest_retired_batches"].push_back(ReportNativeGpuBatchSample(sample));
    }
    return result;
}
} // namespace gow3::renderer
