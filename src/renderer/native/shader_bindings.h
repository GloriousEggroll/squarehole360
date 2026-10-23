#pragma once
#include "renderer/gears_bridge.h"
#include <algorithm>
#include <cstdint>
#include <set>
#include <span>
#include <stdexcept>
#include <utility>

namespace gow3::renderer {
// Enumerate every statically referenced fetch, including conditional EXEC
// clauses. Used by both immutable capture and native operation planning.
inline std::pair<std::set<unsigned>, std::set<unsigned>> ShaderFetchRequirements(std::span<const uint8_t> blob) {
    auto Word=[](std::span<const uint8_t> bytes,size_t offset) {
        if(offset>bytes.size()||bytes.size()-offset<4)throw std::runtime_error("Truncated shader binding metadata");
        return uint32_t(bytes[offset])<<24|uint32_t(bytes[offset+1])<<16|
            uint32_t(bytes[offset+2])<<8|bytes[offset+3];
    };
    const auto flags=Word(blob,0), virtual_bytes=Word(blob,4), physical_bytes=Word(blob,8), metadata=Word(blob,24);
    if ((flags & 1) && (flags & 0x41) != 0x41) throw std::runtime_error("Vertex shader has not been linked");
    const auto offset=Word(blob,metadata), size=Word(blob,metadata+4);
    if (uint64_t(virtual_bytes)+physical_bytes != blob.size() || uint64_t(offset)+size > physical_bytes || size%12)
        throw std::runtime_error("Invalid live shader extent");
    const auto code=blob.subspan(virtual_bytes+offset,size);
    std::set<unsigned> vertices, textures;
    size_t limit=code.size();
    for (size_t pos=0; pos<limit; pos+=12) {
        const uint32_t x=Word(code,pos), y=Word(code,pos+4), z=Word(code,pos+8);
        for (uint64_t cf : {uint64_t(x)|(uint64_t(y&65535)<<32), uint64_t(y>>16)|(uint64_t(z)<<16)}) {
            const auto opcode=cf>>44;
            if (!((opcode>=1 && opcode<=6) || opcode==13 || opcode==14)) continue;
            const size_t address=cf&4095, count=(cf>>12)&7;
            if (!address || (address+count)*12>code.size()) throw std::runtime_error("Invalid live shader EXEC extent");
            limit=std::min(limit,address*12);
            for (unsigned i=0; i<count; ++i) {
                if (!((cf>>(16+2*i))&1)) continue;
                const auto a=Word(code,(address+i)*12), b=Word(code,(address+i)*12+4);
                const unsigned fetch_opcode=a&31;
                if (!fetch_opcode && !((b>>30)&1)) vertices.insert(((a>>20)&31)*3+((a>>25)&3));
                else if (fetch_opcode==1 || fetch_opcode==16 || fetch_opcode==17 || fetch_opcode==19)
                    textures.insert((a>>20)&31);
            }
        }
    }
    return {std::move(vertices),std::move(textures)};
}

inline std::optional<std::pair<std::set<unsigned>,std::set<unsigned>>> SnapshotShaderInputs(std::span<const GraphicsBoundaryBlock> captured) {
  std::array<const std::vector<uint8_t>*,4> blocks{};
  for (const auto& block : captured)
    if (block.kind>=2 && block.kind<=5) blocks[block.kind-2]=&block.bytes;
  std::pair<std::set<unsigned>,std::set<unsigned>> required;
  try {
    for (unsigned stage=0;stage<2;++stage) {
      const auto* header=blocks[stage*2];const auto* code=blocks[stage*2+1];
      if (!header || !code || header->empty()) {
        if (stage) continue; // Depth-only draws may have no pixel shader.
        return {};
      }
      std::vector<uint8_t> shader=*header;
      shader.insert(shader.end(),code->begin(),code->end());
      const auto inputs=ShaderFetchRequirements(shader);
      required.first.insert(inputs.first.begin(),inputs.first.end());
      required.second.insert(inputs.second.begin(),inputs.second.end());
    }
  } catch (const std::exception&) {
    // Preserve the complete diagnostic event if metadata is unsupported.
    // The ordered planner, not this snapshot optimization, decides fallback.
    return {};
  }
  return required;
}

} // namespace gow3::renderer
