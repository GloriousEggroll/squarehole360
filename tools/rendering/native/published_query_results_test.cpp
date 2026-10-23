#include "renderer/native/published_query_results.h"
#include <atomic>
#include <iostream>
#include <stdexcept>
using namespace gow3::renderer;
size_t checks=0;
void Check(bool ok) {++checks;if(!ok)throw std::runtime_error("Published query policy mismatch");}
int main() {
    for(bool observe:{false,true}) {
    PublishedQueryResults results(2,observe);
    PublishedQueryHeader h{0x2000,9,0x9000,1,9,0x40};
    const auto issue=[&](uint32_t handle,uint64_t gen,uint32_t frame,const PublishedQueryHeader& header) {
        results.Invalidate(handle);results.End(handle,gen,frame,header.PhysicalReport());
        Check(results.Bind(handle,header));
    };
    Check(!results.Ready(1,h));issue(1,1,0,h);Check(!results.Ready(1,h));
    Check(!results.Publish(1,1,0x9000));Check(!results.Publish(0,2,0x9000));Check(!results.Publish(0,1,0xa000));
    Check(results.Publish(0,1,0x9000));Check(results.Ready(1,h));
    auto changed=h;changed.flags|=0x80;Check(results.Ready(1,changed));
    for(unsigned n=0;n<7;++n) {
        changed=h;
        switch(n) {case 0:changed.device++;break;case 1:changed.serial++;break;case 2:changed.report+=64;break;
        case 3:changed.count=2;break;case 4:changed.type=10;break;case 5:changed.flags=0;break;case 6:changed.flags^=1;break;}
        Check(!results.Ready(1,changed));
    }
    bool foreign_ready=true,foreign_bound=true;
    std::thread foreign([&]{foreign_ready=results.Ready(1,h);foreign_bound=results.Bind(1,h);});foreign.join();
    Check(!foreign_ready&&!foreign_bound&&!results.Ready(1,h));
    issue(1,2,0,h);results.Invalidate(1);Check(!results.Publish(0,2,0x9000));Check(!results.Ready(1,h));
    issue(1,3,0,h);issue(2,4,0,h);Check(!results.Publish(0,3,0x9000));Check(results.Publish(0,4,0x9000));
    Check(!results.Ready(1,h)&&results.Ready(2,h));
    changed=h;changed.report=0xa000;issue(3,5,0,changed);
    auto third=h;third.report=0xb000;issue(4,6,0,third);
    Check(!results.Ready(2,h)&&!results.Publish(0,5,0xa000));
    // Repeated End generation invalidates both records; unknown identity fails closed.
    results.End(5,6,0,0xc000);Check(!results.Publish(0,6,0xb000));
    PublishedQueryResults empty(0);empty.End(1,1,0,0x9000);Check(!empty.Bind(1,h)&&!empty.Publish(0,1,0x9000));
    // Publication may beat post-Issue binding, but cannot beat the actual fence.
    results.End(1,8,0,0x9000);Check(results.Publish(0,8,0x9000));Check(!results.Ready(1,h));
    Check(results.Bind(1,h)&&results.Ready(1,h));
    results.Invalidate(0,0x9000);Check(!results.Ready(1,h));
    // Simultaneous fence notifications cannot resurrect earlier report generations.
    std::atomic<bool> done=false;
    std::thread publisher([&]{for(uint64_t g=10;g<100010;++g)results.Publish(uint32_t(g),g,0x9000);done=true;});
    for(uint64_t g=10;g<100010;++g) {
        h.serial=uint32_t(g);issue(1,g,uint32_t(g),h);
        Check(!results.Publish(uint32_t(g-1),g-1,0x9000));
        Check(results.Publish(uint32_t(g),g,0x9000));Check(results.Ready(1,h));
        results.Invalidate(1);Check(!results.Ready(1,h));
    }
    publisher.join();Check(done);
    const auto census=results.ObservePolls();uint64_t total=0;
    for(auto count:census.counts)total+=count;
    Check(total==census.latest.sequence);Check((total!=0)==observe);
    }
    PublishedQueryResults census(2,true);
    PublishedQueryHeader h{0x2000,9,0x9000,1,9,0x40};
    using S=PublishedQueryResults::PollStatus;
    const auto expect=[&](S status,bool result) {
        const auto before=census.ObservePolls();
        Check(census.Ready(1,h)==result);
        const auto after=census.ObservePolls();
        Check(after.latest.sequence==before.latest.sequence+1&&after.latest.status==status);
        Check(after.counts[size_t(status)]==before.counts[size_t(status)]+1);
    };
    expect(S::Untracked,false);census.End(1,100,7,0x9000);expect(S::Unbound,false);
    Check(census.Bind(1,h));expect(S::Pending,false);
    const auto pending=census.ObservePolls().latest;
    Check(pending.generation==100&&pending.frame==7&&pending.address==0x9000);
    std::thread foreign([&]{Check(!census.Ready(1,h));});foreign.join();
    Check(census.ObservePolls().latest.status==S::WrongIssuer);
    h.type=10;expect(S::Ineligible,false);h.type=9;h.serial++;expect(S::Mismatch,false);h.serial--;
    Check(census.Publish(7,100,0x9000));expect(S::Ready,true);
    census.Invalidate(1);expect(S::Untracked,false);
    Check(census.ObservePolls().latest.generation==0);
    std::cout<<"Published query policy passed "<<checks<<" checks including100000 generations; no guest memory writes.\n";
}
