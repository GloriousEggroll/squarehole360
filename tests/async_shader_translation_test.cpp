#include <catch2/catch_test_macros.hpp>
#include <rex/graphics/async_shader_translation.h>

#include <latch>
#include <thread>
#include <vector>

using rex::graphics::AsyncShaderTranslation;
using State = AsyncShaderTranslation::State;

TEST_CASE("Pending translations publish all output before becoming usable", "[async-translation]") {
  for (int iteration = 0; iteration < 32; ++iteration) {
    AsyncShaderTranslation work;
    std::vector<int> binary_and_bindings;
    std::latch release_worker(1);
    REQUIRE(work.state() == State::kNotStarted);
    REQUIRE(work.TryQueue());
    std::jthread worker([&] {
      release_worker.wait();
      binary_and_bindings = {iteration, 17, 23, 31};
      work.Complete(true);
    });
    CHECK(work.state() == State::kPending);
    CHECK_FALSE(work.TryQueue());
    State result = State::kNotStarted;
    std::vector<int> observed;
    std::jthread synchronous_consumer([&] {
      result = work.Wait();
      // Read before joining the producer: publication must supply the fence.
      observed = binary_and_bindings;
    });
    release_worker.count_down();
    synchronous_consumer.join();
    CHECK(result == State::kReady);
    CHECK(observed == std::vector<int>{iteration, 17, 23, 31});
    CHECK_FALSE(work.TryQueue());
  }
}

TEST_CASE("Failed translations unblock consumers without being queued repeatedly", "[async-translation]") {
  AsyncShaderTranslation work;
  CHECK(work.Wait() == State::kNotStarted);
  REQUIRE(work.TryQueue());
  std::jthread worker([&] { work.Complete(false); });
  CHECK(work.Wait() == State::kFailed);
  CHECK_FALSE(work.TryQueue());
  CHECK(work.Wait() == State::kFailed);
}
