#pragma once
#include "fragment_invocation_ledger.h"
#include "plume_render_interface.h"
#include <memory>
#include <nlohmann/json.hpp>
#include <string>

namespace gow3::renderer {
// Linked and hooked only by the standalone replay target. No runtime switch,
// canonical shader changes, substitute draw, added fence or guest query.
class NativeFragmentInvocationCensus {
public:
    using Token=NativeFragmentInvocationLedger::Token;
    using Tag=NativeFragmentInvocationLedger::Tag;
    struct Key {
        std::string vertex,pixel;
        std::array<uint32_t,6> raster{};
        uint32_t vertex_variant=0,pixel_variant=0,depth_control=0,color_mask=0,blend_mask=0,width=0,height=0;
        uint32_t pitch=0,guest_depth_base=0,guest_depth_format=0;
        std::array<uint32_t,4> guest_color_bases{},guest_color_formats{},guest_color_masks{},guest_blend_controls{};
        int32_t color_format=-1,depth_format=-1;
        bool queried=false,hardware_depth=false,hardware_depth_write=false;
        uint32_t hardware_depth_compare=0;
        bool operator==(const Key&) const=default;
    };
    NativeFragmentInvocationCensus(plume::RenderDevice&,plume::RenderCommandQueue&,uint32_t capacity,
                                  bool pipeline_statistics_actually_enabled);
    ~NativeFragmentInvocationCensus();
    uint32_t Intern(Key) noexcept;
    bool Begin(Token,Tag,plume::RenderCommandList&) noexcept;
    void End(Token) noexcept;
    void Accept(Token) noexcept;
    void Cancel(Token) noexcept;
    void RejectMetadata() noexcept;
    void Discarded() noexcept;
    void Retire(std::span<const Token>,uint64_t submission) noexcept;
    nlohmann::json Report() const;
private:
    struct State;std::unique_ptr<State> state_;
};
class NativeFragmentInvocationScope {
public:
    NativeFragmentInvocationScope(NativeFragmentInvocationCensus& owner,
        NativeFragmentInvocationCensus::Token token,NativeFragmentInvocationCensus::Tag tag,
        plume::RenderCommandList& commands):owner_(&owner),token_(token) {
        if(!owner_->Begin(token_,tag,commands))owner_=nullptr;
    }
    ~NativeFragmentInvocationScope() noexcept {if(owner_&&!accepted_)owner_->Cancel(token_);}
    NativeFragmentInvocationScope(const NativeFragmentInvocationScope&)=delete;
    void End() noexcept {if(owner_)owner_->End(token_);}
    void Accepted() noexcept {accepted_=true;if(owner_)owner_->Accept(token_);}
private:
    NativeFragmentInvocationCensus* owner_;NativeFragmentInvocationCensus::Token token_;bool accepted_=false;
};
}
