#pragma once
// Source-only candidate. Sidecar recipes never infer guest FSI state from the
// existing generic recipe. They reference sources held by the generic cache.
#include "fsi_state_specialization.h"
#include "renderer/native/pipeline_prewarm.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
namespace gow3::renderer {
struct NativeFsiStateRecipe {
    NativeFsiPipelineRecipe generic;
    NativeFsiStateSpecialization state;
    auto operator<=>(const NativeFsiStateRecipe&) const = default;
};
inline bool ValidFsiStateRecipe(const NativeFsiStateRecipe& recipe) noexcept {
    if(!ValidNativeFsiPipelineRecipe(recipe.generic))return false;
    const auto& words=recipe.state.words;
    if(words[1]>1u||(words[2]&~15u))return false;
    for(uint32_t target=0;target<4;++target) {
        const auto format=words[3+target*3];
        if(format!=0u&&format!=2u&&format!=3u&&format!=4u&&format!=5u&&
           format!=6u&&format!=7u&&format!=14u&&format!=15u)return false;
        if(words[4+target*3]>15u)return false;
    }
    // Full depth-control and blend words are exact uint32 values. Do not
    // canonicalize ignored bits, masks or inactive target interpretations.
    return true;
}
inline nlohmann::json FsiStateRecipeJson(const NativeFsiStateRecipe& recipe) {
    if(!ValidFsiStateRecipe(recipe))throw std::invalid_argument("Invalid native FSI state recipe");
    const auto& r=recipe.generic;
    return {{"vertex",r.vertex},{"pixel",r.pixel?nlohmann::json(*r.pixel):nlohmann::json(nullptr)},
            {"raster",r.raster},{"color",r.color_format},{"depth",r.depth_format},
            {"fsi_state",recipe.state.words}};
}
inline uint32_t FsiStateRecipeWord(const nlohmann::json& value) {
    if(value.is_number_unsigned()) {
        const auto n=value.get<uint64_t>();
        if(n>UINT32_MAX)throw std::invalid_argument("FSI recipe word overflow");
        return uint32_t(n);
    }
    if(!value.is_number_integer())throw std::invalid_argument("FSI recipe word is not an integer");
    const auto n=value.get<int64_t>();
    if(n<0||n>UINT32_MAX)throw std::invalid_argument("FSI recipe word out of range");
    return uint32_t(n);
}
inline NativeFsiStateRecipe ParseFsiStateRecipe(const nlohmann::json& json) {
    NativeFsiStateRecipe result;
    auto& generic=result.generic;
    generic.vertex=json.at("vertex").get<std::string>();
    if(!json.at("pixel").is_null())generic.pixel=json.at("pixel").get<std::string>();
    const auto& raster=json.at("raster");const auto& state=json.at("fsi_state");
    if(!raster.is_array()||raster.size()!=6||!state.is_array()||state.size()!=15)
        throw std::invalid_argument("FSI recipe state extent");
    for(size_t i=0;i<raster.size();++i)generic.raster[i]=FsiStateRecipeWord(raster[i]);
    for(size_t i=0;i<state.size();++i)result.state.words[i]=FsiStateRecipeWord(state[i]);
    // The disk host attachment recipe is deliberately canonical RGBA8/-1.
    // Scene maps color to -1 at startup if attachmentless policy is active.
    const auto color=FsiStateRecipeWord(json.at("color"));
    if(color>INT32_MAX)throw std::invalid_argument("FSI recipe color attachment overflow");
    generic.color_format=int32_t(color);
    const auto& depth=json.at("depth");
    if(!depth.is_number_integer()||depth.get<int64_t>()!=-1)
        throw std::invalid_argument("FSI recipe depth attachment");
    generic.depth_format=-1;
    if(!ValidFsiStateRecipe(result))throw std::invalid_argument("Invalid native FSI state recipe");
    return result;
}

struct RankedNativeFsiStateRecipe {
    NativeFsiStateRecipe recipe;
    uint64_t observed_count=0;
};
struct NativeFsiStateSidecar {
    static constexpr uint32_t kSchema=1;
    static constexpr size_t kMaxRecipes=512,kMaxBytes=512u*1024;
    std::string variant_version;
    std::vector<RankedNativeFsiStateRecipe> records;
    nlohmann::json Json() const {
        if(records.size()>kMaxRecipes)throw std::invalid_argument("FSI sidecar recipe budget");
        auto list=nlohmann::json::array();
        for(const auto& record:records) {
            auto value=FsiStateRecipeJson(record.recipe);value["observed_count"]=record.observed_count;
            list.push_back(std::move(value));
        }
        return {{"schema",kSchema},{"variant",variant_version},{"records",std::move(list)}};
    }
    static NativeFsiStateSidecar Parse(std::string_view text,std::string_view variant) {
        if(text.size()>kMaxBytes)throw std::invalid_argument("FSI sidecar byte budget");
        auto json=nlohmann::json::parse(text);NativeFsiStateSidecar result;
        if(!json.at("schema").is_number_integer()||FsiStateRecipeWord(json.at("schema"))!=kSchema||
           json.at("variant").get<std::string>()!=variant)
            throw std::invalid_argument("FSI sidecar version mismatch");
        result.variant_version=variant;
        const auto& list=json.at("records");
        if(!list.is_array()||list.size()>kMaxRecipes)throw std::invalid_argument("FSI sidecar recipe extent");
        for(const auto& value:list) {
            const auto& count=value.at("observed_count");uint64_t observed=0;
            if(count.is_number_unsigned())observed=count.get<uint64_t>();
            else if(count.is_number_integer()&&count.get<int64_t>()>=0)observed=uint64_t(count.get<int64_t>());
            else throw std::invalid_argument("FSI sidecar observation count");
            result.records.push_back({ParseFsiStateRecipe(value),observed});
        }
        std::ranges::sort(result.records,[](const auto& a,const auto& b){return a.recipe<b.recipe;});
        if(std::adjacent_find(result.records.begin(),result.records.end(),
            [](const auto& a,const auto& b){return a.recipe==b.recipe;})!=result.records.end())
            throw std::invalid_argument("Duplicate FSI sidecar tuple");
        return result;
    }
    std::vector<RankedNativeFsiStateRecipe> Authenticated(std::span<const ObservedNativePipeline> sources) const {
        std::vector<RankedNativeFsiStateRecipe> result;
        for(const auto& record:records)if(std::ranges::any_of(sources,[&](const auto& source){
            return source.recipe==record.recipe.generic;
        }))result.push_back(record);
        std::ranges::sort(result,[](const auto& a,const auto& b){
            return a.observed_count!=b.observed_count?a.observed_count>b.observed_count:a.recipe<b.recipe;
        });
        return result;
    }
};
}
