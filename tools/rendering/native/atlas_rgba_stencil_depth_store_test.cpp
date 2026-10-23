#include "renderer/native/atlas_rgba_stencil_depth_store.h"
#include "atlas_depth_clear_oracle.h"
#include <array>
#include <cfenv>
#include <cmath>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
using namespace gow3::renderer;
using namespace gow3::renderer::validation;
namespace {uint64_t checks=0;void Require(bool v,const char* why){++checks;if(!v)throw std::runtime_error(why);}
std::string Read(const char* p){std::ifstream f(p,std::ios::binary);Require(bool(f),"Cannot open shader source");return std::string(std::istreambuf_iterator<char>(f),std::istreambuf_iterator<char>());}
void ReplaceOnce(std::string& s,const std::string& a,const std::string& b){auto p=s.find(a);Require(p!=s.npos&&s.find(a,p+a.size())==s.npos,"Shader replacement shape is not unique");s.replace(p,a.size(),b);}}
int main(int argc,char** argv)try{
    Require(argc==3,"Usage: test canonical.comp rgba_variant.comp");
    auto original=Read(argv[1]),variant=Read(argv[2]);
    ReplaceOnce(variant,"layout(set=0,binding=2) uniform texture2D stencil_image;","layout(set=0,binding=2) uniform utexture2D stencil_image;");
    ReplaceOnce(variant,"uint stencil=(pc.format&0x80000000u)!=0?uint(roundEven(clamp(texelFetch(stencil_image,ivec2(p)+pc.stencil_offset,0).a,0.0,1.0)*255.0)):0;",
        "uint stencil=(pc.format&0x80000000u)!=0?texelFetch(stencil_image,ivec2(p)+pc.stencil_offset,0).x:0;");
    Require(variant==original,"RGBA stencil variant changed unrelated canonical shader words");
    Require(std::fesetround(FE_TONEAREST)==0,"Cannot select ties-to-even");
    for(unsigned stencil=0;stencil<256;++stencil){volatile float alpha=float(stencil)/255.f;volatile float scaled=alpha*255.f;
        Require(uint32_t(std::nearbyint(scaled))==stencil,"Original alpha->S8 reconstruction failed");}
    // Preserve BOTH original depth rounding operations; this independent check
    // deliberately includes first-round nonidentity codes.
    uint64_t normalized=0;
    for(uint32_t d=0;d<=0xffffff;d+=17){volatile float decoded=float(d+(d>>23))/16777216.f;
        volatile float first=decoded*16777215.f;const uint32_t q1=uint32_t(std::nearbyint(first));
        volatile float resolved=float(q1+(q1>>23))/16777216.f;volatile float second=resolved*16777215.f;
        const uint32_t q2=uint32_t(std::nearbyint(second));
        Require(q1==AtlasPackedRoundTripOracle(d)&&q2==AtlasPackedRoundTripOracle(q1),"Canonical two-round depth oracle mismatch");normalized+=q1!=d;}
    Require(normalized>0,"First-depth-round witness was vacuous");
    const std::array<int32_t,4> rect{13,9,109,80};
    Require(NativeAtlasRgbaStencilStoreRegionAllowed(1120,688,22,1120,688,rect,28,8),"Valid nonzero source offsets refused");
    for(auto offsets:std::array<std::array<int32_t,2>,5>{{{-14,0},{0,-10},{1012,0},{0,609},{std::numeric_limits<int32_t>::max(),std::numeric_limits<int32_t>::min()}}})
        Require(!NativeAtlasRgbaStencilStoreRegionAllowed(1120,688,22,1120,688,rect,offsets[0],offsets[1]),"Invalid source offsets admitted");
    for(unsigned field=0;field<5;++field){auto v=std::array<uint32_t,5>{1120,688,22,1120,688};v[field]^=1;
        Require(!NativeAtlasRgbaStencilStoreRegionAllowed(v[0],v[1],v[2],v[3],v[4],rect,28,8),"Unsupported shape admitted");}
    std::cout<<"{\"passed\":true,\"checks\":"<<checks<<",\"stencil_codes\":256,\"two_depth_rounds_preserved\":true,\"outside_normalized_codes\":"<<normalized<<"}\n";return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<"\n";return 1;}
