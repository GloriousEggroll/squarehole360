#include <catch2/catch_test_macros.hpp>
#include <rex/graphics/pending_query_results.h>
#include <rex/graphics/native_query_accumulator.h>
#include <rex/graphics/native_draw_effects.h>
#include <vector>

TEST_CASE("Masked driver draws retain query and shader memory side effects", "[native-draw-effects]") {
  using rex::graphics::IsNativeDrawWithoutOutputs;
  rex::graphics::NativeDrawEffects helper;
  helper.edram_mode=4;helper.vertex_shader_present=true;
  CHECK(IsNativeDrawWithoutOutputs(helper));
  helper.edram_mode=5;CHECK(IsNativeDrawWithoutOutputs(helper));
  for(uint32_t mode:{0,1,2,3,6,7}) {
    auto changed=helper;changed.edram_mode=mode;
    CHECK_FALSE(IsNativeDrawWithoutOutputs(changed));
  }
  for(unsigned bit=0;bit<32;++bit) {
    auto changed=helper;changed.color_mask=uint32_t(1)<<bit;
    CHECK_FALSE(IsNativeDrawWithoutOutputs(changed));
    changed=helper;changed.depth_control=uint32_t(1)<<bit;
    CHECK_FALSE(IsNativeDrawWithoutOutputs(changed));
  }
  auto missing=helper;missing.vertex_shader_present=false;
  CHECK_FALSE(IsNativeDrawWithoutOutputs(missing));
  for(auto member:{&rex::graphics::NativeDrawEffects::vertex_memory_export,
                   &rex::graphics::NativeDrawEffects::pixel_memory_export,
                   &rex::graphics::NativeDrawEffects::host_query,
                   &rex::graphics::NativeDrawEffects::native_query,
                   &rex::graphics::NativeDrawEffects::resuming_query,
                   &rex::graphics::NativeDrawEffects::publishing_query}) {
    auto changed=helper;changed.*member=true;
    CHECK_FALSE(IsNativeDrawWithoutOutputs(changed));
  }
}

TEST_CASE("Occlusion results remain pending until their GPU submissions complete", "[query-retirement]") {
  rex::graphics::PendingQueryResults queue;
  std::vector<uint32_t> published;
  auto publish = [&](const auto& entry) { published.push_back(entry.host_index); };
  CHECK(queue.empty());
  queue.Push(4, 7, 0x1000);
  queue.Push(4, 8, 0x2000);
  queue.Push(6, 9, 0x3000);
  queue.Drain(3, publish);
  CHECK(published.empty());
  CHECK(queue.first_submission() == 4);
  CHECK(queue.last_submission() == 6);
  queue.Drain(4, publish);
  CHECK(published == std::vector<uint32_t>{7, 8});
  CHECK_FALSE(queue.empty());
  queue.Drain(5, publish);
  CHECK(published.size() == 2);
  queue.Drain(6, publish);
  CHECK(published == std::vector<uint32_t>{7, 8, 9});
  CHECK(queue.empty());
  queue.Drain(6, publish);
  CHECK(published.size() == 3);
}

TEST_CASE("Reused guest query addresses reject stale results without early retirement", "[query-retirement]") {
  rex::graphics::PendingQueryResults queue;
  std::vector<uint32_t> published;
  auto publish = [&](const auto& entry) { published.push_back(entry.host_index); };
  queue.Push(2, 0, 0x1000);
  queue.Push(3, 1, 0x2000);
  queue.InvalidateAddress(0x1000);
  queue.Push(4, 2, 0x1000);
  queue.Drain(1, publish);
  CHECK_FALSE(queue.empty());
  CHECK(queue.first_submission() == 2);
  queue.Drain(2, publish);
  CHECK(published.empty());
  queue.Drain(3, publish);
  CHECK(published == std::vector<uint32_t>{1});
  queue.Drain(4, publish);
  CHECK(published == std::vector<uint32_t>{1, 2});
  CHECK(queue.empty());
}

