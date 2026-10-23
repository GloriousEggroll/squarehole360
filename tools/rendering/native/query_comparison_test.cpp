#include "renderer/native/query_comparison.h"
#include <iostream>
#include <stdexcept>
#include <unordered_map>
using namespace gow3::renderer;
using Json=nlohmann::json;
int main() {
    try {
        if(FindCompletedNativeQuerySamples(Json::object(),0x2000,1)||
           FindCompletedNativeQuerySamples({{"native_occlusion_results",Json::array()}},0x2000,1))
            throw std::runtime_error("Unstarted native query acquired a fabricated history result");
        Json completed={{"native_occlusion_results",Json::array({{{"generation",6},{"end_address",0x2000},{"samples",99}}})}};
        if(FindCompletedNativeQuerySamples(completed,0x2000,7))
            throw std::runtime_error("Reused query report inherited an old generation's samples");
        completed["native_occlusion_results"].back()["generation"]=7;
        completed["native_occlusion_results"].back()["samples"]=0;
        if(FindCompletedNativeQuerySamples(completed,0x2000,7)!=std::optional<uint64_t>(0))
            throw std::runtime_error("Completed native zero was confused with missing history");
        completed["native_occlusion_results"].back()["samples"]=230400;
        if(FindCompletedNativeQuerySamples(completed,0x2000,7)!=std::optional<uint64_t>(230400))
            throw std::runtime_error("Completed native subtotal was lost at handback");
        for(auto boundary:{std::pair{0x2040u,uint64_t(7)},std::pair{0x2000u,uint64_t(6)}}) {
            bool rejected=false;
            try{FindCompletedNativeQuerySamples(completed,boundary.first,boundary.second);}
            catch(const std::runtime_error&){rejected=true;}
            if(!rejected)throw std::runtime_error("Mismatched query completion boundary was accepted");
        }
        Json report={{"native_occlusion_results",Json::array()}};
        auto query=[&](uint32_t frame,uint32_t address,uint64_t samples) {
            report["native_occlusion_results"].push_back({{"frame",frame},{"handle",address+0x1000},
                {"end_address",address},{"samples",samples}});
        };
        query(16,0x2000,99); // A previous frame must not enter this comparison.
        query(17,0x2000,512);query(17,0x2040,0);query(17,0x2080,6);query(17,0x20c0,0);
        query(17,0x2100,3);query(17,0x2100,9); // Reused reports are ambiguous.
        const std::unordered_map<uint32_t,ReferenceQuerySample> references={
            {0x2000,{512,80,512}},{0x2040,{1,80,1}},{0x2080,{12,81,12}},{0x2100,{9,81,9}}};
        unsigned reads=0;
        const auto comparison=CompareNativeQueries(report,17,[&](uint32_t address)->std::optional<ReferenceQuerySample>{
            ++reads;const auto it=references.find(address);
            return it==references.end()?std::nullopt:std::optional(it->second);
        });
        if(comparison.at("queries")!=6||comparison.at("verified")!=3||
            comparison.at("missing_reference")!=1||comparison.at("ambiguous_reports")!=2||reads!=4||
            comparison.at("exact_matches")!=1||comparison.at("sample_mismatches")!=2||
            comparison.at("visibility_matches")!=2||comparison.at("visibility_mismatches")!=1||
            comparison.at("published_to_guest")!=false)
            throw std::runtime_error("Incorrect query comparison: "+comparison.dump());
        // A guest compatibility override must not turn a true GPU zero into
        // evidence that the reference depth test found visible samples.
        const auto override_report=CompareNativeQueries(report,17,[](uint32_t address)->std::optional<ReferenceQuerySample>{
            return address==0x2040?std::optional(ReferenceQuerySample{0,90,1}):std::nullopt;
        });
        if(override_report.at("verified")!=1||override_report.at("exact_matches")!=1||
            override_report.at("visibility_matches")!=1||override_report.at("results").at(1).at("reference_guest_samples")!=1)
            throw std::runtime_error("Guest visibility override contaminated GPU comparison");
        const auto incomplete=CompareNativeQueries(report,17,[](uint32_t address)->std::optional<ReferenceQuerySample>{
            return address==0x2000?std::optional(ReferenceQuerySample{0,91,0,2}):std::nullopt;
        });
        const auto& skipped=incomplete.at("results").at(0);
        if(incomplete.at("verified")!=0||incomplete.at("incomplete_reference")!=1||
            incomplete.at("sample_mismatches")!=0||incomplete.at("visibility_mismatches")!=0||
            skipped.at("reference_samples")!=0||skipped.at("native_samples")!=512||
            skipped.at("reference_skipped_async_draws")!=2||skipped.at("verified")!=false)
            throw std::runtime_error("Incomplete reference query was treated as validation evidence");
        const auto mixed=CompareNativeQueries(report,17,[](uint32_t address)->std::optional<ReferenceQuerySample>{
            if(address==0x2000)return ReferenceQuerySample{0,92,1,1};
            if(address==0x2040)return ReferenceQuerySample{0,92,0};
            if(address==0x2080)return ReferenceQuerySample{9,93,9};
            return std::nullopt;
        });
        if(mixed.at("verified")!=2||mixed.at("incomplete_reference")!=1||
            mixed.at("missing_reference")!=1||mixed.at("ambiguous_reports")!=2||
            mixed.at("exact_matches")!=1||mixed.at("sample_mismatches")!=1||
            mixed.at("visibility_matches")!=2||mixed.at("visibility_mismatches")!=0||
            mixed.at("results").at(0).at("reference_guest_samples")!=1)
            throw std::runtime_error("Mixed query completeness contaminated comparison counts");
        std::cout<<"Query comparisons: exact counts, visibility, missing/stale/reused/incomplete reports and guest override passed\n";
        return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
