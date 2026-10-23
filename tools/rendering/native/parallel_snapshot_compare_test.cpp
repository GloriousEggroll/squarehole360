#include "renderer/native/parallel_snapshot_compare.h"
#include "renderer/native/immutable_snapshots.h"
#include <atomic>
#include <array>
#include <chrono>
#include <future>
#include <iostream>
#include <cstdlib>
#include <new>
#include <limits>
#include <system_error>
#include <stdexcept>
#include <vector>

// Fault the next allocation only in the startup caller, after a prior real
// worker was launched. Other live worker/stdlib threads cannot consume it.
namespace {thread_local bool fail_next_startup_allocation=false;}
void* operator new(std::size_t bytes) {
    if(fail_next_startup_allocation){fail_next_startup_allocation=false;throw std::bad_alloc{};}
    if(auto result=std::malloc(bytes?bytes:1))return result;throw std::bad_alloc{};
}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
using namespace gow3::renderer;
using Bytes=std::vector<uint8_t>;
using namespace std::chrono_literals;
namespace {
void Check(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
bool Exact(ParallelSnapshotCompare::ByteSpan a,ParallelSnapshotCompare::ByteSpan b) {
    return a.size()==b.size()&&(!a.size()||!std::memcmp(a.data(),b.data(),a.size()));
}
struct Gate {
    std::mutex mutex;std::condition_variable condition;unsigned entered=0;bool open=false;
    void Enter() {std::unique_lock lock(mutex);++entered;condition.notify_all();condition.wait(lock,[&]{return open;});}
    void Await(unsigned expected) {std::unique_lock lock(mutex);Check(condition.wait_for(lock,5s,[&]{return entered>=expected;}),"Comparison worker did not reach gate");}
    void Release(){std::lock_guard lock(mutex);open=true;condition.notify_all();}
};
struct OpenOnExit {Gate& gate;~OpenOnExit(){gate.Release();}};
void HardwareBoundsAndAllocation() {
    for(unsigned threads:{0u,1u,2u,3u,4u,5u,8u,16u}) {
        const unsigned limit=threads>2?threads-2:0;
        Check(ParallelSnapshotCompare::HardwareHelperLimit(threads)==limit,"Natural worker bound wrapped or retained reserved cores");
        ParallelSnapshotCompare::TestHooks hooks;hooks.hardware_threads=threads;
        ParallelSnapshotCompare pool(std::numeric_limits<unsigned>::max(),262144,std::move(hooks));
        Check(pool.helpers()==limit,"Unbounded requested value allocated beyond reported hardware");
        Bytes a(1024*1024,0x72),b=a;Check(pool.Equal(a,b)&&pool.Idle(),"Hardware-bound pool lost exact bytes");
    }
    for(unsigned created:{0u,1u,3u,7u}) {
        unsigned starts=0;std::atomic<unsigned> alive=0;
        {
            ParallelSnapshotCompare::TestHooks hooks;hooks.hardware_threads=16;
            hooks.launch=[&](std::function<void()> entry) {
                auto thread=std::thread([&,entry=std::move(entry)]{++alive;entry();--alive;});
                if(++starts==created)fail_next_startup_allocation=true;
                return thread;
            };
            fail_next_startup_allocation=created==0;
            ParallelSnapshotCompare compare(8,262144,std::move(hooks));
            Check(!fail_next_startup_allocation&&compare.helpers()==created,
                "Real startup allocation failure changed accepted thread prefix");
            Bytes a(2*1024*1024,0x33),b=a;
            Check(compare.Equal(a,b)&&compare.Idle(),"Allocation-degraded pool lost exact full-range comparison");
            b[131073]^=1;Check(!compare.Equal(a,b)&&compare.Idle(),"Allocation-degraded pool missed interior mutation");
        }
        Check(!alive,"Allocation startup failure leaked an earlier real worker");
    }
    // A valid launcher can decline before starting a thread without throwing.
    ParallelSnapshotCompare::TestHooks no_thread;no_thread.hardware_threads=16;
    no_thread.launch=[](std::function<void()>){return std::thread{};};
    ParallelSnapshotCompare none(8,262144,std::move(no_thread));
    Check(!none.helpers()&&none.Idle(),"Nonjoinable startup retained a phantom worker");
}
void ExpandedRangeCoverage() {
    // Every Compare hook is a real independently verified memcmp. A multiset
    // of complete byte intervals detects skipped/duplicate ranges and an
    // inactive worker accidentally executing an empty or stale assignment.
    for(unsigned helpers:{1u,2u,4u,8u}) {
        std::mutex lock;std::vector<std::pair<size_t,size_t>> ranges;
        const uint8_t* left_begin=nullptr;const uint8_t* right_begin=nullptr;size_t bytes=0;
        ParallelSnapshotCompare::TestHooks hooks;hooks.hardware_threads=16;
        hooks.compare=[&](auto a,auto b) {
            Check(!a.empty()&&a.size()==b.size(),"Inactive worker compared empty/stale assignment");
            const auto left=uintptr_t(a.data()),right=uintptr_t(b.data());
            const auto left_base=uintptr_t(left_begin),right_base=uintptr_t(right_begin);
            Check(a.size()<=bytes&&left>=left_base&&left-left_base<=bytes-a.size()&&
                right>=right_base&&right-right_base<=bytes-b.size()&&
                left-left_base==right-right_base,"Worker retained a stale source or crossed supplied range");
            const size_t first=size_t(left-left_base);
            {std::lock_guard guard(lock);ranges.emplace_back(first,first+a.size());}
            return Exact(a,b);
        };
        ParallelSnapshotCompare pool(helpers,262144,std::move(hooks));
        const std::array<size_t,10> sizes{262143,262144,262145,393215,393216,1179649,8388609,262144,65537,1179649};
        for(size_t n:sizes) {
            Bytes a(n+64,0x16),b=a;left_begin=a.data()+31;right_begin=b.data()+31;bytes=n;
            const auto left=std::span(a).subspan(31,n),right=std::span(b).subspan(31,n);
            ranges.clear();Check(pool.Equal(left,right)&&pool.Idle(),"Expanded equal pool lost bytes/borrowed a range");
            std::sort(ranges.begin(),ranges.end());
            const unsigned active=n<262144?0:helpers<=2?helpers:std::min(helpers,unsigned(n/131072)-1);
            Check(ranges.size()==size_t(active)+1,"Inactive workers woke or active job count differed from independent coarse bound");
            size_t cursor=0;
            for(const auto [first,last]:ranges) {
                Check(first==cursor&&last>first,"Expanded jobs skipped or duplicated an interior byte");
                if(active&&helpers>2)Check(last-first>=131072,"Expanded job was smaller than coarse128KiB minimum");
                cursor=last;
            }
            Check(cursor==n,"Last helper omitted the nonaligned tail");
            if(active&&helpers<=2)Check(ranges.front().second==((n/(helpers+1))&~size_t(63)),
                "Default1/2 helper partition differs from original64-byte-rounded split");
            const auto recorded=ranges;
            // Mutate every partition's interior while retaining equal64-byte
            // probes, then restore/reuse the same source address each call.
            for(const auto [first,last]:recorded) {
                const size_t changed=first+(last-first)/2;
                if(changed<64||changed>=n-64)continue;
                b[31+changed]^=0x80;
                Check(!pool.Equal(left,right)&&pool.Idle(),"Expanded pool missed late same-address interior mutation");
                b[31+changed]^=0x80;
                Check(pool.Equal(left,right)&&pool.Idle(),"Restored full snapshot remained falsely different");
            }
        }
    }
}
void Startup() {
    for(unsigned fail_at:{0u,1u,2u,4u,8u,9u}) {
        unsigned starts=0;std::atomic<unsigned> alive=0;
        {
            ParallelSnapshotCompare::TestHooks hooks;hooks.hardware_threads=16;
            hooks.launch=[&](std::function<void()> entry) {
                if(starts++==fail_at)throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
                return std::thread([&,entry=std::move(entry)]{++alive;entry();--alive;});
            };
            ParallelSnapshotCompare compare(9,ParallelSnapshotCompare::kThreshold,std::move(hooks));
            Check(compare.helpers()==fail_at,"Partial thread startup did not degrade to available helpers");
            Bytes a(1024*1024,0x5a),b=a;
            Check(compare.Equal(a,b)&&compare.Idle(),"Partial startup changed exact results or retained spans");
            b[b.size()/2]^=1;Check(!compare.Equal(a,b),"Partial startup missed mutation");
        }
        Check(!alive,"Destructor leaked partially created worker");
    }
}
size_t Equality() {
    size_t cases=0;
    for(unsigned helpers:{0u,1u,2u,4u,8u}) {
        ParallelSnapshotCompare::TestHooks hardware;hardware.hardware_threads=16;
        ParallelSnapshotCompare compare(helpers,ParallelSnapshotCompare::kThreshold,std::move(hardware));
        for(size_t n:std::initializer_list<size_t>{0,1,7,8,15,63,64,65,4095,4096,65535,65536,262143,262144,262145,1048576,8388609}) {
            Bytes a(n+64),b(n+64);for(size_t i=0;i<a.size();++i)a[i]=uint8_t(i*17+i/113);b=a;
            for(size_t offset:std::initializer_list<size_t>{0,1,7,31,63}) {
                auto left=std::span(a).subspan(offset,n),right=std::span(b).subspan(offset,n);
                Check(compare.Equal(left,right)==Exact(left,right),"Equal snapshot did not match independent memcmp");++cases;
                if(n)for(size_t changed:{size_t(0),n/3,n/2,n-1}) {
                    b[offset+changed]^=1;
                    Check(compare.Equal(left,right)==Exact(left,right),"Snapshot comparison missed changed byte");++cases;
                    b[offset+changed]^=1;
                }
                Check(compare.Idle(),"Comparison retained borrowed span after return");
            }
        }
        Bytes a(262177,0x91),b=a;
        Check(!compare.Equal(a,std::span(b).first(b.size()-1)),"Different snapshot sizes compared equal");
        for(unsigned i=0;i<1000;++i)Check(compare.Equal(a,b),"Repeated dispatch changed equality");
        compare.StopAndDrain();Check(compare.Idle()&&compare.Equal(a,b),"Shutdown did not leave synchronous comparison available");
    }
    return cases;
}
void Coverage() {
    Bytes a(262177+31,0x91),b=a;const auto left=std::span(a).subspan(31),right=std::span(b).subspan(31);
    std::mutex mutex;std::vector<std::pair<size_t,size_t>> ranges;
    ParallelSnapshotCompare::TestHooks hooks;
    hooks.compare=[&](auto x,auto y) {
        Check(x.data()>=left.data()&&x.data()+x.size()<=left.data()+left.size(),"Worker read beyond left span");
        Check(y.data()>=right.data()&&y.data()+y.size()<=right.data()+right.size(),"Worker read beyond right span");
        const size_t offset=x.data()-left.data();Check(offset==size_t(y.data()-right.data()),"Compared different byte positions");
        {std::lock_guard lock(mutex);ranges.emplace_back(offset,offset+x.size());}
        return Exact(x,y);
    };
    ParallelSnapshotCompare compare(2,ParallelSnapshotCompare::kThreshold,std::move(hooks));
    Check(compare.Equal(left,right),"Exact coverage fixture compared unequal");
    std::sort(ranges.begin(),ranges.end());size_t end=0;
    for(auto [first,last]:ranges){Check(first==end&&last>first,"Exact comparison skipped or duplicated a byte interval");end=last;}
    Check(end==left.size()&&compare.Idle(),"Exact comparison did not cover complete range");
}
void FailureAndLifetime(bool throw_on_caller,unsigned helpers=2) {
    auto a=std::make_shared<Bytes>(1024*1024,0x53),b=std::make_shared<Bytes>(*a);
    std::weak_ptr<Bytes> life_a=a,life_b=b;Gate gate;std::thread::id caller;
    std::atomic<bool> testing=true,claimed=false;
    ParallelSnapshotCompare::TestHooks hooks;hooks.hardware_threads=16;
    hooks.compare=[&](auto x,auto y) {
        if(testing) {
            const bool main=std::this_thread::get_id()==caller;
            if(main&&throw_on_caller)throw std::runtime_error("injected caller failure");
            if(!main) {
                if(!throw_on_caller&&!claimed.exchange(true))throw std::runtime_error("injected worker failure");
                gate.Enter();
            }
        }
        return Exact(x,y);
    };
    ParallelSnapshotCompare compare(helpers,ParallelSnapshotCompare::kThreshold,std::move(hooks));
    const auto active=helpers>2?std::min(helpers,unsigned(a->size()/131072-1)):helpers;
    auto result=std::async(std::launch::async,[&,a,b] {
        caller=std::this_thread::get_id();return compare.Equal(*a,*b);
    });
    OpenOnExit release_on_failure{gate};
    gate.Await(throw_on_caller?active:active-1);
    a.reset();b.reset();
    Check(!life_a.expired()&&!life_b.expired(),"Accepted comparison lost source ownership while workers still read");
    Check(result.wait_for(20ms)==std::future_status::timeout,"Failure returned while another worker still borrowed input");
    gate.Release();bool failed=false;
    try {result.get();}catch(const std::runtime_error& error) {
        failed=std::string(error.what())==(throw_on_caller?"injected caller failure":"injected worker failure");
    }
    Check(failed&&compare.Idle(),"Failure was lost or kept outstanding borrowed spans");
    Check(life_a.expired()&&life_b.expired(),"Comparator retained caller byte owners after failure");
    testing=false;Bytes fresh(1024*1024,0x6c);
    Check(compare.Equal(fresh,fresh),"Drained comparison failure poisoned later exact calls");
}
void Shutdown(unsigned helpers=2) {
    Bytes a(2*1024*1024,0x37),b=a;Gate gate;std::thread::id caller;std::atomic<bool> testing=true;
    ParallelSnapshotCompare::TestHooks hooks;hooks.hardware_threads=16;
    hooks.compare=[&](auto x,auto y){if(testing&&std::this_thread::get_id()!=caller)gate.Enter();return Exact(x,y);};
    ParallelSnapshotCompare compare(helpers,ParallelSnapshotCompare::kThreshold,std::move(hooks));
    auto result=std::async(std::launch::async,[&]{caller=std::this_thread::get_id();return compare.Equal(a,b);});
    OpenOnExit release_on_failure{gate};
    gate.Await(helpers);
    auto later=std::async(std::launch::async,[&]{return compare.Equal(a,b);});
    auto shutdown=std::async(std::launch::async,[&]{compare.StopAndDrain();});
    OpenOnExit release_on_shutdown_failure{gate};
    Check(shutdown.wait_for(20ms)==std::future_status::timeout,"Shutdown abandoned an accepted comparison");
    gate.Release();Check(result.get(),"Concurrent shutdown changed exact comparison");shutdown.get();Check(later.get(),"Concurrent queued caller changed exact bytes during stop");
    testing=false;Check(compare.Idle()&&compare.Equal(a,b),"Shutdown retained spans or disabled scalar fallback");
}
void MultipleExceptions() {
    Bytes a(2*1024*1024,0x29),b=a;std::thread::id caller;
    std::atomic<unsigned> workers=0;std::atomic<bool> testing=true;
    ParallelSnapshotCompare::TestHooks hooks;hooks.hardware_threads=16;
    hooks.compare=[&](auto x,auto y) {
        if(testing) {
            if(std::this_thread::get_id()==caller)throw std::logic_error("preferred caller error");
            ++workers;throw std::runtime_error("simultaneous worker error");
        }
        return Exact(x,y);
    };
    ParallelSnapshotCompare pool(8,262144,std::move(hooks));
    auto result=std::async(std::launch::async,[&]{caller=std::this_thread::get_id();return pool.Equal(a,b);});
    bool exact_error=false;
    try {result.get();}catch(const std::logic_error& error){exact_error=std::string(error.what())=="preferred caller error";}
    Check(exact_error&&workers==8&&pool.Idle(),"Simultaneous caller/worker exceptions skipped drain or lost original error");
    testing=false;Check(pool.Equal(a,b)&&pool.Idle(),"Error drain left stale assigned jobs/results in larger pool");
}
void ImmutableOwnership() {
    ImmutableSnapshots scalar(768*1024,0),parallel(768*1024,2);
    Bytes source(300000,0x14);std::shared_ptr<const Bytes> old_scalar,old_parallel;
    for(unsigned step=0;step<80;++step) {
        const uint32_t address=0x10000+(step%7)*0x100000;
        if(step%3==0)source[(step*7919)%source.size()]^=uint8_t(step+1);
        auto a=scalar.Get(address,source),b=parallel.Get(address,source);
        Check(*a==source&&*b==source&&*a==*b,"Parallel immutable snapshot changed captured bytes");
        Check(scalar.Get(address,source)==a&&parallel.Get(address,source)==b,"Unchanged snapshot lost identity");
        if(step==0){old_scalar=a;old_parallel=b;}
        Check(scalar.retained_bytes()==parallel.retained_bytes(),"Parallel comparison changed cache budget or eviction");
        if(step%11==0){scalar.Clear();parallel.Clear();}
    }
    Check(*old_scalar==*old_parallel&&old_parallel->at(7919)==0x14,"Old immutable owner changed after address reuse");
    parallel.ConfigureComparisonHelpers(0);Check(!parallel.comparison_helpers(),"Scalar reconfiguration kept helper pool");
    Check(*parallel.Get(0x10000,source)==source,"Reconfiguration changed current snapshot");
    parallel.ConfigureComparisonHelpers(1);Check(parallel.comparison_helpers()==std::min(1u,ParallelSnapshotCompare::HardwareHelperLimit()),"Hardware-bounded explicit helper count ignored");
}
}
int main() {
    try {
        HardwareBoundsAndAllocation();ExpandedRangeCoverage();Startup();const auto cases=Equality();Coverage();
        for(unsigned helpers:{2u,4u,8u}) {FailureAndLifetime(true,helpers);FailureAndLifetime(false,helpers);Shutdown(helpers);}
        MultipleExceptions();ImmutableOwnership();
        std::cout<<"PASS: "<<cases<<" independent equality/mutation cases; hardware/allocation bounds, exact default/coarse partitions, inactive workers, partial startup, borrowed-span lifetime, concurrent caller/worker failure drain, shutdown and immutable owner/cache equivalence\n";
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
