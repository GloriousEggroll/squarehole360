#include "renderer/native/native_monotonic_depth_module.h"
#include "renderer/native/content_hash.h"
#include "renderer/native/fsi_state_specialization_compiler.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace gow3::renderer;
namespace fs = std::filesystem;
namespace {
size_t checks = 0;
void Check(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
std::vector<uint8_t> Read(const fs::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    const auto size = file.tellg();
    if (!file || size < 0 || size > 2 * 1024 * 1024)
        throw std::runtime_error("Invalid bounded fixture input: " + path.string());
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), size))
        throw std::runtime_error("Truncated fixture input");
    return bytes;
}
std::span<const uint8_t> Bytes(const std::vector<uint32_t>& words) {
    return {reinterpret_cast<const uint8_t*>(words.data()), words.size() * sizeof(uint32_t)};
}
} // namespace

int main(int argc, char** argv) try {
    if (argc != 4 && argc != 5)
        throw std::runtime_error("Usage: fixture ROOT audit.json result.json [--recompile]");
    const fs::path root = argv[1];
    const bool recompile = argc == 5 && std::string_view(argv[4]) == "--recompile";
    Check(argc == 4 || recompile, "Unknown fixture option");
    const auto manifest_bytes = Read(argv[2]);
    const auto manifest = nlohmann::json::parse(manifest_bytes);
    Check(manifest.at("refused").empty() && manifest.at("modules").size() == 192,
          "Cohort size or structural audit differs");
    for (const auto& [name, hash] : manifest.at("source_sha256").items())
        Check(ContentHash(Read(root / name)) == hash.get<std::string>(), "Audited source changed");
    NativeShaderCompiler compiler; // Explicitly no persistent compiler cache.
    Check(NativeShaderCompiler::CacheVersion() == manifest.at("canonical_compiler_version").get<std::string>(),
          "Linked canonical compiler differs");
    Check(FsiStateSpecializationCompilerVersion() == manifest.at("fsi_state_version").get<std::string>(),
          "Linked state compiler differs");
    size_t vertices = 0, canonical_pixels = 0, state_pixels = 0, recompiled = 0;
    for (const auto& row : manifest.at("modules")) {
        const auto source = row.at("source").get<std::string>();
        const bool pixel = row.at("stage") == "pixel";
        const auto variant = row.at("variant") == "Unmodified" ?
            NativeShaderVariant::Unmodified : NativeShaderVariant::PipelineStateEpilogueV1;
        const auto file = Read(root / row.at("path").get<std::string>());
        const size_t offset = row.at("offset");
        Check(offset <= file.size(), "Fixture offset overflow");
        std::vector<uint8_t> code(file.begin() + offset, file.end());
        Check(code.size() == row.at("bytes").get<size_t>() && ContentHash(code) == row.at("sha256").get<std::string>(),
              "Actual module differs from offline audit");
        Check(NativeMonotonicDepthModuleCertified(source, pixel, variant, code), "Pinned module rejected");
        Check(!NativeMonotonicDepthModuleCertified(source, !pixel, variant, code), "Wrong stage accepted");
        Check(!NativeMonotonicDepthModuleCertified(std::string(64, '0'), pixel, variant, code), "Wrong source accepted");
        Check(!NativeMonotonicDepthModuleCertified(source.substr(1), pixel, variant, code), "Truncated source accepted");
        Check(!NativeMonotonicDepthModuleCertified(source, pixel, variant, {}), "Empty module accepted");
        for (unsigned other = 0; other <= unsigned(NativeShaderVariant::StencilMaterialCutoffV1); ++other)
            if (other != unsigned(variant))
                Check(!NativeMonotonicDepthModuleCertified(source, pixel,
                    static_cast<NativeShaderVariant>(other), code), "Wrong variant accepted");
        Check(!NativeMonotonicDepthModuleCertified(source, pixel,
            static_cast<NativeShaderVariant>(255), code), "Invalid variant accepted");
        for (const size_t at : {size_t(0), size_t(16), code.size() / 2, code.size() - 1}) {
            code[at] ^= 1;
            Check(!NativeMonotonicDepthModuleCertified(source, pixel, variant, code), "Mutated module accepted");
            code[at] ^= 1;
        }
        code.push_back(0);
        Check(!NativeMonotonicDepthModuleCertified(source, pixel, variant, code), "Appended module accepted");
        code.pop_back();
        Check(!NativeMonotonicDepthModuleCertified(source, pixel, variant,
            std::span<const uint8_t>(code).first(code.size() - 1)), "Truncated module accepted");
        if (!pixel) ++vertices;
        else if (variant == NativeShaderVariant::Unmodified) ++canonical_pixels;
        else ++state_pixels;
        if (!recompile) continue;
        const auto container = Read(root / row.at("container").get<std::string>());
        Check(ContentHash(container) == source, "Original guest container changed");
        const auto canonical = compiler.Get(container, pixel ?
            NativeShaderMode::FragmentInterlock : NativeShaderMode::Raster);
        Check(canonical->source_sha256 == source &&
            canonical->stage == (pixel ? NativeShaderStage::Pixel : NativeShaderStage::Vertex) &&
            !canonical->disk_cache_hit, "Cold canonical source provenance differs");
        if (variant == NativeShaderVariant::Unmodified) {
            Check(std::ranges::equal(Bytes(canonical->spirv), code), "Cold canonical recompile differs");
        } else {
            const auto state = TryPrepareFsiStateSpecialization(compiler, container);
            Check(state && !state->disk_cache_hit && state->source_sha256 == source &&
                state->compiler_version == NativeShaderCompiler::CacheVersion() &&
                state->variant_version == FsiStateSpecializationCompilerVersion() &&
                state->specialization_mask == canonical->specialization_mask,
                "Cold state source or guest ABI provenance differs");
            Check(std::ranges::equal(Bytes(state->spirv), code), "Cold state recompile differs");
            Check(state->baseline_hlsl_sha256 == row.at("baseline_hlsl_sha256").get<std::string>() &&
                state->modified_hlsl_sha256 == row.at("modified_hlsl_sha256").get<std::string>() &&
                state->modified_common_sha256 == row.at("modified_common_sha256").get<std::string>(),
                "Reconstructed common/unchanged guest body provenance differs");
        }
        ++recompiled;
    }
    Check(vertices == 33 && canonical_pixels == 75 && state_pixels == 84, "Missing positive stage/variant coverage");
    Check(manifest.at("supporting_uncertified_modules").size() == 9,
          "Missing supporting-only canonical coverage");
    for (const auto& row : manifest.at("supporting_uncertified_modules")) {
        const auto source = row.at("source").get<std::string>();
        const auto code = Read(root / row.at("path").get<std::string>());
        Check(row.at("stage") == "pixel" && row.at("variant") == "Unmodified" &&
              row.at("offset") == 0 && ContentHash(code) == row.at("sha256").get<std::string>(),
              "Supporting-only source bytes differ");
        Check(!NativeMonotonicDepthModuleCertified(source, true,
            NativeShaderVariant::Unmodified, code), "Unauthenticated selected variant admitted");
        Check(!NativeMonotonicDepthModuleCertified(source, true,
            NativeShaderVariant::PipelineStateEpilogueV1, code), "Canonical bytes accepted as derived state module");
        if (recompile) {
            const auto container = Read(root / row.at("container").get<std::string>());
            Check(ContentHash(container) == source, "Supporting source changed");
            const auto canonical = compiler.Get(container, NativeShaderMode::FragmentInterlock);
            Check(!canonical->disk_cache_hit && std::ranges::equal(Bytes(canonical->spirv), code),
                  "Supporting-only canonical cold recompile differs");
        }
    }
    const nlohmann::json result{{"passed", true}, {"checks", checks},
        {"vertices", vertices}, {"canonical_pixels", canonical_pixels},
        {"state_pixels", state_pixels}, {"cold_recompiled_modules", recompiled},
        {"manifest_sha256", ContentHash(manifest_bytes)}, {"gpu_executed", false}};
    std::ofstream out(argv[3]);
    out << result.dump(2) << '\n';
    if (!out) throw std::runtime_error("Cannot write fixture result");
    std::cout << result.dump() << '\n';
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
