#include "renderer/native/retained_opaque_phase_draw_facts.h"
#include <iostream>
#include <type_traits>
using gow3::renderer::NativeRetainedOpaquePhaseDrawFacts;
using Facts=NativeRetainedOpaquePhaseDrawFacts;
using Raster=Facts::Raster;
namespace {
unsigned checks=0;
void Check(bool value,const char* why) {++checks;if(!value)throw std::runtime_error(why);}
template<class F>void LogicError(F&& f,const char* why) {
    bool rejected=false;try{f();}catch(const std::logic_error&){rejected=true;}
    Check(rejected,why);
}
struct Binding {int value=0;};
const Raster expected{3,2,1,4,1794,0},sentinel{9,8,7,6,5,4};
void FalseFacts() {
    Binding draw,owner,other_draw,other_owner;Facts facts(&draw,&owner);
    unsigned classifications=0,residencies=0;Raster out=sentinel;
    auto classify=[&](Raster& raster){++classifications;raster=expected;return false;};
    auto resident=[&](const Raster&){++residencies;return true;};
    for(unsigned n=0;n<24;++n) {
        Check(!facts.Allowed(&draw,&owner,&out,n%2,classify,resident),"Static refusal admitted");
        Check(out==sentinel,"Static refusal changed output raster");
    }
    Check(classifications==1&&residencies==0,"False fact did not reuse only classification");
    LogicError([&]{facts.Allowed(&other_draw,&owner,&out,true,classify,resident);},"Foreign draw reused negative fact");
    LogicError([&]{facts.Allowed(&draw,&other_owner,&out,true,classify,resident);},"Foreign state owner reused negative fact");
    Check(classifications==1&&residencies==0,"Binding refusal touched callbacks");
}
void TrueFactsAndLiveOwners() {
    Binding draw,state,vs,ps,pso,new_vs,new_ps,new_pso,foreign_draw,foreign_state;
    Facts facts(&draw,&state);unsigned classifications=0,residencies=0;
    const void* vertex=&vs;const void* pixel=&ps;const void* pipeline=&pso;
    const void* certified_vertex=&vs;const void* certified_pixel=&ps;
    bool vertex_audited=true,pixel_audited=true,pipeline_audited=true;
    auto classify=[&](Raster& raster){++classifications;raster=expected;return true;};
    auto resident=[&](const Raster& raster){++residencies;Check(raster==expected,"Live lookup raster changed");
        return vertex&&pixel&&pipeline&&vertex_audited&&pixel_audited&&pipeline_audited&&
            certified_vertex==vertex&&certified_pixel==pixel;};
    Raster out=sentinel;
    Check(facts.Allowed(&draw,&state,&out,false,classify,resident)&&out==expected,"Static-only admission failed");
    Check(classifications==1&&residencies==0,"Static-only call checked residency");
    auto live=[&](bool wanted){
        out=sentinel;const unsigned before=residencies;
        Check(facts.Allowed(&draw,&state,&out,true,classify,resident)==wanted,"Live owner decision was cached");
        Check(out==expected&&residencies==before+1&&classifications==1,"Live lookup/output order changed");
    };
    live(true);
    vertex=&new_vs;live(false);certified_vertex=&new_vs;live(true);
    pixel=&new_ps;live(false);certified_pixel=&new_ps;live(true);
    pipeline=nullptr;live(false);pipeline=&new_pso;live(true);
    vertex_audited=false;live(false);vertex_audited=true;
    pixel_audited=false;live(false);pixel_audited=true;
    pipeline_audited=false;live(false);pipeline_audited=true;live(true);
    vertex=nullptr;live(false);vertex=&new_vs;
    pixel=nullptr;live(false);pixel=&new_ps;live(true);
    const unsigned before=residencies;
    LogicError([&]{facts.Allowed(&foreign_draw,&state,&out,true,classify,resident);},"Different input reused positive fact");
    LogicError([&]{facts.Allowed(&draw,&foreign_state,&out,true,classify,resident);},"Different owner reused positive fact");
    Check(classifications==1&&residencies==before,"Identity failure performed live lookup");
    // A new Draw scope evaluates independently even with exactly the same bindings.
    Facts another_scope(&draw,&state);
    Check(another_scope.Allowed(&draw,&state,nullptr,true,classify,resident),"New draw scope failed");
    Check(classifications==2&&residencies==before+1,"Facts escaped one Draw scope");
}
void ThrowingReadsAndResidency() {
    Binding draw,state;Facts facts(&draw,&state);unsigned classifications=0,residencies=0;
    Raster out=sentinel;bool throw_static=true,throw_resident=false,live=true;
    auto classify=[&](Raster& raster){++classifications;raster=expected;if(throw_static)throw std::runtime_error("lazy read failed");return true;};
    auto resident=[&](const Raster&){++residencies;if(throw_resident)throw std::runtime_error("owner lookup failed");return live;};
    bool rejected=false;try{facts.Allowed(&draw,&state,&out,true,classify,resident);}catch(const std::runtime_error&){rejected=true;}
    Check(rejected&&out==sentinel&&classifications==1&&residencies==0,"Throwing read became a cached decision");
    throw_static=false;
    Check(facts.Allowed(&draw,&state,&out,true,classify,resident)&&out==expected&&classifications==2&&residencies==1,"Failed lazy read was not retried");
    throw_resident=true;rejected=false;
    try{facts.Allowed(&draw,&state,&out,true,classify,resident);}catch(const std::runtime_error&){rejected=true;}
    Check(rejected&&classifications==2&&residencies==2,"Live lookup exception was cached/hidden");
    throw_resident=false;live=false;
    Check(!facts.Allowed(&draw,&state,&out,true,classify,resident)&&classifications==2&&residencies==3,"Owner refusal after exception was stale");
    live=true;
    Check(facts.Allowed(&draw,&state,nullptr,true,classify,resident)&&classifications==2&&residencies==4,"Owner recovery after exception was stale");
}
void Boundaries() {
    Binding draw,state;unsigned calls=0;Raster out=sentinel;
    auto classify=[&](Raster& r){++calls;r=expected;return true;};auto resident=[&](const Raster&){++calls;return true;};
    Facts no_draw(nullptr,&state),no_owner(&draw,nullptr);
    LogicError([&]{no_draw.Allowed(nullptr,&state,&out,true,classify,resident);},"Null input admitted");
    LogicError([&]{no_owner.Allowed(&draw,nullptr,&out,true,classify,resident);},"Unowned input admitted");
    Check(calls==0&&out==sentinel,"Invalid binding read provider data");
    static_assert(!std::is_copy_constructible_v<Facts> && !std::is_move_constructible_v<Facts>);
}
}
int main() {
    try {FalseFacts();TrueFactsAndLiveOwners();ThrowingReadsAndResidency();Boundaries();
        std::cout<<"PASS "<<checks<<" draw-local phase fact checks; no GPU or performance claim\n";return 0;
    }catch(const std::exception& error){std::cerr<<"FAIL after "<<checks<<" checks: "<<error.what()<<'\n';return 1;}
}
