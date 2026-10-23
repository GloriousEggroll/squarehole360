#pragma once
#include "renderer/gears_bridge.h"
#include <algorithm>
#include <deque>
#include <iterator>
#include <span>
#include <stdexcept>

namespace gow3::renderer {
struct OwnedDrawPacket {uint32_t address=0,offset=0,bytes=0;};
struct ParsedCommandSpan {
    std::vector<OwnedDrawPacket> draws;
    bool replacement_safe=true;
    // Captured bytes alone cannot prove a predicate. A callback for the sole
    // predicated draw proves its execution, provided no other packet depends
    // on predicates or a control/memory side effect.
    bool executing_draw_proves_safe=false;
};
inline uint32_t PacketWord(std::span<const uint8_t> bytes,size_t offset) {
    if(offset+4>bytes.size())throw std::runtime_error("Truncated native PM4 span");
    return uint32_t(bytes[offset])<<24|uint32_t(bytes[offset+1])<<16|
        uint32_t(bytes[offset+2])<<8|bytes[offset+3];
}
inline ParsedCommandSpan ParseCommandSpan(const GraphicsCommandSpan& span) {
    if(!span.generation||span.address%4||span.bytes.empty()||span.bytes.size()%4||
       span.bytes.size()>1024*1024||uint64_t(span.address)+span.bytes.size()>0x20000000ull)
        throw std::runtime_error("Invalid native PM4 ownership span");
    ParsedCommandSpan result;
    unsigned predicated_draws=0;bool predicated_state=false,control_dependency=false;
    std::vector<size_t> trailing_cache_flushes;
    // WriteRegister may publish SCRATCH_REG0..7 to guest memory when UMSK
    // enables them. Captured command bytes do not include that mask/address,
    // so a register packet touching these is a control dependency too.
    const auto writes_scratch=[](uint32_t first,size_t count) {
        return count&&first<=0x57f&&uint64_t(first)+count>0x578;
    };
    for(size_t at=0;at<span.bytes.size();) {
        const uint32_t header=PacketWord(span.bytes,at),type=header>>30;
        const size_t words=header==0?1:type==1?3:type==2?1:((header>>16)&0x3fff)+2;
        if(words>(span.bytes.size()-at)/4)throw std::runtime_error("Truncated native PM4 packet");
        if(type==0&&header&&writes_scratch(header&0x7fff,header&0x8000?1:words-1))control_dependency=true;
        if(type==1&&(writes_scratch(header&0x7ff,1)||writes_scratch((header>>11)&0x7ff,1)))control_dependency=true;
        if(type==3) {
            // The command processor may skip any predicated type-3 packet
            // using the current bin mask, including state preceding a draw.
            // API order alone cannot prove that its whole effect executes.
            if(header&1)result.replacement_safe=false;
            const unsigned opcode=(header>>8)&127;
            if(opcode==0x22||opcode==0x36) {
                const size_t initiator=opcode==0x22?2:1;
                if(words<=initiator)throw std::runtime_error("Missing native PM4 draw initiator");
                const auto value=PacketWord(span.bytes,at+initiator*4);
                const auto source=(value>>6)&3;
                if(source==0&&words<initiator+3)throw std::runtime_error("Missing native PM4 index buffer");
                if(source!=0&&source!=2)control_dependency=true;
                if(header&1)++predicated_draws;
                // Conditional draw ownership is not inferred from API order.
                if(opcode==0x22&&(PacketWord(span.bytes,at+4)&0x100))control_dependency=true;
                result.draws.push_back({span.address+uint32_t(at),uint32_t(at),uint32_t(words*4)});
            } else {
                if(header&1)predicated_state=true;
                if((opcode==0x55||opcode==0x56)&&writes_scratch(PacketWord(span.bytes,at+4)&0xffff,words-2))
                    control_dependency=true;
                if(opcode==0x2d&&((PacketWord(span.bytes,at+4)>>16)&0xff)>4)control_dependency=true;
                if(opcode==0x46&&words==2&&!(header&1)&&PacketWord(span.bytes,at+4)==6) {
                    // The driver's Resolve appends EVENT_WRITE(CACHE_FLUSH)
                    // after its copy draw. This count-one event only writes
                    // VGT_EVENT_INITIATOR in the SDK; it has no memory payload
                    // or consumer. Native completion is fenced before returning
                    // from that draw, then the SDK executes this trailing event.
                    // Never allow the event to divide multiple owned draws.
                    trailing_cache_flushes.push_back(at);
                } else if(opcode!=0x10&&opcode!=0x2d&&opcode!=0x55&&opcode!=0x56) {
                    // Memory writes, fences, conditionals and indirect buffers
                    // may split the API effect; observing one draw is not proof
                    // that a complete native API effect can cross them.
                    control_dependency=true;
                }
            }
        }
        at+=words*4;
    }
    for(const auto at:trailing_cache_flushes)
        if(result.draws.empty()||at<result.draws.back().offset)control_dependency=true;
    result.replacement_safe=result.replacement_safe&&!control_dependency;
    result.executing_draw_proves_safe=!control_dependency&&!predicated_state&&
        predicated_draws==1&&result.draws.size()==1;
    return result;
}

// The caller serializes enqueue/inspect/consume with its existing queue lock.
// Inspection never consumes a packet before native execution or handback has
// succeeded. Recycled addresses must match the oldest generation's exact bytes.
class PacketOwnership {
public:
    struct Match {
        uint64_t generation=0,api_sequence=0;
        size_t draw_index=0;
        bool last_draw=false,replacement_safe=false,predicate_proven_by_callback=false;
        bool native_committed=false;
    };
    void Enqueue(GraphicsCommandSpan span) {
        if(span.generation<=last_generation_)throw std::runtime_error("Non-monotonic native packet generation");
        auto parsed=ParseCommandSpan(span);
        if(parsed.draws.empty())throw std::runtime_error("Native ownership span contains no draw");
        if(pending_.size()>=8192||retained_bytes_+span.bytes.size()>16u*1024*1024)
            throw std::runtime_error("Native command ownership queue limit exceeded");
        last_generation_=span.generation;retained_bytes_+=span.bytes.size();
        pending_.push_back({std::move(span),std::move(parsed),0,false});
    }
    std::optional<Match> Inspect(uint32_t address,std::span<const uint8_t> bytes) const {
        return InspectWithProof(address,bytes,false);
    }
    // Only call from the command processor's IssueDraw callback, after that
    // packet's predicate test passed. Never infer this from a queued API span.
    std::optional<Match> InspectExecuting(uint32_t address,std::span<const uint8_t> bytes) const {
        return InspectWithProof(address,bytes,true);
    }
private:
    std::optional<Match> InspectWithProof(uint32_t address,std::span<const uint8_t> bytes,bool executing) const {
        if(pending_.empty())return {};
        const auto& entry=pending_.front();const auto& draw=entry.parsed.draws[entry.next];
        if(draw.address!=address)return {};
        if(bytes.size()!=draw.bytes||!std::equal(bytes.begin(),bytes.end(),entry.span.bytes.begin()+draw.offset))
            throw std::runtime_error("Native PM4 bytes changed before command consumption");
        const bool predicate_proven=executing&&entry.parsed.executing_draw_proves_safe;
        return Match{entry.span.generation,entry.span.api_sequence,entry.next,
            entry.next+1==entry.parsed.draws.size(),entry.parsed.replacement_safe||predicate_proven,predicate_proven,entry.native_committed};
    }
public:
    // A whole API effect may replace several draw packets. Once its first
    // callback has successfully committed native execution, every remaining
    // packet belongs to that effect even if later capture/preflight fails.
    // Call under the queue lock before consuming the first packet. The caller
    // must also retain the span while native execution runs outside that lock.
    void CommitSpan(const Match& match) {
        if(pending_.empty()||pending_.front().span.generation!=match.generation||
           pending_.front().span.api_sequence!=match.api_sequence||match.draw_index||
           pending_.front().next||pending_.front().native_committed||!match.replacement_safe)
            throw std::runtime_error("Invalid native packet span commitment");
        pending_.front().native_committed=true;
    }
    bool HasCommittedSpan() const {return !pending_.empty()&&pending_.front().native_committed;}
    uint64_t CommittedGeneration() const {return HasCommittedSpan()?pending_.front().span.generation:0;}
    void Consume(const Match& match) {
        if(pending_.empty()||pending_.front().span.generation!=match.generation||
           pending_.front().span.api_sequence!=match.api_sequence||pending_.front().next!=match.draw_index)
            throw std::runtime_error("Stale native packet completion");
        auto& entry=pending_.front();
        if(++entry.next==entry.parsed.draws.size()) {
            retained_bytes_-=entry.span.bytes.size();pending_.pop_front();
        }
    }
    size_t pending_spans() const {return pending_.size();}
    // A producer failure may discard future snapshots, but cannot authorize
    // replay of packets whose whole effect has already completed natively.
    void ClearUncommitted() {
        if(!HasCommittedSpan()) {pending_.clear();retained_bytes_=0;return;}
        pending_.erase(std::next(pending_.begin()),pending_.end());
        retained_bytes_=pending_.front().span.bytes.size();
    }
    void Clear() {
        if(HasCommittedSpan())throw std::runtime_error("Cannot forget a committed native packet span");
        ClearUncommitted();
    }
    // Only after the command processor has stopped consuming this stream.
    void ClearForShutdown() {pending_.clear();retained_bytes_=0;}
private:
    struct Entry {GraphicsCommandSpan span;ParsedCommandSpan parsed;size_t next;bool native_committed;};
    std::deque<Entry> pending_;
    uint64_t last_generation_=0;
    size_t retained_bytes_=0;
};
} // namespace gow3::renderer
