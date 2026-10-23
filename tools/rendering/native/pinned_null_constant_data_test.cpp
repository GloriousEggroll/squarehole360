#include "renderer/native/pinned_null_constant_data.h"
#include <array>
#include <algorithm>
#include <iostream>
#include <vector>

using namespace gow3::renderer;
namespace {
uint64_t checks=0;
void Require(bool condition,const char* message) {
    ++checks;if(!condition)throw std::runtime_error(message);
}
template<class F>void Refuse(F&& f) {
    bool rejected=false;try{f();}catch(const std::runtime_error&){rejected=true;}
    Require(rejected,"Malformed packing input accepted");
}
}
int main()try {
    std::vector<uint8_t> guest(kGuestFloatConstantOffset+kFloatConstantBytes);
    std::array<uint8_t,sizeof(NativeFsiShaderConstants)> shared{};
    std::vector<uint8_t> original(kFloatConstantBytes+shared.size());
    std::vector<uint8_t> candidate(original.size(),0xa7);
    // Every float register is bit data, including signed zero, infinities,
    // signaling/quiet NaNs and arbitrary exponent/mantissa patterns.
    constexpr uint32_t specials[]{0,0x80000000,0x7f800000,0xff800000,
        0x7fc12345,0x7f812345,0xffffffff,0x00000001};
    uint32_t random=0x8925afe1;
    for(unsigned iteration=0;iteration<512;++iteration) {
        for(size_t at=0;at<kFloatConstantBytes;at+=4) {
            random^=random<<13;random^=random>>17;random^=random<<5;
            const auto bits=at<sizeof(specials)?specials[at/4]:random;
            const auto be=std::endian::native==std::endian::little?std::byteswap(bits):bits;
            std::memcpy(guest.data()+kGuestFloatConstantOffset+at,&be,4);
        }
        for(size_t i=0;i<shared.size();++i)shared[i]=uint8_t(i+iteration*17);
        const auto before=candidate;
        PackDrawConstants(guest,shared,original);
        PackPinnedNullDrawConstants(guest,shared,candidate);
        for(size_t i=0;i<candidate.size();++i) {
            const auto expected=i<kNativeNullDepthFloatPrefixBytes||i>=kFloatConstantBytes?original[i]:before[i];
            Require(candidate[i]==expected,"Pinned packing altered a read byte or an omitted byte");
        }
        // A retired slot can switch to an arbitrary generic module. Its full
        // original pack overwrites every previously omitted byte.
        PackDrawConstants(guest,shared,candidate);
        Require(candidate==original,"Pinned to generic slot reuse changed full packing");
    }
    const auto before=candidate;
    Refuse([&]{PackPinnedNullDrawConstants(std::span(guest).first(guest.size()-1),shared,candidate);});
    Refuse([&]{PackPinnedNullDrawConstants(guest,std::span(shared).first(shared.size()-1),candidate);});
    Refuse([&]{PackPinnedNullDrawConstants(guest,shared,std::span(candidate).first(candidate.size()-1));});
    Require(candidate==before,"Rejected input partly wrote destination");
    std::vector<uint8_t> aliased(2*candidate.size());
    Refuse([&]{PackPinnedNullDrawConstants(std::span(aliased).first(guest.size()),shared,
        std::span(aliased).first(candidate.size()));});
    Refuse([&]{PackPinnedNullDrawConstants(guest,std::span(aliased).first(shared.size()),
        std::span(aliased).first(candidate.size()));});
    Require(kFloatConstantBytes+shared.size()==10816&&
        kNativeNullDepthFloatPrefixBytes+shared.size()==2800,"Packed ABI or footprint changed");
    std::cout<<"PASS "<<checks<<" pinned-null constant packing checks\n";
    return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
