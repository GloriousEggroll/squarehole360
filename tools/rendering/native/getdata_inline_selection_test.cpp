// Source-only differential fixture. Uses actual SDK PPCContext/FPSCR; its
// bounded memory adapter records every original read/write in program order.
#include <rex/ppc/context.h>
#include "renderer/native/getdata_inline_selection.h"
#include "renderer/native/query_poll_yield.h"
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <stdexcept>
#include <type_traits>
#include <vector>
#include <span>

namespace {
size_t checks=0,cases=0;
struct ReachedCoverage {
    uint64_t cases=0,mutations=0,faults=0,kicks=0,memzeros=0,kick_failures=0,memzero_failures=0;
};
std::array<ReachedCoverage,4> reached{};
void Require(bool value,const char* why) {
    ++checks;if(!value)throw std::runtime_error(why);
}
struct Event {
    enum Kind {Load,Store,Trace,Kick,Memzero,Mutation} kind;
    uint32_t address=0,width=0;
    uint64_t value=0;
    bool operator==(const Event&)const=default;
};
struct Memory {
    static constexpr size_t kMaxBytes=16384,kMaxEvents=2048;
    std::map<uint32_t,uint8_t> bytes;
    std::vector<Event> events;
    size_t accesses=0,mutation_at=SIZE_MAX,fault_at=SIZE_MAX;
    uint32_t counter=0,report=0,mutation_value=0;
    bool throw_kick=false,throw_memzero=false;
    uint32_t seed=0;
    void EventOf(Event e) {
        if(events.size()==kMaxEvents)throw std::runtime_error("Fixture event bound exceeded");
        events.push_back(e);
    }
    uint8_t Byte(uint32_t a)const {
        auto it=bytes.find(a);
        return it!=bytes.end()?it->second:uint8_t((a*131u)^(a>>7)^seed);
    }
    void PutByte(uint32_t a,uint8_t value) {
        if(!bytes.contains(a)&&bytes.size()==kMaxBytes)throw std::runtime_error("Fixture byte bound exceeded");
        bytes[a]=value;
    }
    template<class T> void Put(uint32_t a,T value) {
        for(size_t i=0;i<sizeof(T);++i)PutByte(a+uint32_t(i),uint8_t(uint64_t(value)>>((sizeof(T)-i-1)*8)));
    }
    void BeforeAccess() {
        const auto n=accesses++;
        if(n==mutation_at) {
            // A deterministic external writer changes the exact shared words
            // between reads. Both implementations must observe the same trace.
            Put<uint32_t>(counter,mutation_value);
            Put<uint32_t>(report+16,mutation_value^0xA5A5A5A5u);
            Put<uint32_t>(report+20,mutation_value^0x5A5A5A5Au);
            EventOf({Event::Mutation,counter,4,mutation_value});
        }
        if(n==fault_at)throw std::runtime_error("Injected original memory fault");
    }
    template<class T> T Load(uint32_t a) {
        BeforeAccess();uint64_t value=0;
        for(size_t i=0;i<sizeof(T);++i)value=(value<<8)|Byte(a+uint32_t(i));
        EventOf({Event::Load,a,uint32_t(sizeof(T)),value});return T(value);
    }
    template<class T> void Store(uint32_t a,T value) {
        BeforeAccess();Put<T>(a,value);EventOf({Event::Store,a,uint32_t(sizeof(T)),uint64_t(value)});
    }
    bool Same(const Memory& other)const {
        return bytes==other.bytes&&events==other.events&&accesses==other.accesses;
    }
};
thread_local Memory* current_memory=nullptr;
void Trace(uint32_t pc) {current_memory->EventOf({Event::Trace,pc,0,0});}
// Model actual external callees as opaque, state-changing calls. They execute
// once and at the same point; a helper-inline change may not reorder or omit them.
void Kick(PPCContext& ctx,uint8_t*) {
    current_memory->EventOf({Event::Kick,0x82AF58B8,0,ctx.lr});
    ctx.r0.u64^=0xFEDCBA9876543210ull;ctx.cr7.set_raw(13);
    current_memory->Put<uint32_t>(current_memory->counter,0xABCDEF01u);
    if(current_memory->throw_kick)throw std::runtime_error("Injected original kick failure");
}
void Memzero(PPCContext& ctx,uint8_t*) {
    current_memory->EventOf({Event::Memzero,ctx.r3.u32,ctx.r5.u32,ctx.r4.u32});
    if(ctx.r5.u32>64)throw std::runtime_error("Fixture memset bound exceeded");
    for(uint32_t i=0;i<ctx.r5.u32;++i)current_memory->Store<uint8_t>(ctx.r3.u32+i,ctx.r4.u8);
    ctx.r0.u64^=0x123456789ABCDEF0ull;ctx.r12.u64^=0xA55AA55A;
    if(current_memory->throw_memzero)throw std::runtime_error("Injected original memset failure");
}
}

