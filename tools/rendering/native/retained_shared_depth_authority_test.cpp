#include "retained_opaque_authority.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace gow3::renderer;
namespace {
uint64_t checks=0;
void Check(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
template<class F> void Poisons(F&& operation,NativeRetainedOpaqueAuthority& state){
    bool refused=false;try{operation();}catch(const std::logic_error&){refused=true;}
    Check(refused&&state.failed()&&state.dirty(),"Accepted authority mismatch did not retain poisoned dirty state");
    bool materialize=false;try{state.BeforeMaterialize(state.ticket().color,state.ticket().depth);}catch(const std::logic_error&){materialize=true;}
    Check(materialize,"Poisoned state accepted a later canonical commit");
}
uint32_t Next(uint32_t& seed){seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return seed;}
bool Same(NativeRetainedOpaqueAuthority::Ticket a,NativeRetainedOpaqueAuthority::Ticket b){return a.generation==b.generation&&a.color==b.color&&a.depth==b.depth;}
// Independent physical-page maximum oracle. Query draws do not change data,
// but the original conservative RecordSurfaceWrite updates depth page versions.
struct Versions {
    static constexpr uint32_t kPage=4096,kPlane=720*5120,kBytes=2048*5120;
    std::array<uint64_t,kBytes/kPage> pages{};uint64_t serial=0;
    void Write(uint32_t first,uint32_t bytes){++serial;for(uint32_t p=first/kPage;p<=(first+bytes-1)/kPage;++p)pages[p]=serial;}
    uint64_t Read(uint32_t first,uint32_t bytes)const{uint64_t v=0;for(uint32_t p=first/kPage;p<=(first+bytes-1)/kPage;++p)v=std::max(v,pages[p]);return v;}
    uint64_t Color()const{return Read(kPlane,kPlane);}uint64_t Depth()const{return Read(0,kPlane);}
};
void MetadataRefusals(){
    using Ticket=NativeRetainedOpaqueAuthority::Ticket;
    for(unsigned bad=0;bad<11;++bad){
        NativeRetainedOpaqueAuthority s;s.Admit(7,kNativeRetainedOpaqueLayout,10,20);s.AcceptedDraw(s.ticket(),10,20,11,21);
        auto layout=kNativeRetainedOpaqueLayout;auto before=s.ticket();auto observed_color=before.color,observed_depth=before.depth;
        uint64_t next_color=before.color,next_depth=before.depth+1;bool audited=true,discarded=false;
        switch(bad){
            case 0:++layout[3];break;case 1:++before.generation;break;case 2:--before.generation;break;
            case 3:++before.color;break;case 4:++before.depth;break;case 5:++observed_color;break;
            case 6:++observed_depth;break;case 7:++next_color;break;case 8:next_depth=before.depth;break;
            case 9:audited=false;break;case 10:discarded=true;break;
        }
        const Ticket retained=s.ticket();
        // Preparation/allocation/cancellation failure is preaccept: even bogus
        // hypothetical inputs must not alter the authority that remains owned.
        Check(!s.PromoteAcceptedReadonly(layout,before,observed_color,observed_depth,next_color,next_depth,false,audited,discarded),"Unaccepted readonly operation promoted metadata");
        Check(Same(s.ticket(),retained)&&s.dirty()&&!s.failed(),"Failed admission changed authority");
        Poisons([&]{s.PromoteAcceptedReadonly(layout,before,observed_color,observed_depth,next_color,next_depth,true,audited,discarded);},s);
        Check(Same(s.ticket(),retained),"Mismatch changed token before refusal");
    }
    NativeRetainedOpaqueAuthority clean;clean.Admit(1,kNativeRetainedOpaqueLayout,1,2);
    bool refused=false;try{clean.PromoteAcceptedReadonly(kNativeRetainedOpaqueLayout,clean.ticket(),1,2,1,3,true,true,false);}catch(const std::logic_error&){refused=true;}
    Check(refused&&clean.failed()&&!clean.dirty(),"A clean converted generation entered dirty readonly promotion");
}
void PageAndWordOracle(){
    constexpr size_t kWords=Versions::kBytes/4,kPlaneWords=Versions::kPlane/4,kPixels=4096;
    uint32_t seed=0x8f291ace;std::vector<uint32_t> original(kWords);
    for(auto& word:original)word=Next(seed);
    for(size_t i=0;i<kPixels;++i)original[i]|=0x100u; // Strictly positive stored depth guarantees the zero control.
    auto canonical=original,baseline=original;
    // A raw retained copy of the exact used pixels; no float re-encoding. The
    // remainder of the full ten MiB must remain byte-identical at materialize.
    std::vector<uint32_t> retained_depth(canonical.begin(),canonical.begin()+kPixels);
    std::vector<uint32_t> retained_color(canonical.begin()+kPlaneWords,canonical.begin()+kPlaneWords+kPixels);
    Versions versions;versions.Write(0,Versions::kPlane);versions.Write(Versions::kPlane,Versions::kPlane);
    NativeRetainedOpaqueAuthority authority;authority.Admit(1,kNativeRetainedOpaqueLayout,versions.Color(),versions.Depth());
    uint64_t queries=0,positive=0,zero=0;
    for(unsigned iteration=0;iteration<32;++iteration){
        const auto ticket=authority.ticket();const auto color_before=versions.Color(),depth_before=versions.Depth();
        for(size_t i=0;i<kPixels;++i){
            const uint32_t incoming=Next(seed)&0xffffffu,color=Next(seed);
            const auto old=baseline[i];if(incoming>=(old>>8)){baseline[i]=incoming<<8|(old&255);baseline[kPlaneWords+i]=color;}
            const auto held=retained_depth[i];if(incoming>=(held>>8)){retained_depth[i]=incoming<<8|(held&255);retained_color[i]=color;}
        }
        versions.Write(Versions::kPlane,Versions::kPlane);versions.Write(0,Versions::kPlane);
        authority.AcceptedDraw(ticket,color_before,depth_before,versions.Color(),versions.Depth());
        for(unsigned query=0;query<3;++query){
            const auto before=authority.ticket();const auto cv=versions.Color(),dv=versions.Depth();
            uint64_t samples_original=0,samples_retained=0;
            // Explicit zero and positive controls plus arbitrary overlap. On
            // these monotone depth-code grid values integer ordering is exact.
            for(size_t i=0;i<kPixels;++i){
                const uint32_t incoming=query==0?0u:query==1?0xffffffu:(Next(seed)&0xffffffu);
                samples_original+=incoming>=(baseline[i]>>8);samples_retained+=incoming>=(retained_depth[i]>>8);
            }
            Check(samples_original==samples_retained,"Readonly overlap query differs from packed oracle");
            positive+=samples_original>0;zero+=samples_original==0;++queries;
            versions.Write(0,Versions::kPlane);
            Check(authority.PromoteAcceptedReadonly(kNativeRetainedOpaqueLayout,before,cv,dv,versions.Color(),versions.Depth(),true,true,false),"Exact own depth metadata refused");
            Check(authority.ticket().color==before.color&&authority.ticket().depth>before.depth&&authority.dirty(),"Readonly promotion changed color or dirty status");
            Check(authority.Reusable(kNativeRetainedOpaqueLayout,versions.Color(),versions.Depth()),"Consecutive readonly/writer continuation cannot reuse exact generation");
        }
    }
    Check(queries==96&&positive>0&&zero>0,"Query oracle lacks real zero/positive modeled outcomes");
    Check(canonical==original,"Readonly/writer model accidentally changed stale canonical bytes");
    authority.BeforeMaterialize(versions.Color(),versions.Depth());
    std::copy(retained_depth.begin(),retained_depth.end(),canonical.begin());
    std::copy(retained_color.begin(),retained_color.end(),canonical.begin()+kPlaneWords);
    Check(canonical==baseline,"Full ten MiB/color/depth/stencil/untouched words differ after exact materialization");
    const auto last=authority.ticket();authority.Materialized(last,false);
    Check(authority.dirty()&&Same(authority.ticket(),last),"Rejected export dropped dirty owner token");
    authority.Materialized(last,true);Check(!authority.present()&&!authority.dirty(),"Accepted export did not close authority");
    // Unrelated writes leave the exact token alone; physical alias writes must
    // force a poison instead of overwriting authoritative image contents.
    for(unsigned kind=0;kind<3;++kind){
        NativeRetainedOpaqueAuthority s;Versions v;v.Write(0,Versions::kPlane);v.Write(Versions::kPlane,Versions::kPlane);
        s.Admit(1,kNativeRetainedOpaqueLayout,v.Color(),v.Depth());const auto before=s.ticket();
        v.Write(Versions::kPlane,Versions::kPlane);v.Write(0,Versions::kPlane);s.AcceptedDraw(before,before.color,before.depth,v.Color(),v.Depth());
        const auto t=s.ticket();v.Write(kind==0?Versions::kPlane*2:kind==1?Versions::kPlane:0,4);
        if(kind==0){Check(s.Reusable(kNativeRetainedOpaqueLayout,v.Color(),v.Depth()),"Unrelated physical page broke generation");}
        else {Check(!s.Reusable(kNativeRetainedOpaqueLayout,v.Color(),v.Depth()),"Overlapping physical page was ignored");Poisons([&]{s.PromoteAcceptedReadonly(kNativeRetainedOpaqueLayout,t,v.Color(),v.Depth(),v.Color(),v.Depth()+1,true,true,false);},s);}
    }
}
}
int main()try{MetadataRefusals();PageAndWordOracle();std::cout<<"PASS retained shared depth authority: "<<checks<<" checks, 32 writers/96 modeled queries, exact ten MiB words/stencil/color, preaccept refusal and accepted mismatch poison; no resource or GPU proof\n";}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
