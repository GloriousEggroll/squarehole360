#pragma once
#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
namespace gow3::renderer {
inline constexpr std::string_view kForwardVertexIdentity="native-vs-straight-cfg-f7db-v1";
struct ForwardVertexClause {uint32_t opcode=0,address=0,count=0;};
struct ForwardVertexControlFlow {std::vector<ForwardVertexClause> clauses;uint32_t terminal=0;};
// This first vertex candidate is deliberately not a general forward-CFG pass.
// f7db has only straight-line EXEC/ALLOC, terminal EXEC_END, and zero padding.
inline std::optional<ForwardVertexControlFlow> AnalyzeForwardVertexControlFlow(std::span<const uint8_t> source) {
    if(source.size()<36||source.size()>16u*1024*1024)return std::nullopt;
    const auto be=[&](size_t at){return uint32_t(source[at])<<24|uint32_t(source[at+1])<<16|uint32_t(source[at+2])<<8|source[at+3];};
    const uint32_t virt=be(4),physical=be(8),meta=be(24);
    if(be(0)!=0x102a1141u||be(16)||be(28)||be(32)||virt<36||uint64_t(virt)+physical!=source.size()||meta<36||meta>virt||virt-meta<36)return std::nullopt;
    const uint32_t offset=be(meta),bytes=be(meta+4);
    if(!bytes||bytes%12||bytes>4096u*12||offset>physical||bytes>physical-offset)return std::nullopt;
    size_t limit=bytes;const size_t code=size_t(virt)+offset;
    ForwardVertexControlFlow result;
    for(size_t at=0;at<limit;at+=12) {
        if(bytes-at<12||result.clauses.size()>=64)return std::nullopt;
        const uint32_t a=be(code+at),b=be(code+at+4),c=be(code+at+8);
        for(uint64_t bits:{uint64_t(a)|(uint64_t(b&65535)<<32),uint64_t(b>>16)|(uint64_t(c)<<16)}) {
            ForwardVertexClause clause;clause.opcode=uint32_t(bits>>44)&15;
            if(clause.opcode==1||clause.opcode==2) {
                clause.address=uint32_t(bits)&4095;clause.count=uint32_t(bits>>12)&7;
                if(!clause.address||!clause.count||uint64_t(clause.address+clause.count)*12>bytes)return std::nullopt;
                limit=std::min(limit,size_t(clause.address)*12);
            } else if(clause.opcode==0) {if(bits!=0)return std::nullopt;}
            else if(clause.opcode!=12)return std::nullopt; // All jumps, loops, calls, predicate and conditional EXEC refused.
            result.clauses.push_back(clause);
        }
    }
    // Pin the actual f7db container shape in addition to the source digest in
    // preparation. The final zero CF word is unreachable after an unconditional
    // EXEC_END. No jump can enter it because every jump opcode was refused.
    if(result.clauses.size()!=14||limit!=84||result.clauses.size()!=limit/6||
       result.clauses[12].opcode!=2||result.clauses[13].opcode!=0)return std::nullopt;
    result.terminal=12;
    for(size_t pc=0;pc<result.clauses.size();++pc) {
        const auto& c=result.clauses[pc];
        if(pc<12&&c.opcode!=1&&c.opcode!=12)return std::nullopt;
        if((c.opcode==1||c.opcode==2)&&size_t(c.address)*12<limit)return std::nullopt;
    }
    return result;
}
inline size_t ForwardVertexTokenCount(std::string_view text,std::string_view token) {
    size_t count=0,at=0;while((at=text.find(token,at))!=text.npos){++count;at+=token.size();}return count;
}
inline std::optional<std::string> TryForwardVertexControlFlowSource(std::string_view source,const ForwardVertexControlFlow& program) {
    constexpr std::string_view start="\n\tuint pc = 0;\n\twhile (true)\n\t{\n\t\tswitch (pc)\n\t\t{\n";
    constexpr std::string_view finish="\t\t\tbreak;\n\t\t}\n\t\tbreak;\n\t}\n";
    const auto begin=source.find(start);
    if(begin==source.npos||source.find(start,begin+1)!=source.npos||program.clauses.size()!=14||program.terminal!=12)return std::nullopt;
    const auto end=source.find(finish,begin+start.size());
    if(end==source.npos||source.find(finish,end+1)!=source.npos)return std::nullopt;
    auto body=source.substr(begin+start.size(),end-begin-start.size());
    std::string replacement="\n\t{\n";
    for(size_t pc=0;pc<program.clauses.size();++pc) {
        const std::string label="\t\tcase "+std::to_string(pc)+":\n";
        if(!body.starts_with(label))return std::nullopt;
        body.remove_prefix(label.size());const auto next=body.find("\t\tcase ");
        const bool last=pc+1==program.clauses.size();
        if((last&&next!=body.npos)||(!last&&next==body.npos))return std::nullopt;
        const auto clause=body.substr(0,last?body.size():next);
        if(clause.find("continue;")!=clause.npos||clause.find("break;")!=clause.npos||
           clause.find("pc = ")!=clause.npos||clause.find("case ")!=clause.npos||
           ForwardVertexTokenCount(clause,"return;")!=size_t(pc==program.terminal))return std::nullopt;
        if(pc==program.terminal&&ForwardVertexTokenCount(clause,"\toPos.xy += g_HalfPixelOffset * oPos.w;\n")!=1)return std::nullopt;
        if(pc>program.terminal) {if(!clause.empty())return std::nullopt;}
        else replacement+=clause; // Exact original clause bytes, including terminal outputs/return.
        if(!last)body.remove_prefix(next);
    }
    replacement+="\t}\n";
    return std::string(source.substr(0,begin))+replacement+std::string(source.substr(end+finish.size()));
}
} // namespace gow3::renderer
