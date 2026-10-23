#pragma once
#include "texture_data.h"
#include <algorithm>
#include <bit>
#include <limits>
#include <stdexcept>

namespace gow3::renderer {
// The source is decoded cached CPU storage. The destination must also be cached
// CPU memory; UploadTexture copies its completed output into mapped staging.
inline size_t TextureUploadConversionSize(const TextureLayout& t,size_t source_bytes) {
  const bool depth=t.format==22||t.format==23;
  const bool packed10=t.format==7||t.format==54;
  const bool rgba8=t.format==6||t.format==14||t.format==50;
  const bool rgba16=t.format==26;
  if(!depth&&!packed10&&!rgba8&&!rgba16)
    throw std::runtime_error("Unsupported CPU texture upload conversion");
  const size_t stride=rgba16?8:4,output_stride=depth?4:16;
  if(t.block.bytes!=stride||source_bytes%stride||
     source_bytes/stride>std::numeric_limits<size_t>::max()/output_stride)
    throw std::runtime_error("Invalid CPU texture upload conversion extent");
  return source_bytes/stride*output_stride;
}

inline void ConvertTextureUploadTexels(const TextureLayout& t,std::span<const uint8_t> source,
                                      std::span<uint8_t> destination) {
  const size_t output_bytes=TextureUploadConversionSize(t,source.size());
  if(destination.size()!=output_bytes)
    throw std::runtime_error("Incorrect CPU texture upload conversion output extent");
  if(!source.empty()) {
    const auto a=reinterpret_cast<uintptr_t>(source.data()),b=reinterpret_cast<uintptr_t>(destination.data());
    if((a<=b&&b-a<source.size())||(b<a&&a-b<destination.size()))
      throw std::runtime_error("CPU texture upload conversion buffers overlap");
  }
  const bool depth=t.format==22||t.format==23,packed10=t.format==7||t.format==54;
  const size_t output_stride=depth?4:16;
  for(size_t input=0,output=0;input<source.size();input+=t.block.bytes,output+=output_stride) {
    uint64_t packed=0;
    for(unsigned b=0;b<t.block.bytes;++b)packed|=uint64_t(source[input+b])<<(b*8);
    for(uint32_t c=0;c<(depth?1u:4u);++c) {
      float value;
      if(depth)value=DecodeDepthTexel(uint32_t(packed),t.format==23);
      else {
        const unsigned width=packed10?(c==3?2:10):(t.format==26?16:8);
        const unsigned shift=packed10?10*c:width*c;
        const uint32_t mask=(1u<<width)-1,raw=uint32_t(packed>>shift)&mask;
        value=float(raw)/float(mask);
        if(((t.fetch[0]>>(2+c*2))&3)==1) {
          const uint32_t sign=1u<<(width-1);
          value=std::max(-1.f,float(int32_t(raw^sign)-int32_t(sign))/float(sign-1));
        }
      }
      const auto bits=std::bit_cast<uint32_t>(value);
      for(unsigned b=0;b<4;++b)destination[output+c*4+b]=uint8_t(bits>>(b*8));
    }
  }
}
} // namespace gow3::renderer
