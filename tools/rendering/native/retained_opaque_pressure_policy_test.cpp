#include "renderer/native/retained_opaque_pressure_policy.h"
#include "renderer/native/retained_opaque_resource_budget.h"
#include <array>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
using namespace gow3::renderer;
namespace {
unsigned checks=0;
void Check(bool value,const char* message) {++checks;if(!value)throw std::runtime_error(message);}
void Threshold() {
    using D=NativeRetainedOpaquePressureDecision;
    for(uint64_t cap:{uint64_t(1),uint64_t(32),uint64_t(32*1024*1024),UINT64_MAX}) {
        for(uint64_t pair:{uint64_t(0),uint64_t(1),cap}) {
            Check(NativeRetainedOpaquePressurePoll(false,UINT64_MAX,pair,cap,1)==D::Skip,"disabled changed admission");
            Check(NativeRetainedOpaquePressurePoll(true,cap,pair,cap,0)==D::Skip,"staging-only work polled");
            if(!pair) {
                Check(NativeRetainedOpaquePressurePoll(true,cap,pair,cap,1)==D::Skip,"unknown size polled");
                continue;
            }
            Check(NativeRetainedOpaquePressurePoll(true,cap-pair,pair,cap,1)==D::Skip,"exact fit polled");
            Check(NativeRetainedOpaquePressurePoll(true,cap-pair+1,pair,cap,1)==D::Poll,"one-byte deficit skipped");
            Check(NativeRetainedOpaquePressurePoll(true,cap,pair,cap,4)==D::Poll,"full budget skipped");
        }
        if(cap<UINT64_MAX) {
            Check(NativeRetainedOpaquePressurePoll(true,cap+1,1,cap,1)==D::InvalidBudget,"excess charge hidden");
            Check(NativeRetainedOpaquePressurePoll(true,0,cap+1,cap,1)==D::InvalidBudget,"invalid prior pair hidden");
        }
    }
    Check(NativeRetainedOpaquePressurePoll(true,0,0,0,0)==D::InvalidBudget,"zero limit admitted");
}
struct Image {
    NativeRetainedOpaqueResourceBudget& budget;
    explicit Image(NativeRetainedOpaqueResourceBudget& b):budget(b) {
        if(!budget.TryCharge(4,32))throw std::runtime_error("original image admission refused");
    }
    ~Image(){budget.Release(4);}
};
void Ownership() {
    NativeRetainedOpaqueResourceBudget budget;
    std::array<std::shared_ptr<Image>,8> packet,draw;
    for(unsigned i=0;i<packet.size();++i) {packet[i]=std::make_shared<Image>(budget);draw[i]=packet[i];}
    Check(budget.bytes==32&&!budget.TryCharge(8,32),"next pair escaped exact image cap");
    Check(NativeRetainedOpaquePressurePoll(true,budget.bytes,8,32,1)==NativeRetainedOpaquePressureDecision::Poll,"real pressure not selected");
    // A no-progress poll changes no references: original admission still refuses.
    Check(!budget.TryCharge(8,32),"poll hint fabricated credit");
    draw[0].reset();draw[1].reset();
    Check(budget.bytes==32&&!budget.TryCharge(8,32),"draw completion freed unfenced transfer images");
    packet[0].reset();
    Check(budget.bytes==28&&!budget.TryCharge(8,32),"one retired image credited an entire pair");
    packet[1].reset();
    Check(budget.bytes==24,"actual final pair owner did not release exact bytes");
    auto next_color=std::make_shared<Image>(budget),next_depth=std::make_shared<Image>(budget);
    Check(budget.bytes==32,"ordinary next-pair admission changed after retirement");
    // Completed old commands do not override a real external strong owner.
    auto external_color=packet[2],external_depth=packet[3];
    draw[2].reset();draw[3].reset();packet[2].reset();packet[3].reset();
    Check(budget.bytes==32&&!budget.TryCharge(8,32),"extra strong reference was forcibly reclaimed");
    external_color.reset();external_depth.reset();
    Check(budget.bytes==24,"last actual external owner did not release bytes");
    for(auto& p:draw)p.reset();
    for(auto& p:packet)p.reset();
    next_color.reset();
    next_depth.reset();
    Check(budget.bytes==0&&budget.peak==32,"budget conservation or limit changed");
}
}
int main() {
    try {Threshold();Ownership();std::cout<<"retained phase pressure policy: PASS "<<checks<<" CPU checks; no GPU completion proof\n";return 0;}
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
