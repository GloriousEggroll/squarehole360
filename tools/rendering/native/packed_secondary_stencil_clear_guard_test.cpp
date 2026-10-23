#include "renderer/native/packed_secondary_stencil_clear.h"
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace gow3::renderer;
int main() {
    uint64_t checks=0;const auto check=[&](bool x){++checks;if(!x)throw std::runtime_error("Secondary stencil guard failed");};
    NativePackedSecondaryStencilClearInput x{true,false,true,640,360,640,1440,1,167772800,32,0.f,{0,0,640,360}};
    check(NativePackedSecondaryStencilClearAllowed(x));
    const auto refuse=[&](auto change){auto bad=x;change(bad);check(!NativePackedSecondaryStencilClearAllowed(bad));};
    refuse([](auto& a){a.native_fsi=false;});refuse([](auto& a){a.query_active=true;});
    refuse([](auto& a){a.source_depth=false;});refuse([](auto& a){a.width=1280;});
    refuse([](auto& a){a.height=720;});refuse([](auto& a){a.pitch=1120;});
    refuse([](auto& a){a.base=0;});refuse([](auto& a){a.format=0;});
    refuse([](auto& a){a.surface_info^=1;});refuse([](auto& a){a.surface_info|=0x10000;});
    refuse([](auto& a){a.flags=16;});refuse([](auto& a){a.flags=48;});
    refuse([](auto& a){a.depth=-1.f;});refuse([](auto& a){a.depth=std::numeric_limits<float>::infinity();});
    refuse([](auto& a){a.depth=std::numeric_limits<float>::quiet_NaN();});
    refuse([](auto& a){a.rectangle[0]=-1;});refuse([](auto& a){a.rectangle[1]=-1;});
    refuse([](auto& a){a.rectangle[2]=641;});refuse([](auto& a){a.rectangle[3]=361;});
    refuse([](auto& a){a.rectangle[2]=0;});refuse([](auto& a){a.rectangle[3]=0;});
    auto bottom=x;bottom.rectangle={0,352,640,360};check(NativePackedSecondaryStencilClearAllowed(bottom));
    auto last=x;last.rectangle={639,359,640,360};check(NativePackedSecondaryStencilClearAllowed(last));
    for(unsigned bits=0;bits<128;++bits) {
        const bool defined=bits&1,present=bits&2,dirty=bits&4,current=bits&8,
            receipt=bits&16,other_dirty=bits&32,zero=bits&64;
        const uint64_t packed=zero?0:19,host=current?packed:7;
        using S=NativePackedSecondaryStencilClearSource;
        const auto expected=(!defined||dirty||receipt||other_dirty||zero)?S::Refuse:
            present&&current?S::CurrentHost:S::Packed;
        check(NativePackedSecondaryStencilClearChooseSource(defined,present,dirty,host,packed,receipt,other_dirty)==expected);
    }
    std::cout<<"PASS "<<checks<<" secondary stencil layout/provenance checks; no GPU/FP24/padding/Scene proof\n";
}
