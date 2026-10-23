#include "renderer/native/retained_opaque_scratch_sequence.h"
#include "renderer/native/retained_opaque_resource_budget.h"
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>
using namespace gow3::renderer;
namespace {
unsigned checks=0;
void Check(bool value,const char* message) {++checks;if(!value)throw std::runtime_error(message);}
void ExactSequenceAdmission() {
    int batch=0,foreign_batch=0,canonical=0,foreign_canonical=0;
    NativeRetainedOpaqueScratchSequence absent(nullptr),sequence(&batch);
    Check(!absent.CanPrepare(&canonical)&&!absent.BindCanonical(&canonical),"absent FIFO admitted");
    Check(!sequence.CanPrepare(nullptr)&&!sequence.BindCanonical(nullptr),"null canonical admitted");
    Check(!sequence.CanRecord(&batch,&canonical,true,true,true,true),"unbound descriptor admitted");
    Check(sequence.BindCanonical(&canonical)&&sequence.BindCanonical(&canonical),"exact immutable bind refused");
    Check(!sequence.BindCanonical(&foreign_canonical)&&!sequence.CanPrepare(&foreign_canonical),"canonical retarget admitted");
    for(unsigned bits=0;bits<64;++bits) {
        const auto* selected_batch=(bits&1)?&batch:&foreign_batch;
        const auto* selected_canonical=(bits&2)?&canonical:&foreign_canonical;
        const bool active=bits&4,queued=bits&8,allocation=bits&16,lease=bits&32;
        Check(sequence.CanRecord(selected_batch,selected_canonical,active,queued,allocation,lease)==(bits==63),
              "foreign/inactive/standalone/changed-allocation/consumed Prepared admission changed");
    }
    // Pressure before recording neither consumes nor poisons the immutable lease.
    Check(!sequence.CanRecord(&batch,&canonical,false,true,true,true),"pressure scope recorded");
    Check(sequence.CanRecord(&batch,&canonical,true,true,true,true),"fresh pressure retry refused");
    // Accepted work with an uncertain recording/queue failure is terminal.
    sequence.FailRecording();
    Check(sequence.failed()&&!sequence.CanPrepare(&canonical)&&!sequence.BindCanonical(&canonical)&&
          !sequence.CanRecord(&batch,&canonical,true,true,true,true),"failed FIFO scratch reused");
}
struct Resource {
    int* destructions;
    const void* const canonical;
    const unsigned descriptors=2;
    explicit Resource(int& count,const void* buffer):destructions(&count),canonical(buffer){}
    ~Resource(){++*destructions;}
};
void LeaseAndBudgetOwnership() {
    int batch=0,canonical=0,destroyed=0;
    NativeRetainedOpaqueScratchSequence sequence(&batch);Check(sequence.BindCanonical(&canonical),"bind failed");
    auto writer=std::make_shared<Resource>(destroyed,&canonical);
    const auto* original=writer.get();std::weak_ptr<Resource> witness=writer;
    std::vector<std::shared_ptr<void>> first_fence,second_fence;
    auto restore_prepared=writer,store_prepared=writer,next_restore_prepared=writer;
    Check(sequence.CanRecord(&batch,&canonical,true,true,true,bool(restore_prepared)),"restore lease refused");
    first_fence.push_back(restore_prepared);restore_prepared.reset();
    Check(!sequence.CanRecord(&batch,&canonical,true,true,true,bool(restore_prepared)),"Prepared recorded twice");
    first_fence.push_back(store_prepared);store_prepared.reset();
    second_fence.push_back(next_restore_prepared);next_restore_prepared.reset();
    Check(writer.get()==original&&writer->canonical==&canonical&&writer->descriptors==2,"lease rebound between generations");
    writer.reset();first_fence.clear();
    Check(destroyed==0&&!witness.expired(),"earlier fence or writer death freed later packet scratch");
    second_fence.clear();Check(destroyed==1&&witness.expired(),"last actual packet failed to retire scratch");
    // Existing unpooled image limit is independent and unchanged. The shared
    // scratch follows transfer ownership above; it cannot credit image space.
    NativeRetainedOpaqueResourceBudget images;
    constexpr uint64_t maximum=32u*1024*1024,pair=8u*1024*1024;
    for(unsigned i=0;i<4;++i)Check(images.TryCharge(pair,maximum),"original image admission narrowed");
    Check(!images.TryCharge(1,maximum)&&images.bytes==maximum,"shared scratch bypassed image cap");
    images.Release(pair);Check(images.TryCharge(pair,maximum),"actual image retirement did not restore capacity");
    images.Release(maximum);Check(images.bytes==0&&images.peak==maximum,"image charge conservation lost");
}
}
int main() {
    try {ExactSequenceAdmission();LeaseAndBudgetOwnership();
        std::cout<<"retained opaque shared scratch: PASS "<<checks<<" CPU admission/ownership checks; no GPU ordering proof\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
