#include "renderer/native/packed_color_blend_variant8_catalog.h"
#include "renderer/native/packed_color_blend_variant8_module.h"
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
using namespace gow3::renderer;
namespace {
uint64_t checks=0;
void Check(bool condition,const char* message) {++checks;if(!condition)throw std::runtime_error(message);}
NativeFsiConstants Raster(const NativePackedColorBlendVariant8Recipe& row) {
    NativeFsiConstants r;r.pitch=1280;r.depth_format=1;r.depth_control=row.state[0];
    r.blend_enabled=row.state[2];
    r.color[0]={720,3,15,row.state[5]};
    for(unsigned i=1;i<4;++i)r.color[i].blend=0x10001;
    return r;
}
}
int main(int argc,char** argv)try {
    Check(argc==2,"Usage: test module-inputs.tsv");
    Check(kNativePackedColorBlendVariant8Recipes.size()==19,"Closed tuple count changed");
    Check(kNativePackedColorBlendVariant8ModulePins.size()==28,"Closed module count changed");
    size_t index=0,alpha=0,additive=0;
    for(const auto& row:kNativePackedColorBlendVariant8Recipes) {
        NativeFsiStateSpecialization state;state.words=row.state;
        const auto got=NativePackedColorBlendVariant8RecipeIndex(row.vertex,row.pixel,row.raster,state,20,-1);
        Check(got&&*got==index,"Exact tuple/index refused");
        Check(!NativePackedColorVariant8Tuple(row.vertex,row.pixel,row.raster,state,20,-1),"New state overlaps old53");
        if(index) {
            const auto& prev=kNativePackedColorBlendVariant8Recipes[index-1];
            Check(std::tie(prev.vertex,prev.pixel,prev.raster,prev.state)<std::tie(row.vertex,row.pixel,row.raster,row.state),"Unsorted or duplicated tuple");
        }
        for(unsigned i=0;i<row.raster.size();++i) {
            auto r=row.raster;r[i]^=0x80000000u;
            Check(!NativePackedColorBlendVariant8Tuple(row.vertex,row.pixel,r,state,20,-1),"Changed raster accepted");
        }
        for(unsigned i=0;i<state.words.size();++i) {
            auto s=state;s.words[i]^=1;
            Check(!NativePackedColorBlendVariant8Tuple(row.vertex,row.pixel,row.raster,s,20,-1),"Changed specialization accepted");
        }
        Check(!NativePackedColorBlendVariant8Tuple("unknown",row.pixel,row.raster,state,20,-1),"Unknown VS accepted");
        Check(!NativePackedColorBlendVariant8Tuple(row.vertex,"unknown",row.raster,state,20,-1),"Unknown PS accepted");
        Check(!NativePackedColorBlendVariant8Tuple(row.vertex,row.pixel,row.raster,state,-1,-1),"Wrong color accepted");
        Check(!NativePackedColorBlendVariant8Tuple(row.vertex,row.pixel,row.raster,state,20,0),"Host depth accepted");
        int vs=0,ps=0,pso=0,replacement=0;
        NativePackedColorVariant8PassProof p;
        p.vertex_module=p.certified_vertex_module=&vs;p.pixel_module=p.certified_pixel_module=&ps;
        p.pipeline=p.certified_pipeline=&pso;p.pixel_variant=NativeShaderVariant::PipelineStateEpilogueV1;
        p.exact_tuple=p.cpu_geometry=p.indexed_triangles=p.private_framebuffer=p.bounded_scissor=p.primary_surfaces=p.single_sample_layer=true;
        const auto r=Raster(row);
        Check(NativeFsiStateSpecialization::From(r).words==row.state,"Test actual raster state mismatch");
        Check(NativePackedColorBlendVariant8PassAllowed(r,p,index),"Actual owner proof refused");
        for(auto member:{&NativePackedColorVariant8PassProof::vertex_module,&NativePackedColorVariant8PassProof::certified_vertex_module,
            &NativePackedColorVariant8PassProof::pixel_module,&NativePackedColorVariant8PassProof::certified_pixel_module,
            &NativePackedColorVariant8PassProof::pipeline,&NativePackedColorVariant8PassProof::certified_pipeline}) {
            auto changed=p;changed.*member=nullptr;
            Check(!NativePackedColorBlendVariant8PassAllowed(r,changed,index),"Missing current/creation owner accepted");
            changed=p;changed.*member=&replacement;
            Check(!NativePackedColorBlendVariant8PassAllowed(r,changed,index),"Foreign current/creation owner accepted");
        }
        for(auto member:{&NativePackedColorVariant8PassProof::exact_tuple,&NativePackedColorVariant8PassProof::cpu_geometry,
            &NativePackedColorVariant8PassProof::indexed_triangles,&NativePackedColorVariant8PassProof::private_framebuffer,
            &NativePackedColorVariant8PassProof::bounded_scissor,&NativePackedColorVariant8PassProof::primary_surfaces,
            &NativePackedColorVariant8PassProof::single_sample_layer}) {
            auto changed=p;changed.*member=false;
            Check(!NativePackedColorBlendVariant8PassAllowed(r,changed,index),"Missing draw guard accepted");
        }
        for(auto member:{&NativePackedColorVariant8PassProof::active_query,&NativePackedColorVariant8PassProof::discarded,&NativePackedColorVariant8PassProof::rectangle}) {
            auto changed=p;changed.*member=true;
            Check(!NativePackedColorBlendVariant8PassAllowed(r,changed,index),"Forbidden draw accepted");
        }
        auto changed=p;changed.pixel_variant=NativeShaderVariant::Unmodified;
        Check(!NativePackedColorBlendVariant8PassAllowed(r,changed,index),"Wrong PS variant accepted");
        changed=p;changed.vertex_variant=NativeShaderVariant::ForwardOnlyGuestControlFlowV1;
        Check(!NativePackedColorBlendVariant8PassAllowed(r,changed,index),"Wrong VS variant accepted");
        for(unsigned kind=0;kind<4;++kind) {
            auto bad=r;
            if(kind==0)bad.pitch=640;
            if(kind==1)bad.depth_base=720;
            if(kind==2)bad.color[0].base=719;
            if(kind==3)bad.depth_control^=4;
            Check(!NativePackedColorBlendVariant8PassAllowed(bad,p,index),"Alias/layout/depth-write mismatch accepted");
        }
        Check(!NativePackedColorBlendVariant8PassAllowed(r,p,19),"Out-of-range recipe accepted");
        alpha+=row.state[5]==0x01000706u;additive+=row.state[5]==0x01000101u;
        ++index;
    }
    Check(alpha==10&&additive==9,"Exact state subset changed");
    for(const auto& row:kNativePackedColorVariant8Recipes) {
        NativeFsiStateSpecialization s;s.words=kNativePackedColorVariant8State;
        Check(NativePackedColorVariant8Tuple(row.vertex,row.pixel,row.raster,s,20,-1),"Old53 policy changed");
        Check(!NativePackedColorBlendVariant8Tuple(row.vertex,row.pixel,row.raster,s,20,-1),"Old53 widened into new policy");
    }
    for(auto excluded:{"21a996ffdab2d781b6e325fb2f3bc7c03e8d66a04cb6f2844e29746016fd65c2",
        "43b0ac3eb0cc4047af85f3f4376379266b3bd21d94705ca41cb8bc645eb8f0c7",
        "8a78313d6bf03fc137b67460ceb0770e333357eebe60f9d50a552419c60f4689",
        "8c8b01a03e1747d44f43f40b7ec349df53ff965c4f92006bdc5aa3119f3d7eba",
        "eeead879574607f32bb893c90284734eb4201c511c6b410e5818f708e885369d",
        "f832133d003a0f1ab18bb6d4164a51cb084e04d52ba4efa752ede1525a320b26"})
        for(const auto& row:kNativePackedColorBlendVariant8Recipes) {
            NativeFsiStateSpecialization s;s.words=row.state;
            Check(!NativePackedColorBlendVariant8Tuple(row.vertex,excluded,row.raster,s,20,-1),"Unproved CFG source admitted");
        }
    const auto& a=kNativePackedColorBlendVariant8Recipes.front();
    const auto& b=kNativePackedColorBlendVariant8Recipes.back();
    NativeFsiStateSpecialization state;state.words=a.state;
    Check(!NativePackedColorBlendVariant8Tuple(a.vertex,b.pixel,a.raster,state,20,-1),"Cartesian cross-pair accepted");
    std::ifstream input(argv[1]);Check(bool(input),"Missing actual module input list");
    std::string line;size_t module_count=0;
    while(std::getline(input,line)) {
        std::istringstream fields(line);std::string source,stage,path;
        Check(bool(std::getline(fields,source,'\t'))&&bool(std::getline(fields,stage,'\t'))&&bool(std::getline(fields,path)),"Malformed module row");
        const bool pixel=stage=="pixel";
        const auto variant=pixel?NativeShaderVariant::PipelineStateEpilogueV1:NativeShaderVariant::Unmodified;
        std::ifstream file(path,std::ios::binary);Check(bool(file),"Missing original module");
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)),{});
        if(pixel) {
            Check(bytes.size()>=400&&std::string(bytes.begin(),bytes.begin()+8)=="G3FSIC01",
                "Expected authenticated state-module container");
            const auto word=[&](size_t at){return uint32_t(bytes.at(at))|(uint32_t(bytes.at(at+1))<<8)|
                (uint32_t(bytes.at(at+2))<<16)|(uint32_t(bytes.at(at+3))<<24);};
            Check(word(8)==1794&&uint64_t(word(12))*4==bytes.size()-400&&
                std::string(bytes.begin()+16,bytes.begin()+80)==source,"State-module header mismatch");
            std::vector<uint8_t> authenticated(bytes.begin(),bytes.begin()+336);
            authenticated.insert(authenticated.end(),bytes.begin()+400,bytes.end());
            Check(ContentHash(authenticated)==std::string(bytes.begin()+336,bytes.begin()+400),
                "State-module container checksum mismatch");
            bytes.erase(bytes.begin(),bytes.begin()+400);
        }
        Check(NativePackedColorBlendVariant8ModuleCertified(source,pixel,variant,bytes),"Actual original module refused");
        auto foreign=bytes;foreign.at(8)^=1;
        Check(!NativePackedColorBlendVariant8ModuleCertified(source,pixel,variant,foreign),"Generator-word foreign module accepted");
        Check(!NativePackedColorBlendVariant8ModuleCertified(source,!pixel,variant,bytes),"Foreign stage accepted");
        Check(!NativePackedColorBlendVariant8ModuleCertified(source,pixel,NativeShaderVariant::ForwardOnlyGuestControlFlowV1,bytes),"Foreign variant accepted");
        Check(!NativePackedColorBlendVariant8ModuleCertified("unknown",pixel,variant,bytes),"Foreign source accepted");
        bytes.pop_back();Check(!NativePackedColorBlendVariant8ModuleCertified(source,pixel,variant,bytes),"Truncated module accepted");
        ++module_count;
    }
    Check(module_count==28,"Incomplete actual module coverage");
    std::cout<<"PASS "<<checks<<" checks: exact19, preserved53, ownership, state and28 original byte certificates\n";
}catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