#define REX_FUNC_PROLOGUE() ((void)0)
#define REX_TRACE_GUEST_FUNCTION(pc) Trace(pc)
#define REX_LOAD_U8(a) current_memory->Load<uint8_t>(uint32_t(a))
#define REX_LOAD_U16(a) current_memory->Load<uint16_t>(uint32_t(a))
#define REX_LOAD_U32(a) current_memory->Load<uint32_t>(uint32_t(a))
#define REX_LOAD_U64(a) current_memory->Load<uint64_t>(uint32_t(a))
#define REX_STORE_U8(a,v) current_memory->Store<uint8_t>(uint32_t(a),uint8_t(v))
#define REX_STORE_U16(a,v) current_memory->Store<uint16_t>(uint32_t(a),uint16_t(v))
#define REX_STORE_U32(a,v) current_memory->Store<uint32_t>(uint32_t(a),uint32_t(v))
#define REX_STORE_U64(a,v) current_memory->Store<uint64_t>(uint32_t(a),uint64_t(v))
#define sub_82AF58B8 Kick
#define sub_82CC9EA0 Memzero
#if defined(_MSC_VER)
#define GOW3_GETDATA_HELPER_LINKAGE static __declspec(noinline)
#else
#define GOW3_GETDATA_HELPER_LINKAGE static __attribute__((noinline))
#endif
#define GOW3_GETDATA_SAVE_NAME ReferenceSave26
#define GOW3_GETDATA_RESTORE_NAME ReferenceRestore26
#include "generated/native-getdata/getdata_register_helpers.inc"
#undef GOW3_GETDATA_HELPER_LINKAGE
#undef GOW3_GETDATA_SAVE_NAME
#undef GOW3_GETDATA_RESTORE_NAME
#define __savegprlr_26 ReferenceSave26
#define __restgprlr_26 ReferenceRestore26
#define GOW3_GETDATA_LEAF_NAME ReferenceGetData
#include "generated/native-getdata/getdata_leaf_body.inc"
#undef GOW3_GETDATA_LEAF_NAME
#undef __savegprlr_26
#undef __restgprlr_26
#if defined(_MSC_VER)
#define GOW3_GETDATA_HELPER_LINKAGE static __forceinline
#else
#define GOW3_GETDATA_HELPER_LINKAGE static inline __attribute__((always_inline))
#endif
#define GOW3_GETDATA_SAVE_NAME InlineSave26
#define GOW3_GETDATA_RESTORE_NAME InlineRestore26
#include "generated/native-getdata/getdata_register_helpers.inc"
#undef GOW3_GETDATA_HELPER_LINKAGE
#undef GOW3_GETDATA_SAVE_NAME
#undef GOW3_GETDATA_RESTORE_NAME
#define __savegprlr_26 InlineSave26
#define __restgprlr_26 InlineRestore26
#define GOW3_GETDATA_LEAF_NAME InlineGetData
#include "generated/native-getdata/getdata_leaf_body.inc"
#undef GOW3_GETDATA_LEAF_NAME
#undef __savegprlr_26
#undef __restgprlr_26

