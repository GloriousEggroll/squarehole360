#include "renderer/native/query_poll_yield.h"
#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace gow3::renderer;
static size_t assertions=0;
static void Check(bool value) {++assertions;if(!value)throw std::runtime_error("Query poll oracle mismatch");}
struct Register {uint32_t u32=0;bool operator==(const Register&)const=default;};
struct Context {
    Register r3{};
    uint64_t lr=0;
    std::array<uint64_t,96> registers{};
    bool operator==(const Context&)const=default;
};
int main() {
    QueryPollYieldPolicy policy;
    for(uint64_t epoch:std::array<uint64_t,4>{0,2,4,UINT64_MAX-1})
        for(unsigned n=0;n<300;++n)Check(policy.Observe(epoch,0x1000,kQueryPollRetryCaller,1)==QueryPollDecision::Ignore);
    for(auto caller:std::array<uint64_t,4>{0x8296197C,0,0x829619A4,uint64_t(kQueryPollRetryCaller)|(uint64_t{1}<<32)})
        for(unsigned n=0;n<300;++n)Check(policy.Observe(3,0x1000,caller,1)==QueryPollDecision::Ignore);
    for(uint64_t epoch:std::array<uint64_t,3>{3,5,UINT64_MAX}) {
        policy.Reset();
        for(unsigned n=1;n<=1024;++n)
            Check(policy.Observe(epoch,0x1000,kQueryPollRetryCaller,1)==
                  (n%128?QueryPollDecision::Pending:QueryPollDecision::Yield));
    }
    // Every interrupted streak starts again: caller, query, error, completion,
    // disabled epoch, or a new lifetime. No inherited address-reuse countdown.
    for(unsigned reason=0;reason<6;++reason) {
        policy.Reset();
        for(unsigned n=0;n<127;++n)Check(policy.Observe(3,0x1000,kQueryPollRetryCaller,1)==QueryPollDecision::Pending);
        if(reason==0)Check(policy.Observe(3,0x1000,0x8296197C,1)==QueryPollDecision::Ignore);
        if(reason==1)Check(policy.Observe(3,0x2000,kQueryPollRetryCaller,1)==QueryPollDecision::Pending);
        if(reason==2)Check(policy.Observe(3,0x1000,kQueryPollRetryCaller,0x8876086a)==QueryPollDecision::Ignore);
        if(reason==3)Check(policy.Observe(3,0x1000,kQueryPollRetryCaller,0)==QueryPollDecision::Completed);
        if(reason==4)Check(policy.Observe(2,0x1000,kQueryPollRetryCaller,1)==QueryPollDecision::Ignore);
        const auto next_epoch=reason==5?5:3;
        for(unsigned n=1;n<=128;++n)
            Check(policy.Observe(next_epoch,0x1000,kQueryPollRetryCaller,1)==
                  (n==128?QueryPollDecision::Yield:QueryPollDecision::Pending));
    }
    QueryPollYieldPolicy other;
    policy.Reset();
    for(unsigned n=1;n<=256;++n) {
        const auto expected=n%128?QueryPollDecision::Pending:QueryPollDecision::Yield;
        Check(policy.Observe(9,0x1000,kQueryPollRetryCaller,1)==expected);
        Check(other.Observe(9,0x1000,kQueryPollRetryCaller,1)==expected);
    }
    // Actual wrapper helper is checked against the same original-call oracle,
    // including its entire machine context, output memory, and error behavior.
    std::array<uint8_t,64> plain_bytes{},wrapped_bytes{};
    Context plain,wrapped;
    unsigned plain_calls=0,wrapped_calls=0,observed=0,yielded=0;
    policy.Reset();
    for(unsigned n=0;n<1000;++n) {
        const uint32_t query=0x1000+(n/200)*0x100;
        const uint64_t caller=n%200? kQueryPollRetryCaller:0x8296197C;
        const uint32_t result=n%200==199?0:n%200==50?0x8876086a:1;
        plain.r3.u32=wrapped.r3.u32=query;plain.lr=wrapped.lr=caller;
        auto original=[&](Context& context,uint8_t* bytes,unsigned& calls) {
            ++calls;
            for(unsigned i=0;i<context.registers.size();++i)context.registers[i]=(uint64_t(n)<<40)^context.lr^(uint64_t(i)*7919);
            for(unsigned i=0;i<plain_bytes.size();++i)bytes[i]=uint8_t(n+i*3);
            context.lr=0xdead00000000ull+n;context.r3.u32=result;
        };
        original(plain,plain_bytes.data(),plain_calls);
        InvokeQueryPollOriginal(wrapped,wrapped_bytes.data(),
            [&](Context& context,uint8_t* base){original(context,base,wrapped_calls);},
            [&](uint32_t captured_query,uint64_t captured_caller,uint32_t status) {
                ++observed;Check(wrapped_calls==plain_calls);
                Check(captured_query==query&&captured_caller==caller&&status==result);
                Check(plain==wrapped&&plain_bytes==wrapped_bytes);
                if(policy.Observe(3,captured_query,captured_caller,status)==QueryPollDecision::Yield)++yielded;
            });
        Check(plain==wrapped&&plain_bytes==wrapped_bytes);
    }
    Check(observed==1000&&plain_calls==1000&&wrapped_calls==1000&&yielded==5);
    bool caught=false,called=false;
    try {
        InvokeQueryPollOriginal(wrapped,wrapped_bytes.data(),
            [&](Context& context,uint8_t* bytes){context.r3.u32=9;bytes[0]=77;throw std::runtime_error("original failure");},
            [&](uint32_t,uint64_t,uint32_t){called=true;});
    }catch(const std::runtime_error&){caught=true;}
    Check(caught&&!called&&wrapped.r3.u32==9&&wrapped_bytes[0]==77);
    std::cout<<"Query poll yield policy and original-context oracle passed "<<assertions<<" assertions\n";
}
