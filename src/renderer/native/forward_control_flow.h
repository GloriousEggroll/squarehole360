#pragma once
#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace gow3::renderer {
inline constexpr std::string_view kForwardControlFlowIdentity="native-fsi-forward-cfg-49f2-v1";
struct ForwardCfInstruction {
    uint32_t opcode=0,address=0,count=0,target=0,bool_address=0;
    bool unconditional=false,condition=false;
};
struct ForwardCfProgram {std::vector<ForwardCfInstruction> instructions;};
inline std::optional<ForwardCfProgram> AnalyzeForwardControlFlow(std::span<const uint8_t> source) {
    if(source.size()<36||source.size()>16u*1024*1024)return std::nullopt;
    const auto be=[&](size_t at) {return uint32_t(source[at])<<24|uint32_t(source[at+1])<<16|
        uint32_t(source[at+2])<<8|source[at+3];};
    const uint32_t flags=be(0),virt=be(4),physical=be(8),meta=be(24);
    if(flags!=0x102a1100u||be(16)||be(28)||be(32)||virt<36||
       uint64_t(virt)+physical!=source.size()||meta<36||meta>virt||virt-meta<32)return std::nullopt;
    const uint32_t offset=be(meta),bytes=be(meta+4);
    if(!bytes||bytes%12||bytes>4096u*12||offset>physical||bytes>physical-offset)return std::nullopt;
    const size_t code=size_t(virt)+offset;
    size_t limit=bytes;
    ForwardCfProgram result;
    for(size_t at=0;at<limit;at+=12) {
        if(bytes-at<12||result.instructions.size()>=64)return std::nullopt;
        const uint32_t a=be(code+at),b=be(code+at+4),c=be(code+at+8);
        const uint64_t pair[]={uint64_t(a)|(uint64_t(b&65535)<<32),uint64_t(b>>16)|(uint64_t(c)<<16)};
        for(const uint64_t bits:pair) {
            ForwardCfInstruction item;item.opcode=uint32_t(bits>>44)&15;
            if(item.opcode==1||item.opcode==2) {
                item.address=uint32_t(bits)&4095;item.count=uint32_t(bits>>12)&7;
                if(!item.address||!item.count||uint64_t(item.address+item.count)*12>bytes)return std::nullopt;
                limit=std::min(limit,size_t(item.address)*12);
            } else if(item.opcode==11) {
                item.target=uint32_t(bits)&8191;item.unconditional=(bits>>13)&1;
                // Audited direct forward shape only: no per-lane predicate,
                // reverse/relative addressing or reserved control bits.
                constexpr uint64_t jump_allowed=(uint64_t(15)<<44)|(uint64_t(1)<<42)|
                    (uint64_t(255)<<34)|0x3fffull;
                if(bits&~jump_allowed)return std::nullopt;
                item.bool_address=uint32_t(bits>>34)&255;item.condition=(bits>>42)&1;
                if(item.unconditional&&(item.bool_address||item.condition))return std::nullopt;
            } else if(item.opcode!=12)return std::nullopt; // Includes loops/calls/conditional EXEC_END/unknown.
            result.instructions.push_back(item);
        }
    }
    if(result.instructions.empty()||result.instructions.size()>64||limit%12||
       result.instructions.size()!=limit/6||result.instructions.back().opcode!=2)return std::nullopt;
    for(size_t pc=0;pc<result.instructions.size();++pc) {
        const auto& item=result.instructions[pc];
        if(item.opcode==2&&pc+1!=result.instructions.size())return std::nullopt;
        if((item.opcode==1||item.opcode==2)&&size_t(item.address)*12<limit)return std::nullopt;
        if(item.opcode==11&&(item.target<=pc||item.target>=result.instructions.size()))return std::nullopt;
    }
    return result;
}
inline size_t ForwardTokenCount(std::string_view text,std::string_view token) {
    size_t count=0,at=0;while((at=text.find(token,at))!=text.npos){++count;at+=token.size();}return count;
}
// Transform only the exact generated guest-control wrapper. Guest clause bytes,
// material branches, helper functions and the complete late FSI epilogue stay intact.
inline std::optional<std::string> TryForwardControlFlowSource(std::string_view source,const ForwardCfProgram& program) {
    constexpr std::string_view start="\n\tuint pc = 0;\n\twhile (true)\n\t{\n\t\tswitch (pc)\n\t\t{\n";
    constexpr std::string_view finish="\t\t\tbreak;\n\t\t}\n\t\tbreak;\n\t}\n";
    const auto begin=source.find(start);
    if(begin==source.npos||source.find(start,begin+1)!=source.npos||program.instructions.empty())return std::nullopt;
    const auto end=source.find(finish,begin+start.size());
    if(end==source.npos||source.find(finish,end+1)!=source.npos)return std::nullopt;
    auto body=source.substr(begin+start.size(),end-begin-start.size());
    std::string replacement="\n\tuint pc = 0;\n\t{\n";
    for(size_t pc=0;pc<program.instructions.size();++pc) {
        const std::string label="\t\tcase "+std::to_string(pc)+":\n";
        if(!body.starts_with(label))return std::nullopt;
        body.remove_prefix(label.size());
        const auto next=body.find("\t\tcase ");
        const bool last=pc+1==program.instructions.size();
        if((last&&next!=body.npos)||(!last&&next==body.npos))return std::nullopt;
        std::string clause(body.substr(0,last?body.size():next));
        const auto& instruction=program.instructions[pc];
        if(ForwardTokenCount(clause,"return;")!=size_t(last)||clause.find("break;")!=clause.npos||
           clause.find("case ")!=clause.npos)return std::nullopt;
        if(instruction.opcode==11) {
            std::string expected;
            if(instruction.unconditional)expected="\t\t\tpc = "+std::to_string(instruction.target)+";\n\t\t\tcontinue;\n";
            else expected="\t\t\tif (((vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + "+
                std::to_string(512+instruction.bool_address/32*4)+") >> "+std::to_string(instruction.bool_address%32)+
                ") & 1) "+(instruction.condition?"!=":"==")+" 0)\n\t\t\t{\n\t\t\t\tpc = "+
                std::to_string(instruction.target)+";\n\t\t\t\tcontinue;\n\t\t\t}\n";
            if(clause!=expected||ForwardTokenCount(clause,"continue;")!=1||ForwardTokenCount(clause,"pc = ")!=1)return std::nullopt;
            const auto at=clause.find("continue;");clause.erase(at,9);
        } else if(clause.find("continue;")!=clause.npos||clause.find("pc = ")!=clause.npos)return std::nullopt;
        replacement+="\t\tif (pc <= "+std::to_string(pc)+"u)\n\t\t{\n"+clause+"\t\t}\n";
        if(!last)body.remove_prefix(next);
    }
    replacement+="\t}\n";
    return std::string(source.substr(0,begin))+replacement+std::string(source.substr(end+finish.size()));
}
} // namespace gow3::renderer
