#include "renderer/native/fsi_state_forward_cfg.h"
#include "renderer/native/fsi_state_specialization_descriptor.h"
#include "renderer/native/content_hash.h"
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
    std::string text="CANONICAL_COMMON\nHELPER { while (helper) { break; } }\nmain {\nfloat2 gow3FsiDepthSlope = float2(ddx_coarse(iPos.z), ddy_coarse(iPos.z));\n\tuint pc = 0;\n\twhile (true)\n\t{\n\t\tswitch (pc)\n\t\t{\n";
    for(size_t i=0;i<p.instructions.size();++i) {
        text+="\t\tcase "+std::to_string(i)+":\n";const auto& x=p.instructions[i];
        if(x.opcode==11) {
            if(x.unconditional)text+="\t\t\tpc = "+std::to_string(x.target)+";\n\t\t\tcontinue;\n";
            else text+="\t\t\tif (((vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + "+std::to_string(512+x.bool_address/32*4)+") >> "+std::to_string(x.bool_address%32)+") & 1) "+(x.condition?"!=":"==")+" 0)\n\t\t\t{\n\t\t\t\tpc = "+std::to_string(x.target)+";\n\t\t\t\tcontinue;\n\t\t\t}\n";
        } else if(x.opcode==1||x.opcode==2) {
            text+="\t\t\t{\n\t\t\t\tif (guest) { nested_material(); }\n\t\t\t}\n";
            if(x.opcode==2)text+="\t\t\toC0 *= g_ColorScale[0];\n\t\t\tgow3NativeRasterOutput(iPos.xy, iPos.z, iFace, oC0, float4(0,0,0,0), float4(0,0,0,0), float4(0,0,0,0), 1u, false, gow3FsiDepthSlope);\n\t\t\treturn;\n";
        }
    }
    return text+"\t\t\tbreak;\n\t\t}\n\t\tbreak;\n\t}\n}\nUNCHANGED_SUFFIX\n";
}
}
int main(int argc,char** argv)try {
    if(argc!=2)throw std::runtime_error("Usage: gow3-native-fsi-state-forward-cfg-test actual49f2-source.bin");
    const auto source=Read(argv[1]);
    Require(ContentHash(source)==kFsiStateForwardCfgPixel,"Actual source pin mismatch");
    const auto p=AnalyzeForwardPixelCatalog(source);
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
    const auto hlsl=FixtureSource(*p);
    constexpr std::string_view canonical_common="CANONICAL_COMMON\n",state_common="STATE_COMMON_WITH_UNCHANGED_DEPTH_OFFSET_RESTORE\n";
    auto state=hlsl;state.replace(state.find(canonical_common),canonical_common.size(),state_common);
    const auto candidate=TryComposeFsiStateForwardCfgSource(hlsl,state,canonical_common,state_common,*p);
    Require(bool(candidate),"Valid closed composition refused");
    Require(candidate->starts_with(state_common),"Specialized common changed by forward transform");
    Require(candidate->find("HELPER { while (helper) { break; } }")!=candidate->npos,"Common/helper loop changed");
    Require(candidate->ends_with("}\nUNCHANGED_SUFFIX\n"),"Suffix changed");
    Require(ForwardTokenCount(*candidate,"nested_material();")==ForwardTokenCount(hlsl,"nested_material();"),"Material clause lost");
    Require(ForwardTokenCount(*candidate,"gow3NativeRasterOutput(iPos.xy")==1,"Late FSI count changed");
    Require(ForwardTokenCount(*candidate,"oC0 *= g_ColorScale[0];")==1,"Color scale changed");
    Require(ForwardTokenCount(*candidate,"ddx_coarse(iPos.z)")==1&&candidate->find("ddx_coarse")<candidate->find("if (pc <= "),"Derivatives moved inside divergent guest control");
    Require(ForwardTokenCount(*candidate,"continue;")==0,"Guest continue retained");
    Require(ForwardTokenCount(*candidate,"if (pc <= ")==28,"Linear guard count changed");
    auto corrupt=state;corrupt.replace(corrupt.find("nested_material();"),18,"changed_float_fp();");
    Require(!TryComposeFsiStateForwardCfgSource(hlsl,corrupt,canonical_common,state_common,*p),"Changed guest arithmetic passed common restoration");
    Require(!TryComposeFsiStateForwardCfgSource(hlsl,state+std::string(state_common),canonical_common,state_common,*p),"Ambiguous common admitted");
    Require(!TryComposeFsiStateForwardCfgSource(hlsl,state,"",state_common,*p),"Empty common admitted");
    for(const std::string token:{"\t\tcase 13:\n","\t\t\t\tcontinue;","oC0 *= g_ColorScale[0];","float2 gow3FsiDepthSlope = float2(ddx_coarse(iPos.z), ddy_coarse(iPos.z));\n"}) {
        auto changed_baseline=hlsl,changed_state=state;
        const auto at=changed_baseline.find(token),at_state=changed_state.find(token);
        Require(at!=changed_baseline.npos&&at_state!=changed_state.npos,"Missing refusal fixture token");
        changed_baseline.erase(at,token.size());changed_state.erase(at_state,token.size());
        Require(!TryComposeFsiStateForwardCfgSource(changed_baseline,changed_state,canonical_common,state_common,*p),"Malformed derivative/wrapper/late-FSI shape admitted");
    }
    NativeFsiStateRecipe recipe{{std::string(kFsiStateForwardCfgVertex),std::string(kFsiStateForwardCfgPixel),
        {3,1,1,4,0,0},20,-1},{}};
    Require(FsiStateForwardCfgAllowed(recipe),"Actual pair/raster refused");
    recipe.generic.raster[1]=2;Require(FsiStateForwardCfgAllowed(recipe),"Second actual raster refused");
    recipe.generic.raster[0]=1;Require(!FsiStateForwardCfgAllowed(recipe),"Uncovered catalog raster admitted");
    recipe.generic.raster={3,1,1,4,0,0};recipe.generic.pixel.reset();
    Require(!FsiStateForwardCfgAllowed(recipe),"Builtin admitted");
    recipe.generic.pixel=std::string(kFsiStateForwardCfgPixel);recipe.generic.vertex[0]='0';
    Require(!FsiStateForwardCfgAllowed(recipe),"Uncovered companion vertex admitted");
    NativeFsiStateSpecialization exact{};
    for(size_t word=0;word<exact.words.size();++word) {
        auto changed=exact;changed.words[word]=1;
        Require(changed!=exact,"Full15-word PSO key lost a dimension");
    }
    const NativeFsiSpecializationConstants owned(1794,exact);
    Require(owned.values.size()==17&&owned.values[0].index==0&&owned.values[1].index==32,"Owned specialization ABI changed");
    std::cout<<"PASS: checks="<<checks<<" actual_uniform_paths=16 clauses=28 common_restoration_exact=true unchanged_material_derivatives_late_fsi=true\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
