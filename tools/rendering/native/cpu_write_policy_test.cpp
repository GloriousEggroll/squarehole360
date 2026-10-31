#include "renderer/native/cpu_write_policy.h"
#include <rex/graphics/native_query_tickets.h>
#include <array>
#include <algorithm>
#include <iostream>
#include <stdexcept>
using namespace gow3::renderer;
using rex::graphics::NativeQueryIdentity;
using rex::graphics::NativeQueryTickets;
namespace {
unsigned checks=0;
void Check(bool value,const char* message) {
    ++checks;if(!value)throw std::runtime_error(message);
}
}
int main() try {
    for(bool enabled:{false,true})for(bool unified:{false,true}) {
        Check(NativeCpuWriteQueueEligible(enabled,unified,0x1000,4096)==(enabled&&unified),"Flags do not gate queuing");
        Check(!NativeCpuWriteQueueEligible(enabled,unified,0x1000,0),"Empty update queued");
        Check(!NativeCpuWriteQueueEligible(enabled,unified,0x1000,4097),"Large update queued");
        Check(!NativeCpuWriteQueueEligible(enabled,unified,0x1fffffff,2),"Out-of-domain update queued");
    }
    Check(NativeCpuWriteQueueEligible(true,true,0x1ffff000,4096),"Last physical page rejected");
    for(bool enabled:{false,true})for(bool unified:{false,true})
        Check(NativeCpuInvalidationQueueEligible(enabled,unified,0x1000,16u<<20)==(enabled&&unified),"Flags do not gate invalidations");
    Check(!NativeCpuInvalidationQueueEligible(true,true,0x1000,0),"Empty invalidation queued");
    Check(!NativeCpuInvalidationQueueEligible(true,true,0x1fffffff,2),"Out-of-domain invalidation queued");
    for(uint32_t report:{0u,0x1000u,0x1fffffc0u}) {
        Check(NativeCpuWriteOverlapsQueryReport(report,32,report),"End record overlap not eager");
        Check(NativeCpuWriteOverlapsQueryReport(report+32,32,report),"Begin record overlap not eager");
        Check(NativeCpuWriteOverlapsQueryReport(report+63,1,report),"Final report byte overlap not eager");
        if(report)Check(!NativeCpuWriteOverlapsQueryReport(report-1,1,report),"Adjacent previous byte overlaps");
        if(report!=0x1fffffc0)Check(!NativeCpuWriteOverlapsQueryReport(report+64,1,report),"Adjacent next byte overlaps");
    }
    Check(!NativeCpuWriteOverlapsQueryReport(0x2000,4096,0x1000),"Disjoint texture page is eager");
    Check(NativeCpuWriteOverlapsQueryReport(0,0,0x1000),"Malformed empty update not fail-closed");
    Check(NativeCpuWriteOverlapsQueryReport(0x1fffffff,2,0x1000),"Overflowing update not fail-closed");
    Check(NativeCpuWriteOverlapsQueryReport(0x2000,1,0x1001),"Unaligned report not fail-closed");
    Check(NativeCpuWriteOverlapsQueryReport(0x2000,1,0x20000000),"Out-of-domain report not fail-closed");

    // Use real SDK capabilities: a generic CPU update does not suppress an old
    // ticket. The overlap guard therefore completes/publicates before that
    // update, including reuse of one address by multiple exact generations.
    NativeQueryTickets tickets;std::array<uint8_t,64> guest{};
    for(uint64_t generation:{1ull,2ull}) {
        const NativeQueryIdentity id{0x1000,generation,7,true};
        {auto scope=tickets.BeginEndCallback(id);tickets.Defer(id);}
        Check(NativeCpuWriteOverlapsQueryReport(0x1000,4096,id.end_address),"Same-address CPU update was queued");
        Check(tickets.Publish(id,[&]{guest.fill(uint8_t(generation));}),"Exact report capability not written");
        guest.fill(0xcc); // the logically later CPU initialization
        Check(std::all_of(guest.begin(),guest.end(),[](auto b){return b==0xcc;}),"Older publication overwrote CPU initialization");
        Check(tickets.empty(),"Exact generation was not retired");
    }
    const NativeQueryIdentity old{0x1000,3,7,true};
    {auto scope=tickets.BeginEndCallback(old);tickets.Defer(old);}
    bool failed=false;
    try {tickets.Publish(old,[]{throw std::runtime_error("injected publisher failure");});}
    catch(const std::runtime_error&){failed=true;}
    Check(failed&&!tickets.empty(),"Failed publication lost its ownership capability");
    Check(NativeCpuWriteOverlapsQueryReport(0x1020,4,old.end_address),"Ready/current begin overlap was queued");
    tickets.SupersedeAddress(0x1000); // only a newer query BEGIN has this authority
    Check(!tickets.Publish(old,[&]{guest.fill(0);}),"Superseded report was written");
    Check(guest[0]==0xcc&&tickets.empty(),"Supersession changed later guest bytes or lost retirement");
    Check(tickets.statistics().retired==3&&tickets.statistics().guest_written==2&&tickets.statistics().superseded==1,
        "CPU-write guard changed SDK ticket accounting");
    std::cout<<"PASS: "<<checks<<" CPU write bounds/report-pair/capability/publication/failure checks\n";
    return 0;
} catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
