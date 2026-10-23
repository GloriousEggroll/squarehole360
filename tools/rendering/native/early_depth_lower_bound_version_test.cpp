#include "renderer/native/early_depth_lower_bound_version.h"
#include <iostream>
#include <stdexcept>
using namespace gow3::renderer;
namespace {
unsigned checks=0;
void Require(bool v){++checks;if(!v)throw std::runtime_error("Early-depth anchor lifetime failed");}
}
int main() {
    const NativeReadonlyDepthLayoutKey layout{1280,720,1280,0,1,1};
    for(uint64_t generation:{0u,1u,999u})for(uint64_t source:{0u,5u,999u}) {
        NativeEarlyDepthLowerBoundVersion version(generation,layout,source);
        Require(version.Reusable(layout,source)==bool(generation&&source));
        Require(bool(version.Capture(layout,source))==bool(generation&&source));
    }
    NativeEarlyDepthLowerBoundVersion version(7,layout,11);
    Require(version.anchor()==11&&version.current()==11&&version.generation()==7);
    auto ticket=version.Capture(layout,11);Require(bool(ticket));
    for(unsigned field=0;field<6;++field) {
        auto other=layout;++other[field];
        Require(!version.Reusable(other,11));
        Require(!version.PromoteAcceptedMonotonic(other,*ticket,11,12,true,true,false));
        Require(version.Reusable(layout,11));
    }
    for(auto forged:{NativeEarlyDepthLowerBoundVersion::Ticket{8,11,11},
                     NativeEarlyDepthLowerBoundVersion::Ticket{7,12,11},
                     NativeEarlyDepthLowerBoundVersion::Ticket{7,11,12}})
        Require(!version.PromoteAcceptedMonotonic(layout,forged,11,12,true,true,false));
    for(unsigned failure=0;failure<5;++failure) {
        Require(!version.PromoteAcceptedMonotonic(layout,*ticket,11,
            failure==3?11:failure==4?10:12,failure!=0,failure!=1,failure==2));
        Require(version.current()==11&&version.anchor()==11);
    }
    Require(version.PromoteAcceptedMonotonic(layout,*ticket,11,15,true,true,false));
    Require(version.current()==15&&version.anchor()==11);
    Require(!version.Reusable(layout,11)&&version.Reusable(layout,15));
    Require(!version.PromoteAcceptedMonotonic(layout,*ticket,15,16,true,true,false));
    Require(!version.Capture(layout,16));
    for(uint64_t post=16;post<1040;++post) {
        auto next=version.Capture(layout,post-1);Require(bool(next));
        Require(version.PromoteAcceptedMonotonic(layout,*next,post-1,post,true,true,false));
        Require(version.anchor()==11&&version.current()==post);
    }
    auto stale=version.Capture(layout,1039);Require(bool(stale));
    version.Invalidate();
    Require(!version.Reusable(layout,1039)&&!version.Capture(layout,1039));
    Require(!version.PromoteAcceptedMonotonic(layout,*stale,1039,1040,true,true,false));
    NativeEarlyDepthLowerBoundVersion replacement(8,layout,1039);
    Require(!replacement.PromoteAcceptedMonotonic(layout,*stale,1039,1040,true,true,false));
    Require(replacement.Reusable(layout,1039));
    std::cout<<"Early-depth immutable anchor lifetime PASS "<<checks<<" checks; no GPU/FPS proof\n";
}
