#include "renderer/native/packed_color_variant8_catalog.h"
#include "renderer/native/retained_opaque_catalog.h"
#include <iostream>
#include <stdexcept>
#include <string>
using namespace gow3::renderer;
namespace {
uint64_t checks=0;
void Check(bool condition,const char* message) {++checks;if(!condition)throw std::runtime_error(message);}
NativeFsiConstants Raster() {
    NativeFsiConstants r;r.pitch=1280;r.depth_format=1;r.depth_control=0x700766;
    r.color[0]={720,3,15,0x10001};
    for(unsigned i=1;i<4;++i)r.color[i].blend=0x10001;
    return r;
}
}
int main()try {
    NativeFsiStateSpecialization state;state.words=kNativePackedColorVariant8State;
    Check(kNativePackedColorVariant8Recipes.size()==53,"Closed tuple count changed");
    const NativePackedColorVariant8Recipe* previous=nullptr;
    for(const auto& row:kNativePackedColorVariant8Recipes) {
        Check(NativePackedColorVariant8Tuple(row.vertex,row.pixel,row.raster,state,20,-1),"Exact tuple refused");
        Check(!RetainedOpaqueCatalogPair(row.vertex,row.pixel),"New branch overlaps unchanged legacy branch");
        if(previous)Check(std::tie(previous->vertex,previous->pixel,previous->raster)<std::tie(row.vertex,row.pixel,row.raster),"Tuple order/uniqueness violated");
        previous=&row;
        auto source=std::string(row.vertex);source[0]='X';
        Check(!NativePackedColorVariant8Tuple(source,row.pixel,row.raster,state,20,-1),"Unknown VS accepted");
        source=std::string(row.pixel);source[0]='X';
        Check(!NativePackedColorVariant8Tuple(row.vertex,source,row.raster,state,20,-1),"Unknown PS accepted");
        for(unsigned i=0;i<row.raster.size();++i) {
            auto raster=row.raster;raster[i]^=0x80000000u;
            Check(!NativePackedColorVariant8Tuple(row.vertex,row.pixel,raster,state,20,-1),"Changed raster accepted");
        }
        for(unsigned i=0;i<state.words.size();++i) {
            auto changed=state;changed.words[i]^=1;
            Check(!NativePackedColorVariant8Tuple(row.vertex,row.pixel,row.raster,changed,20,-1),"Changed specialization accepted");
        }
        Check(!NativePackedColorVariant8Tuple(row.vertex,row.pixel,row.raster,state,-1,-1),"Foreign color format accepted");
        Check(!NativePackedColorVariant8Tuple(row.vertex,row.pixel,row.raster,state,20,0),"Host depth accepted");
    }
    // The seven unresolved entry-pairing sources remain refused for every VS
    // and raster that is otherwise admitted. No Cartesian broadening.
    for(auto excluded:{"07297a57c1d500b10d63b3eaf81a57e58ab0f0f2f9cf596f52ddad1c51ac83d9",
        "1e3be76bf5f9ca19116d005818c83497d25b7791142af56babff056cd95acc43",
        "28b63a43c49413829c4059148707f9b8456c28926cd22d3ceec9f7e9f18cd286",
        "333ce299839d3d857f8829e4249516ce823ea95e86fcf82473b81c068bd3c79f",
        "472d1f7c9fad138c0dc86011ee5ac9e158c65fda70c0a2d55fbfe1328e66e809",
        "779faa05c69ed3950abea4e20368f3fa6ca965da9755f2eba67a59ef37b0abf5",
        "b129409c8c06202f8c89e1fc57bd4f1c861b74dfeee4acf07ec3cb4f784382a4"})
        for(const auto& row:kNativePackedColorVariant8Recipes)
            Check(!NativePackedColorVariant8Tuple(row.vertex,excluded,row.raster,state,20,-1),"Unproved source accepted");
    const auto& a=kNativePackedColorVariant8Recipes[0];
    const auto& b=kNativePackedColorVariant8Recipes[2];
    Check(!NativePackedColorVariant8Tuple(a.vertex,b.pixel,a.raster,state,20,-1),"Cross-pair broadening");
    int vs=0,ps=0,pso=0,replacement=0;
    NativePackedColorVariant8PassProof p;
    p.vertex_module=p.certified_vertex_module=&vs;p.pixel_module=p.certified_pixel_module=&ps;
    p.pipeline=p.certified_pipeline=&pso;p.pixel_variant=NativeShaderVariant::PipelineStateEpilogueV1;
    p.exact_tuple=p.cpu_geometry=p.indexed_triangles=p.private_framebuffer=p.bounded_scissor=p.primary_surfaces=p.single_sample_layer=true;
    auto raster=Raster();Check(NativePackedColorVariant8PassAllowed(raster,p),"Actual owner proof refused");
    for(auto member:{&NativePackedColorVariant8PassProof::vertex_module,&NativePackedColorVariant8PassProof::certified_vertex_module,
        &NativePackedColorVariant8PassProof::pixel_module,&NativePackedColorVariant8PassProof::certified_pixel_module,
        &NativePackedColorVariant8PassProof::pipeline,&NativePackedColorVariant8PassProof::certified_pipeline}) {
        auto changed=p;changed.*member=nullptr;Check(!NativePackedColorVariant8PassAllowed(raster,changed),"Missing owner accepted");
        changed=p;changed.*member=&replacement;Check(!NativePackedColorVariant8PassAllowed(raster,changed),"Replaced owner accepted");
    }
    for(auto member:{&NativePackedColorVariant8PassProof::exact_tuple,&NativePackedColorVariant8PassProof::cpu_geometry,
        &NativePackedColorVariant8PassProof::indexed_triangles,&NativePackedColorVariant8PassProof::private_framebuffer,
        &NativePackedColorVariant8PassProof::bounded_scissor,&NativePackedColorVariant8PassProof::primary_surfaces,
        &NativePackedColorVariant8PassProof::single_sample_layer}) {
        auto changed=p;changed.*member=false;Check(!NativePackedColorVariant8PassAllowed(raster,changed),"Missing draw guard accepted");
    }
    for(auto member:{&NativePackedColorVariant8PassProof::active_query,&NativePackedColorVariant8PassProof::discarded,&NativePackedColorVariant8PassProof::rectangle}) {
        auto changed=p;changed.*member=true;Check(!NativePackedColorVariant8PassAllowed(raster,changed),"Incompatible draw accepted");
    }
    auto changed=p;changed.vertex_variant=NativeShaderVariant::ForwardOnlyGuestControlFlowV1;
    Check(!NativePackedColorVariant8PassAllowed(raster,changed),"Wrong actual VS variant accepted");
    changed=p;changed.pixel_variant=NativeShaderVariant::Unmodified;
    Check(!NativePackedColorVariant8PassAllowed(raster,changed),"Wrong actual PS variant accepted");
    auto bad=raster;bad.color[0].base=719;Check(!NativePackedColorVariant8PassAllowed(bad,p),"Color alias accepted");
    bad=raster;bad.depth_base=720;Check(!NativePackedColorVariant8PassAllowed(bad,p),"Depth alias accepted");
    bad=raster;bad.pitch=640;Check(!NativePackedColorVariant8PassAllowed(bad,p),"Wrong pitch accepted");
    bad=raster;bad.color[0].blend^=1;Check(!NativePackedColorVariant8PassAllowed(bad,p),"Actual specialization drift accepted");
    std::cout<<"PASS "<<checks<<" checks: exact tuple/state, excluded sources, actual owners and draw guards\n";
}catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