TEST_CASE("Guest report reuse within one pending submission publishes only the latest result", "[query-retirement]") {
  rex::graphics::PendingQueryResults queue;
  std::vector<uint32_t> published;
  auto publish = [&](const auto& entry) { published.push_back(entry.host_index); };
  queue.Push(4, 7, 0x1000);
  queue.Push(4, 8, 0x2000);
  queue.InvalidateAddress(0x1000);
  queue.Push(4, 9, 0x1000);
  queue.Drain(3, publish);
  CHECK(published.empty());
  CHECK(queue.first_submission() == 4);
  CHECK(queue.last_submission() == 4);
  queue.Drain(4, publish);
  CHECK(published == std::vector<uint32_t>{8, 9});
  CHECK(queue.empty());
}

TEST_CASE("Native completion cannot be overwritten by a late reference report", "[query-retirement]") {
  rex::graphics::PendingQueryResults queue;
  uint32_t native_report = 0;
  std::vector<uint32_t> retired;
  queue.Push(10, 4, 0x1000);
  queue.Push(10, 5, 0x2000);
  // The native zero-sample completion owns this use, including after the
  // reference fence retires. A later use of the address is independent.
  queue.InvalidateAddress(0x1000);
  queue.Push(12, 6, 0x1000);
  auto publish = [&](const auto& entry) {
    retired.push_back(entry.host_index);
    if(entry.guest_address == 0x1000)native_report=entry.host_index;
  };
  queue.Drain(10,publish);
  CHECK(native_report == 0);
  CHECK(retired == std::vector<uint32_t>{5});
  queue.Drain(12,publish);
  CHECK(native_report == 6);
  CHECK(retired == std::vector<uint32_t>{5,6});
}

TEST_CASE("Native query handback accumulates residual submissions before normalization", "[query-retirement]") {
  rex::graphics::NativeQueryAccumulator query;
  query.begin_address=0x1020;
  query.native_samples=17;
  CHECK_THROWS(query.Total(0));
  query.RetireSegment(1,false);
  query.RetireSegment(2,false);
  CHECK_FALSE(query.published);
  CHECK_THROWS(query.RetireSegment(3,true));
  query.final_recorded=true;
  query.RetireSegment(3,true);
  CHECK(query.reference_samples==6);
  // Six samples at 2x2 scale round to two, regardless of the three segments.
  CHECK(query.Total((query.reference_samples+2)/4)==19);
  CHECK_THROWS(query.RetireSegment(0,false));
}

TEST_CASE("Native query fallback preserves true zero and saturates counts", "[query-retirement]") {
  rex::graphics::NativeQueryAccumulator zero;
  zero.final_recorded=true;
  zero.RetireSegment(0,true);
  CHECK(zero.Total(0)==0);
  rex::graphics::NativeQueryAccumulator large;
  large.native_samples=UINT64_MAX-2;
  large.RetireSegment(UINT64_MAX,false);
  large.final_recorded=true;
  large.RetireSegment(1,true);
  CHECK(large.reference_samples==UINT64_MAX);
  CHECK(large.Total(large.reference_samples)==UINT64_MAX);
}

TEST_CASE("Residual query segments from an old report generation cannot publish", "[query-retirement]") {
  rex::graphics::PendingQueryResults queue;
  rex::graphics::NativeQueryAccumulator old_query,new_query;
  old_query.native_samples=91;old_query.final_recorded=true;
  new_query.native_samples=7;new_query.final_recorded=true;
  queue.Push(10,0,0x1000); // old residual intermediate
  queue.Push(11,1,0x1000); // old residual final
  queue.InvalidateAddress(0x1000);
  queue.Push(12,2,0x1000); // fresh generation
  uint64_t guest=0;
  queue.Drain(12,[&](const auto& entry) {
    CHECK(entry.host_index==2);
    new_query.RetireSegment(3,true);
    guest=new_query.Total(new_query.reference_samples);
  });
  CHECK_FALSE(old_query.published);
  CHECK(guest==10);
}
