#include "memory_export_copy.h"
#include <picosha2.h>
#include <stdexcept>
#include <vector>

namespace gow3::renderer {
namespace {
uint32_t Word(std::span<const uint8_t> bytes,size_t offset) {
    if(offset+4>bytes.size())throw std::runtime_error("Truncated native memcpy state");
    return uint32_t(bytes[offset])<<24|uint32_t(bytes[offset+1])<<16|
        uint32_t(bytes[offset+2])<<8|bytes[offset+3];
}
void Require(bool valid,const char* message) {if(!valid)throw std::runtime_error(message);}
}
static MemoryExportCopy DecodeState(std::span<const uint8_t> state,const std::array<uint32_t,8>& args) {
    Require(state.size()==0x3600&&args[1]==1&&args[3],"Invalid native memcpy draw");
    // This shader writes -abs(index)>nonnegative to oPos, hence (0,0,0,0).
    // Keep the verified D3D clip-space configuration which discards it.
    Require(Word(state,0x2944)==0x80000&&(Word(state,0x294c)&0x73f)==0x43f&&
        !Word(state,0x28d4),"Unsupported native memcpy clipping or index offset");
    // c7 offsets the export index. The game's memcpy entry point initializes
    // it to zero. Reject another use rather than guessing integer rounding.
    for(unsigned i=0;i<4;++i)Require(!Word(state,0x780+7*16+i*4),"Unsupported native memcpy export offset");
    const uint32_t first=Word(state,0x480+95*8),second=Word(state,0x480+95*8+4);
    Require((first&3)==3&&(second&3)==2&&(second&0xfc000000)==0x10000000,
        "Unsupported native memcpy vertex stream/endian");
    const uint32_t stream_base=Word(state,0x780+6*16),index_base=Word(state,0x780+6*16+4),
        format=Word(state,0x780+6*16+8),limit=Word(state,0x780+6*16+12);
    Require((stream_base>>30)==1&&index_base==0x4b000000&&format==0x4b021a01&&
        (limit&0xff800000)==0x4b000000,"Unsupported native memcpy export stream");
    const uint64_t offset=uint64_t(args[2])*64,bytes=uint64_t(args[3])*64;
    const uint64_t source=(first&~3u)+offset,destination=uint64_t(stream_base&0x3fffffff)*4+offset;
    Require((uint64_t(args[2])+args[3])*8<=(limit&0x7fffff)&&
        offset+bytes<=uint64_t((second>>2)&0xffffff)*4,"Native memcpy exceeds its input/export bounds");
    Require(bytes<=8*1024*1024&&source+bytes<=512*1024*1024&&destination+bytes<=512*1024*1024,
        "Native memcpy exceeds physical memory");
    // The game splits overlapping memmoves into non-overlapping draw chunks.
    // Vulkan buffer copies cannot overlap; an unproven chunk must fall back.
    Require(source+bytes<=destination||destination+bytes<=source,"Overlapping native memcpy chunk");
    return {uint32_t(source),uint32_t(destination),uint32_t(bytes),first&~3u,uint32_t(offset)};
}
MemoryExportCopy DecodeMemoryExportCopy(std::span<const uint8_t> shader,
    std::span<const uint8_t> state,const std::array<uint32_t,8>& args) {
    Require(picosha2::hash256_hex_string(shader.begin(),shader.end())==
        "cc553dc5fb29a7e87906add1fdd997ddca0fc2c0fab558abc57965b45cf7ce6a",
        "Unsupported native memory-export shader; only the verified Gears memcpy kernel is implemented");
    return DecodeState(state,args);
}
MemoryExportCopy DecodeMemoryExportPacket(std::span<const uint32_t> shader_words,
    const std::array<uint32_t,2>& fetch95,const std::array<uint32_t,8>& constants,
    uint32_t clip_control,uint32_t vte_control,uint32_t first,uint32_t count) {
    Require(shader_words.size()==84,"Native memcpy packet uses another shader extent");
    std::vector<uint8_t> shader;shader.reserve(shader_words.size()*4);
    for(auto word:shader_words)for(unsigned b=0;b<4;++b)shader.push_back(uint8_t(word>>(24-b*8)));
    Require(picosha2::hash256_hex_string(shader.begin(),shader.end())==
        "1b0ee79a0d5d49d8426002e1b6f7f238abdbde36aae9ef2a972cfb8b6e7bf0e8",
        "Native memcpy packet active shader differs from its captured kernel");
    std::array<uint8_t,0x3600> state{};
    auto put=[&](size_t at,uint32_t word) {for(unsigned b=0;b<4;++b)state[at+b]=uint8_t(word>>(24-b*8));};
    put(0x2944,clip_control);put(0x294c,vte_control);
    for(unsigned i=0;i<2;++i)put(0x480+95*8+i*4,fetch95[i]);
    for(unsigned i=0;i<8;++i)put(0x780+6*16+i*4,constants[i]);
    return DecodeState(state,{0,1,first,count,0,0,0,0});
}
}
