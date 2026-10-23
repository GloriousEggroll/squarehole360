#include "renderer/native/forward_control_flow.h"
#include <array>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace gow3::renderer;
namespace {
unsigned checks=0;
void Require(bool value,const char* message) {++checks;if(!value)throw std::runtime_error(message);}
std::vector<uint8_t> Read(const char* path) {
    std::ifstream file(path,std::ios::binary|std::ios::ate);
    if(!file||file.tellg()<36||file.tellg()>16u*1024*1024)throw std::runtime_error("Source extent");
    std::vector<uint8_t> bytes(size_t(file.tellg()));file.seekg(0);
    if(!file.read(reinterpret_cast<char*>(bytes.data()),bytes.size()))throw std::runtime_error("Source read");
    return bytes;
}
uint32_t Be(const std::vector<uint8_t>& b,size_t at) {return uint32_t(b[at])<<24|uint32_t(b[at+1])<<16|uint32_t(b[at+2])<<8|b[at+3];}
void Put(std::vector<uint8_t>& b,size_t at,uint32_t v) {for(unsigned i=0;i<4;++i)b[at+i]=uint8_t(v>>(24-i*8));}
void MutateCf(std::vector<uint8_t>& b,size_t pc,uint64_t bits) {
    const size_t at=size_t(Be(b,4))+Be(b,Be(b,24))+pc/2*12;
    if(pc%2==0){Put(b,at,uint32_t(bits));Put(b,at+4,(Be(b,at+4)&0xffff0000u)|uint32_t(bits>>32));}
    else {Put(b,at+4,(Be(b,at+4)&65535u)|(uint32_t(bits)<<16));Put(b,at+8,uint32_t(bits>>16));}
}
bool Taken(const ForwardCfInstruction& x,const std::array<bool,256>& bits) {return x.unconditional||bits[x.bool_address]==x.condition;}
struct Trace {std::vector<uint32_t> instructions;unsigned ends=0;bool operator==(const Trace&)const=default;};
void Clause(Trace& t,const ForwardCfInstruction& x) {
    if(x.opcode==1||x.opcode==2)for(unsigned i=0;i<x.count;++i)t.instructions.push_back(x.address+i);
    if(x.opcode==2)++t.ends;
}
Trace Original(const ForwardCfProgram& p,const std::array<bool,256>& bits) {
    Trace t;size_t cursor=0,steps=0;
    while(cursor<p.instructions.size()) {
        if(++steps>p.instructions.size())throw std::runtime_error("Original trace unexpectedly loops");
        const auto& x=p.instructions[cursor];Clause(t,x);if(x.opcode==2)return t;
        cursor=x.opcode==11&&Taken(x,bits)?x.target:cursor+1;
    }
    throw std::runtime_error("Missing original end");
}
Trace Linear(const ForwardCfProgram& p,const std::array<bool,256>& bits) {
    Trace t;size_t pc=0;
    for(size_t index=0;index<p.instructions.size();++index)if(pc<=index) {
        const auto& x=p.instructions[index];Clause(t,x);if(x.opcode==2)return t;
        if(x.opcode==11&&Taken(x,bits))pc=x.target;
    }
    throw std::runtime_error("Missing linear end");
}
std::string FixtureSource(const ForwardCfProgram& p) {
    std::string text="HELPER { while (helper) { break; } }\nmain {\n\tuint pc = 0;\n\twhile (true)\n\t{\n\t\tswitch (pc)\n\t\t{\n";
    for(size_t i=0;i<p.instructions.size();++i) {
        text+="\t\tcase "+std::to_string(i)+":\n";const auto& x=p.instructions[i];
        if(x.opcode==11) {
            if(x.unconditional)text+="\t\t\tpc = "+std::to_string(x.target)+";\n\t\t\tcontinue;\n";
            else text+="\t\t\tif (((vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + "+std::to_string(512+x.bool_address/32*4)+") >> "+std::to_string(x.bool_address%32)+") & 1) "+(x.condition?"!=":"==")+" 0)\n\t\t\t{\n\t\t\t\tpc = "+std::to_string(x.target)+";\n\t\t\t\tcontinue;\n\t\t\t}\n";
        } else if(x.opcode==1||x.opcode==2) {
            text+="\t\t\t{\n\t\t\t\tif (guest) { nested_material(); }\n\t\t\t}\n";
            if(x.opcode==2)text+="\t\t\traster_epilogue();\n\t\t\treturn;\n";
        }
    }
    return text+"\t\t\tbreak;\n\t\t}\n\t\tbreak;\n\t}\n}\nUNCHANGED_SUFFIX\n";
}
}
int main(int argc,char** argv)try {
    if(argc!=2)throw std::runtime_error("Usage: forward-control-flow-test actual49f2-source.bin");
    const auto source=Read(argv[1]);const auto p=AnalyzeForwardControlFlow(source);
    Require(bool(p),"Actual source refused");Require(p->instructions.size()==28,"Unexpected actual CF count");
    std::vector<unsigned> constants;
    for(const auto& x:p->instructions)if(x.opcode==11&&!x.unconditional)constants.push_back(x.bool_address);
    std::sort(constants.begin(),constants.end());constants.erase(std::unique(constants.begin(),constants.end()),constants.end());
    Require(constants.size()==4,"Unexpected actual uniform branch inputs");
    for(unsigned mask=0;mask<(1u<<constants.size());++mask) {
        std::array<bool,256> bits{};for(size_t i=0;i<constants.size();++i)bits[constants[i]]=(mask>>i)&1;
        const auto original=Original(*p,bits),linear=Linear(*p,bits);
        Require(original==linear,"Actual clause instruction trace changed");Require(original.ends==1,"End count changed");
    }
    for(unsigned opcode:{0u,3u,4u,5u,6u,7u,8u,9u,10u,13u,14u,15u}) {
        auto changed=source;MutateCf(changed,13,uint64_t(opcode)<<44);
        Require(!AnalyzeForwardControlFlow(changed),"Unsupported CF opcode admitted");
    }
    for(uint64_t bits:{(uint64_t(11)<<44)|13u,(uint64_t(11)<<44)|28u,
        (uint64_t(11)<<44)|(uint64_t(1)<<14)|18u,
        (uint64_t(11)<<44)|(uint64_t(1)<<33)|18u,
        (uint64_t(11)<<44)|(uint64_t(1)<<43)|18u,
        (uint64_t(11)<<44)|(uint64_t(1)<<20)|18u}) {
        auto changed=source;MutateCf(changed,13,bits);Require(!AnalyzeForwardControlFlow(changed),"Bad jump admitted");
    }
    auto early=source;MutateCf(early,0,(uint64_t(2)<<44)|(uint64_t(6)<<12)|14u);
    Require(!AnalyzeForwardControlFlow(early),"Early EXEC_END admitted");
    auto noend=source;MutateCf(noend,27,(uint64_t(1)<<44)|(uint64_t(6)<<12)|111u);
    Require(!AnalyzeForwardControlFlow(noend),"Missing EXEC_END admitted");
    for(size_t size:{size_t(0),size_t(35),source.size()-1})
        Require(!AnalyzeForwardControlFlow(std::span<const uint8_t>(source.data(),size)),"Truncation admitted");
    const auto hlsl=FixtureSource(*p);const auto candidate=TryForwardControlFlowSource(hlsl,*p);
    Require(bool(candidate),"Valid closed HLSL shape refused");
    Require(candidate->starts_with("HELPER { while (helper) { break; } }\nmain {"),"Helper loop changed");
    Require(candidate->ends_with("}\nUNCHANGED_SUFFIX\n"),"Suffix changed");
    Require(ForwardTokenCount(*candidate,"nested_material();")==ForwardTokenCount(hlsl,"nested_material();"),"Material clause lost");
    Require(ForwardTokenCount(*candidate,"raster_epilogue();")==1,"Raster epilogue changed");
    Require(ForwardTokenCount(*candidate,"continue;")==0,"Guest continue retained");
    Require(ForwardTokenCount(*candidate,"if (pc <= ")==28,"Linear guard count changed");
    for(const std::string token:{"\t\tcase 13:\n","\t\t\t\tcontinue;","\t\t\traster_epilogue();\n\t\t\treturn;"}) {
        auto changed=hlsl;const auto at=changed.find(token);Require(at!=changed.npos,"Fixture missing token");
        changed.erase(at,token.size());Require(!TryForwardControlFlowSource(changed,*p),"Malformed wrapper admitted");
    }
    auto unexpected=hlsl;unexpected.insert(unexpected.find("nested_material();"),"continue; ");
    Require(!TryForwardControlFlowSource(unexpected,*p),"Nested continue converted");
    std::cout<<"PASS: checks="<<checks<<" actual_uniform_paths=16 clauses=28 unchanged_material_and_late_epilogue=true\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
