#pragma once
#include <array>
#include <cstdint>
#include <limits>

namespace gow3::renderer {
// Pure metadata only. Writer must supply identities read from actual objects,
// and receipt observations from its constructor-bound Native FIFO. A caller's
// numeric frontier by itself is not proof of conversion acceptance.
class NativeRetainedOpaqueAttachmentSequence {
public:
    using Ticket = std::array<uint64_t,3>;
    struct Binding {
        uintptr_t writer=0,batch=0,queue=0,canonical=0,canonical_vk=0;
        uintptr_t canonical_allocation=0,attachments=0;
        uint64_t canonical_extent=0,canonical_allocation_bytes=0;
        bool operator==(const Binding&) const = default;
        bool valid()const noexcept {
            return writer&&batch&&queue&&canonical&&canonical_vk&&
                canonical_allocation&&attachments&&canonical_extent==2048u*5120&&
                canonical_allocation_bytes>=canonical_extent;
        }
    };
    struct Token {
        uint64_t nonce=0; Ticket ticket{}; bool store=false;
        bool operator==(const Token&)const=default;
        explicit operator bool()const noexcept{return nonce!=0;}
    };
    struct Receipt {uint64_t accepted=0,stages=0,packets=0;};
    enum class State {Idle,PreparedRestore,RecordingRestore,RecordedRestore,
                      Active,PreparedStore,RecordingStore,RecordedStore,Exported,Failed};

    NativeRetainedOpaqueAttachmentSequence()=default;
    NativeRetainedOpaqueAttachmentSequence(const NativeRetainedOpaqueAttachmentSequence&)=delete;
    NativeRetainedOpaqueAttachmentSequence& operator=(const NativeRetainedOpaqueAttachmentSequence&)=delete;
    State state()const noexcept{return state_;}
    bool failed()const noexcept{return state_==State::Failed;}
    void Fail()noexcept{state_=State::Failed;}
    bool Bind(const Binding& binding)noexcept {
        if(failed()||!binding.valid())return false;
        if(bound_)return binding==binding_;
        if(state_!=State::Idle)return false;
        binding_=binding;bound_=true;return true;
    }
    Token Prepare(const Binding& binding,Ticket ticket,bool store)noexcept {
        if(!Matches(binding)||!ticket[0]||!ticket[1]||!ticket[2]||
           next_nonce_==std::numeric_limits<uint64_t>::max())return {};
        if(store) {
            if(state_!=State::Active||ticket[0]!=generation_||
               ticket[1]<restored_[1]||ticket[2]<restored_[2])return {};
        } else {
            if(state_!=State::Idle||ticket[0]<=last_generation_)return {};
            generation_=last_generation_=ticket[0];restored_=ticket;
        }
        token_={++next_nonce_,ticket,store};
        state_=store?State::PreparedStore:State::PreparedRestore;
        return token_;
    }
    bool CanRecord(const Binding& binding,Token token,bool active_scope,
                   bool clean_scope,bool healthy_queue)const noexcept {
        return Matches(binding)&&token&&token==token_&&active_scope&&clean_scope&&healthy_queue&&
            state_==(token.store?State::PreparedStore:State::PreparedRestore);
    }
    // Call only after successful reservations, immediately BEFORE BeginStage.
    bool BeginRecord(const Binding& binding,Token token,Receipt before,
                     bool active_scope,bool clean_scope,bool healthy_queue)noexcept {
        if(!CanRecord(binding,token,active_scope,clean_scope,healthy_queue)||
           before.accepted==std::numeric_limits<uint64_t>::max()||
           before.stages==std::numeric_limits<uint64_t>::max()||
           before.packets==std::numeric_limits<uint64_t>::max())return false;
        before_=before;state_=token.store?State::RecordingStore:State::RecordingRestore;return true;
    }
    bool Recorded(Token token,uint64_t stages,bool has_recording)noexcept {
        if(!token||token!=token_||state_!=(token.store?State::RecordingStore:State::RecordingRestore)||
           !has_recording||stages!=before_.stages+1) {Fail();return false;}
        state_=token.store?State::RecordedStore:State::RecordedRestore;return true;
    }
    // After RunTransferScope returned, read the SAME actual queue and batch.
    // Exactly one stage and sealed packet tie the +1 frontier to this nonce.
    bool Acknowledge(const Binding& binding,Token token,Receipt after,
                     bool closed_scope,bool healthy_queue)noexcept {
        if(!Matches(binding)||!token||token!=token_||
           state_!=(token.store?State::RecordedStore:State::RecordedRestore)||
           !closed_scope||!healthy_queue||after.accepted!=before_.accepted+1||
           after.stages!=before_.stages+1||after.packets!=before_.packets+1) {Fail();return false;}
        if(token.store)exported_=token.ticket;
        state_=token.store?State::Exported:State::Active;token_={};return true;
    }
    bool CanClose(const Binding& binding,Ticket current,bool dirty)const noexcept {
        return Matches(binding)&&current[0]==generation_&&
            (dirty?(state_==State::Exported&&current==exported_):
                   (state_==State::Active&&current==restored_));
    }
    // Writer validates CanClose, then calls authority.Materialized, then this.
    // Unexpected failure after authority mutation remains permanently poisoned.
    bool Close(const Binding& binding,Ticket current,bool dirty)noexcept {
        if(!CanClose(binding,current,dirty)){Fail();return false;}
        generation_=0;restored_={};exported_={};state_=State::Idle;return true;
    }
    // Prepared's destructor: an old consumed token cannot cancel newer work.
    // Before-record pressure retries do not call Cancel until abandonment.
    bool Cancel(Token token)noexcept {
        if(!token||token!=token_)return false;
        if(state_==State::PreparedRestore) {
            generation_=0;restored_={};token_={};state_=State::Idle;return true;
        }
        if(state_==State::PreparedStore) {token_={};state_=State::Active;return true;}
        Fail();return false;
    }
private:
    bool Matches(const Binding& binding)const noexcept {
        return !failed()&&bound_&&binding==binding_;
    }
    Binding binding_{};bool bound_=false;
    State state_=State::Idle;
    Token token_{};Ticket restored_{},exported_{};
    uint64_t generation_=0,last_generation_=0,next_nonce_=0;
    Receipt before_{};
};
}
