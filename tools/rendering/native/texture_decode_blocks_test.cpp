// Differential test and optional microbenchmark for the lossless block decoder.
// The build script links an independently named, preserved pre-change TU.
#include "texture_data.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace gow3::renderer {
TextureData DecodeTextureOriginal(const std::array<uint32_t, 6>&,
                                 std::span<const uint8_t>, std::span<const uint8_t>);
TextureLayout DescribeTextureOriginal(const std::array<uint32_t, 6>&);
uint64_t TextureBlockOffsetOriginal(const TextureLayout&, const TextureLevel&,
                                    uint32_t, uint32_t, uint32_t);
}
namespace {
using namespace gow3::renderer;
using Fetch = std::array<uint32_t, 6>;
using Decoder = TextureData (*)(const Fetch&, std::span<const uint8_t>, std::span<const uint8_t>);
constexpr std::array<unsigned, 31> formats{2,8,9,6,7,14,50,54,10,18,51,19,20,52,53,22,23,24,27,30,25,28,31,26,29,32,36,37,38,49,59};
uint64_t checks=0, valid_cases=0, invalid_cases=0, captured_cases=0, captured_skipped=0;
uint64_t halfword_offsets_checked=0, narrow_boundary_checks=0, narrow_literal_checks=0;
volatile uint64_t sink=0;
void Need(bool b,const std::string& text) { if(!b)throw std::runtime_error(text); }
bool Equal(const TextureLevel& a,const TextureLevel& b) {
  return a.mip==b.mip&&a.layer==b.layer&&a.width==b.width&&a.height==b.height&&a.depth==b.depth&&
    a.pitch_blocks==b.pitch_blocks&&a.slice_rows==b.slice_rows&&a.x==b.x&&a.y==b.y&&a.z==b.z&&a.offset==b.offset&&a.mips==b.mips;
}
bool Equal(const TextureLayout& a,const TextureLayout& b) {
  if(a.fetch!=b.fetch||a.width!=b.width||a.height!=b.height||a.depth!=b.depth||a.layers!=b.layers||
     a.dimension!=b.dimension||a.format!=b.format||a.endian!=b.endian||a.min_mip!=b.min_mip||a.max_mip!=b.max_mip||
     a.base_address!=b.base_address||a.mip_address!=b.mip_address||a.tiled!=b.tiled||
     a.block.block_width!=b.block.block_width||a.block.block_height!=b.block.block_height||
     a.block.bytes!=b.block.bytes||a.block.components!=b.block.components||a.levels.size()!=b.levels.size())return false;
  for(size_t i=0;i<a.levels.size();++i)if(!Equal(a.levels[i],b.levels[i]))return false;
  return true;
}
struct Outcome { bool success=false;std::string error;TextureData data; };
Outcome Run(Decoder f,const Fetch& fetch,std::span<const uint8_t> base,std::span<const uint8_t> mips) {
  try {return {true,{},f(fetch,base,mips)};}
  catch(const std::runtime_error& e){return {false,e.what(),{}};}
}
void Compare(const Fetch& fetch,std::span<const uint8_t> base,std::span<const uint8_t> mips,const std::string& label,bool expect_valid) {
  const auto a=Run(DecodeTextureOriginal,fetch,base,mips),b=Run(DecodeTexture,fetch,base,mips);++checks;
  Need(a.success==expect_valid,label+": oracle success expectation differs");
  Need(a.success==b.success&&a.error==b.error,label+": outcome/error differs: "+a.error+" versus "+b.error);
  if(!a.success){++invalid_cases;return;}
  ++valid_cases;
  Need(Equal(a.data.layout,b.data.layout)&&a.data.levels.size()==b.data.levels.size(),label+": layout differs");
  for(size_t i=0;i<a.data.levels.size();++i)
    Need(Equal(a.data.levels[i].layout,b.data.levels[i].layout)&&a.data.levels[i].bytes==b.data.levels[i].bytes,label+": bytes differ at level "+std::to_string(i));
}
struct Shape {const char* name;unsigned dimension,w,h,d,layers;bool tiled,packed;unsigned min_mip,max_mip;bool mip_only;};
Fetch Make(unsigned format,unsigned endian,const Shape& s) {
  Fetch f{};f[0]=2|(((s.w+31)/32)<<22)|(uint32_t(s.tiled)<<31);
  f[1]=format|(endian<<6)|(s.mip_only?0:0x1000);
  if(s.dimension==0)f[2]=s.w-1;
  else if(s.dimension==2)f[2]=(s.w-1)|((s.h-1)<<11)|((s.d-1)<<22);
  else {f[2]=(s.w-1)|((s.h-1)<<13);if(s.dimension==3)f[2]|=5u<<26;
    else if(s.layers>1){f[1]|=1024;f[2]|=(s.layers-1)<<26;}}
  f[3]=0x688<<1;f[4]=(s.min_mip<<2)|(s.max_mip<<6);
  f[5]=(s.dimension<<9)|(unsigned(s.packed)<<11)|(s.max_mip?0x2000:0);
  return f;
}
struct Bytes {std::vector<uint8_t> base,mips;size_t base_size=0,mip_size=0,pad=0;};
Bytes Payloads(const Fetch& f,size_t pad,size_t maximum=32u*1024*1024) {
  const auto t=DescribeTextureOriginal(f);uint64_t base=0,mips=0;
  constexpr unsigned swaps[]{0,1,3,2};
  for(const auto& l:t.levels){
    const auto columns=(l.width+t.block.block_width-1)/t.block.block_width;
    const auto rows=(l.height+t.block.block_height-1)/t.block.block_height;
    for(unsigned z=0;z<l.depth;++z)for(unsigned y=0;y<rows;++y)for(unsigned x=0;x<columns;++x){
      const auto offset=TextureBlockOffsetOriginal(t,l,x,y,z);
      if(t.block.bytes==2) {
        ++halfword_offsets_checked;
        Need((offset&1)==0&&TextureBlockOffset(t,l,x,y,z)==offset,"Two-byte offset alignment/address differs");
      }
      for(unsigned b=0;b<t.block.bytes;++b){
        const auto extent=((offset+b)^swaps[t.endian])+1;
        Need(extent<=maximum,"payload exceeds test bound");
        (l.mips?mips:base)=std::max(l.mips?mips:base,extent);
      }
    }
  }
  Bytes data{{},{},size_t(base),size_t(mips),pad};
  data.base.resize(base+pad);data.mips.resize(mips+pad);
  for(size_t i=0;i<base;++i)data.base[i+pad]=uint8_t((i*37u)^(i>>7)^(i>>13)^0xa7u);
  for(size_t i=0;i<mips;++i)data.mips[i+pad]=uint8_t((i*53u)^(i>>5)^(i>>11)^0x39u);
  return data;
}
std::span<const uint8_t> Base(const Bytes& b){return std::span(b.base).subspan(b.pad,b.base_size);}
std::span<const uint8_t> Mips(const Bytes& b){return std::span(b.mips).subspan(b.pad,b.mip_size);}
void Exercise(const Fetch& f,const std::string& label,bool all_alignments=true) {
  for(size_t pad: {size_t(0),size_t(1),size_t(2),size_t(3),size_t(7)}) {
    if(!all_alignments&&pad!=1)continue;
    auto data=Payloads(f,pad);const auto base=Base(data),mips=Mips(data);
    Compare(f,base,mips,label+" aligned+"+std::to_string(pad),true);
    if(pad!=1)continue;
    const auto block=DescribeTextureOriginal(f).block.bytes;
    // Every byte of the final complete block, plus wholly absent regions.
    for(size_t cut=1;cut<=block;++cut){
      if(cut<=base.size())Compare(f,base.first(base.size()-cut),mips,label+" base minus "+std::to_string(cut),false);
      if(cut<=mips.size())Compare(f,base,mips.first(mips.size()-cut),label+" mips minus "+std::to_string(cut),false);
    }
    if(!base.empty())Compare(f,{},mips,label+" missing base",false);
    if(!mips.empty())Compare(f,base,{},label+" missing mips",false);
  }
}
void NarrowCases() {
  // Independent literal byte-order oracle across two words. Short spans check
  // every prefix, including cross-texel reads when one output texel is requested.
  constexpr std::array<std::array<uint8_t,8>,4> expected{{
    {{0x12,0x34,0x56,0x78,0x9a,0xbc,0xde,0xf0}},
    {{0x34,0x12,0x78,0x56,0xbc,0x9a,0xf0,0xde}},
    {{0x78,0x56,0x34,0x12,0xf0,0xde,0xbc,0x9a}},
    {{0x56,0x78,0x12,0x34,0xde,0xf0,0x9a,0xbc}}}};
  const std::array<uint8_t,9> literal{0xff,0x12,0x34,0x56,0x78,0x9a,0xbc,0xde,0xf0};
  const auto payload=std::span(literal).subspan(1);
  for(unsigned bytes:{1u,2u})for(unsigned endian=0;endian<4;++endian)
    for(unsigned width=1;width<=8/bytes;++width) {
      Shape shape{"narrow-literal",1,width,1,1,1,false,false,0,0,false};
      const auto fetch=Make(bytes==1?2:10,endian,shape);
      for(auto decoder:{DecodeTextureOriginal,DecodeTexture}) {
        const auto output=decoder(fetch,payload,{});++checks;++narrow_literal_checks;
        Need(output.levels.size()==1&&output.levels[0].bytes==
             std::vector<uint8_t>(expected[endian].begin(),expected[endian].begin()+width*bytes),
             "Narrow literal byte permutation differs");
      }
      for(size_t size=0;size<=payload.size();++size) {
        const auto shortened=payload.first(size);
        const bool valid=Run(DecodeTextureOriginal,fetch,shortened,{}).success;
        Compare(fetch,shortened,{},"narrow prefix "+std::to_string(bytes)+":"+
                std::to_string(endian)+":"+std::to_string(width)+":"+std::to_string(size),valid);
        ++narrow_boundary_checks;
      }
    }
  // Cross several 2D/3D macrotiles and packed-mip boundaries using the actual
  // TextureBlockOffset implementation, not a hand-copied address formula.
  constexpr Shape boundaries[]{
    {"narrow-tiled-boundaries",1,67,65,1,1,true,true,0,6,false},
    {"narrow-volume-boundaries",2,65,33,9,1,true,false,0,6,false},
    {"narrow-packed-volume-boundaries",2,65,33,9,1,true,true,0,6,false}};
  for(unsigned format:{2u,8u,9u,10u,24u,27u,30u})for(unsigned endian=0;endian<4;++endian) {
    for(const auto& shape:boundaries)Exercise(Make(format,endian,shape),shape.name,false);
    auto fetch=Make(format,endian,boundaries[0]);fetch[0]&=~(511u<<22);
    Compare(fetch,{}, {},"narrow zero pitch",false);
    fetch=Make(format,endian,boundaries[0]);fetch[1]&=~0x1ffff000u;fetch[5]&=~0x1ffff000u;
    Compare(fetch,{}, {},"narrow absent storage",false);
  }
}
void Correctness(const std::string& captures) {
  NarrowCases();
  constexpr Shape shapes[]{
    {"linear1d",0,37,1,1,1,false,false,0,0,false},
    {"linear1d_mips",0,65,1,1,1,false,false,0,6,false},
    {"linear2d",1,35,19,1,1,false,false,0,0,false},
    {"tiled2d",1,35,19,1,1,true,false,0,0,false},
    {"linear_mips",1,129,73,1,1,false,false,0,7,false},
    {"packed_mips",1,65,35,1,1,true,true,0,6,false},
    {"packed_mip_only",1,65,35,1,1,true,true,2,6,true},
    {"linear3d",2,17,9,5,1,false,false,0,4,false},
    {"tiled3d",2,17,9,5,1,true,false,0,4,false},
    {"packed3d",2,17,9,5,1,true,true,0,4,false},
    {"linear_cube",3,33,17,1,6,false,false,0,5,false},
    {"packed_cube",3,33,17,1,6,true,true,0,5,false},
    {"linear_stack",1,55,35,1,3,false,false,0,5,false},
    {"packed_stack",1,55,35,1,3,true,true,0,5,false},
    {"one_block",1,1,1,1,1,true,true,0,0,false}};
  for(unsigned format:formats)for(unsigned endian=0;endian<4;++endian)for(const auto& s:shapes)
    Exercise(Make(format,endian,s),std::to_string(format)+":"+std::to_string(endian)+":"+s.name);
  // Literal endian oracle: independent of both implementation functions and
  // their tiled-address helpers, on one unaligned four-byte texel.
  constexpr std::array<std::array<uint8_t,4>,4> expected{{{{0x12,0x34,0x56,0x78}},{{0x34,0x12,0x78,0x56}},{{0x78,0x56,0x34,0x12}},{{0x56,0x78,0x12,0x34}}}};
  const std::array<uint8_t,5> literal{0xff,0x12,0x34,0x56,0x78};
  Shape literal_shape{"literal",1,1,1,1,1,false,false,0,0,false};
  for(unsigned endian=0;endian<4;++endian)for(auto decoder:{DecodeTextureOriginal,DecodeTexture}) {
    auto data=decoder(Make(6,endian,literal_shape),std::span(literal).subspan(1),{});++checks;
    Need(data.levels.size()==1&&data.levels[0].bytes==std::vector<uint8_t>(expected[endian].begin(),expected[endian].end()),"Literal endian permutation differs");
  }
  auto invalid=[&](Fetch f,const std::string& name){Compare(f,{},{},name,false);};
  Shape basic{"invalid",1,35,19,1,1,false,false,0,0,false};
  auto f=Make(6,0,basic);f[0]&=~3u;invalid(f,"not texture");
  for(unsigned format=0;format<64;++format)if(std::find(formats.begin(),formats.end(),format)==formats.end())invalid(Make(format,0,basic),"unsupported format"+std::to_string(format));
  f=Make(6,0,basic);f[1]&=~0x1ffff000u;invalid(f,"no resident region");
  f=Make(6,0,basic);f[0]&=~(511u<<22);invalid(f,"zero pitch");
  Shape one{"bad1d",0,37,1,1,1,true,false,0,0,false};invalid(Make(6,0,one),"tiled 1D");
  one.tiled=false;one.packed=true;invalid(Make(6,0,one),"packed 1D");
  one.packed=false;one.w=8193;invalid(Make(6,0,one),"oversized width");
  Shape cube{"badcube",3,33,17,1,6,false,false,0,0,false};f=Make(6,0,cube);f[2]&=~(63u<<26);invalid(f,"incomplete cube");
  Shape volume{"badstack",2,17,9,5,1,false,false,0,0,false};f=Make(6,0,volume);f[1]|=1024;invalid(f,"stacked volume");
  Shape huge{"stride",1,8192,8192,1,1,false,false,0,0,false};invalid(Make(38,0,huge),"stride safety limit");
  huge.h=2049;invalid(Make(38,0,huge),"decoded safety limit before allocation");
  volume.w=2048;volume.h=2048;volume.d=1024;invalid(Make(6,0,volume),"pixel-volume safety limit");
  if(!captures.empty()) {
    std::ifstream in(captures);Need(bool(in),"Missing captured fetch fixture");std::string line;
    while(std::getline(in,line)){
      if(line.empty()||line[0]=='#')continue;
      Fetch cf{};std::istringstream fields(line);for(auto& word:cf)Need(bool(fields>>std::hex>>word),"Malformed fetch fixture");
      try{(void)Payloads(cf,1);}catch(const std::runtime_error& e){
        // Some captured fetches describe GPU-only/unsupported layouts. Preserve
        // rejected cases as explicit differential error checks where possible.
        const auto a=Run(DecodeTextureOriginal,cf,{},{}),b=Run(DecodeTexture,cf,{},{});
        Need(!a.success&&!b.success&&a.error==b.error,"Captured rejected input differs");++captured_skipped;continue;
      }
      Exercise(cf,"captured"+std::to_string(captured_cases),false);++captured_cases;
    }
  }
  std::cout<<"{\"passed\":true,\"checks\":"<<checks<<",\"valid\":"<<valid_cases<<",\"invalid\":"<<invalid_cases
           <<",\"captured_fetches\":"<<captured_cases<<",\"captured_rejected_or_bounded\":"<<captured_skipped
           <<",\"halfword_offsets_checked\":"<<halfword_offsets_checked
           <<",\"narrow_boundary_checks\":"<<narrow_boundary_checks
           <<",\"narrow_literal_checks\":"<<narrow_literal_checks<<"}\n";
}
uint64_t Consume(const TextureData& d) {
  uint64_t value=d.levels.size();for(const auto& l:d.levels)if(!l.bytes.empty())value=value*131+l.bytes.front()+l.bytes.back()+l.bytes[l.bytes.size()/2];return value;
}
void Benchmark(const std::string& captures) {
  struct Workload {const char* name;unsigned format,w,h;};
  constexpr Workload cases[]{{"R8",2,512,512},{"R16",10,512,512},{"BC1",18,512,512},{"BC3",20,512,512},{"RGBA8",6,512,512},{"RGBA32F",38,256,256},{"float-row",38,4096,1}};
  struct Case {std::string name;Fetch fetch;};std::vector<Case> inputs;
  for(const auto& w:cases)for(bool tiled:{false,true})for(unsigned endian:{0u,1u,2u,3u}) {
    Shape shape{w.name,1,w.w,w.h,1,1,tiled,false,0,0,false};
    inputs.push_back({w.name,Make(w.format,endian,shape)});
  }
  if(!captures.empty()) {
    std::ifstream in(captures);Need(bool(in),"Missing captured benchmark fetches");std::string line;unsigned index=0;
    while(std::getline(in,line)) {
      if(line.empty()||line[0]=='#')continue;
      Fetch fetch{};std::istringstream fields(line);
      for(auto& word:fetch)Need(bool(fields>>std::hex>>word),"Malformed benchmark fetch");
      const unsigned format=fetch[1]&63;
      if(format!=2&&format!=8&&format!=9&&format!=10&&format!=24&&format!=27&&format!=30)continue;
      // Actual saved fetch/mips/layout; deterministic synthetic source bytes.
      // These are decoder workloads, not a claim about live resource identity.
      inputs.push_back({"captured-narrow-"+std::to_string(index++),fetch});
    }
    Need(index==69,"Saved narrow fetch cohort differs");
  }
  std::cout<<"workload,tiled,endian,decoded_bytes,iterations,original_median_ns,candidate_median_ns,ratio,original_first_original_median_ns,original_first_candidate_median_ns,candidate_first_original_median_ns,candidate_first_candidate_median_ns";
  for(unsigned round=0;round<8;++round)std::cout<<",round_"<<round<<"_original_ns,round_"<<round<<"_candidate_ns";
  std::cout<<'\n';
  for(const auto& input:inputs){
    const auto& f=input.fetch;const auto data=Payloads(f,1);const auto layout=DescribeTextureOriginal(f);
    const bool tiled=layout.tiled;const auto endian=layout.endian;
    Compare(f,Base(data),Mips(data),input.name,true);
    auto run=[&](Decoder decode,unsigned n){const auto start=std::chrono::steady_clock::now();for(unsigned i=0;i<n;++i){auto out=decode(f,Base(data),Mips(data));sink=sink^Consume(out);}return double(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count())/n;};
    (void)run(DecodeTextureOriginal,2);(void)run(DecodeTexture,2);
    std::vector<double>a,b;constexpr unsigned iterations=8;
    for(unsigned round=0;round<8;++round){if(round%2){b.push_back(run(DecodeTexture,iterations));a.push_back(run(DecodeTextureOriginal,iterations));}
      else{a.push_back(run(DecodeTextureOriginal,iterations));b.push_back(run(DecodeTexture,iterations));}}
    auto median=[](std::vector<double> values){std::sort(values.begin(),values.end());return (values[values.size()/2-1]+values[values.size()/2])/2;};
    std::vector<double> original_first_a,original_first_b,candidate_first_a,candidate_first_b;
    for(unsigned round=0;round<8;++round){(round%2?candidate_first_a:original_first_a).push_back(a[round]);(round%2?candidate_first_b:original_first_b).push_back(b[round]);}
    auto output=DecodeTextureOriginal(f,Base(data),Mips(data));size_t bytes=0;for(const auto& l:output.levels)bytes+=l.bytes.size();
    std::cout<<input.name<<','<<tiled<<','<<endian<<','<<bytes<<','<<iterations<<','<<median(a)<<','<<median(b)<<','<<median(a)/median(b)<<','<<median(original_first_a)<<','<<median(original_first_b)<<','<<median(candidate_first_a)<<','<<median(candidate_first_b);
    for(unsigned round=0;round<8;++round)std::cout<<','<<a[round]<<','<<b[round];
    std::cout<<'\n';
  }
}
}
int main(int argc,char** argv) {
  try {bool bench=false;std::string captures;
    for(int i=1;i<argc;++i){const std::string arg=argv[i];if(arg=="--benchmark")bench=true;
      else if(arg=="--fetches"&&i+1<argc)captures=argv[++i];else throw std::runtime_error("Unknown argument: "+arg);}
    if(bench)Benchmark(captures);else Correctness(captures);return 0;
  }catch(const std::exception& e){std::cerr<<"texture decode differential failure: "<<e.what()<<'\n';return 1;}
}
