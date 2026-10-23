#include <rex/graphics/native_completion.h>
#include <rex/graphics/native_query_tickets.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <map>
#include <stdexcept>
#include <vector>

using namespace rex::graphics;
namespace {
void Require(bool condition,const char* message) {
  if(!condition)throw std::runtime_error(message);
}
template<class F> void Reject(F&& action,const char* message) {
  bool rejected=false;
  try {action();}catch(const std::runtime_error&){rejected=true;}
  Require(rejected,message);
}
NativeQueryIdentity Query(uint64_t generation,uint32_t address=0x1000) {
  return {address,generation,7,true};
}

// This is a completion-provider fixture, not an occlusion-query implementation.
// Counts are scripted inputs from a hypothetical completed GPU batch. A serial
// oracle independently checks that report writes follow *all* accepted work,
// including unrelated draws that own no occlusion slot. Production ticket and
// completion-latch classes own the capabilities and exception behavior.
struct CompletionFixture {
  struct Result {NativeQueryIdentity identity;uint64_t samples,required_serial;};
  struct Write {NativeQueryIdentity identity;uint64_t samples,completed_serial;};
  NativeCompletionState completion;
  NativeQueryTickets tickets;
  uint64_t accepted_serial=0,completed_serial=0;
  uint64_t fence_calls=0,retired=0,fail_publication_generation=0;
  bool fail_fence=false,accepting=true;
  std::vector<Result> results;
  size_t next_result=0;
  std::vector<Write> writes;
  std::vector<NativeCompletionReason> drains;
  std::map<uint32_t,uint64_t> latest_generation,guest_reports;

