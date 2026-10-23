#include "renderer/native/texture_upload_conversion.h"
#include <array>
#include <cstring>
#include <iostream>

using namespace gow3::renderer;
namespace {
void Check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
// Retained pre-change algorithm, using ordinary memory as its old staging
// destination. It copies then rereads depth bytes in place, unlike the new
// source-only chunk conversion. All comparisons below are byte-for-byte.
std::vector<uint8_t> OldUpload(const TextureLayout& t,std::span<const uint8_t> source) {
  const bool depth=t.format==22||t.format==23,packed10=t.format==7||t.format==54;
  std::vector<uint8_t> result(depth?source.size():source.size()/t.block.bytes*16);
  if(depth) {
    std::memcpy(result.data(),source.data(),source.size());
    for(size_t j=0;j<source.size();j+=4) {
      const uint32_t packed=uint32_t(result[j])|uint32_t(result[j+1])<<8|
          uint32_t(result[j+2])<<16|uint32_t(result[j+3])<<24;
      const auto bits=std::bit_cast<uint32_t>(DecodeDepthTexel(packed,t.format==23));
      for(unsigned b=0;b<4;++b)result[j+b]=uint8_t(bits>>(b*8));
    }
  } else for(size_t j=0;j<source.size();j+=t.block.bytes) {
    uint64_t packed=0;
    for(unsigned b=0;b<t.block.bytes;++b)packed|=uint64_t(source[j+b])<<(b*8);
    for(unsigned c=0;c<4;++c) {
      const unsigned width=packed10?(c==3?2:10):(t.format==26?16:8),shift=packed10?10*c:width*c;
      const uint32_t mask=(1u<<width)-1,raw=uint32_t(packed>>shift)&mask;
      float value=float(raw)/float(mask);
      if(((t.fetch[0]>>(2+c*2))&3)==1) {
        const uint32_t sign=1u<<(width-1);
        value=std::max(-1.f,float(int32_t(raw^sign)-int32_t(sign))/float(sign-1));
      }
      const auto bits=std::bit_cast<uint32_t>(value);
      for(unsigned b=0;b<4;++b)result[(j/t.block.bytes)*16+c*4+b]=uint8_t(bits>>(b*8));
    }
  }
  return result;
}
uint64_t compared=0;
void Compare(const TextureLayout& t,size_t texels,unsigned endian) {
  std::vector<uint8_t> storage(texels*t.block.bytes+1);
  auto source=std::span(storage).subspan(1); // Deliberately unaligned.
  uint32_t random=0x213ab57d;
  constexpr uint64_t edges[]{0,UINT64_MAX,0x8000800080008000ull,0x7fff7fff7fff7fffull,
      0x0000000100000001ull,0x3fffffff3fffffffull,0xffffff00ffffff00ull};
  constexpr unsigned permutations[]{0,1,3,2};
  for(size_t i=0;i<texels;++i) {
    uint64_t packed;
    if(i<std::size(edges))packed=edges[i];
    else {random^=random<<13;random^=random>>17;random^=random<<5;packed=uint64_t(random)<<32;
      random^=random<<13;random^=random>>17;random^=random<<5;packed|=random;}
    for(unsigned b=0;b<t.block.bytes;++b)source[i*t.block.bytes+b]=uint8_t(packed>>((b^permutations[endian])*8));
  }
  const auto original=storage;const auto expected=OldUpload(t,source);
  std::vector<uint8_t> actual(expected.size()+7,0xa5),scratch(std::min<size_t>(expected.size(),64*1024));
  const size_t stride=(t.format==22||t.format==23)?4:16;
  for(size_t input=0,output=0;input<source.size();) {
    const size_t bytes=std::min(source.size()-input,scratch.size()/stride*t.block.bytes);
    const size_t converted=TextureUploadConversionSize(t,bytes);
    ConvertTextureUploadTexels(t,source.subspan(input,bytes),std::span(scratch).first(converted));
    std::memcpy(actual.data()+3+output,scratch.data(),converted);
    input+=bytes;output+=converted;
  }
  Check(std::equal(expected.begin(),expected.end(),actual.begin()+3),"Cached upload conversion changed float bits");
  Check(std::all_of(actual.begin(),actual.begin()+3,[](auto x){return x==0xa5;})&&
      std::all_of(actual.end()-4,actual.end(),[](auto x){return x==0xa5;}),"Conversion overwrote boundary guards");
  Check(storage==original,"Conversion changed immutable CPU source");compared+=expected.size();
}
}
int main() try {
  for(uint32_t format:{22u,23u,7u,54u,6u,14u,50u,26u}) {
    TextureLayout t{};t.format=format;t.block={1,1,format==26?8u:4u,4};
    for(unsigned signs=0;signs<256;++signs) {
      t.fetch[0]=signs<<2;
      for(unsigned endian=0;endian<4;++endian)Compare(t,31,endian);
    }
    for(size_t texels:{size_t(1),size_t(4095),size_t(4096),size_t(4097),size_t(16383),size_t(16384),size_t(16385)})
      Compare(t,texels,0);
    std::array<uint8_t,17> source{};std::array<uint8_t,64> destination;destination.fill(0xa5);
    for(unsigned failure=0;failure<3;++failure) {
      bool rejected=false;
      try {
        if(failure==0)ConvertTextureUploadTexels(t,std::span(source).first(t.block.bytes-1),destination);
        else if(failure==1)ConvertTextureUploadTexels(t,std::span(source).first(t.block.bytes),std::span(destination).first(1));
        else ConvertTextureUploadTexels(t,std::span(destination).first(t.block.bytes),
            std::span(destination).first(TextureUploadConversionSize(t,t.block.bytes)));
      } catch(const std::runtime_error&){rejected=true;}
      Check(rejected&&std::all_of(destination.begin(),destination.end(),[](auto x){return x==0xa5;}),
          "Invalid conversion changed output before rejection");
    }
  }
  std::cout<<"PASS: "<<compared<<" exact upload bytes across both depth formats, packed10 aliases, mixed RGBA8/16, all256 signs/four byte orders, unaligned input, chunk boundaries and24 invalid extents/overlaps\n";
  return 0;
} catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
