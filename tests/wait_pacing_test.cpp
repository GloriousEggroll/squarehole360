#include <catch2/catch_test_macros.hpp>
#include <rex/thread.h>
#include <chrono>
#include <ctime>

TEST_CASE("Short multi-object waits sleep instead of spinning for their final millisecond", "[wait-pacing]") {
  using namespace std::chrono_literals;
  auto first = rex::thread::Event::CreateAutoResetEvent(false);
  auto second = rex::thread::Event::CreateAutoResetEvent(false);
  rex::thread::WaitHandle* handles[] = {first.get(), second.get()};
  timespec before{}, after{};
  REQUIRE(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &before) == 0);
  auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < 100; ++i) {
    auto result = rex::thread::WaitAny(handles, 2, true, 1ms);
    REQUIRE(result.first == rex::thread::WaitResult::kTimeout);
  }
  double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  REQUIRE(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &after) == 0);
  double cpu = double(after.tv_sec - before.tv_sec) + double(after.tv_nsec - before.tv_nsec) / 1e9;
  INFO("Wall seconds: " << wall << "; thread CPU seconds: " << cpu);
  CHECK(wall >= 0.09);
  CHECK(cpu < wall * 0.5);

  second->Set();
  auto signaled = rex::thread::WaitAny(handles, 2, false, 0ms);
  REQUIRE(signaled.first == rex::thread::WaitResult::kSuccess);
  CHECK(signaled.second == 1);
  CHECK(rex::thread::WaitAny(handles, 2, false, 0ms).first == rex::thread::WaitResult::kTimeout);
  first->Set();
  CHECK(rex::thread::WaitAll(handles, 2, false, 0ms) == rex::thread::WaitResult::kTimeout);
  second->Set();
  CHECK(rex::thread::WaitAll(handles, 2, false, 0ms) == rex::thread::WaitResult::kSuccess);
}
