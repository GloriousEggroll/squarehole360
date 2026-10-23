#include "api_capture_reader.h"
#include <iostream>
#include <vector>
using namespace gow3::renderer;
namespace {
void Require(bool okay,const char* message){if(!okay)throw std::runtime_error(message);}
void Put(std::vector<uint8_t>& bytes,size_t at,uint32_t value) {
    for(unsigned i=0;i<4;++i)bytes.at(at+i)=uint8_t(value>>(i*8));
}
std::vector<uint8_t> Fixture() {
    // First event has one literal three-byte payload. Second refers exactly
    // to that earlier block; the footer counts both API events.
    std::vector<uint8_t> bytes(16+75+72+16);
    const char magic[]="G3D3D001";std::copy_n(magic,8,bytes.begin());Put(bytes,8,6);Put(bytes,12,0x3600);
    const size_t first=16,second=91,footer=163;
    for(auto at:{first,second}) {
        Put(bytes,at,at==first?75:72);Put(bytes,at+4,0x82ABCDEF);Put(bytes,at+8,1);
        Put(bytes,at+12,7);Put(bytes,at+16,0x82FEDCBA);Put(bytes,at+52,23);Put(bytes,at+56,1);
        for(unsigned i=0;i<8;++i)Put(bytes,at+20+i*4,100+i);
        Put(bytes,at+60,14);Put(bytes,at+64,0x12345000);
        Put(bytes,at+68,at==first?3:0x80000058u);
    }
    bytes[88]=1;bytes[89]=2;bytes[90]=3;Put(bytes,footer,16);Put(bytes,footer+12,2);return bytes;
}
void Reject(std::vector<uint8_t> bytes) {
    bool rejected=false;try{validation::ReadApiCapture(bytes,[](auto&&){});}catch(const std::runtime_error&){rejected=true;}
    Require(rejected,"Malformed API capture was accepted");
}
}
int main()try {
    std::vector<GraphicsBoundaryEvent> events;
    const auto valid=Fixture();const auto count=validation::ReadApiCapture(valid,[&](auto&& e){events.push_back(std::move(e));});
    Require(count==2&&events.size()==2,"API capture lost event count");
    for(size_t i=0;i<events.size();++i) {
        const auto& e=events[i];Require(e.sequence==i&&e.function==0x82ABCDEF&&e.phase==1&&e.frame==7&&
            e.lr==0x82FEDCBA&&e.result==23&&e.args[0]==100&&e.args[7]==107&&e.blocks.size()==1&&
            e.blocks[0].kind==14&&e.blocks[0].address==0x12345000&&e.blocks[0].bytes==std::vector<uint8_t>({1,2,3}),
            "Literal/backreferenced API event changed fields or exact bytes");
    }
    events[0].blocks[0].bytes[0]=99;Require(events[1].blocks[0].bytes[0]==1,"Backreferences exposed mutable shared capture memory");
    auto bad=valid;bad[0]='X';Reject(bad);
    bad=valid;Put(bad,8,5);Reject(bad);
    bad=valid;Put(bad,12,0x100);Reject(bad);
    bad=valid;Put(bad,16,59);Reject(bad);
    bad=valid;Put(bad,16,UINT32_MAX);Reject(bad);
    bad=valid;Put(bad,16+56,7);Reject(bad);
    bad=valid;Put(bad,16+68,4);Reject(bad);
    bad=valid;Put(bad,16+68,0x80000058);Reject(bad);
    bad=valid;Put(bad,91+68,0x80000059);Reject(bad);
    bad=valid;Put(bad,163+12,1);Reject(bad);
    bad=valid;bad.pop_back();Reject(bad);
    bad=valid;bad.push_back(0);Reject(bad);
    bad=valid;bad.resize(163);Reject(bad);
    std::cout<<"PASS: exact API fields/backreferences and13 malformed record/header/footer bounds\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
