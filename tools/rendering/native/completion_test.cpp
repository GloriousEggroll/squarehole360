#include "renderer/native/packet_ownership.h"
#include <rex/graphics/native_completion.h>
#include <array>
#include <algorithm>
#include <iostream>
#include <stdexcept>

using namespace rex::graphics;
using namespace gow3::renderer;
namespace {
void Require(bool condition) {if(!condition)throw std::runtime_error("Native completion assertion failed");}
template<class F> void Reject(F&& f) {
  bool rejected=false;
  try {f();}catch(const std::runtime_error&){rejected=true;}
  Require(rejected);
}
GraphicsCommandSpan SplitDraw() {
  GraphicsCommandSpan span;span.generation=1;span.api_sequence=17;span.address=0x4000;
  for(auto word:{0xc0003600u,0x00030084u,0xc0003600u,0x00060084u})
    for(unsigned byte=0;byte<4;++byte)span.bytes.push_back(uint8_t(word>>(24-8*byte)));
  return span;
}
}
int main() {
  try {
    // These state-only packets occur between the game's ordinary draw packets.
    // None may introduce an accidental per-draw CPU fence. All other decoded
    // and unknown opcodes conservatively fence before externally visible work.
    constexpr std::array state_only={0x10u,0x22u,0x36u,0x2du,0x55u,0x56u,0x27u,0x2bu,
        0x2fu,0x37u,0x3fu,0x50u,0x51u,0x60u,0x61u,0x62u,0x63u};
    for(uint32_t opcode=0;opcode<128;++opcode) {
      const bool state=std::find(state_only.begin(),state_only.end(),opcode)!=state_only.end();
      Require(NativePacketNeedsCompletion(opcode,1)==(!state&&opcode!=0x46&&opcode!=0x3b));
      Require(NativePacketNeedsCompletion(opcode,3)==!state);
    }
    // Every enabled scratch register can publish a guest fence even when
    // reached through type-0/type-1 or a SET_CONSTANT packet.
    for(uint32_t mask=0;mask<256;++mask)
      for(uint32_t index=0x577;index<=0x580;++index) {
        const bool expected=index>=0x578&&index<=0x57f&&((mask>>(index-0x578))&1);
        Require(NativeScratchWriteNeedsCompletion(index,mask)==expected);
      }

    NativeCompletionState completion;
    PacketOwnership ownership;
    const auto span=SplitDraw();ownership.Enqueue(span);
    const auto first=ownership.InspectExecuting(0x4000,{span.bytes.data(),8});Require(bool(first));
    // NativePending accepts the whole API effect immediately. Failure/stop
    // cannot turn the second exact packet back into a reference raster draw.
    ownership.CommitSpan(*first);ownership.Consume(*first);completion.AcceptPending();
    Reject([&]{ownership.Clear();});
    ownership.ClearUncommitted();
    const auto second=ownership.InspectExecuting(0x4008,{span.bytes.data()+8,8});
    Require(second&&second->native_committed&&completion.pending());
    ownership.Consume(*second);completion.AcceptPending();
    Require(!ownership.HasCommittedSpan()&&completion.pending());
    uint32_t submitted=0,published=0;
    const auto fence=[&]{++submitted;};
    for(auto opcode:state_only)if(NativePacketNeedsCompletion(opcode,1))completion.Complete(fence);
    Require(submitted==0&&completion.pending());
    // A guest fence becomes visible only after the accepted draw batch drains.
    if(NativeScratchWriteNeedsCompletion(0x578,1))completion.Complete(fence);
    Require(submitted==1&&!completion.pending());++published;
    completion.Complete(fence);Require(submitted==1&&published==1);
    // Wait, presentation, reference handback, provider removal and shutdown all
    // use this same completion capability, including a batch accepted after an
    // asynchronous close request but before its queued terminal callback.
    for(auto reason:{NativeCompletionReason::Wait,NativeCompletionReason::Presentation,
        NativeCompletionReason::ReferenceDraw,NativeCompletionReason::ProviderRemoval,
        NativeCompletionReason::QueryPressure,NativeCompletionReason::Shutdown,NativeCompletionReason::PrimaryBufferEnd}) {
      (void)reason;completion.AcceptPending();completion.Complete(fence);
      Require(!completion.pending());
    }
    Require(submitted==8);
    completion.Complete(fence,true);Require(submitted==9); // Interrupted shadow callback.

    // Model the SDK/native queue boundary with the production reopen policy.
    // Reopening after every native draw used to queue an empty reference submit
    // at the next boundary. Frame opening and real reference work must survive
    // while a long native-only sequence leaves the reference buffer closed.
    struct ReferenceQueue {
      bool frame_open=false,submission_open=false;
      unsigned submissions=0,frames_opened=0,frames_closed=0,queued=0,executed=0;
      void Begin() {
        if(!frame_open){frame_open=true;++frames_opened;}
        submission_open=true;
      }
      void End() {
        if(submission_open){++submissions;executed+=queued;queued=0;submission_open=false;}
      }
      void Resume(bool reference_required,bool suspended_query=false) {
        if(NativeBoundaryNeedsReferenceSubmission(frame_open,reference_required,suspended_query))Begin();
      }
      void NativeDraw() {End();Resume(false);}
      void NativeQueryEnd() {End();Resume(false);}
      void ReferenceWork() {Begin();++queued;}
      void Swap() {Begin();++queued;End();frame_open=false;++frames_closed;}
    } reference;
    for(unsigned draw=0;draw<800;++draw) {
      reference.NativeDraw();
      if(draw%5==0)reference.NativeQueryEnd();
    }
    Require(reference.frames_opened==1&&reference.submissions==1&&!reference.submission_open);
    // Real queued reference work is flushed, not discarded by an empty-buffer
    // heuristic. A reference continuation and a residual query still reopen.
    reference.ReferenceWork();reference.ReferenceWork();reference.NativeDraw();
    Require(reference.executed==2&&reference.submissions==2&&!reference.submission_open);
    reference.Resume(true);Require(reference.submission_open);reference.End();
    reference.Resume(false,true);Require(reference.submission_open);reference.End();
    reference.NativeDraw();Require(!reference.submission_open);
    reference.Swap();
    Require(reference.frames_closed==1&&!reference.frame_open&&reference.executed==3);
    reference.NativeDraw();
    Require(reference.frames_opened==2&&reference.frame_open&&reference.submission_open);
    // EndSubmission(false) remains valid even when no reference buffer is open;
    // swap is what finalizes the guest frame and its reclamation generation.
    reference.End();reference.End();reference.Swap();
    Require(reference.frames_closed==2&&!reference.frame_open&&reference.executed==4);
    // A failed fence must not clear ownership or allow the following guest
    // publication/reference replay. Recursive drain is rejected for the same
    // reason, with the original accepted work still retained.
    completion.AcceptPending();
    Reject([&]{completion.Complete([&]{throw std::runtime_error("Injected fence failure");});++published;});
    Require(completion.pending()&&published==1);
    Reject([&]{completion.Complete([&]{completion.Complete(fence);});++published;});
    Require(completion.pending()&&published==1&&submitted==9);
    std::cout<<"Native completion PASS: 256 opcode policies, 2560 scratch masks, pending split-span ownership, ordered publication, lazy reference submission/frame lifecycle and failed-fence retention\n";
  }catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
