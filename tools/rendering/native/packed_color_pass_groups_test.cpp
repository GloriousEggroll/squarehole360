#include "renderer/native/packed_color_pass_groups.h"
#include "renderer/native/fsi_shader_variant.h"
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
using namespace gow3::renderer;
namespace {
uint64_t checks=0;
void Check(bool x,const char* why) {++checks;if(!x)throw std::runtime_error(why);}
std::vector<uint32_t> Read(const std::filesystem::path& p) {
    std::ifstream f(p,std::ios::binary|std::ios::ate);Check(f&&f.tellg()>=20&&f.tellg()%4==0,"Invalid shader input");
    std::vector<uint32_t> w(size_t(f.tellg())/4);f.seekg(0);Check(bool(f.read(reinterpret_cast<char*>(w.data()),w.size()*4)),"Truncated shader");return w;
}
void AuditVertex(std::span<const uint32_t> w) {
    std::map<uint32_t,uint32_t> pointers,values;uint32_t stores=0,entries=0;
    for(size_t at=5;at<w.size();) {
        const auto n=w[at]>>16,op=w[at]&65535;Check(n&&n<=w.size()-at,"Invalid instruction");const auto a=w.subspan(at+1,n-1);
        if(op==15) {Check(a[0]==0,"Not vertex entry");++entries;}
        if(op==32)pointers[a[0]]=a[1];
        if((op==59||op==61||op==65||op==124||op==169||op==245)&&a.size()>=2&&pointers.contains(a[0]))values[a[1]]=pointers[a[0]];
        if(op==59)Check(a[2]==1||a[2]==3||a[2]==7||a[2]==9,"Vertex descriptor/global storage outside owned inputs");
        if(op==62) {Check(values.contains(a[0])&&(values.at(a[0])==3||values.at(a[0])==7),"Vertex has external store");++stores;}
        Check(op!=63&&op!=64&&op!=99&&op!=57&&!(op>=227&&op<=242),"Unexpected vertex memory operation/function call");at+=n;
    }
    Check(entries==1&&stores>0,"Vacuous vertex audit");
}
}
int main(int argc,char** argv)try {
    Check(argc==2,"Usage: packed-color-pass-groups-test compiled-shader-directory");
    for(const auto& pair:kRetainedOpaqueCatalogPairs)for(bool pixel:{false,true}) {
        const auto name=pixel?pair.pixel:pair.vertex;
        auto w=Read(std::filesystem::path(argv[1])/(std::string(name)+(pixel?".fsi.spv":".spv")));
        auto bytes=[&]{return std::span(reinterpret_cast<const uint8_t*>(w.data()),w.size()*4);};
        Check(NativePackedColorModuleCertified(name,pixel,bytes()),"Pinned module refused");
        if(pixel)Check(TransformNativeFsiImplicitBarriers(w).size()+6==w.size(),"Coherent ordered epilogue audit failed");
        else AuditVertex(w);
        w[2]^=1;Check(!NativePackedColorModuleCertified(name,pixel,bytes()),"Changed module accepted");w[2]^=1;
        Check(!NativePackedColorModuleCertified("unknown",pixel,bytes()),"Unknown source accepted");
        Check(!NativePackedColorModuleCertified(name,!pixel,bytes()),"Wrong stage accepted");
    }
    // Independent bitmap over the entire physical store. Every color/depth
    // pixel must have one distinct owner; untouched planes must stay outside.
    std::vector<bool> touched(2048*1280);
    for(uint32_t y=0;y<720;++y)for(uint32_t x=0;x<1280;++x)for(bool depth:{false,true}) {
        const auto at=NativePackedColorPassWord(x,y,depth);
        const uint32_t independent=(depth?0:720*1280)+(y/16)*16*1280+(x/80)*1280+(y%16)*80+
            (depth?(x%80<40?x%80+40:x%80-40):x%80);
        Check(at==independent&&at<1440*1280&&!touched.at(at),"Physical plane aliases a different pixel");touched[at]=true;
    }
    NativeFsiConstants raster;raster.pitch=1280;raster.depth_format=1;raster.depth_control=0x700766;raster.color[0]={720,3,15,0x10001};
    NativePackedColorPassProof proof;proof.audited_pair=proof.unmodified=proof.cpu_geometry=proof.private_framebuffer=proof.bounded_scissor=proof.primary_surfaces=true;
    Check(NativePackedColorPassAllowed(raster,proof),"Primary cohort refused");
    for(auto member:{&NativePackedColorPassProof::audited_pair,&NativePackedColorPassProof::unmodified,&NativePackedColorPassProof::cpu_geometry,&NativePackedColorPassProof::private_framebuffer,&NativePackedColorPassProof::bounded_scissor,&NativePackedColorPassProof::primary_surfaces}) {
        auto p=proof;p.*member=false;Check(!NativePackedColorPassAllowed(raster,p),"Missing certificate accepted");
    }
    for(auto member:{&NativePackedColorPassProof::active_query,&NativePackedColorPassProof::discarded,&NativePackedColorPassProof::rectangle}) {
        auto p=proof;p.*member=true;Check(!NativePackedColorPassAllowed(raster,p),"Unaudited operation accepted");
    }
    for(uint32_t bad:{0u,719u,721u,1440u,2047u}) {auto r=raster;r.color[0].base=bad;Check(!NativePackedColorPassAllowed(r,proof),"Color alias accepted");}
    for(uint32_t bad:{0u,4u,5u,7u,15u}) {auto r=raster;r.color[0].format=bad;Check(!NativePackedColorPassAllowed(r,proof),"Unaudited format accepted");}
    auto r=raster;r.color[1].mask=15;Check(!NativePackedColorPassAllowed(r,proof),"MRT accepted");
    r=raster;r.depth_base=720;Check(!NativePackedColorPassAllowed(r,proof),"Depth/color overlap accepted");
    r=raster;r.pitch=640;Check(!NativePackedColorPassAllowed(r,proof),"Pitch alias accepted");
    r=raster;r.depth_control=0x700762;Check(!NativePackedColorPassAllowed(r,proof),"Other depth state accepted");
    std::cout<<"PASS "<<checks<<" checks: actual immutable vertex/fragment modules, full two-plane injectivity and refusals\n";
}catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
