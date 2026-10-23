#include "renderer/native/stencil_material_cutoff.h"
#include <array>
#include <bit>
#include <iostream>
#include <stdexcept>
#include <string>
using namespace gow3::renderer;
namespace {
uint64_t checks=0;
void Check(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
NativeFsiConstants Raster(){NativeFsiConstants r{};r.pitch=1280;r.depth_format=1;r.depth_control=0x00700571;r.stencil_front=r.stencil_back=0x00ffff00;r.color[0]={720,0,1,0x00010001};return r;}
NativeStencilMaterialProof Proof(){NativeStencilMaterialProof p{};p.original_modules=p.unmodified_variants=p.owned_up_cube=true;p.primary_surfaces=p.private_framebuffer=p.bounded_scissor=p.packed_current=p.single_sample_single_view=true;p.width=1280;p.height=720;p.raw_color_mask=1;p.index_count=36;p.raster={3,2,1,4,0,0};return p;}
// A separately written bitfield oracle avoids floating point normalization.
uint32_t OracleStore(uint32_t previous){
  const auto stencil=previous%256u;const auto depth=previous/256u;
  const bool stencil_pass=0u!=stencil;
  const bool depth_enabled=false,depth_write=false;const unsigned selected_op=stencil_pass?0:0;
  const auto result=selected_op==0?stencil:0u;
  return (depth_enabled&&depth_write?0u:depth)*256u+result;
}
[[gnu::noinline]] uint32_t Store(uint32_t previous){return NativeStencilMaterialIdentityStore(previous);}
void Policy(){
 const auto r=Raster();const auto p=Proof();Check(NativeStencilMaterialAllowed(r,p),"baseline admission");
 for(auto member:{&NativeStencilMaterialProof::original_modules,&NativeStencilMaterialProof::unmodified_variants,&NativeStencilMaterialProof::owned_up_cube,&NativeStencilMaterialProof::primary_surfaces,&NativeStencilMaterialProof::private_framebuffer,&NativeStencilMaterialProof::bounded_scissor,&NativeStencilMaterialProof::packed_current,&NativeStencilMaterialProof::single_sample_single_view}){auto q=p;q.*member=false;Check(!NativeStencilMaterialAllowed(r,q),"missing ownership or state proof");}
 for(auto member:{&NativeStencilMaterialProof::active_query,&NativeStencilMaterialProof::discarded,&NativeStencilMaterialProof::rectangle}){auto q=p;q.*member=true;Check(!NativeStencilMaterialAllowed(r,q),"forbidden control");}
 for(auto member:{&NativeStencilMaterialProof::width,&NativeStencilMaterialProof::height,&NativeStencilMaterialProof::raw_color_mask,&NativeStencilMaterialProof::index_count})for(uint32_t value:{0u,1u,35u,37u,1279u,1281u,719u,721u,~0u}){if(p.*member==value)continue;auto q=p;q.*member=value;Check(!NativeStencilMaterialAllowed(r,q),"shape/bounds refusal");}
 for(unsigned field=0;field<p.raster.size();++field){auto q=p;q.raster[field]^=1;Check(!NativeStencilMaterialAllowed(r,q),"raster key refusal");}
 for(auto member:{&NativeFsiConstants::pitch,&NativeFsiConstants::depth_base,&NativeFsiConstants::depth_format,&NativeFsiConstants::depth_control,&NativeFsiConstants::stencil_front,&NativeFsiConstants::stencil_back})for(unsigned bit=0;bit<32;++bit){auto q=r;q.*member^=1u<<bit;Check(!NativeStencilMaterialAllowed(q,p),"exact depth/stencil field refusal");}
 for(unsigned bit=0;bit<32;++bit){auto q=r;q.color[0].base^=1u<<bit;Check(!NativeStencilMaterialAllowed(q,p),"color base refusal");q=r;q.color[0].format^=1u<<bit;Check(!NativeStencilMaterialAllowed(q,p),"color format refusal");q=r;q.color[0].mask^=1u<<bit;Check(!NativeStencilMaterialAllowed(q,p),"color mask refusal");q=r;q.color[0].blend^=1u<<bit;Check(!NativeStencilMaterialAllowed(q,p),"blend refusal");}
 for(unsigned target=1;target<4;++target)for(unsigned bit=0;bit<32;++bit){auto q=r;q.color[target].mask=1u<<bit;Check(!NativeStencilMaterialAllowed(q,p),"other target write refused");}
 auto blend=r;blend.blend_enabled=1;blend.color[0].blend=0x01000008;Check(NativeStencilMaterialAllowed(blend,p),"second exact blend admitted");
 for(unsigned bit=1;bit<32;++bit){auto q=blend;q.blend_enabled|=1u<<bit;Check(!NativeStencilMaterialAllowed(q,p),"unreviewed target blending");}
 // Bias and color constants do not participate in the integer stencil proof;
 // unchanged survivor material/epilogue still consumes original values.
 auto irrelevant=r;irrelevant.front_slope_bias=std::bit_cast<float>(0x7fc01234u);irrelevant.back_constant_bias=std::bit_cast<float>(0xff800000u);irrelevant.blend_constant={1,-1,0,0.5f};Check(NativeStencilMaterialAllowed(irrelevant,p),"non-depth arithmetic not silently constrained");
}
void WordsAndAddress(){
 // Every possible high24 value is tested with zero and nonzero low8 values.
 // All256 low8 values are separately crossed with depth boundary/NaN encodings.
 for(uint32_t depth=0;depth<0x1000000u;++depth)for(uint32_t stencil:{0u,255u,(depth^0x5au)&255u}){const uint32_t word=(depth<<8)|stencil;Check(Store(word)==OracleStore(word),"all-high24 identity");Check(NativeStencilMaterialPass(word)==(stencil!=0),"all-high24 predicate");}
 for(uint32_t depth:{0u,1u,0x1fffffu,0x200000u,0x7fffffu,0x800000u,0xff0000u,0xffffffu})for(uint32_t stencil=0;stencil<256;++stencil){const auto word=(depth<<8)|stencil;Check(Store(word)==word,"all-low8 identity");Check(NativeStencilMaterialPass(word)==(stencil!=0),"all-low8 predicate");}
 std::vector<bool> seen(720u*1280u,false);
 for(uint32_t y=0;y<720;++y)for(uint32_t x=0;x<1280;++x){const uint32_t actual=((y/16)*16+x/80)*1280+(y%16)*80+(x%80+40)%80;const uint32_t column=x%80;const uint32_t oracle=((y/16)*((1280+79)/80)+x/80)*1280+(y%16)*80+(column<40?column+40:column-40);Check(actual==oracle,"prefix exact canonical address");Check(actual<seen.size()&&!seen[actual],"injective bounded primary depth footprint");seen[actual]=true;const uint32_t color=(720+(y/16)*16+x/80)*1280+(y%16)*80+x%80;Check(color>=720*1280&&color<1440*1280,"disjoint packed color footprint");}
}
template<class F> void Throws(F&& function,const char* message){bool refused=false;try{function();}catch(const std::logic_error&){refused=true;}Check(refused,message);}
void Ownership(){
 using Owner=NativeStencilMaterialCounterOwnership;using Phase=Owner::Phase;Owner owner;
 Check(owner.phase()==Phase::Free&&!owner.pending(),"new owner is free");
 Throws([&]{owner.Prepare(0);},"zero generation refused");Throws([&]{owner.Begin();},"begin without preparation refused");Throws([&]{owner.End();},"end without begin refused");Throws([&]{owner.Accept(1);},"accept without commands refused");Throws([&]{owner.Retire(1,1);},"read before acceptance refused");
 owner.Prepare(1);Check(owner.phase()==Phase::Preparing&&!owner.pending(),"prepared generation");
 Throws([&]{owner.Prepare(2);},"preparing owner cannot be reused");Throws([&]{owner.End();},"end before clear refused");Throws([&]{owner.Accept(1);},"unrecorded acceptance refused");Throws([&]{owner.ValidateRetirement(1,1);},"unaccepted read validation refused");
 owner.Begin();Check(owner.phase()==Phase::Begun,"begin transition");Throws([&]{owner.Begin();},"double clear refused");Throws([&]{owner.Prepare(2);},"begun owner cannot be reused");owner.End();Check(owner.phase()==Phase::Recorded&&!owner.pending(),"recorded transition");
 Throws([&]{owner.End();},"double end refused");Throws([&]{owner.Accept(2);},"foreign generation acceptance refused");Throws([&]{owner.Prepare(2);},"unaccepted recorded work cannot be reused");owner.Accept(1);Check(owner.pending()&&owner.phase()==Phase::Accepted,"accepted generation pending");
 Throws([&]{owner.Accept(1);},"double acceptance refused");Throws([&]{owner.Prepare(2);},"accepted work cannot be reused");Throws([&]{owner.ValidateRetirement(2,4);},"foreign generation read refused");Throws([&]{owner.ValidateRetirement(1,0);},"zero submission refused");Throws([&]{owner.Retire(2,4);},"wrong owner retirement refused");
 owner.ValidateRetirement(1,4);Check(owner.pending(),"validation does not free ownership");owner.Retire(1,4);Check(!owner.pending()&&owner.phase()==Phase::Free,"completed generation freed");Throws([&]{owner.Retire(1,4);},"double retirement refused");Throws([&]{owner.Prepare(1);},"stale generation refused");
 owner.Prepare(2);owner.Begin();owner.End();owner.Accept(2);Throws([&]{owner.ValidateRetirement(2,3);},"older submission refused");Throws([&]{owner.Retire(2,4);},"reused submission refused");Check(owner.pending(),"failed retirement retains owner");owner.Retire(2,5);
 owner.Prepare(~uint64_t(0));owner.Begin();owner.End();owner.Accept(~uint64_t(0));owner.Retire(~uint64_t(0),~uint64_t(0));Throws([&]{owner.Prepare(1);},"generation wrap refuses reuse");Throws([&]{owner.Prepare(~uint64_t(0));},"generation maximum cannot be reused");
 // This CPU class validates generation/phase/monotonic IDs. Actual fence signal
 // and token-to-submission binding remain Scene RetireGpuWork obligations.
}
void Counts(){
 static_assert(sizeof(NativeStencilMaterialCounts)==16);
 for(const auto c:std::array<NativeStencilMaterialCounts,5>{{{0,0,0,0},{1,1,0,0},{1,0,1,0},{kStencilMaterialCounterLimit,kStencilMaterialCounterLimit,0,0},{kStencilMaterialCounterLimit,kStencilMaterialCounterLimit/2,kStencilMaterialCounterLimit/2,0}}})Check(c.Valid(),"valid exact bounded counter sum");
 for(const auto c:std::array<NativeStencilMaterialCounts,6>{{{1,0,0,0},{0,1,0,0},{kStencilMaterialCounterLimit+1,kStencilMaterialCounterLimit+1,0,0},{0,0xffffffffu,1,0},{0xffffffffu,0xffffffffu,0,0},{1,1,1,0}}})Check(!c.Valid(),"invalid/wrapped/over-budget counters refused");
 for(unsigned bit=0;bit<32;++bit){NativeStencilMaterialCounts c{1,1,0,1u<<bit};Check(!c.Valid(),"every overflow flag bit refused");}
}
void Sources(){
 const std::string original="prefix\n"+std::string(kStencilMaterialEntry)+"\nbody\nbool nativeDepthStencil(uint2 p,float depth,bool front_face) {\n"+std::string(kStencilMaterialStore)+"\n}\nsuffix\n";
 for(auto mode:{NativeStencilMaterialMode::Observe,NativeStencilMaterialMode::Cutoff}){
  const auto derived=TryNativeStencilMaterialSource(original,mode);Check(bool(derived),"source transform");const auto restored=RestoreNativeStencilMaterialSource(*derived,mode);Check(restored&&*restored==original,"entire source reversible");
  Check(!TryNativeStencilMaterialSource(*derived,mode),"double transform refused");
  Check(!RestoreNativeStencilMaterialSource(*derived,mode==NativeStencilMaterialMode::Observe?NativeStencilMaterialMode::Cutoff:NativeStencilMaterialMode::Observe),"other mode restoration refused");
  for(auto needle:{kStencilMaterialEntry,kStencilMaterialStore,std::string_view("bool nativeDepthStencil(uint2 p,float depth,bool front_face) {")}){auto missing=original;missing.erase(missing.find(needle),needle.size());Check(!TryNativeStencilMaterialSource(missing,mode),"missing exact marker refused");Check(!TryNativeStencilMaterialSource(original+std::string(needle),mode),"duplicate exact marker refused");}
  for(auto needle:{kStencilMaterialHelper,kStencilMaterialStoreRemoved}){auto missing=*derived;missing.erase(missing.find(needle),needle.size());Check(!RestoreNativeStencilMaterialSource(missing,mode),"missing derived marker refused");Check(!RestoreNativeStencilMaterialSource(*derived+std::string(needle),mode),"duplicate derived marker refused");}
  Check(!TryNativeStencilMaterialSource(original+"gow3StencilMaterialForeign",mode),"foreign prefix refused");
 }
 for(auto mode:{NativeStencilMaterialMode::Disabled,static_cast<NativeStencilMaterialMode>(99)}){Check(!TryNativeStencilMaterialSource(original,mode),"unknown mode refused");Check(!RestoreNativeStencilMaterialSource(original,mode),"unknown restore mode refused");}
 Check(!NativeStencilMaterialCanonicalCertified({},false),"empty pixel certificate");Check(!NativeStencilMaterialCanonicalCertified({},true),"empty vertex certificate");Check(!NativeStencilMaterialDerivedCertified({}),"empty derived certificate");
 const std::array<uint32_t,5> malformed{0x07230203,0x00010500,0,1,0};Check(!NativeStencilMaterialDerivedCertified(malformed),"header-only derived refused");
}
}
int main()try{Policy();WordsAndAddress();Ownership();Counts();Sources();std::cout<<"PASS: "<<checks<<" stencil-material policy, all-high24/all-low8 identity, full-frame injective/disjoint addresses, ownership/counts and reversible-source checks; no GPU\n";}catch(const std::exception& error){std::cerr<<"FAIL after "<<checks<<": "<<error.what()<<'\n';return 1;}
