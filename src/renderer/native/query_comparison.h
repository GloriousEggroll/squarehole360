#pragma once
#include <nlohmann/json.hpp>
#include <cstdint>
#include <functional>
#include <optional>

namespace gow3::renderer {
struct ReferenceQuerySample {
    uint64_t samples = 0, submission = 0;
    uint32_t guest_samples = 0;
    uint32_t skipped_async_draws = 0;
};
// Read only reference-owned completion data, never a concurrently reused guest
// report. The caller must first poll reference retirement after the native
// same-queue fence. Missing/stale results are reported as unverified.
nlohmann::json CompareNativeQueries(const nlohmann::json& scene_report,uint32_t api_frame,
    const std::function<std::optional<ReferenceQuerySample>(uint32_t end_address)>& read_reference);
// An empty tracker during handback may mean no native query has begun yet.
// Missing history is therefore valid; a completed result is usable only for
// this exact packet generation and report address, including a true zero.
std::optional<uint64_t> FindCompletedNativeQuerySamples(const nlohmann::json& scene_report,
    uint32_t end_address,uint64_t generation);
}
