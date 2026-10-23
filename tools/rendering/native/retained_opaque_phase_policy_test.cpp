#include "renderer/native/retained_opaque_phase_catalog.h"
#include "renderer/native/retained_opaque_phase_headroom.h"
#include "renderer/native/retained_opaque_catalog.h"
#include "renderer/native/cache_bookkeeping.h"
#include <algorithm>
#include <array>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using namespace gow3::renderer;
namespace {
uint64_t checks = 0;
void Check(bool result, const char* reason) {
    ++checks;
    if (!result) throw std::runtime_error(reason);
}
using Raster = std::array<uint32_t, 6>;
std::string Key(std::string_view vertex, std::string_view pixel, const Raster& raster) {
    std::string key = std::string(vertex) + '|' + std::string(pixel) + '|';
    for (size_t i = 0; i < raster.size(); ++i) {
        if (i) key += ',';
        key += std::to_string(raster[i]);
    }
    return key + "|3,1\n";
}
void ClosedInventory() {
    static_assert(kRetainedOpaquePhaseMaxPrograms == 53);
    static_assert(kRetainedOpaquePhaseMaxPipelines == 65);
    static_assert(kRetainedOpaquePhaseMaxModuleBytes == 3432836);
    static_assert(kRetainedOpaqueCatalogMaxPrograms == 3 && kRetainedOpaqueCatalogMaxPipelines == 6);
    static_assert(kRetainedOpaqueCatalogMaxModuleBytes == 16u * 1024 * 1024);
    static_assert(!std::is_assignable_v<decltype((std::declval<NativeRetainedOpaquePhaseProgram&>().spirv)),
                                      std::vector<uint32_t>>);
    static_assert(!std::is_assignable_v<decltype((std::declval<NativeRetainedOpaquePhaseProgram&>().source_sha256)),
                                      std::string>);
    static_assert(!std::is_copy_assignable_v<NativeRetainedOpaquePhaseProgram>);
    std::set<std::string> exact;
    std::set<std::string_view> pixels, vertices;
    std::set<std::pair<std::string_view, std::string_view>> pairs;
    std::map<std::string_view, std::set<std::string_view>> pixel_vertices;
    std::map<Raster, unsigned> raster_counts;
    for (const auto& recipe : kRetainedOpaquePhaseRecipes) {
        Check(exact.insert(Key(recipe.vertex, recipe.pixel, recipe.raster)).second, "Duplicate recipe");
        Check(recipe.guest_color_format == 3 && recipe.guest_depth_format == 1, "Guest formats changed");
        Check(NativeRetainedOpaquePhaseCatalogRecipe(recipe.vertex, recipe.pixel, recipe.raster) == &recipe,
              "Exact recipe did not retain its identity");
        Check(NativeRetainedOpaquePhaseRecipe(recipe.vertex, recipe.pixel, recipe.raster) ==
              (recipe.pixel == "333ce299839d3d857f8829e4249516ce823ea95e86fcf82473b81c068bd3c79f" ? nullptr : &recipe),
              "Exact numerical exclusion changed");
        pixels.insert(recipe.pixel); vertices.insert(recipe.vertex);
        pairs.emplace(recipe.vertex, recipe.pixel); pixel_vertices[recipe.pixel].insert(recipe.vertex);
        ++raster_counts[recipe.raster];
    }
    // Independently generated from actual C++ DecodeDrawState for399 captured draws, using the
    // sorted full VS|PS|six decimal raster words|guest formats serialization.
    // This pins content independently of the lookup implementation.
    uint64_t digest = 14695981039346656037ull;
    for (const auto& row : exact) for (unsigned char byte : row) {
        digest ^= byte; digest *= 1099511628211ull;
    }
    Check(digest == 0x7069f2ad89817eb6ull, "Exact captured recipe inventory changed");
    Check(pixels.size() == 53 && vertices.size() == 20 && pairs.size() == 55, "Inventory cardinality changed");
    Check(raster_counts.size() == 3 && raster_counts[{3,2,1,4,0,0}] == 46 &&
          raster_counts[{3,1,1,4,0,0}] == 13 && raster_counts[{1,2,1,4,0,0}] == 6,
          "Observed raster classes changed");
    for (size_t i = 0; i < kRetainedOpaquePhasePixels.size(); ++i) {
        const auto source = kRetainedOpaquePhasePixels[i];
        Check(pixels.contains(source) && NativeRetainedOpaquePhasePixelIndex(source) == i,
              "Pixel list disagrees with recipes or unique index");
        Check(NativeRetainedOpaquePhasePixelSupported(source), "Pixel support missing");
        Check(NativeRetainedOpaquePhasePixelEligible(source) ==
              (source != "333ce299839d3d857f8829e4249516ce823ea95e86fcf82473b81c068bd3c79f"), "Pixel eligibility changed");
        Check(std::count(kRetainedOpaquePhasePixels.begin(), kRetainedOpaquePhasePixels.end(), source) == 1,
              "Duplicate pixel module identity");
    }
    for (auto source : kRetainedOpaquePhaseVertices)
        Check(vertices.contains(source) && NativeRetainedOpaquePhaseVertexSupported(source), "Vertex list differs");
    for (const auto& [pixel, allowed_vertices] : pixel_vertices) {
        const bool shared = pixel.starts_with("fff7edec") || pixel.starts_with("300a99f2");
        Check(allowed_vertices.size() == (shared ? 2u : 1u), "Multi-vertex pixel was lost or broadened");
    }
    size_t accepted = 0, catalogued = 0;
    for (auto vertex : vertices) for (auto pixel : pixels) for (const auto& [raster, count] : raster_counts) {
        const bool present = exact.contains(Key(vertex, pixel, raster));
        Check(bool(NativeRetainedOpaquePhaseCatalogRecipe(vertex, pixel, raster)) == present,
              "Raw catalogue Cartesian broadening");
        catalogued += present;
        const bool expected = present && pixel != "333ce299839d3d857f8829e4249516ce823ea95e86fcf82473b81c068bd3c79f";
        const bool actual = NativeRetainedOpaquePhaseRecipe(vertex, pixel, raster) != nullptr;
        Check(actual == expected, "Cartesian source/raster broadening");
        accepted += actual;
    }
    Check(catalogued == 65 && accepted == 64, "Catalogue or eligible enumeration count changed");
    for (const auto& recipe : kRetainedOpaquePhaseRecipes) {
        for (size_t word = 0; word < 6; ++word) for (uint32_t bit = 0; bit < 32; ++bit) {
            auto raster = recipe.raster; raster[word] ^= uint32_t(1) << bit;
            Check(bool(NativeRetainedOpaquePhaseRecipe(recipe.vertex, recipe.pixel, raster)) ==
                  (recipe.pixel != "333ce299839d3d857f8829e4249516ce823ea95e86fcf82473b81c068bd3c79f" && exact.contains(Key(recipe.vertex, recipe.pixel, raster))),
                  "Raster bit mutation broadened admission");
        }
        for (size_t i = 0; i < 64; ++i) {
            std::string vertex(recipe.vertex), pixel(recipe.pixel);
            vertex[i] = vertex[i] == '0' ? '1' : '0'; pixel[i] = pixel[i] == '0' ? '1' : '0';
            Check(!NativeRetainedOpaquePhaseRecipe(vertex, recipe.pixel, recipe.raster), "Foreign VS accepted");
            Check(!NativeRetainedOpaquePhaseRecipe(recipe.vertex, pixel, recipe.raster), "Foreign PS accepted");
            Check(!NativeRetainedOpaquePhasePixelSupported(pixel), "Foreign pixel source supported");
            Check(!NativeRetainedOpaquePhasePixelEligible(pixel), "Foreign pixel source eligible");
            Check(!NativeRetainedOpaquePhaseVertexSupported(vertex), "Foreign vertex source supported");
        }
    }
    for (std::string_view invalid : {std::string_view{}, std::string_view("0"), std::string_view("invalid")}) {
        Check(!NativeRetainedOpaquePhasePixelSupported(invalid), "Malformed pixel accepted");
        Check(!NativeRetainedOpaquePhasePixelEligible(invalid), "Malformed pixel eligible");
        Check(!NativeRetainedOpaquePhaseVertexSupported(invalid), "Malformed vertex accepted");
        Check(!NativeRetainedOpaquePhaseRecipe(invalid, invalid, {}), "Malformed recipe accepted");
    }
    NativeRetainedOpaquePhaseCatalog catalog;
    catalog.programs.push_back(std::make_shared<const NativeRetainedOpaquePhaseProgram>(
        NativeRetainedOpaquePhaseProgram{std::string(*pixels.begin()), "baseline", "variant", "digest", 1794, {1,2,3}}));
    Check(catalog.programs[0]->source_sha256 == *pixels.begin() && catalog.programs[0]->spirv.size() == 3,
          "Immutable program publication failed");
}

using Headroom = NativeRetainedOpaquePhaseHeadroom;
void AdmissionAndPartialPreparation() {
    Headroom credit;
    Check(!credit.AdmitModule(53, 4) && !credit.AdmitModule(SIZE_MAX, 4), "Invalid pixel index admitted");
    Check(!credit.AdmitModule(0, 0) && !credit.AdmitModule(0, UINT64_MAX), "Invalid byte count admitted");
    Check(!credit.AdmitPipeline(0) && !credit.AdmitPipeline(65) && !credit.AdmitPipeline(SIZE_MAX),
          "Pipeline admitted without its module or outside inventory");
    const auto& first = kRetainedOpaquePhaseRecipes[0];
    const auto first_pixel = *NativeRetainedOpaquePhasePixelIndex(first.pixel);
    Check(credit.AdmitModule(first_pixel, 128), "Initial module not admitted");
    Check(!credit.AdmitModule(first_pixel, 128) && !credit.AdmitModule(first_pixel, 256), "Duplicate module credited");
    Check(credit.AdmitPipeline(0) && !credit.AdmitPipeline(0), "Duplicate pipeline credited");
    Check(credit.modules() == 1 && credit.bytes() == 128 && credit.pipelines() == 1, "Partial preparation miscounted");
    const auto before = credit.AddToSoftTargets(true, {100,1000,100}, {200,2000,200});
    // A failed creation never calls Admit*. Probing other permissible recipes
    // cannot reserve or charge their modules/PSOs before successful creation.
    for (size_t i = 0; i < 53; ++i) (void)credit.CanAdmitModule(i, 64);
    for (size_t i = 0; i < 65; ++i) (void)credit.CanAdmitPipeline(i);
    Check(credit.AddToSoftTargets(true, {100,1000,100}, {200,2000,200}) == before, "Preflight probes added credit");
    Check(credit.RetireModule(first_pixel) && !credit.RetireModule(first_pixel), "Module retirement not unique");
    Check(credit.modules() == 0 && credit.bytes() == 0 && credit.pipelines() == 1, "PSO lifetime coupled to module retirement");
    Check(!credit.CanAdmitPipeline(1), "Absent module admitted another PSO");
    Check(credit.AdmitModule(first_pixel, 64), "Recreated module not counted once");
    Check(!credit.AdmitPipeline(0), "Resident PSO double-counted after module replacement");
    Check(credit.RetirePipeline(0) && !credit.RetirePipeline(0) && credit.AdmitPipeline(0), "PSO retirement/recreation failed");
    Check(!credit.RetireModule(SIZE_MAX) && !credit.RetirePipeline(SIZE_MAX), "Invalid retirement accepted");

    // Both multi-VS pixels own a single module, but each exact recipe gets its
    // own unique pipeline credit. No vertex is stored in the module identity.
    for (std::string_view prefix : {"fff7edec", "300a99f2"}) {
        Headroom shared;
        size_t pixel_index = 0, recipes = 0;
        for (; pixel_index < 53; ++pixel_index) if (kRetainedOpaquePhasePixels[pixel_index].starts_with(prefix)) break;
        Check(pixel_index < 53 && shared.AdmitModule(pixel_index, 1024), "Shared pixel absent");
        std::set<std::string_view> vertices;
        for (size_t i = 0; i < 65; ++i) if (kRetainedOpaquePhaseRecipes[i].pixel == kRetainedOpaquePhasePixels[pixel_index]) {
            Check(shared.AdmitPipeline(i), "Exact shared-pixel recipe refused");
            vertices.insert(kRetainedOpaquePhaseRecipes[i].vertex); ++recipes;
        }
        Check(vertices.size() == 2 && shared.modules() == 1 && shared.bytes() == 1024 && shared.pipelines() == recipes,
              "Shared pixel residency multiplied by vertex count");
    }
    Headroom zero(0), limited(128), enlarged(UINT64_MAX);
    Check(!zero.AdmitModule(0, 1), "Zero pressure budget admitted");
    Check(limited.AdmitModule(0, 127) && limited.AdmitModule(1, 1), "Exact pressure boundary refused");
    Check(!limited.AdmitModule(2, 1) && !limited.AdmitModule(2, UINT64_MAX), "Byte cap overflow accepted");
    Check(limited.bytes() == 128 && limited.modules() == 2, "Refused bytes affected totals");
    Check(enlarged.byte_limit() == kRetainedOpaquePhaseMaxModuleBytes, "Caller enlarged closed phase cap");
    Check(enlarged.AdmitModule(0, kRetainedOpaquePhaseMaxModuleBytes) && !enlarged.AdmitModule(1, 1),
          "Measured aggregate cap not enforced");
}

void AdditiveTrimAndHardBounds() {
    constexpr uint64_t mib = 1024 * 1024;
    const Headroom::Targets hard{1280, 80*mib, 2048};
    // Existing composition targets supplied by Scene: its 75% base plus
    // already admitted FSI credits. The phase helper must not replace them.
    const Headroom::Targets existing{960+208, 60*mib+16701124, 1536+254};
    Headroom credit;
    struct Entry { uint64_t last_used_epoch, bytes; };
    std::map<size_t, Entry> modules, pipelines;
    PersistentCacheStatistics module_stats, pipeline_stats;
    constexpr uint64_t original_bytes = 67142112;
    for (size_t i = 0; i < 955; ++i) {
        const auto bytes = original_bytes/955 + (i < original_bytes%955);
        modules.emplace(i, Entry{1,bytes}); module_stats.bytes += bytes;
    }
    for (size_t i = 0; i < 1110; ++i) pipelines.emplace(i, Entry{1,0});
    // Synthetic per-module sizes partition the measured total exactly. They
    // exercise accounting; actual SPIR-V byte/owner proof is a separate test.
    for (size_t i = 0; i < 53; ++i) {
        const auto bytes = kRetainedOpaquePhaseMaxModuleBytes/53 + (i < kRetainedOpaquePhaseMaxModuleBytes%53);
        Check(credit.AdmitModule(i, bytes), "Full module set failed bounded admission");
        modules.emplace(955+i, Entry{2,bytes}); module_stats.bytes += bytes;
    }
    for (size_t i = 0; i < 65; ++i) {
        Check(credit.AdmitPipeline(i), "Full recipe set failed admission"); pipelines.emplace(1110+i, Entry{2,0});
    }
    Check(credit.modules() == 53 && credit.bytes() == kRetainedOpaquePhaseMaxModuleBytes && credit.pipelines() == 65,
          "Full admissions not exact");
    Check(!credit.AdmitModule(0, 1) && !credit.AdmitPipeline(0), "Full set duplicate overflow");
    const auto soft = credit.AddToSoftTargets(true, existing, hard);
    Check(soft == Headroom::Targets{1221, existing.bytes+3432836, 1855}, "Existing composition credits replaced");
    Check(credit.AddToSoftTargets(false, existing, hard) == existing, "Disabled phase changed existing credits");
    TrimPersistentCache(modules, soft.modules, soft.bytes, module_stats,
        [](const auto& e) { return e.bytes; }, [](const auto&) { return true; });
    TrimPersistentCache(pipelines, soft.pipelines, UINT64_MAX, pipeline_stats,
        [](const auto& e) { return e.bytes; }, [](const auto&) { return true; });
    Check(modules.size() == 1008 && pipelines.size() == 1175 && !module_stats.evictions && !pipeline_stats.evictions,
          "Additive frame trim evicted existing working set");
    for (size_t i = 0; i < 955; ++i) Check(modules.contains(i), "Original module lost to optional admission");
    for (size_t i = 0; i < 1110; ++i) Check(pipelines.contains(i), "Original pipeline lost to optional admission");
    for (size_t i = 1110-254; i < 1110; ++i) Check(pipelines.contains(i), "One of original254 FSI states lost");
    // Pressure can still remove optional entries. Explicit resource-retirement
    // accounting releases just those credits; it neither pins nor recompiles.
    for (size_t i = 0; i < 65; ++i) { pipelines.erase(1110+i); Check(credit.RetirePipeline(i), "Pipeline eviction credit lost"); }
    for (size_t i = 0; i < 53; ++i) { modules.erase(955+i); Check(credit.RetireModule(i), "Module eviction credit lost"); }
    Check(credit.AddToSoftTargets(true, existing, hard) == existing, "Retired entries retained phantom credits");

    Headroom full;
    Check(full.AdmitModule(0, 10), "Boundary test preparation failed");
    for (uint64_t ceiling : std::array<uint64_t,9>{0,1,2,3,4,1280,UINT64_MAX-10,UINT64_MAX-1,UINT64_MAX})
        for (uint64_t base : std::array<uint64_t,7>{0,1,4,100,UINT64_MAX-10,UINT64_MAX-1,UINT64_MAX}) {
            const auto capped = std::min(base, ceiling);
            const auto on = full.AddToSoftTargets(true, {base,base,base}, {ceiling,ceiling,ceiling});
            const auto off = full.AddToSoftTargets(false, {base,base,base}, {ceiling,ceiling,ceiling});
            Check(on.modules == capped + std::min(uint64_t(1),ceiling-capped) &&
                  on.bytes == capped + std::min(uint64_t(10),ceiling-capped) && on.pipelines == capped,
                  "Soft-target addition overflow or bound mismatch");
            Check(off == Headroom::Targets{capped,capped,capped}, "Disabled targets violate unchanged hard limit");
        }
}
} // namespace
int main() try {
    ClosedInventory(); AdmissionAndPartialPreparation(); AdditiveTrimAndHardBounds();
    std::cout << "PASS retained opaque phase policy: " << checks
              << " checks; exact53 pixels/65 recipes; original254 FSI states preserved in additive trim model\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
