#include "renderer/native/index_data.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>

using gow3::renderer::DecodeIndexPayload;
void Require(bool condition,const char* message) {if(!condition)throw std::runtime_error(message);}
template<class F>void Reject(F&& action) {
    bool rejected=false;try {action();}catch(const std::runtime_error&){rejected=true;}
    Require(rejected,"Invalid index input was accepted");
}
// Frozen pre-change implementation, including its in-place byte swaps and
// runtime-width reconstruction. Used only as an independent output oracle.
std::vector<uint32_t> Previous(std::span<const uint8_t> source,uint32_t count,unsigned width,unsigned endian,bool up) {
    auto bytes=std::vector<uint8_t>(source.begin(),source.end());
    if(bytes.size()!=(up?(count*width+3)/4*4:count*width)||(width==2&&endian>1))throw std::runtime_error("Invalid index payload");
    if(endian==1||endian==2)for(size_t i=0;i<bytes.size();i+=2)std::swap(bytes[i],bytes[i+1]);
    if(endian==2||endian==3)for(size_t i=0;i<bytes.size();i+=4){std::swap(bytes[i],bytes[i+2]);std::swap(bytes[i+1],bytes[i+3]);}
    std::vector<uint32_t> indices;
    for(unsigned i=0;i<count;++i) {uint32_t n=0;for(unsigned j=0;j<width;++j)n|=uint32_t(bytes[i*width+j])<<(8*j);indices.push_back(n);}
    return indices;
}
int main(int argc,char**) try {
    uint64_t words=0,cases=0,rejected=0;
    for(unsigned width:{2u,4u})for(unsigned endian=0;endian<4;++endian)
        for(unsigned count:{1u,2u,3u,4u,7u,16u,255u,1024u,65535u})
        for(bool up:{false,true})for(unsigned prefix:{0u,1u,3u}) {
            const size_t size=up?(count*width+3)/4*4:count*width;
            std::vector<uint8_t> storage(size+prefix+7,0xcd);
            for(size_t i=0;i<size;++i)storage[prefix+i]=uint8_t((i*37u)^(i>>7)^0x96);
            const auto unchanged=storage;
            const std::span<const uint8_t> bytes(storage.data()+prefix,size);
            if(width==2&&endian>1) {
                Reject([&]{DecodeIndexPayload(bytes,count,width,endian,up);});++rejected;continue;
            }
            const auto expected=Previous(bytes,count,width,endian,up);
            const auto actual=DecodeIndexPayload(bytes,count,width,endian,up);
            Require(actual==expected,"Specialized index decode differs from previous path");
            Require(storage==unchanged,"Index decoding changed source/padding/guard bytes");
            // This span starts inside a larger captured allocation: no hidden
            // assumption about host alignment or the allocation's first byte.
            Require(actual.size()==count,"UP padding became an index");
            ++cases;words+=count;
        }
    const std::array<uint8_t,8> known{0x12,0x34,0x56,0x78,0x9a,0xbc,0xde,0xf0};
    Require(DecodeIndexPayload(known,4,2,0,false)==std::vector<uint32_t>{0x3412,0x7856,0xbc9a,0xf0de},"LE16 values");
    Require(DecodeIndexPayload(known,4,2,1,false)==std::vector<uint32_t>{0x1234,0x5678,0x9abc,0xdef0},"BE16 values");
    const std::array<uint32_t,4> expected32{0x78563412,0x56781234,0x12345678,0x34127856};
    for(unsigned endian=0;endian<4;++endian)
        Require(DecodeIndexPayload(known,2,4,endian,false)[0]==expected32[endian],"32-bit endian permutation");
    for(unsigned width:{2u,4u})for(bool up:{false,true}) {
        const auto expected=up?(3*width+3)/4*4:3*width;
        std::vector<uint8_t> bytes(expected);
        for(int extra:{-1,1}) {
            auto wrong=bytes;wrong.resize(bytes.size()+extra);
            Reject([&]{DecodeIndexPayload(wrong,3,width,0,up);});++rejected;
        }
    }
    Reject([&]{DecodeIndexPayload({},0,2,0,false);});
    Reject([&]{DecodeIndexPayload({},4u*1024*1024+1,4,0,false);});
    Reject([&]{DecodeIndexPayload(known,2,3,0,false);});
    Reject([&]{DecodeIndexPayload(known,2,4,4,false);});rejected+=4;
    std::cout<<"Index decode PASS: "<<cases<<" exact old/new cases, "<<words<<" words, "<<rejected
        <<" invalid extents/modes, endian known vectors, immutable source and UP padding\n";
    if(argc>1) {
        constexpr unsigned count=262144,iterations=128;
        std::vector<uint8_t> bytes(count*2);
        for(size_t i=0;i<bytes.size();++i)bytes[i]=uint8_t(i*37);
        uint64_t checksum=0;
        const auto run=[&](auto decode) {
            const auto start=std::chrono::steady_clock::now();
            for(unsigned i=0;i<iterations;++i) {
                auto result=decode(bytes,count,2,1,false);checksum+=result[(i*997)%count];
            }
            return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        };
        const auto before=run(Previous),after=run([](std::span<const uint8_t> source,unsigned n,unsigned width,unsigned endian,bool up) {
            // Both timings include the current live provider's payload copy.
            const auto snapshot=std::vector<uint8_t>(source.begin(),source.end());
            return DecodeIndexPayload(snapshot,n,width,endian,up);
        });
        std::cout<<"Synthetic BE16 decode: old "<<before<<" ms, direct "<<after<<" ms; checksum "<<checksum<<" (not gameplay FPS)\n";
    }
    return 0;
} catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
