#include "renderer/native/atlas_depth_clear.h"
#include "atlas_depth_clear_oracle.h"
#include <array>
#include <bit>
#include <cfenv>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace gow3::renderer;
using namespace gow3::renderer::validation;
namespace {uint64_t checks=0;void Require(bool v,const char* why){++checks;if(!v)throw std::runtime_error(why);}}
int main()try{
    Require(std::fesetround(FE_TONEAREST)==0,"Cannot select IEEE ties-to-even");
    uint64_t changed=0;
    for(uint32_t d=0;d<=0xffffff;++d){
        volatile float decoded=float(d+(d>>23))/16777216.f;
        volatile float product=decoded*16777215.f;
        const uint32_t floating=uint32_t(std::nearbyint(product));
        const auto integer=AtlasPackedRoundTripOracle(d);
        Require(floating==integer,"Packed integer oracle disagrees with real separately rounded binary32");
        Require(AtlasHostQuantizeOracle(std::bit_cast<uint32_t>(float(decoded)))==integer,"General host bit oracle disagrees");
        changed+=d!=integer;
    }
    Require(changed==0x200000,"Outside roundtrip normalization coverage wrong");
    Require(AtlasPackedRoundTripOracle(0x400001)==0x400000,"Nonidentity outside witness missing");
    for(uint32_t st=0;st<256;++st){volatile float value=float(st)/255.f;volatile float scaled=value*255.f;
        Require(uint32_t(std::nearbyint(scaled))==st,"Receipt stencil roundtrip is not exact");}
    // Exact actual rect/ABI and independent one-field admission refusals.
    const std::array<int32_t,4> rect{41,17,179,91};
    Require(NativeAtlasDepthClearParameters(1120,688,1120,1440,0,true,rect)==std::array<uint32_t,8>{1120,688,1120,1440,41,17,179,91},"Push ABI mismatch");
    for(unsigned field=0;field<6;++field){auto v=std::array<uint32_t,6>{1120,688,1120,1440,0,1};v[field]^=1;
        Require(!NativeAtlasDepthClearLayoutAllowed(v[0],v[1],v[2],v[3],v[4],bool(v[5]),rect),"One-field unsupported layout admitted");}
    for(auto bad:std::array<std::array<int32_t,4>,6>{{{-1,0,3,3},{0,-1,3,3},{0,0,0,3},{0,0,3,0},{0,0,1121,3},{0,0,3,689}}})
        Require(!NativeAtlasDepthClearLayoutAllowed(1120,688,1120,1440,0,true,bad),"Invalid rect admitted");
    std::vector<bool> words(2048*1280);uint64_t pixels=0;
    for(uint32_t y=0;y<688;++y)for(uint32_t x=0;x<1120;++x){auto at=AtlasDepthOffsetOracle(x,y)/4;
        Require(at<words.size()&&!words[at],"Independent tile addresses wrap or collide");words[at]=true;++pixels;}
    Require(pixels==770560&&AtlasDepthOffsetOracle(1119,687)<2048*5120,"Surface coverage wrong");
    for(uint32_t bits:std::array<uint32_t,14>{0,0x80000000,1,0x00800000,0x3effffff,0x3f000000,0x3f000001,0x3f7fffff,0x3f800000,0x3fa00000,0xbf000000,0x3f200000,0x3e800000,0x3f400000}){
        volatile float d=std::bit_cast<float>(bits);volatile float p=std::fmax(0.f,std::fmin(1.f,d))*16777215.f;
        Require(AtlasHostQuantizeOracle(bits)==uint32_t(std::nearbyint(p)),"Host bit quantizer boundary mismatch");}
    std::cout<<"{\"passed\":true,\"checks\":"<<checks<<",\"all_24bit_codes\":16777216,\"outside_changed_codes\":"<<changed<<",\"atlas_pixels\":"<<pixels<<"}\n";
    return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<"\n";return 1;}
