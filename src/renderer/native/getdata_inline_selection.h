#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <utility>

namespace gow3::renderer {
// Complete exact PPC instruction spans from the supported user-provided XEX.
// Runtime capability validation is startup-only; this stores no guest pointers.
// The words are XDK code, so they are generated locally from the user's
// executable (tools/porting/generate_getdata.py), not distributed.
#include "generated/native-getdata/getdata_xex_words.inc"

// Enable/disable control is serialized by the bridge capture lock. The
// atomic read is the sole retry-path decision and owns neither guest nor GPU state.
class GetDataInlineSelection {
public:
    void Reset() noexcept {enabled_.clear(std::memory_order_release);}
    bool enabled()const noexcept {return enabled_.test(std::memory_order_acquire);}
    // Read must refuse inaccessible words. Exceptions propagate with selection
    // already reset; no owner/address is retained after this startup decision.
    template<class Read>
    bool TryEnable(bool requested,bool native_sink,bool available,Read&& read) {
        Reset();
        if(!requested||!native_sink||!available)return false;
        const auto exact=[&](uint32_t address,const auto& words) {
            for(std::size_t i=0;i<words.size();++i) {
                uint32_t value=0;
                if(!read(address+uint32_t(i)*4,value)||value!=words[i])return false;
            }
            return true;
        };
        if(!exact(kInlineGetDataAddress,kInlineGetDataInstructions)||
           !exact(kInlineSave26Address,kInlineSave26Instructions)||
           !exact(kInlineRestore26Address,kInlineRestore26Instructions))return false;
        enabled_.test_and_set(std::memory_order_release);return true;
    }
private:
    // atomic_flag operations are guaranteed lock-free; no spin is used.
    std::atomic_flag enabled_=ATOMIC_FLAG_INIT;
};

// The real ABI wrapper and differential fixture share this dispatch. Exactly
// one permanently linked full body executes; original exceptions propagate.
template<class Context,class Original,class Inline>
void InvokeGetDataSelected(Context& context,uint8_t* base,bool selected,Original&& original,Inline&& leaf) {
    if(selected)std::forward<Inline>(leaf)(context,base);
    else std::forward<Original>(original)(context,base);
}
} // namespace gow3::renderer
