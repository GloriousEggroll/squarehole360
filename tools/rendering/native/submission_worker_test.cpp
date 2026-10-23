#include "renderer/native/submission_worker.h"
#include <atomic>
#include <future>
#include <iostream>
#include <semaphore>
#include <vector>
using namespace gow3::renderer;
void Require(bool yes,const char* why){if(!yes)throw std::runtime_error(why);}
template<size_t N> void Exercise() {
    struct Request{uint64_t value=0;};
    std::vector<uint64_t> seen;
    std::binary_semaphore release(0);
    std::promise<void> entered;
    NativeSubmissionWorker<Request,N> worker([&](const Request& request){
        if(request.value==1){entered.set_value();release.acquire();}
        seen.push_back(request.value);
    },true);
    for(size_t i=0;i<N;++i)worker.Submit(i,i+1,{i+1});
    Require(entered.get_future().wait_for(std::chrono::seconds(5))==std::future_status::ready,"Worker did not enter");
    Require(!worker.Collect(0,1,false),"Unreturned driver call got a receipt");
    bool rejected=false;try{worker.Submit(0,N+1,{N+1});}catch(const std::logic_error&){rejected=true;}
    Require(rejected,"Occupied fence slot was overwritten");
    rejected=false;try{worker.Collect(0,99,false);}catch(const std::logic_error&){rejected=true;}
    Require(rejected,"Wrong generation got a receipt");
    release.release();
    for(size_t i=0;i<N;++i)Require(worker.Collect(i,i+1,true).has_value(),"Missing receipt");
    for(uint64_t base=N+1;base<10000;base+=N) {
        for(size_t i=0;i<N;++i)worker.Submit(i,base+i,{base+i});
        for(size_t i=0;i<N;++i)Require(worker.Collect(i,base+i,true)->submit_ms>=0,"Invalid time");
    }
    for(size_t i=0;i<seen.size();++i)Require(seen[i]==i+1,"Submission order changed");
}
void Failure() {
    struct Request{uint64_t value=0;};
    std::binary_semaphore release(0);std::promise<void> entered;std::atomic<unsigned> calls=0;
    NativeSubmissionWorker<Request,4> worker([&](const Request&){
        ++calls;entered.set_value();release.acquire();throw std::runtime_error("driver failure");
    });
    worker.Submit(0,1,{1});
    Require(entered.get_future().wait_for(std::chrono::seconds(5))==std::future_status::ready,"Failure worker did not enter");
    for(size_t i=1;i<4;++i)worker.Submit(i,i+1,{i+1});
    release.release();
    for(size_t i=0;i<4;++i) {
        bool rejected=false;try{worker.Collect(i,i+1,true);}catch(const std::runtime_error& e){rejected=std::string_view(e.what())=="driver failure";}
        Require(rejected,"Failed or cancelled work got success receipt");
    }
    bool rejected=false;try{worker.Submit(0,5,{5});}catch(const std::runtime_error&){rejected=true;}
    Require(rejected&&calls==1,"Failure was retried or later work was executed");
}
int main() try {
    Exercise<1>();Exercise<2>();Exercise<4>();Failure();
    std::atomic<unsigned> calls=0;
    {NativeSubmissionWorker<unsigned,4> worker([&](const unsigned&){++calls;});
     for(unsigned i=0;i<4;++i)worker.Submit(i,i+1,i);}
    Require(calls==4,"Shutdown did not join accepted driver calls");
    std::cout<<"PASS: 29999 ordered requests; bounded receipt generations, held driver call, poisoned queue, cancellation, joined shutdown\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
