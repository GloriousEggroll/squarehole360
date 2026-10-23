#include "renderer/native/draw_timestamp_ledger.h"
#include <cmath>
#include <iostream>
#include <memory>
using namespace gow3::renderer;
using Ledger = NativeDrawTimestampLedger;
size_t checks = 0;
void Check(bool value, const char *message) {
  ++checks;
  if (!value)
    throw std::runtime_error(message);
}
template <class F> void Reject(F &&f) {
  bool threw = false;
  try {
    f();
  } catch (const std::exception &) {
    threw = true;
  }
  Check(threw, "Invalid diagnostic ownership accepted");
}
NativeGpuWorkTag Tag(uint32_t frame = 16, uint32_t pair = 1) {
  NativeGpuWorkTag t;
  t.kind = NativeGpuWorkKind::Draw;
  t.frame = frame;
  t.pair = pair;
  t.depth_control = 0x700762;
  t.queried = true;
  t.width = 640;
  t.height = 360;
  return t;
}
void Ready(Ledger &ledger, Ledger::Token token, NativeGpuWorkTag tag = Tag()) {
  ledger.Begin(token, tag.frame);
  ledger.End(token, tag);
  ledger.Accept(token);
}
int main() try {
  Check(!Ledger::SamplesFrame(UINT32_MAX) && !Ledger::SamplesFrame(15) &&
            !Ledger::SamplesFrame(17) && Ledger::SamplesFrame(0) &&
            Ledger::SamplesFrame(16) && Ledger::SamplesFrame(32),
        "Wrong API-frame sampling policy");
  Reject([] { auto bad = std::make_unique<Ledger>(65); });
  for (bool every_frame : {false,true}) {
    auto ledger=std::make_unique<Ledger>(64,every_frame);
    Check(ledger->sample_period()==(every_frame?1u:Ledger::kPeriod),"Wrong diagnostic period");
    Check(!ledger->ShouldSampleFrame(UINT32_MAX),"Invalid capture frame sampled");
    for(uint32_t frame=0;frame<50;++frame) {
      const bool sampled=every_frame||frame%Ledger::kPeriod==0;
      Check(ledger->ShouldSampleFrame(frame)==sampled,"Explicit replay sampling changed frame identity");
      if(sampled){ledger->Begin({0,uint64_t(frame)+1},frame);ledger->Cancel({0,uint64_t(frame)+1});}
      else Reject([&]{ledger->Begin({0,uint64_t(frame)+1},frame);});
    }
  }
  const auto interval = DecodeNativeGpuTimestamps(250, 5, 8, 1000);
  Check(interval && interval->elapsed_ticks == 11 &&
            interval->begin_tick == 250 && interval->end_tick == 5 &&
            std::abs(interval->milliseconds - .011) < 1e-12,
        "Valid counter wrapping differs");
  Check(DecodeNativeGpuTimestamps(0x1fa, 0x205, 8, 1000)->elapsed_ticks == 11,
        "High counter bits not masked");
  Check(DecodeNativeGpuTimestamps(UINT64_MAX - 2, 4, 64, 1)->elapsed_ticks == 7,
        "64-bit wrap differs");
  for (auto bits : {0u, 65u})
    Check(!DecodeNativeGpuTimestamps(0, 1, bits, 1),
          "Invalid counter width accepted");
  for (double period : {0., -1., double(INFINITY), double(NAN)})
    Check(!DecodeNativeGpuTimestamps(0, 1, 64, period),
          "Invalid period accepted");
  for (uint32_t capacity : {64u, 128u}) {
    auto owner = std::make_unique<Ledger>(capacity);
    auto &l = *owner;
    std::string vertex(64, 'a'), pixel(64, 'b');
    Check(l.InternPair(vertex, pixel) == 1 && l.InternPair(vertex, pixel) == 1,
          "Pair identity not reused");
    vertex.clear();
    pixel.clear();
    Check(l.pairs()[0].vertex == std::string(64, 'a'),
          "Pair borrows caller lifetime");
    Check(l.InternPair(std::string(64, 'a'), std::nullopt) == 2,
          "Builtin identity conflated");
    Reject([&] { l.Begin({0, 1}, 15); });
    Reject([&] { l.Begin({capacity, 1}, 16); });
    l.Begin({0, 1}, 16);
    Reject([&] { l.Accept({0, 1}); });
    Reject([&] { l.End({0, 1}, Tag(32)); });
    l.Cancel({0, 1});
    l.Begin({0, 2}, 16);
    l.End({0, 2}, Tag());
    l.Cancel({0, 2});
    for (uint32_t i = 0; i < capacity; ++i)
      Ready(l, {i, 3});
    Reject([&] { l.Begin({0, 4}, 16); });
    Reject([&] { l.Retire({0, 2}, interval, 1, 10); });
    Reject([&] { l.Cancel({0, 3}); });
    Reject([&] { l.Pending({0, 2}); });
    // First exact fenced half retires, while a newer half still owns its
    // physical indices. Reuse only the earlier half with a new generation.
    for (uint32_t i = 0; i < capacity / 2; ++i)
      l.Retire({i, 3}, interval, 1, 100);
    for (uint32_t i = capacity / 2; i < capacity; ++i)
      Check(l.Pending({i, 3}), "Newer token retired by earlier fence");
    for (uint32_t i = 0; i < capacity / 2; ++i)
      Ready(l, {i, 4});
    for (uint32_t i = capacity / 2; i < capacity; ++i)
      l.Retire({i, 3}, {}, 2, 200);
    Check(l.totals().available == capacity / 2 &&
              l.totals().unavailable == capacity / 2,
          "Unavailable data treated as zero timing");
    for (uint32_t i = 0; i < capacity / 2; ++i)
      l.Retire({i, 4}, interval, 3, 300);
    Check(!l.pending() && l.totals().completed == capacity * 3 / 2,
          "Ring retirement leaked a token");
    Reject([&] { l.Retire({0, 4}, interval, 3, 301); });
    // Optional-failure policy never retires or resets accepted ownership.
    Ready(l, {0, 5});
    NativeDrawTimestampHealth health;
    try {
      l.Pending({0, 4});
    } catch (...) {
      health.Disable("stale fence");
    }
    Check(health.disabled && health.failures == 1 && l.Pending({0, 5}),
          "Diagnostic failure changed accepted ownership");
    health.Disable("later failure");
    Check(std::string_view(health.reason) == "stale fence",
          "First failure provenance lost");
    // Actual queue fencing is independent; the healthy oracle may complete
    // this token even though a disabled GPU adapter would retain its pool.
    l.Retire({0, 5}, interval, 4, 400);
  }
  auto owner = std::make_unique<Ledger>(64);
  auto &l = *owner;
  for (uint32_t i = 0; i < Ledger::kPairs + 2; ++i) {
    const auto id = l.InternPair(std::to_string(i), std::nullopt);
    Check(id == (i < Ledger::kPairs ? i + 1 : 0), "Pair bound exceeded");
  }
  Check(l.pairs().size() == Ledger::kPairs && l.totals().pair_overflows == 2,
        "Pair overflow not explicit");
  for (uint32_t i = 0; i < Ledger::kAggregates + 10; ++i) {
    auto tag = Tag(16, i + 1);
    tag.event = 9000 + i;
    Ready(l, {0, uint64_t(i + 1)}, tag);
    l.Retire({0, uint64_t(i + 1)}, interval, 1, 100);
  }
  Check(l.aggregates().size() == Ledger::kAggregates &&
            l.totals().aggregate_overflows == 10,
        "Aggregate bound exceeded");
  Check(l.frame(0).groups == Ledger::kFrameGroups &&
            l.frame(0).group_overflows ==
                Ledger::kAggregates + 10 - Ledger::kFrameGroups,
        "Per-frame group overflow not counted");
  Check(l.recent_count() == Ledger::kRecent &&
            l.slowest().size() == Ledger::kSlowest,
        "Sample retention unbounded");
  const auto last = l.recent(l.recent_count() - 1);
  Check(last.tag.event == 9000 + Ledger::kAggregates + 9,
        "Recent sample FIFO differs");
  for (uint32_t i = 1; i <= Ledger::kFrames + 4; ++i) {
    auto tag = Tag(i * 16, 1);
    tag.width = 1 + i;
    Ready(l, {0, 10000 + i}, tag);
    l.Retire({0, 10000 + i}, interval, 2, 200);
  }
  Check(l.frame_count() == Ledger::kFrames && l.totals().frame_evictions == 4 &&
            l.frame(0).frame == 5 * 16 &&
            l.frame(l.frame_count() - 1).frame == (Ledger::kFrames + 4) * 16,
        "Frame cohorts are unbounded or reordered");
  Check(l.totals().recent_evictions > 0 && !l.pending(),
        "Final bounded retirement failed");
  std::cout << "{\"passed\":true,\"checks\":" << checks
            << ",\"ledger_bytes\":" << sizeof(Ledger) << "}\n";
} catch (const std::exception &e) {
  std::cerr << e.what() << '\n';
  return 1;
}
