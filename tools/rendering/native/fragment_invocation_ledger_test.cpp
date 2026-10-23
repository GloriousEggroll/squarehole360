#include "renderer/native/fragment_invocation_ledger.h"
#include <iostream>
#include <string>

using gow3::renderer::NativeFragmentInvocationLedger;
using Ledger=NativeFragmentInvocationLedger;
namespace {
unsigned checks=0;
void Check(bool value,const char* message) {++checks;if(!value)throw std::runtime_error(message);}
template<class F>void Refuses(F work) {bool refused=false;try{work();}catch(const std::exception&){refused=true;}Check(refused,"Expected invalid ownership to refuse");}
void Capacity(uint32_t capacity) {
    Ledger l(capacity);std::vector<Ledger::Token> first,second;
    for(uint32_t i=0;i<capacity;++i) {
        Ledger::Token token{i,1};l.Begin(token,{100+i,42,1,3});
        Refuses([&]{(void)l.Pending(token);});l.End(token);l.Accept(token);
        (i<capacity/2?first:second).push_back(token);
    }
    Refuses([&]{l.Begin({0,2},{100,42,1,3});});
    Refuses([&]{l.Cancel(first.front());});
    Refuses([&]{l.Accept(first.front());});
    Refuses([&]{l.ValidateRetirement(std::array{first.front(),first.front()});});
    auto wrong=first;wrong.back().generation=2;Refuses([&]{l.ValidateRetirement(wrong);});
    Check(l.totals().completed==0&&l.totals().pending==capacity,"Invalid fence changed owner prefix");
    l.ValidateRetirement(first);
    for(auto token:first)l.Retire(token,1,0); // Available zero is not unavailable.
    for(auto token:first)l.Begin({token.slot,2},{200+token.slot,43,2,6});
    Refuses([&]{l.ValidateRetirement(first);});
    for(auto token:first) {token.generation=2;l.End(token);l.Accept(token);}
    l.ValidateRetirement(second);
    for(auto token:second)l.Retire(token,2,7);
    Check(l.totals().pending==capacity/2,"Older completed fence retired newer draw generation");
    for(auto token:first) {token.generation=2;l.Retire(token,3,9);}
    Check(l.totals().pending==0&&l.totals().accepted==capacity+capacity/2,"Accepted/retired totals differ");
    Check(l.totals().invocations==uint64_t(capacity/2)*16,"Exact integer invocation sum differs");
    Check(l.samples().front().invocations&&*l.samples().front().invocations==0,"Real zero result lost availability");
    Refuses([&]{l.Begin({0,2},{300,44,1,3});});
    l.Begin({0,3},{300,44,1,3});l.End({0,3});l.Accept({0,3});
    Refuses([&]{l.Retire({0,3},0,1);});l.Retire({0,3},4,std::nullopt);
    Check(l.totals().unavailable==1&&!l.samples().back().invocations,"Unavailable query fabricated a zero");
    l.Begin({0,4},{301,44,1,3});l.Cancel({0,4});
    Check(l.totals().cancelled==1&&l.totals().pending==0,"Unaccepted cancellation leaked ownership");
    Refuses([&]{l.Begin({capacity,1},{0,0,1,0});});
    Refuses([&]{l.Begin({0,0},{0,0,1,0});});
    Refuses([&]{l.Begin({0,5},{0,0,0,0});});
    Refuses([&]{l.Begin({0,5},{0,0,Ledger::kKeys+1,0});});
}
}
int main()try {
    Refuses([]{Ledger wrong(63);});Capacity(64);Capacity(128);
    Ledger overflow(64);overflow.Begin({0,UINT64_MAX},{0,0,1,0});overflow.End({0,UINT64_MAX});overflow.Accept({0,UINT64_MAX});
    overflow.Retire({0,UINT64_MAX},1,UINT64_MAX);
    Refuses([&]{overflow.Begin({0,1},{0,0,1,0});});
    overflow.Begin({1,1},{0,0,1,0});overflow.End({1,1});overflow.Accept({1,1});
    Refuses([&]{overflow.Retire({1,1},2,1);});
    Check(overflow.totals().pending==1&&overflow.totals().completed==1,"Overflow partially published a result");
    Check(Ledger::Add(UINT64_MAX,0)==UINT64_MAX,"Exact maximum counter rejected");
    Refuses([]{(void)Ledger::Add(UINT64_MAX,1);});
    Ledger bounded(64);
    for(uint64_t i=1;i<=Ledger::kSamples;++i) {bounded.Begin({0,i},{i,1,1,3});bounded.End({0,i});bounded.Accept({0,i});bounded.Retire({0,i},i,1);}
    Check(!bounded.BudgetAvailable()&&bounded.samples().size()==Ledger::kSamples,"Sample cap is not exact");
    Refuses([&]{bounded.Begin({0,Ledger::kSamples+1},{0,1,1,3});});
    std::cout<<"PASS: fragment census ownership, two live generations, zero/unavailable, cancellation, bounded storage and counter overflow; checks="<<checks<<'\n';
}catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
