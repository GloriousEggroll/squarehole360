#include "query_comparison.h"
#include <unordered_map>
namespace gow3::renderer {
std::optional<uint64_t> FindCompletedNativeQuerySamples(const nlohmann::json& report,
    uint32_t address,uint64_t generation) {
    const auto it=report.find("native_occlusion_results");
    if(it==report.end())return {};
    if(!it->is_array())throw std::runtime_error("Invalid native query completion history");
    if(it->empty())return {};
    const auto& latest=it->back();
    const auto completed_generation=latest.at("generation").get<uint64_t>();
    if(completed_generation<generation)return {};
    if(completed_generation!=generation||latest.at("end_address").get<uint32_t>()!=address)
        throw std::runtime_error("Native query completion history is ahead of its packet boundary");
    return latest.at("samples").get<uint64_t>();
}
nlohmann::json CompareNativeQueries(const nlohmann::json& report,uint32_t frame,
    const std::function<std::optional<ReferenceQuerySample>(uint32_t)>& read_reference) {
    using Json=nlohmann::json;
    Json comparison={{"frame",frame},{"queries",0},{"verified",0},{"missing_reference",0},{"ambiguous_reports",0},{"incomplete_reference",0},
        {"exact_matches",0},{"visibility_matches",0},{"sample_mismatches",0},{"visibility_mismatches",0},
        {"results",Json::array()},{"published_to_guest",false}};
    if(!report.contains("native_occlusion_results"))return comparison;
    std::unordered_map<uint32_t,size_t> addresses;
    for(const auto& query:report.at("native_occlusion_results"))
        if(query.at("frame")==frame)++addresses[query.at("end_address").get<uint32_t>()];
    auto increment=[&](const char* key){comparison[key]=comparison[key].get<uint64_t>()+1;};
    for(const auto& query:report.at("native_occlusion_results")) {
        if(query.at("frame")!=frame)continue;
        increment("queries");
        const uint32_t address=query.at("end_address");const uint64_t native=query.at("samples");
        Json result={{"handle",query.at("handle")},{"end_address",address},{"native_samples",native}};
        // The reference cache identifies the last completed use of an address.
        // Reused report storage within one frame needs generation tracking; do
        // not compare an earlier native query against a later reference query.
        if(addresses.at(address)>1) {
            increment("ambiguous_reports");result["verified"]=false;
            result["reason"]="report_address_reused_in_frame";
            comparison["results"].push_back(std::move(result));continue;
        }
        const auto reference=read_reference(address);
        if(!reference) {
            increment("missing_reference");result["verified"]=false;
        } else {
            result["reference_samples"]=reference->samples;
            result["reference_guest_samples"]=reference->guest_samples;
            result["reference_submission"]=reference->submission;
            result["reference_skipped_async_draws"]=reference->skipped_async_draws;
            // A pending reference shader/pipeline causes that backend to skip
            // its draw. The raw count is useful evidence, but cannot validate
            // native depth or visibility while reference work is incomplete.
            if(reference->skipped_async_draws) {
                increment("incomplete_reference");result["verified"]=false;
                result["reason"]="reference_skipped_async_draws";
                comparison["results"].push_back(std::move(result));continue;
            }
            increment("verified");result["verified"]=true;
            result["exact_match"]=native==reference->samples;
            result["visibility_match"]=bool(native)==bool(reference->samples);
            increment(native==reference->samples?"exact_matches":"sample_mismatches");
            increment(bool(native)==bool(reference->samples)?"visibility_matches":"visibility_mismatches");
        }
        comparison["results"].push_back(std::move(result));
    }
    return comparison;
}
}
