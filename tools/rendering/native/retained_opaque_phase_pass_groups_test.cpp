#include "renderer/native/retained_opaque_phase_pass_groups.h"
#include "renderer/native/retained_opaque_authority.h"
#include "renderer/native/draw_command_groups.h"
#include <array>
#include <iostream>
#include <memory>
#include <stdexcept>
using namespace gow3::renderer;
namespace {
unsigned checks=0;
void Check(bool p,const char* why){++checks;if(!p)throw std::runtime_error(why);}
template<class F>void Reject(F&& f,const char* why){bool rejected=false;try{f();}catch(const std::exception&){rejected=true;}Check(rejected,why);}
void IdentityAndAuthority(){
    int owner=0,other=0,framebuffer=0,color=0,depth=0;
    NativeRetainedOpaquePhasePassIdentity a{&owner,1,&framebuffer,&color,&depth};
    Check(NativeRetainedOpaquePhasePassContinues(a,a),"Exact generation did not continue");
    auto different=[&](auto edit){auto b=a;edit(b);Check(!NativeRetainedOpaquePhasePassContinues(a,b),"Foreign identity joined pass");};
    different([&](auto&b){b.owner=&other;});different([](auto&b){++b.generation;});
    different([&](auto&b){b.framebuffer=&other;});different([&](auto&b){b.color=&other;});different([&](auto&b){b.depth=&other;});
    different([](auto&b){b.owner=nullptr;});different([](auto&b){b.generation=0;});different([](auto&b){b.framebuffer=nullptr;});
    different([](auto&b){b.color=nullptr;});different([](auto&b){b.depth=nullptr;});different([](auto&b){b.depth=b.color;});
    NativeRetainedOpaqueAuthority authority;authority.Admit(1,kNativeRetainedOpaqueLayout,2,3);
    const auto ticket=authority.ticket();authority.AcceptedDraw(ticket,2,3,4,5);
    Check(authority.dirty()&&authority.ticket().generation==a.generation,"Logical draw changed generation");
    Check(NativeRetainedOpaquePhasePassContinues(a,a),"Own version promotion prevented continuation");
    authority.Materialized(authority.ticket(),true);authority.Admit(2,kNativeRetainedOpaqueLayout,4,5);
    auto b=a;b.generation=authority.ticket().generation;
    Check(!NativeRetainedOpaquePhasePassContinues(a,b),"Restored shared physical images joined old generation");
    NativeRetainedOpaquePhasePassProof p{a,true,true,true,false,false,false};
    Check(NativeRetainedOpaquePhasePassAllowed(p),"Certified pass member refused");
    for(unsigned mask=0;mask<64;++mask){
        auto q=p;q.actual_pipeline_certified=mask&1;q.actual_attachments=mask&2;q.exact_variants=mask&4;
        q.queried=mask&8;q.discarded=mask&16;q.rectangle=mask&32;
        Check(NativeRetainedOpaquePhasePassAllowed(q)==(mask==7),"Dynamic admission broadened");
    }
    auto strong=std::make_shared<int>(0);std::weak_ptr<int> weak=strong;strong.reset();
    Check(!weak.lock(),"Expired open generation remains a live owner");
}
void LogicalOwnership(){
    NativeDrawCommandGroups g;std::array<NativeDrawGroupEntry,16> entries{};
    std::array<std::shared_ptr<int>,16> owners;std::array<std::weak_ptr<int>,16> weak;
    for(unsigned i=0;i<16;++i){owners[i]=std::make_shared<int>(int(i));weak[i]=owners[i];
        const auto a=g.Accept(42);entries[i]={i?0u:42u,a.group,0,true,i+1};}
    Reject([&]{NativeDrawCommandGroups::Plan(entries);},"Open pass became submit-ready");
    Reject([&]{g.Accept(42);},"Pass exceeded existing sixteen-draw cap");
    entries[0].group_draws=g.Seal(42).draws;
    const auto p=NativeDrawCommandGroups::Plan(entries);Check(p.physical_lists==1&&p.logical_draws==16,"Grouping lost logical work");
    Reject([&]{NativeDrawCommandGroups::Plan(std::span(entries).first(15));},"A partial fence split the physical leader");
    auto bad=entries;bad[7]={0,entries[0].group,0,false,0};
    Reject([&]{NativeDrawCommandGroups::Plan(bad);},"Transfer entered retained render pass");
    for(unsigned i=0;i<16;++i)Check(!weak[i].expired(),"Grouped logical owner retired before fence");
    for(auto&o:owners)o.reset();
    for(auto&w:weak)Check(w.expired(),"Completed owner remained leaked");
    Check(g.Accept(42).group==2,"Recycled physical leader reused old group ID");
    Check(g.Seal(42).draws==1,"Boundary restart changed closure count");
}
}
int main(){try{IdentityAndAuthority();LogicalOwnership();std::cout<<checks<<" checks passed (CPU ownership/policy only; no GPU ordering proof)\n";return 0;}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
