#pragma once
#include <array>
#include <cstdint>
#include <stdexcept>

namespace gow3::renderer {
// A single exact pair of authoritative attachments. This is metadata only:
// it cannot prove shader/container identity, resource ownership or visibility.
// Scene must authenticate those separately before admitting a generation.
using NativeRetainedOpaqueLayout = std::array<uint32_t,12>;
inline constexpr NativeRetainedOpaqueLayout kNativeRetainedOpaqueLayout{
    1280,720,1280,720,3,0,1280,720,1280,0,1,1};
class NativeRetainedOpaqueAuthority {
public:
    NativeRetainedOpaqueAuthority()=default;
    NativeRetainedOpaqueAuthority(const NativeRetainedOpaqueAuthority&)=delete;
    NativeRetainedOpaqueAuthority& operator=(const NativeRetainedOpaqueAuthority&)=delete;
    NativeRetainedOpaqueAuthority(NativeRetainedOpaqueAuthority&&)=delete;
    NativeRetainedOpaqueAuthority& operator=(NativeRetainedOpaqueAuthority&&)=delete;
    struct Ticket {uint64_t generation=0,color=0,depth=0;};
    bool present()const noexcept{return generation_!=0;}
    bool dirty()const noexcept{return dirty_;}
    bool failed()const noexcept{return failed_;}
    Ticket ticket()const noexcept{return {generation_,color_,depth_};}
    bool Reusable(const NativeRetainedOpaqueLayout& layout,uint64_t color,uint64_t depth)const noexcept {
        return !failed_&&present()&&layout==layout_&&color&&depth&&color==color_&&depth==depth_;
    }
    // Scene may initialize this private preparing object before conversion,
    // but must not publish it until owned canonical-to-image work is accepted.
    void Admit(uint64_t generation,const NativeRetainedOpaqueLayout& layout,uint64_t color,uint64_t depth) {
        Check();
        if(dirty_||!generation||generation<=last_generation_||layout!=kNativeRetainedOpaqueLayout||!color||!depth)
            Fail("Invalid retained opaque generation admission");
        generation_=last_generation_=generation;layout_=layout;color_=color;depth_=depth;
    }
    // Only after the graphics list is accepted and both original packed
    // RecordSurfaceWrite calls have advanced their physical page tokens.
    void AcceptedDraw(Ticket before,uint64_t observed_color,uint64_t observed_depth,
                      uint64_t new_color,uint64_t new_depth) {
        Check();
        if(!Matches(before)||observed_color!=color_||observed_depth!=depth_||
           new_color<=color_||new_depth<=depth_)
            Fail("Retained opaque writer lost physical authority");
        color_=new_color;depth_=new_depth;dirty_=true;
    }
    // This is the original readonly draw's metadata update, not a physical
    // image write. A failed admission leaves all tokens and dirty owners intact.
    // Once GPU work was accepted, any wrong ticket/version is a fatal authority
    // violation: never recopy stale canonical EDRAM over the authoritative image.
    bool PromoteAcceptedReadonly(const NativeRetainedOpaqueLayout& layout,Ticket before,
        uint64_t observed_color,uint64_t observed_depth,uint64_t new_color,uint64_t new_depth,
        bool accepted,bool audited_no_write,bool discarded) {
        Check();
        if(!accepted)return false;
        if(!audited_no_write||discarded)Fail("Accepted retained query is not an audited readonly draw");
        if(!dirty_||layout!=layout_||!Matches(before)||observed_color!=color_||observed_depth!=depth_||
           new_color!=color_||new_depth<=depth_)
            Fail("Retained readonly query lost exact physical authority");
        depth_=new_depth;return true; // Color and dirty status deliberately unchanged.
    }
    void BeforeMaterialize(uint64_t color,uint64_t depth) {
        Check();
        if(present()&&(color!=color_||depth!=depth_))
            Fail("Cannot materialize retained opaque image over a foreign write");
    }
    // A false or throwing recorder must retain dirty authority and all owners.
    // Accepted FIFO export restores the canonical buffer's authority; Scene
    // still keeps image owners in the transfer packet until its actual fence.
    void Materialized(Ticket before,bool accepted) {
        Check();
        if(!Matches(before))Fail("Retained opaque materialization ticket changed");
        if(!accepted)return;
        generation_=color_=depth_=0;dirty_=false;
    }
private:
    NativeRetainedOpaqueLayout layout_{};
    uint64_t generation_=0,last_generation_=0,color_=0,depth_=0;
    bool dirty_=false,failed_=false;
    bool Matches(Ticket t)const noexcept {
        return present()&&t.generation==generation_&&t.color==color_&&t.depth==depth_;
    }
    void Check()const {if(failed_)throw std::logic_error("Retained opaque authority is poisoned");}
    [[noreturn]]void Fail(const char* reason){failed_=true;throw std::logic_error(reason);}
};
}
