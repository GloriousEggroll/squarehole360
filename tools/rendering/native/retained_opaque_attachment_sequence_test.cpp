#include "retained_opaque_attachment_sequence.h"
#include "retained_opaque_authority.h"
#include <iostream>
#include <stdexcept>

using namespace gow3::renderer;
using S=NativeRetainedOpaqueAttachmentSequence;
static unsigned checks=0;
static void Require(bool value){++checks;if(!value)throw std::runtime_error("attachment sequence check "+std::to_string(checks));}
static S::Binding Binding(){return {1,2,3,4,5,6,7,2048u*5120,2048u*5120+4096};}
static S::Ticket Ticket(const NativeRetainedOpaqueAuthority& a){const auto t=a.ticket();return {t.generation,t.color,t.depth};}
struct Harness {
    S sequence;S::Binding binding=Binding();S::Receipt receipt{100,20,20};
    Harness(){Require(sequence.Bind(binding));}
    S::Token Prepare(uint64_t generation,bool store=false,uint64_t color=10,uint64_t depth=20){
        auto t=sequence.Prepare(binding,{generation,color,depth},store);Require(bool(t));return t;
    }
    void Record(S::Token t){
        Require(sequence.CanRecord(binding,t,true,true,true));
        Require(sequence.BeginRecord(binding,t,receipt,true,true,true));
        ++receipt.stages;Require(sequence.Recorded(t,receipt.stages,true));
        Require(!sequence.CanRecord(binding,t,true,true,true));
    }
    void Ack(S::Token t){++receipt.accepted;++receipt.packets;Require(sequence.Acknowledge(binding,t,receipt,true,true));}
    void Accept(S::Token t){Record(t);Ack(t);}
};
int main(){try{
    // Actual authority transitions, distinct logical owners, one physical binding.
    Harness h;
    NativeRetainedOpaqueAuthority first;
    first.Admit(1,kNativeRetainedOpaqueLayout,10,20);
    auto restore=h.Prepare(1);h.Accept(restore);
    first.AcceptedDraw(first.ticket(),10,20,11,21);
    Require(!h.sequence.CanClose(h.binding,Ticket(first),true));
    auto store=h.Prepare(1,true,11,21);
    // Original pressure contract: no stage was recorded, same Prepared retries.
    Require(h.sequence.CanRecord(h.binding,store,true,true,true));
    Require(h.sequence.CanRecord(h.binding,store,true,true,true));
    h.Accept(store);auto closed=Ticket(first);
    Require(h.sequence.CanClose(h.binding,closed,true));
    first.Materialized(first.ticket(),true);
    Require(h.sequence.Close(h.binding,closed,true));
    Require(!first.present());
    NativeRetainedOpaqueAuthority second;
    second.Admit(2,kNativeRetainedOpaqueLayout,12,22);
    auto next=h.Prepare(2,false,12,22);
    Require(!h.sequence.CanRecord(h.binding,restore,true,true,true));
    Require(!h.sequence.CanRecord(h.binding,store,true,true,true));
    Require(!h.sequence.Cancel(restore)); // old destructor cannot cancel new gen
    h.Accept(next);
    // An accepted restore with no accepted draw is a valid clean closure.
    closed=Ticket(second);Require(h.sequence.CanClose(h.binding,closed,false));
    Require(!h.sequence.CanClose(h.binding,closed,true));
    second.Materialized(second.ticket(),true);Require(h.sequence.Close(h.binding,closed,false));
    Require(!h.sequence.Prepare(h.binding,{2,12,22},false));
    Require(!h.sequence.Prepare(h.binding,{1,12,22},false));

    // Cancel before any GPU recording: store keeps active authority, restore idle.
    Harness cancel;auto c=cancel.Prepare(1);Require(cancel.sequence.Cancel(c));
    Require(cancel.sequence.state()==S::State::Idle);
    c=cancel.Prepare(2);cancel.Accept(c);
    auto cs=cancel.Prepare(2,true,11,21);Require(cancel.sequence.Cancel(cs));
    Require(cancel.sequence.state()==S::State::Active);
    cs=cancel.Prepare(2,true,11,21);cancel.Accept(cs);
    Require(cancel.sequence.CanClose(cancel.binding,{2,11,21},true));

    // Each identity dimension independently refuses before recording.
    for(unsigned field=0;field<9;++field){
        Harness x;auto t=x.Prepare(1);auto wrong=x.binding;
        switch(field){case 0:++wrong.writer;break;case 1:++wrong.batch;break;
        case 2:++wrong.queue;break;case 3:++wrong.canonical;break;
        case 4:++wrong.canonical_vk;break;case 5:++wrong.canonical_allocation;break;
        case 6:++wrong.attachments;break;case 7:++wrong.canonical_extent;break;
        case 8:++wrong.canonical_allocation_bytes;break;}
        Require(!x.sequence.Bind(wrong));
        Require(!x.sequence.CanRecord(wrong,t,true,true,true));
        Require(!x.sequence.BeginRecord(wrong,t,x.receipt,true,true,true));
        Require(x.sequence.state()==S::State::PreparedRestore);
    }
    for(unsigned field=0;field<3;++field){
        Harness x;auto t=x.Prepare(1);
        Require(!x.sequence.CanRecord(x.binding,t,field!=0,field!=1,field!=2));
        Require(!x.sequence.BeginRecord(x.binding,t,x.receipt,field!=0,field!=1,field!=2));
    }
    // Mere numeric progress is not accepted: one exact stage and packet required.
    for(unsigned field=0;field<8;++field){
        Harness x;auto t=x.Prepare(1);x.Record(t);auto after=x.receipt;
        ++after.accepted;++after.packets;bool closed_scope=true,healthy=true;
        switch(field){case 0:--after.accepted;break;case 1:++after.accepted;break;
        case 2:--after.stages;break;case 3:++after.stages;break;
        case 4:--after.packets;break;case 5:++after.packets;break;
        case 6:closed_scope=false;break;case 7:healthy=false;break;}
        Require(!x.sequence.Acknowledge(x.binding,t,after,closed_scope,healthy));
        Require(x.sequence.failed());Require(!x.sequence.Bind(x.binding));
        Require(!x.sequence.Prepare(x.binding,{2,10,20},false));
    }
    // Uncertain recorded/recording tails can never become cancellation success.
    for(bool recorded:{false,true}){
        Harness x;auto t=x.Prepare(1);
        Require(x.sequence.BeginRecord(x.binding,t,x.receipt,true,true,true));
        if(recorded)Require(x.sequence.Recorded(t,x.receipt.stages+1,true));
        Require(!x.sequence.Cancel(t));Require(x.sequence.failed());
    }
    Harness dirty;auto d=dirty.Prepare(1);dirty.Accept(d);
    Require(!dirty.sequence.CanClose(dirty.binding,{1,11,21},false));
    Require(!dirty.sequence.Close(dirty.binding,{1,11,21},true));
    Require(dirty.sequence.failed());
    Harness max;auto m=max.Prepare(UINT64_MAX);max.Accept(m);
    Require(max.sequence.Close(max.binding,{UINT64_MAX,10,20},false));
    Require(!max.sequence.Prepare(max.binding,{0,10,20},false));
    Require(!max.sequence.Prepare(max.binding,{1,10,20},false));
    for(unsigned field=0;field<3;++field){
        Harness x;auto t=x.Prepare(1);auto before=x.receipt;
        if(field==0)before.accepted=UINT64_MAX;
        if(field==1)before.stages=UINT64_MAX;
        if(field==2)before.packets=UINT64_MAX;
        Require(!x.sequence.BeginRecord(x.binding,t,before,true,true,true));
        Require(x.sequence.Cancel(t));
    }
    std::cout<<"PASS "<<checks<<" attachment sequence/actual-authority checks\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
