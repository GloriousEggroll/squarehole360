#include <catch2/catch_test_macros.hpp>
#include <rex/graphics/shader_storage_progress.h>

#include <atomic>
#include <thread>
#include <vector>

TEST_CASE("Shader preload progress stays consistent across workers and phases", "[shader-preload]") {
  using Progress = rex::graphics::ShaderStorageProgress;
  Progress progress;
  progress.Begin(Progress::Phase::kPipelines, 8000);
  std::atomic<bool> finished{false};
  std::atomic<bool> valid_snapshots{true};
  std::thread reader([&] {
    while (!finished.load()) {
      auto status = progress.Read();
      if (status.failed + status.skipped > status.completed || status.completed > status.total) {
        valid_snapshots = false;
      }
    }
  });
  std::vector<std::thread> workers;
  for (int worker = 0; worker < 8; ++worker) {
    workers.emplace_back([&] {
      for (int i = 0; i < 1000; ++i) {
        if (i % 10 == 1) progress.Advance(Progress::Outcome::kSkipped);
        else progress.Advance(i % 10 != 0);
      }
    });
  }
  for (auto& worker : workers) worker.join();
  finished = true;
  reader.join();
  auto status = progress.Read();
  REQUIRE(valid_snapshots.load());
  REQUIRE(status.completed == 8000);
  REQUIRE(status.failed == 800);
  REQUIRE(status.skipped == 800);
  progress.Finish();
  REQUIRE(progress.Read().phase == Progress::Phase::kComplete);
  // Empty cache and a subsequent load cannot retain the previous phase counts.
  progress.Begin(Progress::Phase::kShaders);
  status = progress.Read();
  REQUIRE(status.phase == Progress::Phase::kShaders);
  REQUIRE(status.completed == 0);
  REQUIRE(status.total == 0);
  REQUIRE(status.failed == 0);
  REQUIRE(status.skipped == 0);
}

TEST_CASE("Optional shader admission refusals remain distinct from preparation errors", "[shader-preload]") {
  using Progress = rex::graphics::ShaderStorageProgress;
  Progress progress;
  progress.Begin(Progress::Phase::kShaders, 4);
  progress.Advance();
  // A valid optional module outside its startup budget was not loaded, but
  // did not fail compilation. A real compiler exception remains a failure.
  progress.Advance(Progress::Outcome::kSkipped);
  progress.Advance(false);
  progress.Advance(Progress::Outcome::kPrepared);
  auto status = progress.Read();
  REQUIRE(status.completed == 4);
  REQUIRE(status.total == 4);
  REQUIRE(status.skipped == 1);
  REQUIRE(status.failed == 1);

  // A new PSO phase cannot carry a shader skip/error into its own counts.
  progress.Begin(Progress::Phase::kPipelines, 2);
  REQUIRE(progress.Read().skipped == 0);
  REQUIRE(progress.Read().failed == 0);
  progress.Advance(Progress::Outcome::kSkipped);
  progress.Advance(Progress::Outcome::kFailed);
  progress.Finish();
  status = progress.Read();
  REQUIRE(status.phase == Progress::Phase::kComplete);
  REQUIRE(status.completed == 2);
  REQUIRE(status.skipped == 1);
  REQUIRE(status.failed == 1);
}
