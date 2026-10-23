#include "renderer/native/gpu_batch_attribution_json.h"
#include <cstdlib>
#include <iostream>
#include <new>
#include <memory>
static bool deny_allocation=false;
static uint64_t allocations=0;
void* operator new(std::size_t bytes){if(deny_allocation)throw std::bad_alloc();++allocations;if(auto* p=std::malloc(bytes?bytes:1))return p;throw std::bad_alloc();}
static void ReleaseAllocation(void* p) noexcept {void (*volatile release)(void*)=std::free;release(p);}
void operator delete(void* p) noexcept{ReleaseAllocation(p);}
void operator delete(void* p,std::size_t) noexcept{ReleaseAllocation(p);}
using namespace gow3::renderer;
static unsigned checks=0;
void Check(bool value,const char* what){++checks;if(!value)throw std::runtime_error(what);}
NativeGpuBatchSample Sample(uint64_t submission,uint32_t pair,double ms) {
    NativeGpuBatchSample s;s.submission=submission;s.interval=NativeGpuTimestampInterval{10,20,10,ms};
    NativeGpuWorkTag tag;tag.event=submission*10;tag.frame=8;tag.pair=pair;tag.kind=NativeGpuWorkKind::Draw;
    tag.depth_control=2;tag.width=640;tag.height=360;tag.indices=3;s.Add(tag,true);return s;
}
int main() try {
    static_assert(uint8_t(NativeGpuWorkKind::Unknown)==0&&uint8_t(NativeGpuWorkKind::Draw)==1&&
        uint8_t(NativeGpuWorkKind::Clear)==2&&uint8_t(NativeGpuWorkKind::Resolve)==3&&uint8_t(NativeGpuWorkKind::AliasLoad)==4);
    static_assert(uint8_t(NativeGpuWorkKind::RetainedPhaseRestore)==5&&uint8_t(NativeGpuWorkKind::RetainedPhaseExport)==6);
    auto phase=std::make_unique<NativeGpuBatchAttribution>();
    constexpr std::array kinds{NativeGpuWorkKind::RetainedPhaseRestore,NativeGpuWorkKind::RetainedPhaseExport};
    constexpr std::array names{"retained_phase_restore","retained_phase_export"};
    for(size_t i=0;i<kinds.size();++i) {
        NativeGpuWorkTag tag;tag.kind=kinds[i];tag.frame=8;tag.event=100+i;tag.width=1280;tag.height=720;
        NativeGpuBatchSample sample;sample.submission=i+1;sample.interval=NativeGpuTimestampInterval{10,20,10,2};sample.Add(tag,false);
        Check(sample.Homogeneous()&&sample.transfers==1&&!sample.draws&&!sample.untagged,"Phase transfer lost distinct known classification");
        const auto allocated=allocations;deny_allocation=true;phase->Record(sample);deny_allocation=false;
        Check(allocations==allocated,"Phase diagnostic recording allocated");
        const auto row=ReportNativeGpuBatchSample(sample);
        Check(row.at("composition").at(0).at("kind")==names[i]&&std::string_view(NativeGpuWorkKindName(kinds[i]))==names[i],"Phase kind JSON spelling changed");
        tag.frame=UINT32_MAX;NativeGpuBatchSample unknown;unknown.Add(tag,false);
        Check(!unknown.Homogeneous()&&!unknown.frame_known,"Unscoped phase transfer invented an API frame");
    }
    const auto phase_report=ReportNativeGpuBatchAttribution(*phase,true);
    Check(phase_report.at("homogeneous_categories").size()==8&&phase->pairs().empty(),"Phase categories changed shader identity/bucket bounds");
    for(size_t i=0;i<kinds.size();++i) {
        const auto& category=phase_report.at("homogeneous_categories").at(5+i);
        Check(category.at("kind")==names[i]&&category.at("batches")==1&&category.at("lists")==1&&category.at("inclusive_ms")==2,"Phase category accounting lost actual interval");
    }
    NativeGpuBatchSample mixed_phase;NativeGpuWorkTag phase_tag;phase_tag.frame=8;
    phase_tag.kind=kinds[0];mixed_phase.Add(phase_tag,false);phase_tag.kind=kinds[1];mixed_phase.Add(phase_tag,false);
    Check(!mixed_phase.Homogeneous()&&mixed_phase.component_count==2,"Restore/export mixed work was mislabeled homogeneous");
    auto profile=std::make_unique<NativeGpuBatchAttribution>();
    std::string v(64,'a'),p(64,'b');const auto id=profile->InternPair(v,p);
    v[0]='c';p[0]='d';Check(profile->pairs()[id-1].vertex==std::string(64,'a')&&profile->pairs()[id-1].pixel==std::string(64,'b'),"Pair identity borrowed caller storage");
    Check(profile->InternPair(std::string(64,'a'),std::string(64,'b'))==id,"Exact shader pair did not reuse identity");
    const auto other=profile->InternPair(v,std::nullopt);Check(other!=id,"Builtin depth identity collapsed into a pixel shader");
    auto homogeneous=Sample(1,id,2);auto second=homogeneous.components[0].tag;second.event=19;homogeneous.Add(second,true);
    profile->Record(homogeneous);Check(profile->pairs()[0].homogeneous_inclusive_ms==2&&profile->pairs()[0].draw_lists==2,"Homogeneous interval was duplicated per draw");
    auto mixed=Sample(2,id,7);second.pair=other;mixed.Add(second,true);profile->Record(mixed);
    Check(!mixed.Homogeneous()&&profile->pairs()[0].homogeneous_inclusive_ms==2&&profile->pairs()[1].homogeneous_inclusive_ms==0,"Mixed interval was prorated to shaders");
    auto transfers=Sample(3,id,8);second.kind=NativeGpuWorkKind::Resolve;transfers.Add(second,false);profile->Record(transfers);
    Check(!transfers.Homogeneous()&&profile->pairs()[0].homogeneous_inclusive_ms==2,"Transfer time was charged to a shader");
    auto unavailable=Sample(4,id,0);unavailable.interval.reset();profile->Record(unavailable);
    Check(profile->pairs()[0].homogeneous_intervals==1&&profile->totals().unavailable==1,"Unavailable interval masqueraded as a zero GPU measurement");
    auto discarded=Sample(5,id,1);discarded.components[0].tag.discarded=true;profile->Record(discarded);
    Check(profile->recent(4).components[0].tag.discarded&&profile->pairs()[0].homogeneous_inclusive_ms==2,
        "Discard-only command list lost classification or was charged as shader execution");
    auto query_mix=Sample(6,id,3);second=query_mix.components[0].tag;second.queried=true;query_mix.Add(second,true);
    Check(!query_mix.Homogeneous(),"Queried and unqueried work lost category distinction");
    auto frames=Sample(6,id,3);second=frames.components[0].tag;second.frame=9;frames.Add(second,true);profile->Record(frames);
    Check(!frames.Homogeneous(),"Cross-frame batch was attributed to one view");
    auto unknown=Sample(7,0,9);profile->Record(unknown);Check(!unknown.Homogeneous()&&unknown.untagged==1,"Unknown shader identity was attributed");
    auto overflow=Sample(8,id,100);
    for(uint32_t i=1;i<=NativeGpuBatchSample::kComponents;++i){second=overflow.components[0].tag;second.width+=i;overflow.Add(second,true);}
    profile->Record(overflow);Check(overflow.overflow_lists==1&&!overflow.Homogeneous(),"Composition bound did not mark ambiguity");
    const auto before=allocations;deny_allocation=true;
    for(uint64_t i=9;i<1200;++i)profile->Record(Sample(i,id,double(i)));
    deny_allocation=false;Check(allocations==before,"Fence-retired recording allocated memory");
    Check(profile->recent_count()==256&&profile->recent(0).submission==944&&profile->recent(255).submission==1199,"Recent ring order/bound changed");
    Check(profile->slowest().size()==32&&profile->totals().evicted_recent==1199-256,"Slow/recent bounds are incorrect");
    auto report=ReportNativeGpuBatchAttribution(*profile,true);Check(report==ReportNativeGpuBatchAttribution(*profile,true),"Report observation changed counters");
    Check(report.at("recent_retired_batches").size()==256&&report.at("slowest_retired_batches").size()==32,"Cold JSON report exceeded retained bounds");
    Check(report.at("recent_window").at("batches")==256&&report.at("recent_window").at("homogeneous_batches")==256&&
        report.at("recent_window").at("top_homogeneous_shader_pairs").at(0).at("homogeneous_batches")==256,
        "Recent camera window was confused with cumulative startup batches");
    auto full=std::make_unique<NativeGpuBatchAttribution>();
    for(uint32_t i=0;i<NativeGpuBatchAttribution::kPairs;++i){std::string key(64,'f');for(unsigned j=0;j<8;++j)key[j]="0123456789abcdef"[(i>>(j*4))&15];Check(full->InternPair(key,std::nullopt)==i+1,"Shader dictionary admission failed");}
    Check(full->InternPair(std::string(64,'z'),std::nullopt)==0&&full->pairs().size()==2048&&full->totals().pair_overflows==1,"Shader dictionary exceeded its bound");
    deny_allocation=true;Check(profile->InternPair(std::string_view(v),std::string_view(p))==0,"Diagnostic allocation failure escaped or admitted partial identity");deny_allocation=false;
    Check(profile->totals().pair_failures==1,"Allocation failure counter missing");
    Check(profile->InternPair("bad",std::nullopt)==0,"Malformed digest exceeded bounded identity contract");
    std::cout<<"PASS "<<checks<<" checks; fixed attribution bytes "<<sizeof(NativeGpuBatchAttribution)<<", sample bytes "<<sizeof(NativeGpuBatchSample)<<'\n';
} catch(const std::exception& e){deny_allocation=false;std::cerr<<e.what()<<'\n';return 1;}
