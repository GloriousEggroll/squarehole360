#include "renderer/native/pipeline_prewarm.h"
#include "renderer/native/content_hash.h"
#include "renderer/native/pipeline_startup_task.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace gow3::renderer;
using namespace plume;
void Require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
int main() try {
    // Pending tasks may outlive the loader in a paused CP queue. Cancellation
    // releases every captured owner and prevents later progress/device access.
    {
        auto owner = std::make_shared<int>(1);
        std::weak_ptr<int> weak = owner;
        unsigned calls = 0;
        auto task = std::make_shared<NativePipelineStartupTask>([owner, &calls] { ++calls; });
        owner.reset();
        auto done = task->Future();
        Require(!weak.expired(), "Pending startup owner lost");
        Require(task->CancelPending(), "Pending startup task could not cancel");
        done.get();
        Require(weak.expired(), "Cancelled startup task retained device/progress ownership");
        task->Run();
        Require(calls == 0 && !task->CancelPending(),
                "Cancelled startup task ran after loader exit");
    }
    {
        std::promise<void> entered, release;
        auto released = release.get_future().share();
        auto owner = std::make_shared<int>(1);
        std::weak_ptr<int> weak = owner;
        auto task = std::make_shared<NativePipelineStartupTask>([owner, &entered, released] {
            entered.set_value();
            released.wait();
        });
        owner.reset();
        auto done = task->Future();
        auto started = entered.get_future();
        std::thread worker([task] { task->Run(); });
        started.get();
        const bool cancelled = task->CancelPending();
        const bool premature =
            done.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
        const bool retained = !weak.expired();
        release.set_value();
        worker.join();
        done.get();
        Require(!cancelled && !premature && retained && weak.expired(),
                "Running task cancellation released a live driver/progress owner");
        task->Run(); // Repeated callback cannot publish completion twice.
    }
    {
        NativePipelineStartupTask task([] { throw std::runtime_error("fixture startup failure"); });
        auto done = task.Future();
        task.Run();
        bool rejected = false;
        try {
            done.get();
        } catch (const std::runtime_error &) {
            rejected = true;
        }
        Require(rejected && !task.CancelPending(),
                "Startup task failure did not fulfill its waiter");
    }
    const auto directory =
        std::filesystem::temp_directory_path() /
        ("gow3-pipeline-fixture-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    } cleanup{directory};
    auto vs = std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{1, 2, 3, 4});
    auto ps = std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{5, 6, 7, 8});
    NativeFsiPipelineRecipe recipe{ContentHash(*vs),
                                   ContentHash(*ps),
                                   {uint32_t(RenderCullMode::BACK),
                                    uint32_t(RenderFrontFace::CLOCKWISE), 1,
                                    uint32_t(RenderPrimitiveTopology::TRIANGLE_LIST), 2, 0},
                                   int32_t(RenderFormat::R8G8B8A8_UNORM),
                                   -1};
    Require(ValidNativeFsiPipelineRecipe(recipe), "Valid FSI recipe rejected");
    unsigned checks = 0;
    for (unsigned cull = 1; cull <= 3; ++cull)
        for (unsigned face = 1; face <= 2; ++face)
            for (unsigned clip = 0; clip < 2; ++clip)
                for (unsigned rect = 0; rect < 2; ++rect)
                    for (unsigned alpha = 0; alpha < 8; ++alpha) {
                        auto r = recipe;
                        r.raster = {cull, face, clip, 4, alpha == 1 ? 0 : 2 | (alpha << 8), rect};
                        Require(ValidNativeFsiPipelineRecipe(r),
                                "Observed raster combination rejected");
                        NativeFsiPipelineDescriptor d(r.raster, r.color_format, r.depth_format,
                                                      nullptr, nullptr, nullptr);
                        const auto &p = d.descriptor;
                        Require(uint32_t(p.cullMode) == cull && uint32_t(p.frontFace) == face &&
                                    p.depthClipEnabled == bool(clip) &&
                                    uint32_t(p.primitiveTopology) == 4 && !p.depthEnabled &&
                                    !p.depthWriteEnabled && !p.stencilEnabled &&
                                    !p.dynamicDepthBiasEnabled &&
                                    p.specConstants == &d.specialization &&
                                    p.specConstants[0].value == r.raster[4] &&
                                    p.renderTargetCount == 1 &&
                                    p.renderTargetFormat[0] == RenderFormat::R8G8B8A8_UNORM &&
                                    p.inputSlotsCount == rect && p.inputElementsCount == rect,
                                "Shared FSI descriptor differs from its exact active recipe");
                        if (rect)
                            Require(p.inputSlots == &d.slot && p.inputElements == &d.element,
                                    "Rectangle pointers escaped owned descriptor");
                        for (const auto &blend : p.renderTargetBlend)
                            Require(!blend.blendEnabled && !blend.renderTargetWriteMask,
                                    "FSI host blend enabled");
                        ++checks;
                    }
    for (unsigned word = 0; word < 6; ++word) {
        auto invalid = recipe;
        invalid.raster[word] = UINT32_MAX;
        Require(!ValidNativeFsiPipelineRecipe(invalid), "Unbounded raster value accepted");
    }
    {
        auto invalid = recipe;
        invalid.raster[4] = 0x102;
        Require(!ValidNativeFsiPipelineRecipe(invalid),
                "Unsupported alpha specialization accepted");
    }
    {
        auto invalid = recipe;
        invalid.vertex = "../shader";
        Require(!ValidNativeFsiPipelineRecipe(invalid), "Untrusted shader path accepted");
    }
    {
        auto invalid = recipe;
        invalid.depth_format = 0;
        Require(!ValidNativeFsiPipelineRecipe(invalid), "Unexpected FSI depth attachment accepted");
    }
    auto observe = [&](NativePipelineRecipeCache &cache, const ObservedNativePipeline &job) {
        const auto previous = cache.statistics().admitted;
        for (unsigned i = 0; i < 100 && cache.statistics().admitted == previous; ++i) {
            cache.Observe(job);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        cache.Drain();
        Require(cache.statistics().admitted == previous + 1, "Cache admission did not settle");
    };
    {
        NativePipelineRecipeCache cache(directory, std::string(64, 'a'));
        Require(cache.Load().empty(), "Absent observed cache was not a miss");
        observe(cache, {recipe, vs, ps});
        auto loaded = cache.Load();
        Require(loaded.size() == 1 && loaded[0].recipe == recipe && *loaded[0].vertex == *vs &&
                    *loaded[0].pixel == *ps,
                "Observed recipe/source round trip changed bytes");
        cache.Observe({recipe, vs, ps});
        cache.Drain();
        Require(cache.statistics().admitted == 1, "Duplicate recipe retained another owner");
        auto depth = recipe;
        depth.pixel.reset();
        depth.raster[5] = 1;
        observe(cache, {depth, vs, nullptr});
        loaded = cache.Load();
        Require(loaded.size() == 2 && loaded[0].vertex == loaded[1].vertex && !loaded[1].pixel,
                "Sources were not deduplicated or builtin depth PS lost");
        // An unsupported or incorrect identity never becomes a valid startup job.
        auto bad = recipe;
        bad.vertex = std::string(64, 'f');
        observe(cache, {bad, vs, ps});
        Require(cache.statistics().failures == 1 && cache.Load().size() == 2,
                "Incorrect source bytes poisoned persisted recipes");
        cache.StopAccepting();
        cache.Observe({bad, vs, ps});
        Require(cache.statistics().pending == 0, "Stopped cache accepted work");
    }
    {
        NativePipelineRecipeCache wrong(directory, std::string(64, 'b'));
        Require(wrong.Load().empty(), "Compiler namespace mismatch loaded recipes");
    }
    {
        const auto shared = directory / "concurrent";
        NativePipelineRecipeCache first(shared, std::string(64, 'a'));
        observe(first, {recipe, vs, ps});
        NativePipelineRecipeCache second(shared, std::string(64, 'a'));
        auto another = recipe;
        another.raster[5] = 1;
        observe(second, {another, vs, ps});
        Require(second.statistics().failures == 1 && second.Load().size() == 1,
                "Concurrent recorder bypassed the global source/recipe bound");
    }
    // Test hard bounds with smaller instances of the production policy.
    for (unsigned mode = 0; mode < 4; ++mode) {
        NativePipelineRecipeCache::Limits limits;
        if (mode == 0)
            limits.recipes = 1;
        if (mode == 1)
            limits.sources = 1;
        if (mode == 2)
            limits.source_bytes = 7;
        if (mode == 3)
            limits.pending_bytes = 7;
        NativePipelineRecipeCache cache(directory / ("bounds-" + std::to_string(mode)),
                                        std::string(64, 'a'), limits);
        if (mode == 3) {
            cache.Observe({recipe, vs, ps});
            cache.Drain();
            Require(cache.statistics().admitted == 0 && cache.statistics().pending_bytes == 0,
                    "Oversized pending source admitted");
        } else {
            observe(cache, {recipe, vs, ps});
            if (mode == 0) {
                auto another = recipe;
                another.raster[5] = 1;
                cache.Observe({another, vs, ps});
                cache.Drain();
                Require(cache.Load().size() == 1 && cache.statistics().admitted == 1,
                        "Recipe count bound exceeded");
            } else
                Require(cache.Load().empty() && cache.statistics().failures == 1,
                        "Source count/byte bound exceeded");
        }
        Require(cache.statistics().peak_pending <= limits.pending &&
                    cache.statistics().peak_pending_bytes <= limits.pending_bytes,
                "Writer outstanding ownership exceeded its bound");
    }
    {
        auto orphan_dir = directory / "orphan";
        std::filesystem::create_directories(orphan_dir);
        const std::vector<uint8_t> orphan{8, 8, 8, 8, 8, 8, 8, 8};
        {
            std::ofstream file(orphan_dir / (ContentHash(orphan) + ".bin"), std::ios::binary);
            file.write(reinterpret_cast<const char *>(orphan.data()), orphan.size());
        }
        NativePipelineRecipeCache::Limits limits;
        limits.source_bytes = 8;
        NativePipelineRecipeCache cache(orphan_dir, std::string(64, 'a'), limits);
        observe(cache, {recipe, vs, ps});
        Require(cache.Load().empty() && cache.statistics().failures == 1,
                "Orphan files bypassed disk source budget");
    }
    {
        const auto recovery = directory / "temporary-recovery";
        std::filesystem::create_directories(recovery);
        const auto source_tmp = recovery / (recipe.vertex + ".bin.tmp-123-1");
        const auto manifest_tmp = recovery / "recipes.json.tmp-123-2";
        const auto unrelated = recovery / "notes.tmp-123-3";
        const auto malformed = recovery / "recipes.json.tmp-123-invalid";
        for (const auto &path : {source_tmp, manifest_tmp, unrelated, malformed}) {
            std::ofstream file(path);
            file << "abandoned";
        }
        NativePipelineRecipeCache cache(recovery, std::string(64, 'a'));
        observe(cache, {recipe, vs, ps});
        Require(!std::filesystem::exists(source_tmp) && !std::filesystem::exists(manifest_tmp) &&
                    std::filesystem::exists(unrelated) && std::filesystem::exists(malformed) &&
                    cache.Load().size() == 1,
                "Recovery removed unrelated files or retained an owned abandoned temporary");
    }
    {
        const auto recovery = directory / "partial-commit-recovery";
        // Source renames succeed, but an occupied manifest destination fails.
        std::filesystem::create_directories(recovery / "recipes.json");
        NativePipelineRecipeCache::Limits limits;
        limits.source_bytes = 12;
        NativePipelineRecipeCache cache(recovery, std::string(64, 'a'), limits);
        observe(cache, {recipe, vs, ps});
        Require(cache.statistics().failures == 1 &&
                    std::filesystem::exists(recovery / (recipe.vertex + ".bin")) &&
                    std::filesystem::exists(recovery / (*recipe.pixel + ".bin")),
                "Fixture did not reach a partially committed source write");
        std::filesystem::remove(recovery / "recipes.json");
        // Model the untracked final rename from an allocation failure after a
        // source commit. The failed job must force a fresh disk census, rather
        // than trust its previous in-memory table at the next admission.
        const std::vector<uint8_t> orphan{9, 10, 11, 12};
        {
            std::ofstream file(recovery / (ContentHash(orphan) + ".bin"), std::ios::binary);
            file.write(reinterpret_cast<const char *>(orphan.data()), orphan.size());
        }
        const auto new_ps =
            std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{13, 14, 15, 16});
        auto next = recipe;
        next.pixel = ContentHash(*new_ps);
        observe(cache, {next, vs, new_ps});
        Require(cache.statistics().failures == 2 && cache.Load().empty() &&
                    !std::filesystem::exists(recovery / (*next.pixel + ".bin")),
                "Failed-job accounting reused stale source totals and exceeded the disk budget");
    }
    {
        const auto blocked = directory / "not-a-directory";
        {
            std::ofstream file(blocked);
            file << "blocked";
        }
        NativePipelineRecipeCache cache(blocked, std::string(64, 'a'));
        observe(cache, {recipe, vs, ps});
        Require(cache.Load().empty() && cache.statistics().failures == 1,
                "Writer error escaped graceful cache miss");
    }
    {
        const auto path = directory / "recipes.json";
        nlohmann::json saved;
        {
            std::ifstream file(path);
            file >> saved;
        }
        for (const auto &invalid :
             {nlohmann::json(4294967300ull), nlohmann::json(4.0), nlohmann::json(-4294967292ll)}) {
            auto manifest = saved;
            manifest["recipes"][0]["raster"][3] = invalid;
            {
                std::ofstream file(path);
                file << manifest;
            }
            NativePipelineRecipeCache cache(directory, std::string(64, 'a'));
            Require(cache.Load().size() == 1, "Malformed number narrowed to a valid raster value");
        }
        {
            std::ofstream file(path);
            file << saved;
        }
    }
    {
        std::ofstream file(directory / (recipe.pixel.value() + ".bin"),
                           std::ios::binary | std::ios::trunc);
        file << "corrupt";
    }
    {
        NativePipelineRecipeCache cache(directory, std::string(64, 'a'));
        const auto loaded = cache.Load();
        Require(loaded.size() == 1 && !loaded[0].pixel,
                "Corrupt source prevented independent valid builtin recipe recovery");
    }
    {
        std::ofstream file(directory / "recipes.json", std::ios::trunc);
        file << "{";
    }
    {
        NativePipelineRecipeCache cache(directory, std::string(64, 'a'));
        Require(cache.Load().empty(), "Malformed recipe manifest was not a miss");
    }
    std::cout << "PASS observed pipeline cache, cancellation/admission, corruption and " << checks
              << " descriptor combinations\n";
    return 0;
} catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
}
