#include "renderer/native/retained_opaque_resource_budget.h"
#include "renderer/native/retained_opaque_authority.h"
#include "renderer/native/retired_image_pool.h"
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
using namespace gow3::renderer;
namespace {
constexpr uint64_t kMaximum=32u*1024*1024,kAllocation=8u*1024*1024;
uint64_t checks=0;
void Check(bool value,const char* message) {++checks;if(!value)throw std::runtime_error(message);}
struct Child {
    std::string name;std::vector<std::string>* destruction;
    Child(std::string label,std::vector<std::string>& log):name(std::move(label)),destruction(&log){}
    Child(const Child&)=delete;Child& operator=(const Child&)=delete;
    ~Child(){destruction->push_back(name);}
};
struct Resource {
    std::shared_ptr<NativeRetainedOpaqueResourceBudget> budget;
    std::vector<std::string>* destruction;
    uint64_t bytes=0,serial=0;
    std::unique_ptr<Child> color,depth,framebuffer;
    Resource(std::shared_ptr<NativeRetainedOpaqueResourceBudget> b,std::vector<std::string>& d,uint64_t id,uint64_t size)
        :budget(std::move(b)),destruction(&d),bytes(size),serial(id) {
        if(!budget->TryCharge(bytes,kMaximum))throw std::runtime_error("Fixture admission exceeded actual budget");
        try {
            color=std::make_unique<Child>("color",d);
            depth=std::make_unique<Child>("depth",d);
            framebuffer=std::make_unique<Child>("framebuffer",d);
        }catch(...) {framebuffer.reset();depth.reset();color.reset();budget->Release(bytes);throw;}
    }
    ~Resource(){framebuffer.reset();depth.reset();color.reset();budget->Release(bytes);}
};
using Pool=RetiredImagePool<Resource>;
void BudgetPolicy() {
    NativeRetainedOpaqueResourceBudget budget;
    Check(!budget.TryCharge(0,kMaximum),"zero allocation admitted");
    Check(!budget.TryCharge(kMaximum+1,kMaximum),"oversize allocation admitted");
    for(unsigned i=0;i<4;++i)Check(budget.TryCharge(kAllocation,kMaximum),"bounded live allocation refused");
    Check(budget.bytes==kMaximum&&budget.peak==kMaximum,"actual peak differs");
    Check(!budget.TryCharge(1,kMaximum),"fifth active or idle allocation exceeded bound");
    budget.Release(kAllocation);Check(budget.TryCharge(kAllocation,kMaximum),"retirement failed to free exact bytes");
    budget.Release(kMaximum);Check(budget.bytes==0&&budget.peak==kMaximum,"budget conservation lost");
}
void FenceOwnedAliases() {
    std::vector<std::string> destruction;
    auto budget=std::make_shared<NativeRetainedOpaqueResourceBudget>();Pool pool(1,kMaximum);
    auto owner=pool.Share(std::make_unique<Resource>(budget,destruction,1,kAllocation),1280,720,kAllocation,17);
    std::shared_ptr<Child> transfer_color(owner,owner->color.get()),transfer_depth(owner,owner->depth.get());
    auto draw_slot=owner;
    owner.reset();
    Check(!pool.Take(1280,720,17),"materialized host generation released an unfenced transfer/draw bundle");
    transfer_color.reset();transfer_depth.reset(); // The simulated transfer fence completed.
    Check(!pool.Take(1280,720,17),"transfer fence alone released an unretired DrawSlot");
    draw_slot.reset(); // Only this actual simulated draw-fence retirement releases the last owner.
    const auto before=pool.statistics();Check(before.returns==1&&before.idle_images==1&&before.idle_bytes==kAllocation,"last owner return not conserved");
    Check(budget->bytes==kAllocation,"idle allocation bypassed hard budget");
    Check(!pool.Take(1280,720,18)&&!pool.Take(720,1280,17),"description/format mismatch reused bundle");
    auto reused=pool.Take(1280,720,17);Check(bool(reused)&&reused->serial==1,"retired physical bundle not reused");
    Check(destruction.empty(),"unfenced resource was destroyed");
    reused.reset();Check(budget->bytes==0,"destroyed physical bundle remained charged");
    Check(destruction.size()==3&&destruction[0]=="framebuffer"&&destruction[1]=="depth"&&destruction[2]=="color","reverse GPU dependency destruction lost");
}
void ReservedStoreAndScratchLeases() {
    std::vector<std::string> destruction;
    auto budget=std::make_shared<NativeRetainedOpaqueResourceBudget>();Pool attachments(1,kMaximum),scratch(2,kMaximum);
    auto image=attachments.Share(std::make_unique<Resource>(budget,destruction,11,kAllocation),1280,720,kAllocation);
    auto restore=scratch.Share(std::make_unique<Resource>(budget,destruction,12,kAllocation),1280,720,kAllocation);
    auto store=scratch.Share(std::make_unique<Resource>(budget,destruction,13,kAllocation),1280,720,kAllocation);
    auto queued_restore=restore;auto reserved_store=store;
    restore.reset();store.reset();
    Check(!scratch.Take(1280,720),"reserved or unfenced conversion scratch reused");
    Check(budget->bytes==3*kAllocation,"both conversion reserves not charged");
    Check(!budget->TryCharge(2*kAllocation,kMaximum),"overlap scratch escaped bound");
    auto queued_store=reserved_store;reserved_store.reset();image.reset();
    Check(!scratch.Take(1280,720),"unfenced store scratch reused");
    queued_restore.reset();auto retired_restore=scratch.Take(1280,720);Check(retired_restore&&retired_restore->serial==12,"actual restore retirement did not permit scratch reuse");
    Check(!scratch.Take(1280,720),"later store was considered fenced by earlier completion");
    queued_store.reset();auto retired_store=scratch.Take(1280,720);Check(retired_store&&retired_store->serial==13,"store retirement did not release scratch");
    retired_restore.reset();retired_store.reset();
    Check(budget->bytes==kAllocation,"idle attachment accounting lost after scratch retirement");
}
void FreshAuthorityOnSameResource() {
    NativeRetainedOpaqueAuthority first;first.Admit(101,kNativeRetainedOpaqueLayout,11,21);
    const auto old=first.ticket();first.AcceptedDraw(old,11,21,12,22);first.Materialized(first.ticket(),true);
    NativeRetainedOpaqueAuthority next;next.Admit(102,kNativeRetainedOpaqueLayout,12,22);
    Check(next.ticket().generation==102&&!next.dirty()&&next.Reusable(kNativeRetainedOpaqueLayout,12,22),"fresh generation reused stale authority");
    bool rejected=false;try{next.AcceptedDraw(old,12,22,13,23);}catch(const std::logic_error&){rejected=true;}
    Check(rejected&&next.failed(),"old physical resource ticket was accepted");
}
void SharedImmutableScratchWithinGeneration() {
    std::vector<std::string> destruction;
    auto budget=std::make_shared<NativeRetainedOpaqueResourceBudget>();Pool attachments(1,kMaximum),scratch(2,kMaximum);
    auto image1=attachments.Share(std::make_unique<Resource>(budget,destruction,31,kAllocation),1280,720,kAllocation);
    auto conversion1=scratch.Share(std::make_unique<Resource>(budget,destruction,32,kAllocation),1280,720,kAllocation);
    auto restore_packet=conversion1,store_reserve=conversion1;
    conversion1.reset();
    Check(restore_packet.get()==store_reserve.get(),"within-generation immutable lease changed pointer");
    Check(!scratch.Take(1280,720),"restore+store shared lease returned before any fence");
    // A separate generation uses a separate still-owned conversion allocation.
    auto image2=attachments.Share(std::make_unique<Resource>(budget,destruction,33,kAllocation),1280,720,kAllocation);
    auto conversion2=scratch.Share(std::make_unique<Resource>(budget,destruction,34,kAllocation),1280,720,kAllocation);
    Check(budget->bytes==kMaximum&&budget->peak==kMaximum,"two live generations failed exact combined bound");
    Check(conversion2.get()!=restore_packet.get()&&image1.get()!=image2.get(),"cross-generation reuse ignored live owners");
    Check(!budget->TryCharge(1,kMaximum),"third generation bypassed actual charged bound");
    auto store_packet=store_reserve;
    restore_packet.reset(); // Earlier restore fence only.
    Check(!scratch.Take(1280,720),"restore retirement stole scratch still used by later store");
    store_reserve.reset();
    Check(!scratch.Take(1280,720),"logical materialization stole scratch still in its store packet");
    store_packet.reset(); // Actual later store fence, last scratch owner.
    auto reused=scratch.Take(1280,720);Check(reused&&reused->serial==32,"last actual fence did not release shared conversion lease");
    auto next=scratch.Share(std::move(reused),1280,720,kAllocation);
    Check(next->serial==32&&next.get()!=conversion2.get(),"retired immutable scratch failed independent next-generation acquisition");
}
void IdleEvictionAndPoolDeath() {
    std::vector<std::string> destruction;
    auto budget=std::make_shared<NativeRetainedOpaqueResourceBudget>();std::shared_ptr<Resource> surviving;
    {
        Pool pool(1,kMaximum);
        auto a=pool.Share(std::make_unique<Resource>(budget,destruction,21,kAllocation),1280,720,kAllocation);
        auto b=pool.Share(std::make_unique<Resource>(budget,destruction,22,kAllocation),1280,720,kAllocation);
        a.reset();b.reset();Check(pool.statistics().evictions==1&&budget->bytes==kAllocation,"idle eviction failed to release exact allocation");
        auto taken=pool.Take(1280,720);Check(taken&&taken->serial==22,"deterministic idle eviction removed newer bundle");
        surviving=pool.Share(std::move(taken),1280,720,kAllocation);
    }
    Check(surviving&&budget->bytes==kAllocation,"pool death released a live fence lease");
    surviving.reset();Check(budget->bytes==0,"lease outliving pool leaked resource/budget");
}
void TwoSimultaneouslyRetiredAttachments() {
    std::vector<std::string> destruction;
    auto budget=std::make_shared<NativeRetainedOpaqueResourceBudget>();
    {
        Pool attachments(2,kMaximum),scratch(2,kMaximum);
        auto image1=attachments.Share(std::make_unique<Resource>(budget,destruction,61,kAllocation),1280,720,kAllocation);
        auto image2=attachments.Share(std::make_unique<Resource>(budget,destruction,62,kAllocation),1280,720,kAllocation);
        auto scratch1=scratch.Share(std::make_unique<Resource>(budget,destruction,63,kAllocation),1280,720,kAllocation);
        auto scratch2=scratch.Share(std::make_unique<Resource>(budget,destruction,64,kAllocation),1280,720,kAllocation);
        const auto* physical1=image1.get();const auto* physical2=image2.get();
        std::weak_ptr<Resource> old1=image1,old2=image2;
        auto transfer1=image1,transfer2=image2,draw1=image1,draw2=image2;
        image1.reset();image2.reset();
        Check(budget->bytes==kMaximum&&budget->peak==kMaximum,"two image/scratch generations escaped the original32MiB budget");
        Check(!attachments.Take(1280,720)&&!old1.expired()&&!old2.expired(),"unfenced generation entered two-slot idle pool");
        // Both transfer lists retire, but the corresponding DrawSlots still
        // own each whole image/framebuffer bundle on the same modeled fence.
        transfer1.reset();transfer2.reset();
        Check(!attachments.Take(1280,720)&&attachments.statistics().returns==0,"transfer completion stole an accepted DrawSlot bundle");
        draw1.reset();
        Check(old1.expired()&&!old2.expired()&&attachments.statistics().idle_images==1,"first retired owner lost control-block separation");
        Check(!budget->TryCharge(1,kMaximum),"idle plus active owners bypassed the total actual allocation bound");
        draw2.reset(); // Same covering-fence retirement, second generation.
        const auto retired=attachments.statistics();
        Check(old2.expired()&&retired.returns==2&&retired.idle_images==2&&retired.idle_bytes==2*kAllocation&&retired.evictions==0,
            "simultaneous retirement evicted a reusable attachment bundle");
        Check(destruction.empty()&&budget->bytes==kMaximum,"two idle attachment bundles were destroyed or became uncharged");
        auto returned1=attachments.Take(1280,720),returned2=attachments.Take(1280,720);
        Check(returned1&&returned2&&returned1->serial==61&&returned2->serial==62&&returned1.get()==physical1&&returned2.get()==physical2,
            "two subsequent admissions allocated or aliased instead of taking the exact retired bundles");
        Check(attachments.statistics().hits==2&&attachments.statistics().evictions==0&&attachments.statistics().idle_images==0,
            "two-slot reuse statistics do not match the independent physical-owner oracle");
        auto next1=attachments.Share(std::move(returned1),1280,720,kAllocation);
        auto next2=attachments.Share(std::move(returned2),1280,720,kAllocation);
        Check(old1.expired()&&old2.expired()&&next1.get()!=next2.get(),"physical reuse resurrected an old generation control block");
        Check(!attachments.Take(1280,720),"later generation reused either unretired new owner");
        Check(budget->bytes==kMaximum&&!budget->TryCharge(1,kMaximum),"two-slot metadata increased the unchanged live+idle byte budget");
        next1.reset();next2.reset();scratch1.reset();scratch2.reset();
        Check(attachments.statistics().idle_images==2&&scratch.statistics().idle_images==2&&budget->bytes==kMaximum,
            "last actual owners did not return all four charged resources");
    }
    Check(budget->bytes==0&&budget->peak==kMaximum&&destruction.size()==12,"pool teardown leaked any actual allocation");
    for(size_t i=0;i<destruction.size();i+=3)
        Check(destruction[i]=="framebuffer"&&destruction[i+1]=="depth"&&destruction[i+2]=="color",
            "two-slot pool changed framebuffer/image dependency destruction order");
}
}
int main() {
    try {
        BudgetPolicy();FenceOwnedAliases();ReservedStoreAndScratchLeases();FreshAuthorityOnSameResource();SharedImmutableScratchWithinGeneration();IdleEvictionAndPoolDeath();TwoSimultaneouslyRetiredAttachments();
        std::cout<<"retained resource pool CPU ownership policy: PASS "<<checks<<" checks; simulated fence ownership only, no GPU proof\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
