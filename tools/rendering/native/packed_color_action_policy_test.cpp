#include "renderer/native/packed_color_action_variant8.h"
#include "renderer/native/retained_opaque_catalog.h"
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
using namespace gow3::renderer;
namespace {
uint64_t checks=0;
void Check(bool condition,const char* why) {++checks;if(!condition)throw std::runtime_error(why);}
std::vector<uint8_t> Read(const char* path) {
    std::ifstream file(path,std::ios::binary);if(!file)throw std::runtime_error("Missing module");
    return {std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
}
NativeFsiConstants Raster() {
    NativeFsiConstants r;r.pitch=1280;r.depth_format=1;r.depth_control=0x700766;
    r.color[0]={720,3,15,0x10001};return r;
}
}
int main(int argc,char**argv)try {
    if(argc!=3)throw std::runtime_error("Usage: test vertex.spv pixel.spv");
    const auto vs=Read(argv[1]),ps=Read(argv[2]);
    // The inputs are the actual original runtime modules, not the specialized
    // offline proof module. Independently corrupt header, body and final words.
    for(bool pixel:{false,true}) {
        const auto& code=pixel?ps:vs;
        const auto source=pixel?kPackedColorActionPixel:kPackedColorActionVertex;
        const auto variant=pixel?NativeShaderVariant::PipelineStateEpilogueV1:NativeShaderVariant::Unmodified;
        auto cert=[&](std::span<const uint8_t> bytes) {return NativePackedColorActionVariant8ModuleCertified(source,pixel,variant,bytes);};
        Check(cert(code),"Actual module rejected");
        Check(!cert({}),"Empty module accepted");
        Check(!cert(std::span(code).first(code.size()-4)),"Truncated module accepted");
        auto appended=code;appended.push_back(0);Check(!cert(appended),"Appended module accepted");
        Check(!NativePackedColorActionVariant8ModuleCertified(source,!pixel,variant,code),"Wrong stage accepted");
        Check(!NativePackedColorActionVariant8ModuleCertified(pixel?kPackedColorActionVertex:kPackedColorActionPixel,pixel,variant,code),"Wrong source accepted");
        for(uint32_t v=0;v<=25;++v)if(v!=uint32_t(variant))
            Check(!NativePackedColorActionVariant8ModuleCertified(source,pixel,NativeShaderVariant(v),code),"Wrong variant accepted");
        for(size_t offset:{size_t(0),size_t(8),size_t(20),code.size()/2,code.size()-4})
            for(uint8_t bit=0;bit<8;++bit) {auto corrupt=code;corrupt[offset]^=uint8_t(1u<<bit);Check(!cert(corrupt),"Changed module accepted");}
    }
    NativeFsiStateSpecialization state;state.words=kNativePackedColorVariant8State;
    auto tuple=[&](const auto& raster,const auto& specialization) {return NativePackedColorActionVariant8Tuple(
        kPackedColorActionVertex,kPackedColorActionPixel,raster,specialization,20,-1);};
    Check(tuple(kPackedColorActionRaster,state),"Actual tuple rejected");
    Check(!NativePackedColorVariant8Tuple(kPackedColorActionVertex,kPackedColorActionPixel,kPackedColorActionRaster,state,20,-1),"Action was admitted by old53");
    Check(!RetainedOpaqueCatalogPair(kPackedColorActionVertex,kPackedColorActionPixel),"Action overlaps legacy branch");
    for(const auto& old:kNativePackedColorVariant8Recipes) {
        Check(NativePackedColorVariant8Tuple(old.vertex,old.pixel,old.raster,state,20,-1),"Old53 changed");
        Check(!NativePackedColorActionVariant8Tuple(old.vertex,old.pixel,old.raster,state,20,-1),"Action gate widened");
    }
    for(unsigned i=0;i<15;++i)for(unsigned bit=0;bit<32;++bit) {
        auto changed=state;changed.words[i]^=1u<<bit;Check(!tuple(kPackedColorActionRaster,changed),"Changed state accepted");
    }
    for(unsigned i=0;i<6;++i)for(unsigned bit=0;bit<32;++bit) {
        auto changed=kPackedColorActionRaster;changed[i]^=1u<<bit;Check(!tuple(changed,state),"Changed raster accepted");
    }
    Check(!NativePackedColorActionVariant8Tuple(kPackedColorActionPixel,kPackedColorActionVertex,kPackedColorActionRaster,state,20,-1),"Reversed pair accepted");
    for(int format:{-1,0,1,3,19,21})Check(!NativePackedColorActionVariant8Tuple(kPackedColorActionVertex,kPackedColorActionPixel,kPackedColorActionRaster,state,format,-1),"Foreign host color accepted");
    for(int format:{0,1,20})Check(!NativePackedColorActionVariant8Tuple(kPackedColorActionVertex,kPackedColorActionPixel,kPackedColorActionRaster,state,20,format),"Host depth accepted");
    int vertex=0,pixel=0,pipeline=0,replacement=0;
    NativePackedColorVariant8PassProof proof;
    proof.vertex_module=proof.certified_vertex_module=&vertex;
    proof.pixel_module=proof.certified_pixel_module=&pixel;
    proof.pipeline=proof.certified_pipeline=&pipeline;
    proof.pixel_variant=NativeShaderVariant::PipelineStateEpilogueV1;
    proof.exact_tuple=proof.cpu_geometry=proof.indexed_triangles=proof.private_framebuffer=
        proof.bounded_scissor=proof.primary_surfaces=proof.single_sample_layer=true;
    const auto raster=Raster();
    Check(!NativePackedColorActionVariant8PassAllowed(false,raster,proof),"Default OFF admitted");
    Check(NativePackedColorActionVariant8PassAllowed(true,raster,proof),"Valid owners rejected");
    for(auto member:{&NativePackedColorVariant8PassProof::vertex_module,&NativePackedColorVariant8PassProof::certified_vertex_module,
        &NativePackedColorVariant8PassProof::pixel_module,&NativePackedColorVariant8PassProof::certified_pixel_module,
        &NativePackedColorVariant8PassProof::pipeline,&NativePackedColorVariant8PassProof::certified_pipeline}) {
        auto changed=proof;changed.*member=nullptr;Check(!NativePackedColorActionVariant8PassAllowed(true,raster,changed),"Missing owner accepted");
        changed=proof;changed.*member=&replacement;Check(!NativePackedColorActionVariant8PassAllowed(true,raster,changed),"Replaced owner accepted");
    }
    // Simulates valid current modules with an uncertified creation-time PSO.
    // This is a CPU predicate control, not an actual Vulkan owner replacement.
    auto stale=proof;stale.certified_pipeline=nullptr;
    Check(!NativePackedColorActionVariant8PassAllowed(true,raster,stale),"Current modules promoted old PSO");
    for(auto member:{&NativePackedColorVariant8PassProof::exact_tuple,&NativePackedColorVariant8PassProof::cpu_geometry,
        &NativePackedColorVariant8PassProof::indexed_triangles,&NativePackedColorVariant8PassProof::private_framebuffer,
        &NativePackedColorVariant8PassProof::bounded_scissor,&NativePackedColorVariant8PassProof::primary_surfaces,
        &NativePackedColorVariant8PassProof::single_sample_layer}) {
        auto changed=proof;changed.*member=false;Check(!NativePackedColorActionVariant8PassAllowed(true,raster,changed),"Missing draw guard accepted");
    }
    for(auto member:{&NativePackedColorVariant8PassProof::active_query,&NativePackedColorVariant8PassProof::discarded,&NativePackedColorVariant8PassProof::rectangle}) {
        auto changed=proof;changed.*member=true;Check(!NativePackedColorActionVariant8PassAllowed(true,raster,changed),"Incompatible draw accepted");
    }
    for(uint32_t v=0;v<=25;++v) {
        auto changed=proof;changed.vertex_variant=NativeShaderVariant(v);
        if(v!=0)Check(!NativePackedColorActionVariant8PassAllowed(true,raster,changed),"Wrong selected VS variant");
        changed=proof;changed.pixel_variant=NativeShaderVariant(v);
        if(v!=8)Check(!NativePackedColorActionVariant8PassAllowed(true,raster,changed),"Wrong selected PS variant");
    }
    for(auto member:{&NativeFsiConstants::pitch,&NativeFsiConstants::depth_base,&NativeFsiConstants::depth_control,
        &NativeFsiConstants::depth_format,&NativeFsiConstants::blend_enabled}) {
        auto changed=raster;changed.*member^=1;Check(!NativePackedColorActionVariant8PassAllowed(true,changed,proof),"Layout/state drift accepted");
    }
    for(unsigned target=0;target<4;++target)for(auto member:{&NativeFsiConstants::ColorTarget::format,&NativeFsiConstants::ColorTarget::mask,&NativeFsiConstants::ColorTarget::blend}) {
        auto changed=raster;changed.color[target].*member^=1;Check(!NativePackedColorActionVariant8PassAllowed(true,changed,proof),"Actual target state drift accepted");
    }
    auto alias=raster;alias.color[0].base=0;Check(!NativePackedColorActionVariant8PassAllowed(true,alias,proof),"Color/depth alias accepted");
    std::cout<<"PASS "<<checks<<" checks: exact actual modules, closed state, actual-owner policy and default OFF\n";
}catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
