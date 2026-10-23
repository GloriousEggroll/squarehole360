#include <catch2/catch_test_macros.hpp>
#include <rex/graphics/frame_limiter.h>

TEST_CASE("Frame limiter imposes a ceiling without speeding up late frames", "[frame-limit]") {
  using Limiter = rex::graphics::FrameLimiter;
  using namespace std::chrono_literals;
  Limiter limiter;
  auto start = Limiter::Clock::time_point{};
  REQUIRE(limiter.Deadline(start, 60) == start);
  limiter.Presented(start);
  REQUIRE(limiter.Deadline(start + 1ms, 60) == start + 16666666ns);
  REQUIRE(limiter.Deadline(start + 1ms, 144) == start + 6944444ns);
  REQUIRE(limiter.Deadline(start + 1ms, 50) == start + 20ms);
  REQUIRE(limiter.Deadline(start + 1ms, 0) == start + 1ms);
  REQUIRE(limiter.Deadline(start + 40ms, 60) == start + 40ms);
  limiter.Presented(start + 40ms);
  REQUIRE(limiter.Deadline(start + 41ms, 60) == start + 56666666ns);
}