  void Draw() {
    Require(accepting,"Draw admitted after stop");
    ++accepted_serial;completion.AcceptPending();
  }
  void Begin(const NativeQueryIdentity& identity) {
    Require(accepting,"Query admitted after stop");
    latest_generation[identity.end_address]=identity.generation;
    tickets.SupersedeAddress(identity.end_address);
  }
  void SealCurrent(const NativeQueryIdentity& identity,uint64_t samples) {
    Require(accepting,"Query end admitted after stop");
    Require(!tickets.full(),"Query end requires a pre-admission pressure drain");
    results.push_back({identity,samples,accepted_serial});
    tickets.Defer(identity);completion.AcceptPending();
  }
  void End(const NativeQueryIdentity& identity,uint64_t samples) {
    const auto scope=tickets.BeginEndCallback(identity);
    SealCurrent(identity,samples);
  }
  void Drain(NativeCompletionReason reason,bool force=false) {
    completion.Complete([&] {
      drains.push_back(reason);
      if(completed_serial!=accepted_serial) {
        ++fence_calls;
        if(fail_fence)throw std::runtime_error("Injected fence failure");
        completed_serial=accepted_serial;
      }
      for(;next_result<results.size();++next_result) {
        const auto& result=results[next_result];
        tickets.Publish(result.identity,[&] {
          Require(completed_serial>=result.required_serial,"Report published before earlier draw completion");
          Require(latest_generation.at(result.identity.end_address)==result.identity.generation,
                  "Old query overwrote a reused report");
          if(fail_publication_generation==result.identity.generation)
            throw std::runtime_error("Injected report publication failure");
          guest_reports[result.identity.end_address]=result.samples;
          writes.push_back({result.identity,result.samples,completed_serial});
        });
        ++retired;
      }
      Require(tickets.empty(),"Completion returned with an unconsumed ticket");
      results.clear();next_result=0;
    },force);
  }
};

void CheckOrderingAndIdle() {
  CompletionFixture fixture;
  fixture.Draw(); // Unrelated draw, outside the zero-draw query.
  const auto zero=Query(1);
  fixture.Begin(zero);fixture.End(zero,0);
  Require(fixture.writes.empty()&&fixture.tickets.size()==1&&fixture.completion.pending(),
          "Deferred zero query became visible before a completion boundary");
  // Ordinary state packets cannot force per-query completion. The next input
  // may never arrive; CP idle/PrepareForWait must drain the existing capability.
  for(uint32_t opcode:{0x10u,0x2du,0x55u,0x56u,0x27u,0x2bu})
    Require(!NativePacketNeedsCompletion(opcode,1),"State-only packet became a query fence");
  fixture.Drain(NativeCompletionReason::Wait);
  Require(fixture.writes.size()==1&&fixture.writes[0].identity==zero&&
          fixture.writes[0].samples==0&&fixture.writes[0].completed_serial==1&&fixture.fence_calls==1,
          "Idle did not publish actual zero after the unrelated draw fence");
  Require(!fixture.completion.pending()&&fixture.tickets.empty(),"Idle retained completed work");
  fixture.Drain(NativeCompletionReason::Wait);
  Require(fixture.writes.size()==1&&fixture.fence_calls==1,"Idle repeated completed work");

  CompletionFixture no_draw;
  no_draw.Begin(zero);no_draw.End(zero,0);
  Require(no_draw.completion.pending(),"Zero-draw query did not set the completion latch");
  no_draw.Drain(NativeCompletionReason::Wait);
  Require(no_draw.writes.size()==1&&no_draw.fence_calls==0&&!no_draw.completion.pending(),
          "A lone zero-draw query depended on future draws or presentation");
}

void CheckPublicationBoundaries() {
  constexpr std::array reasons={NativeCompletionReason::CommandPacket,NativeCompletionReason::ScratchWrite,
    NativeCompletionReason::Wait,NativeCompletionReason::PrimaryBufferEnd,NativeCompletionReason::Presentation,
    NativeCompletionReason::ReferenceDraw,NativeCompletionReason::ProviderRemoval,NativeCompletionReason::QueryPressure,NativeCompletionReason::Shutdown};
  for(const auto reason:reasons) {
    CompletionFixture fixture;
    const auto first=Query(1),second=Query(2,0x1040);
    fixture.Begin(first);fixture.Draw();fixture.End(first,17);
    fixture.Begin(second);fixture.Draw();fixture.Draw();fixture.End(second,23);
    Require(fixture.fence_calls==0&&fixture.writes.empty(),"Query end forced an early completion");
    fixture.Drain(reason);
    Require(fixture.fence_calls==1&&fixture.retired==2&&fixture.writes.size()==2&&
            fixture.writes[0].identity==first&&fixture.writes[0].samples==17&&
            fixture.writes[1].identity==second&&fixture.writes[1].samples==23&&
            fixture.completed_serial==3&&!fixture.completion.pending(),
            "Publication boundary lost query order, counts, or the common fence");
    // The caller may now expose its scratch write/read pointer or release its
    // provider; both reports must already be visible to that observer.
    Require(fixture.guest_reports.at(0x1000)==17&&fixture.guest_reports.at(0x1040)==23,
            "External effect preceded deferred report publication");
  }
}

void CheckReuseAndPressure() {
  CompletionFixture reuse;
  const auto old=Query(1),newer=Query(2);
  reuse.Begin(old);reuse.Draw();reuse.End(old,91);
  reuse.Begin(newer);reuse.End(newer,0);
  reuse.Drain(NativeCompletionReason::PrimaryBufferEnd);
  Require(reuse.retired==2&&reuse.writes.size()==1&&reuse.writes[0].identity==newer&&
          reuse.guest_reports.at(newer.end_address)==0&&reuse.tickets.empty(),
          "Address reuse published an old generation or dropped its retirement");

  CompletionFixture pressure;
  for(size_t i=0;i<NativeQueryTickets::kCapacity;++i) {
    const auto identity=Query(i+1,0x1000+uint32_t(i)*64);
    pressure.Begin(identity);pressure.End(identity,i);
  }
  Require(pressure.tickets.full()&&pressure.writes.empty()&&pressure.completion.pending(),
          "Zero-draw tickets did not apply bounded pressure");
  // Drain before entering the next end callback. Its immediate capability is
  // not available while older tickets complete, so it cannot be consumed by
  // that pressure drain before the new Defer call.
  pressure.Drain(NativeCompletionReason::QueryPressure);
  const auto next=Query(NativeQueryTickets::kCapacity+1,0x9000);
  pressure.Begin(next);pressure.End(next,123);
  Require(pressure.tickets.size()==1&&pressure.retired==NativeQueryTickets::kCapacity&&
          pressure.writes.size()==NativeQueryTickets::kCapacity,
          "Pressure drain consumed the next end or lost an older zero-draw result");
  pressure.Drain(NativeCompletionReason::Wait);
  Require(pressure.writes.back().identity==next&&pressure.writes.back().samples==123,
          "Query after ticket pressure did not complete");
}

void CheckFailureAndScope() {
  CompletionFixture failed_fence;
  const auto first=Query(1),second=Query(2,0x1040);
  failed_fence.Begin(first);failed_fence.Draw();failed_fence.End(first,7);
  failed_fence.fail_fence=true;
  Reject([&]{failed_fence.Drain(NativeCompletionReason::Wait);},"Injected fence failure was ignored");
  Require(failed_fence.completion.pending()&&failed_fence.tickets.size()==1&&
          failed_fence.writes.empty()&&failed_fence.completed_serial==0,
          "Fence failure retired ownership or published unfenced samples");

  CompletionFixture failed_write;
  failed_write.Begin(first);failed_write.Draw();failed_write.End(first,7);
  failed_write.Begin(second);failed_write.End(second,0);
  failed_write.fail_publication_generation=second.generation;
  Reject([&]{failed_write.Drain(NativeCompletionReason::ProviderRemoval);},"Injected publication failure was ignored");
  Require(failed_write.completion.pending()&&failed_write.tickets.size()==1&&failed_write.writes.size()==1&&
          failed_write.writes[0].identity==first&&failed_write.next_result==1,
          "Partial publication failure lost the exact unconsumed result");
  // A retained capability can be retried without writing the completed prefix
  // twice or submitting already-fenced drawing again. Production may instead
  // stop fatally; this proves the primitives do not silently lose ownership.
  failed_write.fail_publication_generation=0;
  failed_write.Drain(NativeCompletionReason::ProviderRemoval);
  Require(failed_write.writes.size()==2&&failed_write.writes[1].identity==second&&
          failed_write.fence_calls==1&&failed_write.retired==2&&!failed_write.completion.pending(),
          "Publication retry repeated a report, lost zero, or replayed drawing");

  CompletionFixture scope;
  scope.Begin(first);scope.Draw();
  {
    const auto callback=scope.tickets.BeginEndCallback(first);
    scope.SealCurrent(first,13);
    scope.Drain(NativeCompletionReason::ReferenceDraw);
    Require(scope.tickets.current_end()==first&&scope.tickets.end_status()==NativeQueryTickets::EndStatus::Published&&
            scope.tickets.empty()&&scope.writes.size()==1,
            "Deferral/completion erased end-callback identity needed for handback");
  }
  Require(!scope.tickets.current_end(),"End callback scope outlived its owner");
}

void CheckTerminalDrain() {
  CompletionFixture closing;
  const auto identity=Query(1);
  closing.Begin(identity);closing.Draw();closing.End(identity,31);
  closing.accepting=false;
  Reject([&]{closing.Draw();},"Stop admitted another draw");
  Reject([&]{closing.Begin(Query(2));},"Stop admitted another query");
  closing.Drain(NativeCompletionReason::Shutdown,true);
  Require(closing.writes.size()==1&&closing.writes[0].samples==31&&closing.tickets.empty()&&
          !closing.completion.pending()&&closing.results.empty(),"Terminal close lost accepted query ownership");
  closing.Drain(NativeCompletionReason::Shutdown,true);
  Require(closing.writes.size()==1&&closing.fence_calls==1,"Repeated terminal drain replayed completed work");
}
}

int main() {
  try {
    CheckOrderingAndIdle();CheckPublicationBoundaries();CheckReuseAndPressure();
    CheckFailureAndScope();CheckTerminalDrain();
    std::cout<<"Deferred query completion PASS: zero-draw order and idle progress, nine publication boundaries, "
      "ticket pressure, reused addresses, fence/publication failure retention, exact-once retry, end scope and terminal drain\n";
    return 0;
  }catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
