#include "renderer/native/forward_pixel_catalog.h"
#include <array>
#include <filesystem>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace gow3::renderer;
namespace {
unsigned checks=0;
void Require(bool value,const char* message) {++checks;if(!value)throw std::runtime_error(message);}
std::vector<uint8_t> Read(const std::filesystem::path& path) {
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
    std::string text="HELPER { while (helper) { break; } }\nmain {\nfloat2 gow3FsiDepthSlope = float2(ddx_coarse(iPos.z), ddy_coarse(iPos.z));\n\tuint pc = 0;\n\twhile (true)\n\t{\n\t\tswitch (pc)\n\t\t{\n";
    for(size_t i=0;i<p.instructions.size();++i) {
        text+="\t\tcase "+std::to_string(i)+":\n";const auto& x=p.instructions[i];
        if(x.opcode==11) {
            if(x.unconditional)text+="\t\t\tpc = "+std::to_string(x.target)+";\n\t\t\tcontinue;\n";
            else text+="\t\t\tif (((vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + "+std::to_string(512+x.bool_address/32*4)+") >> "+std::to_string(x.bool_address%32)+") & 1) "+(x.condition?"!=":"==")+" 0)\n\t\t\t{\n\t\t\t\tpc = "+std::to_string(x.target)+";\n\t\t\t\tcontinue;\n\t\t\t}\n";
        } else if(x.opcode==1||x.opcode==2) {
            text+="\t\t\t{\n\t\t\t\tif (guest) { nested_material(); }\n\t\t\t}\n";
            if(x.opcode==2)text+="\t\t\toC0 *= g_ColorScale[0];\n#ifdef GOW3_NATIVE_FSI\ngow3NativeRasterOutput(iPos.xy, iPos.z, iFace, oC0, float4(0,0,0,0), float4(0,0,0,0), float4(0,0,0,0), 1u, false, gow3FsiDepthSlope);\n#endif\n\t\t\treturn;\n";
        }
    }
    return text+"\t\t\tbreak;\n\t\t}\n\t\tbreak;\n\t}\n}\nUNCHANGED_SUFFIX\n";
}
}
int main(int argc,char** argv)try {
    if(argc!=2)throw std::runtime_error("Usage: forward-pixel-catalog-test source-cache-directory");
    size_t paths=0,clause_count=0;
    for(const auto digest:kForwardPixelCatalogSources) {
        Require(ForwardPixelCatalogSource(digest),"Pinned source refused");
        const auto source=Read(std::filesystem::path(argv[1])/(std::string(digest)+".bin"));
        const auto p=AnalyzeForwardPixelCatalog(source);Require(bool(p),"Actual catalog source refused");
        clause_count+=p->instructions.size();
        std::vector<unsigned> constants;
        for(const auto& x:p->instructions)if(x.opcode==11&&!x.unconditional)constants.push_back(x.bool_address);
        std::sort(constants.begin(),constants.end());constants.erase(std::unique(constants.begin(),constants.end()),constants.end());
        Require(constants.size()<=4,"Actual uniform path bound changed");
        for(unsigned mask=0;mask<(1u<<constants.size());++mask) {
            std::array<bool,256> bits{};for(size_t i=0;i<constants.size();++i)bits[constants[i]]=(mask>>i)&1;
            const auto original=Original(*p,bits),linear=Linear(*p,bits);
            Require(original==linear,"Actual clause instruction trace changed");Require(original.ends==1,"End count changed");++paths;
        }
        for(unsigned opcode:{0u,3u,4u,5u,6u,7u,8u,9u,10u,13u,14u,15u}) {
            auto changed=source;MutateCf(changed,0,uint64_t(opcode)<<44);
            Require(!AnalyzeForwardPixelCatalog(changed),"Unsupported opcode admitted");
        }
        for(uint64_t bits:{(uint64_t(11)<<44)|0u,(uint64_t(11)<<44)|uint64_t(p->instructions.size()),
            (uint64_t(11)<<44)|(uint64_t(1)<<14)|1u,(uint64_t(11)<<44)|(uint64_t(1)<<33)|1u,
            (uint64_t(11)<<44)|(uint64_t(1)<<43)|1u,(uint64_t(11)<<44)|(uint64_t(1)<<20)|1u}) {
            auto changed=source;MutateCf(changed,0,bits);Require(!AnalyzeForwardPixelCatalog(changed),"Bad jump admitted");
        }
        auto outputs=source;Put(outputs,size_t(Be(outputs,24))+28,33);Require(!AnalyzeForwardPixelCatalog(outputs),"Depth export admitted");
        auto early=source;const auto& last=p->instructions.back();
        MutateCf(early,0,(uint64_t(2)<<44)|(uint64_t(last.count)<<12)|last.address);
        Require(!AnalyzeForwardPixelCatalog(early),"Early EXEC_END admitted");
        auto noend=source;MutateCf(noend,p->instructions.size()-1,(uint64_t(1)<<44)|(uint64_t(last.count)<<12)|last.address);
        Require(!AnalyzeForwardPixelCatalog(noend),"Missing EXEC_END admitted");
        for(size_t size:{size_t(0),size_t(35),source.size()-1})
            Require(!AnalyzeForwardPixelCatalog(std::span<const uint8_t>(source.data(),size)),"Truncation admitted");
        const auto hlsl=FixtureSource(*p);const auto candidate=TryForwardPixelCatalogSource(hlsl,*p);
        Require(bool(candidate),"Closed actual-CF synthetic HLSL refused");
        Require(candidate->starts_with("HELPER { while (helper) { break; } }\nmain {"),"Helper loop changed");
        Require(candidate->ends_with("}\nUNCHANGED_SUFFIX\n"),"Suffix changed");
        Require(ForwardTokenCount(*candidate,"nested_material();")==ForwardTokenCount(hlsl,"nested_material();"),"Material clause lost");
        Require(ForwardTokenCount(*candidate,"gow3NativeRasterOutput(iPos.xy, iPos.z,")==1,"Late FSI changed");
        Require(ForwardTokenCount(*candidate,"continue;")==0,"Guest continue retained");
        for(const std::string token:{"float2 gow3FsiDepthSlope = float2(ddx_coarse(iPos.z), ddy_coarse(iPos.z));\n",
            "oC0 *= g_ColorScale[0];\n","gow3NativeRasterOutput(iPos.xy, iPos.z,","return;"}) {
            auto changed=hlsl;const auto at=changed.find(token);Require(at!=changed.npos,"Missing fixture token");
            changed.erase(at,token.size());Require(!TryForwardPixelCatalogSource(changed,*p),"Malformed terminal/derivatives admitted");
        }
        auto unexpected=hlsl;unexpected.insert(unexpected.find("nested_material();"),"continue; ");
        Require(!TryForwardPixelCatalogSource(unexpected,*p),"Nested continue converted");
    }
    Require(!ForwardPixelCatalogSource("unknown-source"),"Unpinned source admitted");
    std::cout<<"PASS: checks="<<checks<<" actual_sources="<<kForwardPixelCatalogSources.size()<<" actual_uniform_paths="<<paths
        <<" clauses="<<clause_count<<" unchanged_material_and_late_epilogue=true\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
