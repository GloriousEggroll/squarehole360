#include "renderer/native/memory_export_copy.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace gow3::renderer;
namespace {
std::vector<uint8_t> Read(const std::filesystem::path& p) {
    std::ifstream f(p,std::ios::binary|std::ios::ate);const auto count=f.tellg();
    if(!f||count<0||count>65536)throw std::runtime_error("Invalid packet fixture");
    std::vector<uint8_t> b(count);f.seekg(0);f.read(reinterpret_cast<char*>(b.data()),count);return b;
}
uint32_t Word(std::span<const uint8_t> b,size_t at) {
    if(at+4>b.size())throw std::runtime_error("Truncated packet fixture");
    return uint32_t(b[at])<<24|uint32_t(b[at+1])<<16|uint32_t(b[at+2])<<8|b[at+3];
}
}
int main(int argc,char** argv) {
    try {
        if(argc!=2)throw std::runtime_error("usage: memory-export-packet-test fixture-directory");
        const std::filesystem::path p=argv[1];
        const auto h=Read(p/"block-2.bin"),code=Read(p/"block-3.bin"),state=Read(p/"block-1.bin");
        const auto descriptor=Word(h,24),offset=Word(h,descriptor),bytes=Word(h,descriptor+4);
        std::vector<uint32_t> words;for(unsigned i=0;i<bytes;i+=4)words.push_back(Word(code,offset+i));
        std::array<uint32_t,2> fetch;for(unsigned i=0;i<2;++i)fetch[i]=Word(state,0x480+95*8+i*4);
        std::array<uint32_t,8> constants;for(unsigned i=0;i<8;++i)constants[i]=Word(state,0x780+6*16+i*4);
        const auto clip=Word(state,0x2944),vte=Word(state,0x294c);
        const auto copy=DecodeMemoryExportPacket(words,fetch,constants,clip,vte,0,128);
        if(copy.source!=0x16b22000||copy.destination!=0x164ab000||copy.bytes!=8192)
            throw std::runtime_error("Packet-state decode differs from API fixture");
        const auto tail=DecodeMemoryExportPacket(words,fetch,constants,clip,vte,127,1);
        if(tail.source!=copy.source+8128||tail.destination!=copy.destination+8128||tail.bytes!=64)
            throw std::runtime_error("Packet-state split chunk differs from API fixture");
        auto reject=[&](auto shader,auto f,auto c,uint32_t cl,uint32_t vt) {
            bool rejected=false;try{DecodeMemoryExportPacket(shader,f,c,cl,vt,0,128);}catch(const std::runtime_error&){rejected=true;}
            if(!rejected)throw std::runtime_error("Changed active packet state was accepted");
        };
        auto changed=words;changed[5]^=1;reject(changed,fetch,constants,clip,vte);
        auto bad_fetch=fetch;bad_fetch[1]^=1;reject(words,bad_fetch,constants,clip,vte);
        auto bad_constants=constants;bad_constants[6]=1;reject(words,fetch,bad_constants,clip,vte);
        reject(words,fetch,constants,clip^1,vte);reject(words,fetch,constants,clip,vte^1);
        std::cout<<"Actual shader/register memcpy validation, split offset and changed-state rejection passed\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