namespace {
struct Scenario {
    uint32_t type=9,flags=0x40,tag=7,issued=10,completed=8,count=1;
    uint32_t end0=0,end1=0,mode=0,half=0;
    uint16_t half0=0,half1=0;
    uint32_t output=0x4000,report_base=0xC0009000;
    size_t mutation=SIZE_MAX,fault=SIZE_MAX;
    bool throw_kick=false,throw_memzero=false;
    uint32_t seed=1;
    uint64_t epoch=0;
};
constexpr uint32_t kQuery=0x1000,kDevice=0x2000,kCounter=0x6000,kStack=0x8000;
PPCContext Initial(uint32_t seed) {
    static_assert(std::is_trivially_copyable_v<PPCContext>);
    std::array<std::byte,sizeof(PPCContext)> bytes{};
    uint32_t state=seed;
    for(auto& b:bytes) {state=state*1664525u+1013904223u;b=std::byte(state>>24);}
    auto ctx=std::bit_cast<PPCContext>(bytes);
    ctx.r1.u64=kStack;ctx.r3.u64=kQuery;ctx.r4.u64=0x4000;
    ctx.lr=0x829619A0;ctx.xer={1,0,1};
    // Safe host state uses actual SDK FPSCR; retain all unused registers/padding
    // to compare the complete original context rather than only return value.
    ctx.fpscr.InitHost();ctx.fpscr.enableFlushMode();
    return ctx;
}
Memory InitMemory(const Scenario& s) {
    Memory m;m.seed=s.seed;m.counter=kCounter;m.report=s.report_base;
    m.mutation_at=s.mutation;m.fault_at=s.fault;m.mutation_value=s.seed*977u+11u;
    m.throw_kick=s.throw_kick;m.throw_memzero=s.throw_memzero;
    m.Put<uint32_t>(kQuery,kDevice);m.Put<uint32_t>(kQuery+4,s.type);
    m.Put<uint8_t>(kQuery+20,uint8_t(s.flags));m.Put<uint32_t>(kQuery+24,s.tag);
    m.Put<uint32_t>(kQuery+148,s.count);m.Put<uint32_t>(kDevice+11024,kCounter);
    m.Put<uint32_t>(kDevice+11036,s.issued);m.Put<uint32_t>(kCounter,s.completed);
    m.Put<uint8_t>(kDevice+11069,uint8_t(s.mode));m.Put<uint16_t>(kDevice+10376,uint16_t(s.half));
    for(uint32_t n=0;n<9;++n) {
        const auto report=s.report_base+n*64;
        // This is the inverse of the original pointer+page adjustment for
        // these aligned report bases, keeping high physical-alias bits.
        m.Put<uint32_t>(kQuery+28+n*4,report-0xC0000000u);
        m.Put<uint32_t>(report+16,s.end0);m.Put<uint32_t>(report+20,s.end1);
        m.Put<uint32_t>(report+48,s.seed+n);m.Put<uint32_t>(report+52,s.seed+n+1);
        m.Put<uint16_t>(report,2);m.Put<uint16_t>(report+2,4);
        m.Put<uint16_t>(report+4,6);m.Put<uint16_t>(report+6,8);
        m.Put<uint16_t>(report+8,s.half0);m.Put<uint16_t>(report+10,s.half1);
    }
    return m;
}
struct RunResult {
    PPCContext context;
    Memory memory;
    bool threw=false;
    uint32_t host_fpscr=0;
    uint32_t observed_query=0,observed_status=0;
    uint64_t observed_caller=0;
    unsigned observations=0;
};
RunResult Run(const Scenario& s,const PPCContext& initial,unsigned mode) {
    RunResult r{};std::memcpy(&r.context,&initial,sizeof(PPCContext));
    r.memory=InitMemory(s);r.context.r4.u64=s.output;
    r.context.fpscr.setcsr(initial.fpscr.csr);current_memory=&r.memory;
    const auto invoke=[mode](PPCContext& context,uint8_t* base) {
        if(mode==0)ReferenceGetData(context,base);
        else gow3::renderer::InvokeGetDataSelected(context,base,mode==2,ReferenceGetData,InlineGetData);
    };
    try {
        if(!(s.epoch&1))invoke(r.context,nullptr);
        else gow3::renderer::InvokeQueryPollOriginal(r.context,nullptr,invoke,
            [&](uint32_t query,uint64_t caller,uint32_t status) noexcept {
                ++r.observations;r.observed_query=query;r.observed_caller=caller;r.observed_status=status;
            });
    }
    catch(const std::runtime_error&){r.threw=true;}
    r.host_fpscr=r.context.fpscr.getcsr();current_memory=nullptr;return r;
}
void Pair(const Scenario& s,uint32_t expected_status=UINT32_MAX) {
    ++cases;auto scenario=s;scenario.epoch=(cases&1)?3:0;const auto initial=Initial(s.seed);
    auto a=Run(scenario,initial,0),b=Run(scenario,initial,2),off=Run(scenario,initial,1);
    auto& coverage=reached[s.type>=8&&s.type<=10?s.type-8:3];++coverage.cases;
    for(const auto& event:a.memory.events) {
        coverage.mutations+=event.kind==Event::Mutation;
        coverage.kicks+=event.kind==Event::Kick;
        coverage.memzeros+=event.kind==Event::Memzero;
    }
    coverage.faults+=s.fault!=SIZE_MAX&&a.memory.accesses>s.fault&&a.threw;
    coverage.kick_failures+=s.throw_kick&&a.threw;
    coverage.memzero_failures+=s.throw_memzero&&a.threw;
    Require(a.threw==off.threw&&std::memcmp(&a.context,&off.context,sizeof(PPCContext))==0&&
        a.host_fpscr==off.host_fpscr&&a.memory.Same(off.memory),"Production selector OFF route changed original effects");
    const auto same_observe=[&](const RunResult& r) {
        return a.observations==r.observations&&a.observed_query==r.observed_query&&
            a.observed_caller==r.observed_caller&&a.observed_status==r.observed_status;
    };
    Require(same_observe(b)&&same_observe(off),"Selector changed original copied query observer ABI");
    Require(a.observations==unsigned(!a.threw&&bool(scenario.epoch&1)),"Observer executed on wrong epoch/exception path");
    Require(a.threw==b.threw,"Original exception outcome changed");
    Require(std::memcmp(&a.context,&b.context,sizeof(PPCContext))==0,"Whole CPU context changed");
    Require(a.host_fpscr==b.host_fpscr,"Host FPSCR changed");
    Require(a.memory.Same(b.memory),"Ordered guest reads/writes/external calls changed");
    Require(a.memory.bytes.size()<=Memory::kMaxBytes&&a.memory.events.size()<=Memory::kMaxEvents,"Fixture bound violated");
    Require(!a.memory.events.empty()&&a.memory.events.front()==Event{Event::Trace,0x82AFA708,0,0},"Original entry trace missing");
    if(expected_status!=UINT32_MAX)Require(!a.threw&&a.context.r3.u32==expected_status,"Independent expected status mismatch");
}
void Capability() {
    using namespace gow3::renderer;
    std::map<uint32_t,uint32_t> words;
    for(const auto pair:{std::pair{kInlineGetDataAddress,std::span<const uint32_t>(kInlineGetDataInstructions)},
                        std::pair{kInlineSave26Address,std::span<const uint32_t>(kInlineSave26Instructions)},
                        std::pair{kInlineRestore26Address,std::span<const uint32_t>(kInlineRestore26Instructions)}})
        for(size_t i=0;i<pair.second.size();++i)words.emplace(pair.first+uint32_t(i)*4,pair.second[i]);
    Require(words.size()==233,"Complete instruction authentication extent changed");
    GetDataInlineSelection selection;Require(!selection.enabled(),"Selector default is not original");
    unsigned reads=0;const auto read=[&](uint32_t address,uint32_t& value) {
        ++reads;const auto it=words.find(address);if(it==words.end())return false;value=it->second;return true;
    };
    Require(selection.TryEnable(true,true,true,read)&&selection.enabled()&&reads==233,"Exact complete code refused");
    for(const auto [requested,native,available]:std::array<std::array<bool,3>,3>{{{{false,true,true}},{{true,false,true}},{{true,true,false}}}}) {
        reads=0;Require(!selection.TryEnable(requested,native,available,read)&&!selection.enabled()&&reads==0,
            "OFF/Xenia/missing/trace/helper-refusal capability read guest memory or stayed enabled");
        Require(selection.TryEnable(true,true,true,read),"Exact route failed to reenable after refusal");
    }
    for(auto& [address,value]:words) {
        value^=1;Require(!selection.TryEnable(true,true,true,read)&&!selection.enabled(),"Changed guest instruction admitted");
        value^=1;Require(selection.TryEnable(true,true,true,read),"Fresh exact generation did not reenable");
        Require(!selection.TryEnable(true,true,true,[&](uint32_t a,uint32_t& v){return a!=address&&read(a,v);})&&
            !selection.enabled(),"Inaccessible instruction admitted");
        Require(selection.TryEnable(true,true,true,read),"Unmapped refusal retained stale state");
        bool threw=false;try {selection.TryEnable(true,true,true,[&](uint32_t a,uint32_t& v) {
            if(a==address)throw std::runtime_error("Injected startup access failure");return read(a,v);
        });}catch(const std::runtime_error&){threw=true;}
        Require(threw&&!selection.enabled(),"Startup exception retained selected body");
        Require(selection.TryEnable(true,true,true,read),"Exception refusal retained stale state");
    }
    selection.Reset();Require(!selection.enabled(),"Sink/teardown reset retained selection");
    std::cout<<"CAPABILITY instruction_words="<<words.size()<<" mutation_refusals="<<words.size()
        <<" unreadable_refusals="<<words.size()<<" exception_refusals="<<words.size()<<" no_native_owners=true\n";
}

}
int main() {
    Capability();
    PPCFPSCRRegister saved;saved.csr=saved.getcsr();
    Scenario s;s.flags=0;Pair(s,0x8876086A);s={};s.type=123;Pair(s,0);
    s={};s.type=8;s.tag=9;Pair(s,1);s.tag=7;Pair(s,0);s.tag=10;Pair(s,1);
    s={};s.type=9;s.tag=9;Pair(s,1);s.tag=10;Pair(s,1);
    s={};s.tag=0;Pair(s,0);s.end0=s.end1=uint32_t(-275);Pair(s,1);
    s.end1=0;Pair(s,0);s.mode=2;Pair(s,0);
    s={};s.type=10;s.tag=9;Pair(s,1);s.tag=10;Pair(s,1);
    s.tag=7;Pair(s,0);s.mode=2;Pair(s,0);
    // Exact unsigned-distance wrap behavior; no naive target<=completed change.
    for(uint32_t issued:{0u,1u,UINT32_MAX,0x80000000u})
        for(uint32_t completed:{0u,1u,UINT32_MAX,0x80000000u})
            for(uint32_t tag:{0u,1u,UINT32_MAX,0x80000000u})
                for(uint32_t type:{8u,9u,10u}) {s={};s.issued=issued;s.completed=completed;s.tag=tag;s.type=type;Pair(s);}
    // Distinct pointer aliases, zero-count original do/while, plural reports,
    // sentinel combinations, flag caching and every depth conversion shape.
    for(uint32_t type:{8u,9u,10u,7u})
        for(uint32_t count:{0u,1u,2u,8u})
            for(uint32_t mode:{0u,2u})
                for(uint32_t half:{0u,1u})
                    for(uint16_t bits:{uint16_t(0),uint16_t(1),uint16_t(0x100),uint16_t(0xFFF),uint16_t(0x8000),uint16_t(0xFFFF)}) {
                        s={};s.type=type;s.count=count;s.mode=mode;s.half=half;s.half0=bits;s.half1=bits;s.tag=0;
                        s.seed=uint32_t(cases+1);s.output=(cases&1)?kStack-56:0x4000;
                        s.report_base=(cases&2)?0xC000A000:0xC0009000;Pair(s);
                    }
    // Changes BETWEEN exact reads: may become ready, become pending, or reset
    // the current words. Reusing an address must re-read its new values.
    for(uint32_t type:{8u,9u,10u})for(size_t n=0;n<96;++n) {
        s={};s.type=type;s.tag=(n&1)?0:9;s.end0=s.end1=uint32_t(-275);
        s.mutation=n;s.seed=uint32_t(n+901);Pair(s);
    }
    // An exception at every bounded memory step must leave identical partially
    // executed CPU context/stack/output. Also test both real opaque call sites.
    for(uint32_t type:{8u,9u,10u})for(size_t n=0;n<96;++n) {
        s={};s.type=type;s.tag=0;s.count=8;s.half=1;s.fault=n;s.seed=uint32_t(n+1201);Pair(s);
    }
    for(uint32_t type:{8u,9u,10u}) {s={};s.type=type;s.tag=s.issued;s.throw_kick=true;Pair(s);}
    s={};s.type=10;s.mode=2;s.throw_memzero=true;Pair(s);
    // More independent contexts and owner/address reuse, no retained memo state.
    uint32_t rng=0x912AF008u;
    for(unsigned n=0;n<4096;++n) {
        rng=rng*1664525u+1013904223u;s={};s.seed=rng;s.type=8u+(rng%3);
        s.tag=(rng>>3)&15;s.issued=(rng>>7)&15;s.completed=(rng>>11)&15;
        s.count=(rng>>17)&7;s.flags=(rng&0x100)?0xC0:0x40;s.mode=(rng&0x200)?2:0;
        s.half=(rng>>10)&1;s.half0=uint16_t(rng);s.half1=uint16_t(rng>>16);
        s.end0=(rng&0x400)?uint32_t(-275):rng;s.end1=(rng&0x800)?uint32_t(-275):~rng;
        Pair(s);
    }
    saved.setcsr(saved.csr);
    for(size_t row=0;row<reached.size();++row) {
        const auto& c=reached[row];
        std::cout<<"REACHED type="<<(row==3?0:row+8)<<" cases="<<c.cases<<" mutations="<<c.mutations
            <<" faults="<<c.faults<<" kicks="<<c.kicks<<" memzeros="<<c.memzeros
            <<" kick_failures="<<c.kick_failures<<" memzero_failures="<<c.memzero_failures<<"\n";
    }
    std::cout<<"GetData production selector/source differential passed "<<checks<<" checks across "<<cases
             <<" bounded cases; shared ON/OFF dispatch + actual SDK context + original observer + ordered memory + faults; no volatile codegen/GPU/FPS proof\n";
}
