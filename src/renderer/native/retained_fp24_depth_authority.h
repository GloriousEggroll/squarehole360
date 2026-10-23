#pragma once
#include <array>
#include <cstdint>
#include <stdexcept>

namespace gow3::renderer {
using NativeRetainedFp24DepthLayout = std::array<uint32_t,6>;
inline constexpr NativeRetainedFp24DepthLayout kNativeRetainedFp24DepthLayout{1280,720,1280,0,1,1};
// Metadata only. Scene separately proves source/pipeline identity, exact
// physical-page tokens and accepted GPU resource ownership.
class NativeRetainedFp24DepthAuthority {
public:
    struct Ticket {uint64_t generation=0,depth=0;};
    NativeRetainedFp24DepthAuthority()=default;
    NativeRetainedFp24DepthAuthority(const NativeRetainedFp24DepthAuthority&)=delete;
    NativeRetainedFp24DepthAuthority& operator=(const NativeRetainedFp24DepthAuthority&)=delete;
    NativeRetainedFp24DepthAuthority(NativeRetainedFp24DepthAuthority&&)=delete;
    NativeRetainedFp24DepthAuthority& operator=(NativeRetainedFp24DepthAuthority&&)=delete;
    bool present()const noexcept{return generation_!=0;}
    bool dirty()const noexcept{return dirty_;}
    bool failed()const noexcept{return failed_;}
    Ticket ticket()const noexcept{return {generation_,depth_};}
    bool Reusable(const NativeRetainedFp24DepthLayout& layout,uint64_t depth)const noexcept {
        return !failed_&&present()&&layout==kNativeRetainedFp24DepthLayout&&depth&&depth==depth_;
    }
    void Admit(uint64_t generation,const NativeRetainedFp24DepthLayout& layout,uint64_t depth) {
        Check();
        if(dirty_||!generation||generation<=last_generation_||layout!=kNativeRetainedFp24DepthLayout||!depth)
            Fail("Invalid retained FP24 depth admission");
        generation_=last_generation_=generation;depth_=depth;
    }
    // Only the original accepted depth RecordSurfaceWrite advances this token.
    // Color and stencil have no authority or token in this depth-only path.
    void AcceptedDraw(Ticket before,uint64_t observed,uint64_t after,bool audited_writer,bool discarded) {
        Check();
        if(!audited_writer||discarded||!Matches(before)||observed!=depth_||after<=depth_)
            Fail("Accepted retained FP24 depth writer lost physical authority");
        depth_=after;dirty_=true;
    }
    void BeforeMaterialize(uint64_t depth) {
        Check();if(present()&&depth!=depth_)Fail("Foreign write overlaps retained FP24 depth");
    }
    void Materialized(Ticket before,bool accepted) {
        Check();if(!Matches(before))Fail("Retained FP24 depth export ticket changed");
        if(!accepted)return;
        generation_=depth_=0;dirty_=false;
    }
private:
    uint64_t generation_=0,last_generation_=0,depth_=0;
    bool dirty_=false,failed_=false;
    bool Matches(Ticket t)const noexcept{return present()&&t.generation==generation_&&t.depth==depth_;}
    void Check()const {if(failed_)throw std::logic_error("Retained FP24 depth authority is poisoned");}
    [[noreturn]]void Fail(const char* message){failed_=true;throw std::logic_error(message);}
};
}
