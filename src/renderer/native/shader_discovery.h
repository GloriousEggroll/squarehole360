#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <vector>

namespace gow3::renderer {
// ABI witnesses from the supported game's generated instruction stream. The
// two initializers already have copied virtual headers; the link hook is after
// its physical program rewrite and linked-header update, never before them.
inline constexpr std::array<std::array<uint32_t,2>,11> kShaderDiscoverySites{{
    {{0x82AF2628,0x39600000}}, // li r11,0
    {{0x82AF262C,0x39400007}}, // li r10,7 (pixel object)
    {{0x82AF2658,0x90830018}}, // stw r4,24(r3) (physical code)
    {{0x82AF265C,0x4E800020}}, // blr
    {{0x82AF2910,0x7D8802A6}}, // mflr r12
    {{0x82AF2920,0x38A00368}}, // li r5,872 (vertex object prefix)
    {{0x82AF2938,0x93DF0020}}, // stw r30,32(r31) (physical code)
    {{0x82AF9688,0x7D8802A6}}, // mflr r12
    {{0x82AF97F8,0x817F0368}}, // lwz r11,872(r31)
    {{0x82AF97FC,0x616B0040}}, // ori r11,r11,64 (linked container)
    {{0x82AF9800,0x917F0368}}, // stw r11,872(r31)
}};

inline uint32_t ShaderDiscoveryWord(std::span<const uint8_t> bytes,size_t at) {
  return uint32_t(bytes[at])<<24|uint32_t(bytes[at+1])<<16|
         uint32_t(bytes[at+2])<<8|bytes[at+3];
}

// Read returns exactly the requested readable virtual span or an empty span.
// All borrowed guest spans are consumed synchronously. Only this exact owned
// virtual+physical concatenation may be handed to background compilation.
template<class Read>
std::shared_ptr<const std::vector<uint8_t>> SnapshotDiscoveredShader(
    uint32_t object,bool pixel,Read&& read) {
  const uint32_t offset=pixel?0x28:0x368;
  if(!object||uint64_t(object)+offset+36>(uint64_t{1}<<32))return {};
  const auto header=read(object,offset+36);
  if(header.size()!=offset+36)return {};
  const auto container=header.subspan(offset);
  const uint32_t flags=ShaderDiscoveryWord(container,0);
  const uint32_t allowed=pixel?0u:0x71u;
  if((flags&~allowed)!=0x102A1100u||bool(flags&1)==pixel)return {};
  const uint32_t virtual_size=ShaderDiscoveryWord(container,4);
  const uint32_t physical_size=ShaderDiscoveryWord(container,8);
  const uint32_t physical=ShaderDiscoveryWord(header,pixel?0x18:0x20);
  if(virtual_size<36||virtual_size>128*1024||physical_size>128*1024||
      uint64_t(object)+offset+virtual_size>(uint64_t{1}<<32)||
      (physical_size&&(!physical||uint64_t(physical)+physical_size>(uint64_t{1}<<32))))return {};
  const auto virtual_bytes=read(object+offset,virtual_size);
  if(virtual_bytes.size()!=virtual_size)return {};
  const auto physical_bytes=physical_size?read(physical,physical_size):std::span<const uint8_t>{};
  if(physical_bytes.size()!=physical_size)return {};
  auto snapshot=std::make_shared<std::vector<uint8_t>>(size_t(virtual_size)+physical_size);
  std::memcpy(snapshot->data(),virtual_bytes.data(),virtual_size);
  if(physical_size)std::memcpy(snapshot->data()+virtual_size,physical_bytes.data(),physical_size);
  return snapshot;
}
} // namespace gow3::renderer
