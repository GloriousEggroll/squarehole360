#include "renderer/native/native_overflow_prewarm.h"
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <limits>
#include <stdexcept>

using namespace gow3::renderer;
namespace {
uint64_t checks=0;
using Status=NativeOverflowPrewarmStatus;
void Require(bool condition) {
    ++checks;if(!condition)throw std::runtime_error("Overflow prewarm admission fixture failed");
}
Status Admit(uint64_t pipelines,uint64_t modules,uint64_t bytes,uint64_t extra_modules,uint64_t extra_bytes,
        uint64_t pipeline_limit=2048,uint64_t module_limit=1024,uint64_t byte_limit=64ull*1024*1024) {
    return NativeOverflowPrewarmAdmission(pipelines,pipeline_limit,modules,module_limit,bytes,byte_limit,extra_modules,extra_bytes);
}
}
int main() {
    constexpr uint64_t hard=64ull*1024*1024;
    constexpr uint64_t maximum=std::numeric_limits<uint64_t>::max();
    Require(Admit(1535,1022,hard-8192,2,8192)==Status::Prepared);
    Require(Admit(1536,1022,hard-8192,2,8192)==Status::PipelineCap);
    Require(Admit(1535,1023,hard-8192,2,8192)==Status::ModuleCountCap);
    Require(Admit(1535,1022,hard-8191,2,8192)==Status::ByteCap);
    Require(Admit(0,1024,hard,0,0)==Status::Prepared);
    Require(Admit(0,1024,hard,1,0)==Status::ModuleCountCap);
    Require(Admit(0,1024,hard,0,1)==Status::ByteCap);
    Require(Admit(4,8,4096,2,8192,4)==Status::PipelineCap);
    Require(Admit(3,8,4096,2,8192,4,9)==Status::ModuleCountCap);
    Require(Admit(3,8,4096,2,8192,4,10,12287)==Status::ByteCap);
    Require(Admit(3,8,4096,2,8192,4,10,12288)==Status::Prepared);
    Require(Admit(1536,0,0,0,0,maximum,maximum,maximum)==Status::PipelineCap);
    Require(Admit(0,1024,0,1,0,maximum,maximum,maximum)==Status::ModuleCountCap);
    Require(Admit(0,0,hard,0,1,maximum,maximum,maximum)==Status::ByteCap);
    Require(Admit(0,maximum,0,1,0)==Status::ModuleCountCap);
    Require(Admit(0,0,maximum,0,1)==Status::ByteCap);
    Require(Admit(0,1,0,maximum,0)==Status::ModuleCountCap);
    Require(Admit(0,0,1,0,maximum)==Status::ByteCap);
    // Boundary cross-product uses small finite values: additions cannot wrap.
    // Independent mathematical inequalities cover exact fit and all priorities.
    for(uint64_t pipelines:{uint64_t(0),uint64_t(1535),uint64_t(1536)})
    for(uint64_t modules:{uint64_t(745),uint64_t(1022),uint64_t(1023),uint64_t(1024),uint64_t(1025)})
    for(uint64_t bytes:{uint64_t(0),hard-4096,hard,hard+1})
    for(uint64_t extra_modules:{uint64_t(0),uint64_t(1),uint64_t(2)})
    for(uint64_t extra_bytes:{uint64_t(0),uint64_t(1),uint64_t(4096)}) {
        const auto expected=pipelines>=1536?Status::PipelineCap:
            modules+extra_modules>1024?Status::ModuleCountCap:
            bytes+extra_bytes>hard?Status::ByteCap:Status::Prepared;
        Require(Admit(pipelines,modules,bytes,extra_modules,extra_bytes)==expected);
    }
    // Every extra PSO releases its temporary modules. Prospective admission is
    // per synchronous creation, not cumulative over all 112 omitted recipes.
    const uint64_t persistent_modules=746,persistent_bytes=50326612+109676;
    for(uint64_t index=0;index<112;++index) {
        Require(Admit(738+index,persistent_modules,persistent_bytes,2,131072)==Status::Prepared);
    }
    const NativeOverflowPrewarmResult prepared{Status::Prepared,2,8192};
    const NativeOverflowPrewarmResult existing{Status::AlreadyResident,0,0};
    const NativeOverflowPrewarmResult refused{Status::ByteCap,0,0};
    Require(prepared.ready());Require(existing.ready());Require(!refused.ready());
    std::printf("PASS %llu exact shared admission checks; no GPU ownership claim\n",static_cast<unsigned long long>(checks));
}
