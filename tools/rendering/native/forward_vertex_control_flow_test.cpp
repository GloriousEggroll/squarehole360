#include "renderer/native/forward_vertex_control_flow.h"
#include <array>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace gow3::renderer;
namespace {
unsigned checks=0;
void Require(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
uint32_t Be(const std::vector<uint8_t>& b,size_t at){return uint32_t(b[at])<<24|uint32_t(b[at+1])<<16|uint32_t(b[at+2])<<8|b[at+3];}
void Put(std::vector<uint8_t>& b,size_t at,uint32_t v){for(unsigned i=0;i<4;++i)b[at+i]=uint8_t(v>>(24-i*8));}
void MutateCf(std::vector<uint8_t>& b,size_t pc,uint64_t bits){
    const size_t at=size_t(Be(b,4))+Be(b,Be(b,24))+pc/2*12;
    if(pc%2==0){Put(b,at,uint32_t(bits));Put(b,at+4,(Be(b,at+4)&0xffff0000u)|uint32_t(bits>>32));}
    else{Put(b,at+4,(Be(b,at+4)&65535u)|(uint32_t(bits)<<16));Put(b,at+8,uint32_t(bits>>16));}
}
std::string FixtureSource(const ForwardVertexControlFlow& p){
    std::string s="HELPER { while (helper) { break; } }\nmain {\n\tuint pc = 0;\n\twhile (true)\n\t{\n\t\tswitch (pc)\n\t\t{\n";
    for(size_t pc=0;pc<p.clauses.size();++pc){
        s+="\t\tcase "+std::to_string(pc)+":\n";const auto& c=p.clauses[pc];
        if(c.opcode==1||c.opcode==2)for(uint32_t i=0;i<c.count;++i)s+="\t\t\t{ if (fetch) { vertex_instruction_"+std::to_string(c.address+i)+"(); } }\n";
        if(pc==p.terminal)s+="\toPos.xy += g_HalfPixelOffset * oPos.w;\n\t\t\treturn;\n";
    }
    return s+"\t\t\tbreak;\n\t\t}\n\t\tbreak;\n\t}\n}\nUNCHANGED_SUFFIX\n";
}
}
int main(int argc,char** argv)try{
    if(argc!=2)throw std::runtime_error("Usage: forward-vertex-test actualf7db-source.bin");
    std::ifstream in(argv[1],std::ios::binary|std::ios::ate);
    if(!in||in.tellg()<36||in.tellg()>16u*1024*1024)throw std::runtime_error("Source extent");
    std::vector<uint8_t> source(size_t(in.tellg()));in.seekg(0);
    if(!in.read(reinterpret_cast<char*>(source.data()),source.size()))throw std::runtime_error("Source read");
    const auto p=AnalyzeForwardVertexControlFlow(source);Require(bool(p),"Actual source refused");
    Require(p->clauses.size()==14&&p->terminal==12,"Actual terminal or padding changed");
    std::vector<uint32_t> original,linear;
    for(size_t pc=0;pc<p->clauses.size();++pc){const auto& c=p->clauses[pc];if(c.opcode==1||c.opcode==2)for(unsigned i=0;i<c.count;++i)original.push_back(c.address+i);if(c.opcode==2)break;}
    for(size_t pc=0;pc<=p->terminal;++pc){const auto& c=p->clauses[pc];if(c.opcode==1||c.opcode==2)for(unsigned i=0;i<c.count;++i)linear.push_back(c.address+i);}
    Require(original==linear&&original.size()==54,"Actual straight-line instruction trace changed");
    for(unsigned op:{0u,3u,4u,5u,6u,7u,8u,9u,10u,11u,13u,14u,15u}){auto b=source;MutateCf(b,0,uint64_t(op)<<44);Require(!AnalyzeForwardVertexControlFlow(b),"Unsupported control admitted");}
    auto early=source;MutateCf(early,0,(uint64_t(2)<<44)|(uint64_t(6)<<12)|7u);Require(!AnalyzeForwardVertexControlFlow(early),"Early end admitted");
    auto missing=source;MutateCf(missing,12,(uint64_t(1)<<44)|(uint64_t(4)<<12)|57u);Require(!AnalyzeForwardVertexControlFlow(missing),"Missing end admitted");
    auto pad=source;MutateCf(pad,13,1u);Require(!AnalyzeForwardVertexControlFlow(pad),"Nonzero padding admitted");
    auto jump=source;MutateCf(jump,13,(uint64_t(11)<<44)|12u);Require(!AnalyzeForwardVertexControlFlow(jump),"Padding jump admitted");
    auto zero=source;MutateCf(zero,12,(uint64_t(2)<<44)|57u);Require(!AnalyzeForwardVertexControlFlow(zero),"Zero-length end admitted");
    for(size_t n:{size_t(0),size_t(35),source.size()-1})Require(!AnalyzeForwardVertexControlFlow(std::span<const uint8_t>(source.data(),n)),"Truncated source admitted");
    const auto original_source=FixtureSource(*p);const auto candidate=TryForwardVertexControlFlowSource(original_source,*p);Require(bool(candidate),"Closed wrapper refused");
    Require(candidate->starts_with("HELPER { while (helper) { break; } }\nmain {"),"Helper changed");Require(candidate->ends_with("}\nUNCHANGED_SUFFIX\n"),"Suffix changed");
    Require(ForwardVertexTokenCount(*candidate,"vertex_instruction_")==54,"Vertex instructions lost");
    for(uint32_t instruction:original){const auto token="vertex_instruction_"+std::to_string(instruction)+"();";Require(ForwardVertexTokenCount(*candidate,token)==1,"Instruction changed or duplicated");}
    Require(ForwardVertexTokenCount(*candidate,"\toPos.xy += g_HalfPixelOffset * oPos.w;\n")==1&&ForwardVertexTokenCount(*candidate,"return;")==1,"Terminal output changed");
    Require(candidate->find("switch (pc)")==candidate->npos&&candidate->find("case ")==candidate->npos&&candidate->find("uint pc")==candidate->npos,"Dispatcher retained");
    for(const std::string token:{"\t\tcase 12:\n","\toPos.xy += g_HalfPixelOffset * oPos.w;\n","\t\t\treturn;\n"}){auto s=original_source;const auto at=s.find(token);Require(at!=s.npos,"Fixture token missing");s.erase(at,token.size());Require(!TryForwardVertexControlFlowSource(s,*p),"Unknown terminal admitted");}
    for(const std::string token:{"continue;","break;","pc = 1;","return;"}){auto s=original_source;s.insert(s.find("vertex_instruction_7"),token);Require(!TryForwardVertexControlFlowSource(s,*p),"Nested control rewritten");}
    auto padding=original_source;padding.insert(padding.find("\t\t\tbreak;\n"),"padding_side_effect();\n");Require(!TryForwardVertexControlFlowSource(padding,*p),"Nonempty unreachable case admitted");
    std::cout<<"PASS: checks="<<checks<<" actual_instructions=54 clauses=14 terminal=12 zero_padding=1 unchanged_vertex_output=true\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
