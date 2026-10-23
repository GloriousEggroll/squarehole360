#include "renderer/native/submission_capacity.h"
#include <array>
#include <iostream>
#include <stdexcept>
using gow3::renderer::NativeSubmissionCapacity;
static void Require(bool value,const char* why) {if(!value)throw std::runtime_error(why);}
int main() try {
    static_assert(NativeSubmissionCapacity::kMaximum==128);
    static_assert(NativeSubmissionCapacity::kDefault==64);
    static_assert(NativeSubmissionCapacity::kFences==2);
    Require(NativeSubmissionCapacity{}.lists==64,"Default capacity changed");
    for(uint32_t requested:{64u,128u})for(bool async:{false,true}) {
        const NativeSubmissionCapacity value(requested,async);
        Require(value.lists==(async?requested:64),"Synchronous fallback or requested capacity changed");
        Require(value.AsyncBatchLists()==value.lists/2,"Two equal asynchronous batches no longer fit");
        Require(value.AsyncBatchLists()<=64,"Submission grew past its legacy maximum draw target");
    }
    // Include values beside each accepted endpoint and the full uint32 range.
    for(uint32_t requested:{0u,1u,32u,63u,65u,127u,129u,UINT32_MAX})for(bool async:{false,true}) {
        bool rejected=false;
        try {(void)NativeSubmissionCapacity(requested,async);}
        catch(const std::invalid_argument&) {rejected=true;}
        Require(rejected,"Unsupported capacity was silently clamped or overflowed");
    }
    std::cout<<"PASS: exact 64/128 physical capacities, two fences, default and synchronous fallback, rejected unsupported endpoints\n";
    return 0;
} catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
