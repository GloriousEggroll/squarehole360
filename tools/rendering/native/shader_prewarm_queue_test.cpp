#include <atomic>
#include "renderer/native/shader_prewarm_queue.h"
#include "renderer/native/shader_discovery.h"
#include <chrono>
#include <iostream>
#include <span>

using namespace gow3::renderer;
void Require(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
struct Gate {
  std::mutex mutex;std::condition_variable changed;unsigned entered=0;bool open=false;
  void Enter() {std::unique_lock lock(mutex);++entered;changed.notify_all();changed.wait(lock,[&]{return open;});}
  void Wait(unsigned count) {std::unique_lock lock(mutex);Require(changed.wait_for(lock,std::chrono::seconds(5),[&]{return entered>=count;}),"Worker did not reach deterministic gate");}
  void Open() {std::lock_guard lock(mutex);open=true;changed.notify_all();}
};
struct Release {Gate& gate;~Release(){gate.Open();}};
auto Source(size_t bytes,uint8_t value=7) {return std::make_shared<const std::vector<uint8_t>>(bytes,value);}
void DiagnosticTests() {
  using Status=NativeShaderPrewarmQueue::JobStatus;
  {
    Gate gate;NativeShaderPrewarmQueue queue([&](const auto&){gate.Enter();},1,2,8);
    Release release{gate};
    const auto unknown=queue.Inspect("a.raster");
    Require(unknown.status==Status::Unknown&&unknown.coverage_complete&&
        !unknown.queued_ns&&!unknown.started_ns&&!unknown.finished_ns,"Unknown diagnostic state is not empty");
    auto source=Source(4);std::weak_ptr weak=source;
    Require(queue.TryEnqueue({"a.raster",source,false}),"Diagnostic running admission failed");
    source.reset();gate.Wait(1);
    const auto running=queue.Inspect("a.raster");
    Require(running.status==Status::Running&&running.coverage_complete&&running.queued_ns&&
        running.started_ns>=running.queued_ns&&!running.finished_ns,"Running timestamp state is invalid");
    Require(queue.TryEnqueue({"a.fsi",Source(4),true}),"Diagnostic queued admission failed");
    const auto queued=queue.Inspect("a.fsi");
    Require(queued.status==Status::Queued&&queued.queued_ns&&!queued.started_ns&&!queued.finished_ns,
        "Queued mode identity was confused with running identity");
    gate.Open();queue.Drain();
    const auto completed=queue.Inspect("a.raster");
    Require(completed.status==Status::Completed&&completed.queued_ns==running.queued_ns&&
        completed.started_ns==running.started_ns&&completed.finished_ns>=completed.started_ns,
        "Completion lost original admission/start timestamps");
    Require(weak.expired(),"Diagnostic history retained source ownership");
    const auto report=queue.diagnostics();
    Require(report.entries==2&&report.maximum_entries==2&&report.coverage_complete&&
        !report.metadata_failures&&report.failures.empty(),"Unexpected diagnostic metadata loss");
  }
  {
    Gate gate;unsigned attempts=0;
    NativeShaderPrewarmQueue queue([&](const auto& job){
      if(job.key=="block")gate.Enter();
      else if(++attempts==1)throw std::runtime_error("Intentional exact failure reason");
    },1,2,8);Release release{gate};
    Require(queue.TryEnqueue({"retry",Source(1),false}),"Diagnostic failure admission failed");queue.Drain();
    const auto failed=queue.Inspect("retry");
    Require(failed.status==Status::Failed&&failed.queued_ns&&failed.started_ns>=failed.queued_ns&&
        failed.finished_ns>=failed.started_ns,"Failed job timestamps missing");
    Require(queue.TryEnqueue({"block",Source(1),false}),"Retry gate admission failed");gate.Wait(1);
    Require(queue.TryEnqueue({"retry",Source(1),false}),"Diagnostics suppressed failure retry");
    const auto queued=queue.Inspect("retry");
    Require(queued.status==Status::Queued&&queued.queued_ns>=failed.finished_ns&&
        !queued.started_ns&&!queued.finished_ns,"Retry retained stale terminal timestamps");
    gate.Open();queue.Drain();
    Require(queue.Inspect("retry").status==Status::Completed,"Successful retry remained failed");
    const auto report=queue.diagnostics();
    Require(report.failures.size()==1&&report.failures[0].key=="retry"&&
        report.failures[0].reason=="Intentional exact failure reason"&&
        report.failures[0].queued_ns==failed.queued_ns&&report.failures[0].finished_ns==failed.finished_ns&&
        !report.failures[0].reason_truncated&&!report.failures[0].key_truncated,
        "Failure history lost exact original attempt");
  }
  {
    Gate gate;NativeShaderPrewarmQueue queue([&](const auto& job){if(job.key=="c")gate.Enter();},1,2,8);
    Release release{gate};
    for(const char* key:{"a","b"}) {queue.TryEnqueue({key,Source(1),false});queue.Drain();}
    queue.TryEnqueue({"c",Source(1),false});gate.Wait(1);
    Require(queue.Inspect("a").status==Status::Unknown&&!queue.Inspect("a").coverage_complete,
        "Evicted history falsely claims complete coverage");
    Require(!queue.TryEnqueue({"a",Source(1),false}),"Diagnostic eviction changed successful deduplication");
    queue.TryEnqueue({"d",Source(1),false});
    Require(queue.Inspect("c").status==Status::Running&&queue.Inspect("d").status==Status::Queued,
        "Diagnostic history displaced an active job");
    const auto report=queue.diagnostics();
    Require(report.entries==2&&report.history_evictions==2&&!report.coverage_complete,
        "Diagnostic registry exceeded or misreported its bound");
    queue.StopAccepting();
    Require(queue.Inspect("d").status==Status::Unknown&&!queue.Inspect("d").coverage_complete,
        "Cancelled job remained queued or implied unseen identity");
    gate.Open();queue.StopAndJoin();
  }
  {
    const std::string long_reason(700,'x');
    NativeShaderPrewarmQueue queue([&](const auto& job){
      if(job.key=="failure39")throw 39;
      throw std::runtime_error(long_reason);
    },1,4,8);
    for(unsigned i=0;i<40;++i) {
      Require(queue.TryEnqueue({"failure"+std::to_string(i),Source(1),false}),"Failure history admission failed");
      queue.Drain();
    }
    const auto report=queue.diagnostics();
    Require(report.entries==4&&report.failures.size()==32&&report.failure_records_dropped==8&&
        report.failures.front().key=="failure8"&&report.failures.back().key=="failure39",
        "Failure ring did not retain exactly the newest bounded records");
    Require(report.failures.front().reason==long_reason.substr(0,512)&&report.failures.front().reason_truncated&&
        report.failures.back().reason=="Non-standard exception"&&!report.failures.back().reason_truncated,
        "Failure reasons were unbounded or non-standard exception escaped");
    Require(queue.statistics().failures==40&&queue.statistics().completed==0,"Diagnostics changed failure accounting");
  }
  {
    NativeShaderPrewarmQueue queue([](const auto&){throw std::runtime_error("short");},1,1,8);
    const std::string key(256,'k');
    Require(queue.TryEnqueue({key,Source(1),false}),"Diagnostic key limit changed queue admission");queue.Drain();
    const auto state=queue.Inspect(key);const auto report=queue.diagnostics();
    Require(state.status==Status::Unknown&&!state.coverage_complete&&report.entries==0&&report.key_refusals==1,
        "Oversized key consumed unbounded diagnostic history");
    Require(report.failures.size()==1&&report.failures[0].key.size()==128&&report.failures[0].key_truncated,
        "Oversized failure identity was silently accepted as exact");
    Require(queue.TryEnqueue({key,Source(1),false}),"Oversized diagnostic identity prevented actual retry");queue.Drain();
  }
}
void QueueTests() {
  {
    Gate gate;
    NativeShaderPrewarmQueue queue([&](const auto& job){Require(job.source->at(0)==7,"Owned input changed");gate.Enter();},1,2,8);
    Release release{gate};
    auto source=Source(4);std::weak_ptr weak=source;
    Require(queue.TryEnqueue({"a.raster",source,false}),"First admission failed");gate.Wait(1);
    source.reset();Require(!weak.expired(),"Running source was released");
    Require(!queue.TryEnqueue({"a.raster",Source(4),false}),"Pending duplicate admitted");
    Require(queue.TryEnqueue({"a.fsi",Source(4),true}),"Mode variant collided");
    Require(!queue.TryEnqueue({"b.raster",Source(1),false}),"Running+queued job bound exceeded");
    const auto pending=queue.statistics();
    Require(pending.started==1&&pending.outstanding==2&&pending.retained_bytes==8,"Active bytes not charged");
    gate.Open();queue.Drain();
    Require(weak.expired(),"Completed source retained by dedup metadata");
    Require(!queue.TryEnqueue({"a.raster",Source(4),false}),"Completed duplicate admitted");
    queue.StopAndJoin();const auto stats=queue.statistics();
    Require(stats.admitted==2&&stats.completed==2&&stats.duplicate==2&&stats.full==1&&stats.outstanding==0&&stats.retained_bytes==0,"Admission accounting mismatch");
  }
  {
    Gate gate;NativeShaderPrewarmQueue queue([&](const auto&){gate.Enter();},1,4,5);Release release{gate};
    Require(queue.TryEnqueue({"a",Source(4),false}),"Byte case admission failed");gate.Wait(1);
    Require(!queue.TryEnqueue({"b",Source(2),false}),"Active source byte bound exceeded");
    auto pending=Source(1);std::weak_ptr weak=pending;
    Require(queue.TryEnqueue({"c",pending,false}),"Remaining byte was unavailable");pending.reset();
    queue.StopAccepting();
    Require(weak.expired(),"Cancelled source retained");
    Require(!queue.TryEnqueue({"d",Source(1),false}),"Stopped queue accepted work");
    gate.Open();queue.StopAndJoin();const auto stats=queue.statistics();
    Require(stats.admitted==2&&stats.cancelled==1&&stats.completed==1&&stats.stopped==1&&stats.full==1&&stats.outstanding==0,"Cancellation accounting mismatch");
  }
  {
    NativeShaderPrewarmQueue queue([](const auto& job){if(job.key=="bad")throw std::runtime_error("Intentional speculative failure");},1);
    Require(queue.TryEnqueue({"bad",Source(1),false}),"Failure case admission failed");queue.Drain();
    Require(queue.TryEnqueue({"bad",Source(1),false}),"Speculative failure was memoized as success");
    Require(queue.TryEnqueue({"good",Source(1),false}),"Failure disabled queue");queue.Drain();queue.StopAndJoin();
    const auto stats=queue.statistics();Require(stats.failures==2&&stats.completed==1&&stats.started==3,"Failure accounting mismatch");
  }
  {
    NativeShaderPrewarmQueue queue([](const auto&){},1,2,8);
    for(const char* key:{"a","b","c"}) {Require(queue.TryEnqueue({key,Source(1),false}),"Dedup history fixture failed");queue.Drain();}
    Require(!queue.TryEnqueue({"c",Source(1),false}),"Recent completed identity was lost");
    Require(queue.TryEnqueue({"a",Source(1),false}),"Completed identity history was not bounded");queue.Drain();
  }
  {
    Gate gate;NativeShaderPrewarmQueue queue([&](const auto&){gate.Enter();},2);Release release{gate};
    queue.TryEnqueue({"a",Source(1),false});queue.TryEnqueue({"b",Source(1),false});
    gate.Wait(2);Require(queue.statistics().started==2,"Workers did not overlap");gate.Open();queue.Drain();
  }
}
void SnapshotTests() {
  std::vector<uint8_t> memory(8192);
  auto word=[&](size_t at,uint32_t value){for(unsigned i=0;i<4;++i)memory.at(at+i)=uint8_t(value>>(24-i*8));};
  auto read=[&](uint32_t address,uint32_t bytes)->std::span<const uint8_t>{
    if(address>memory.size()||bytes>memory.size()-address)return {};
    return std::span<const uint8_t>(memory).subspan(address,bytes);
  };
  for(bool pixel:{false,true}) {
    const uint32_t object=128,offset=pixel?0x28:0x368,physical=4096;
    std::fill(memory.begin(),memory.end(),0);
    word(object+(pixel?0x18:0x20),physical);word(object+offset,pixel?0x102A1100:0x102A1141);
    word(object+offset+4,64);word(object+offset+8,24);
    for(unsigned i=0;i<24;++i)memory[physical+i]=uint8_t(i+1);
    auto snapshot=SnapshotDiscoveredShader(object,pixel,read);
    Require(snapshot&&snapshot->size()==88,"Exact shader snapshot missing");
    Require(std::equal(snapshot->begin(),snapshot->begin()+64,memory.begin()+object+offset),"Virtual bytes changed");
    Require(std::equal(snapshot->begin()+64,snapshot->end(),memory.begin()+physical),"Physical bytes changed");
    memory[physical]=254;word(object+offset,0);
    Require(snapshot->at(64)==1&&snapshot->at(0)==0x10,"Guest pointers escaped immutable snapshot");
    Require(!SnapshotDiscoveredShader(object,pixel,read),"Unsupported flags accepted");
    word(object+offset,pixel?0x102A1100:0x102A1101);word(object+offset+4,35);
    Require(!SnapshotDiscoveredShader(object,pixel,read),"Truncated virtual size accepted");
    word(object+offset+4,64);word(object+offset+8,128*1024+1);
    Require(!SnapshotDiscoveredShader(object,pixel,read),"Oversized physical section accepted");
    word(object+offset+8,24);word(object+(pixel?0x18:0x20),UINT32_MAX-3);
    Require(!SnapshotDiscoveredShader(object,pixel,read),"Wrapped physical address accepted");
    word(object+(pixel?0x18:0x20),8000);word(object+offset+8,256);
    Require(!SnapshotDiscoveredShader(object,pixel,read),"Unreadable physical range accepted");
    word(object+offset+8,0);auto zero=SnapshotDiscoveredShader(object,pixel,read);
    Require(zero&&zero->size()==64,"Zero code extent was invented or rejected before compiler validation");
  }
  Require(!SnapshotDiscoveredShader(UINT32_MAX-20,false,read),"Wrapped object address accepted");
  Require(!SnapshotDiscoveredShader(0,false,read),"Null object accepted");
}
void SpeculativeTests() {
  {
    // Without the speculative worker, speculative jobs use the normal lane.
    std::atomic<unsigned> ran{0};
    NativeShaderPrewarmQueue queue([&](const auto&){++ran;},1);
    Require(queue.TryEnqueue({"s",Source(1),false,true}),"Speculative admission without worker failed");
    queue.Drain();Require(ran==1&&queue.statistics().speculative_admitted==0,"Speculative job lost without its worker");
  }
  {
    // Speculative jobs never occupy the normal worker, and run on their own.
    Gate normal_gate,speculative_gate;std::mutex m;std::vector<std::string> normal_keys,speculative_keys;
    NativeShaderPrewarmQueue queue([&](const NativeShaderPrewarmQueue::Job& job) {
      if(job.speculative){speculative_gate.Enter();std::lock_guard l(m);speculative_keys.push_back(job.key);}
      else {normal_gate.Enter();std::lock_guard l(m);normal_keys.push_back(job.key);}
    },1,NativeShaderPrewarmQueue::kMaxJobs,NativeShaderPrewarmQueue::kMaxBytes,true);
    Release release_normal{normal_gate},release_speculative{speculative_gate};
    Require(queue.statistics().speculative_worker,"Speculative worker not started");
    Require(queue.TryEnqueue({"s1",Source(1),false,true}),"Speculative s1 admission failed");
    speculative_gate.Wait(1);  // s1 is running on the speculative worker.
    Require(queue.TryEnqueue({"s2",Source(1),false,true}),"Speculative s2 admission failed");
    Require(queue.TryEnqueue({"s3",Source(1),false,true}),"Speculative s3 admission failed");
    // A real request for queued s3 promotes it to the normal worker.
    Require(!queue.TryEnqueue({"s3",Source(1),false,false}),"Promotion must report a duplicate");
    normal_gate.Wait(1);
    Require(queue.statistics().promoted==1,"Queued speculative job was not promoted");
    normal_gate.Open();speculative_gate.Open();queue.Drain();
    std::lock_guard l(m);
    Require(normal_keys==std::vector<std::string>{"s3"},"Normal worker ran a speculative job");
    Require(speculative_keys==std::vector<std::string>{"s1","s2"},"Speculative order or lane is wrong");
    const auto stats=queue.statistics();
    Require(stats.speculative_admitted==3&&stats.speculative_started==2&&stats.completed==3,"Speculative statistics are wrong");
  }
  {
    // Shutdown cancels queued speculative work without running it.
    Gate gate;std::atomic<unsigned> ran{0};
    auto queue=std::make_unique<NativeShaderPrewarmQueue>([&](const auto&){gate.Enter();++ran;},1,
        NativeShaderPrewarmQueue::kMaxJobs,NativeShaderPrewarmQueue::kMaxBytes,true);
    Require(queue->TryEnqueue({"a",Source(1),false,true}),"Speculative a admission failed");gate.Wait(1);
    Require(queue->TryEnqueue({"b",Source(1),false,true}),"Speculative b admission failed");
    queue->StopAccepting();gate.Open();queue->StopAndJoin();
    Require(ran==1&&queue->statistics().cancelled==1,"Speculative shutdown ran or leaked queued work");
  }
}
// A shader a draw waits for runs before queued prewarm work, whether it is new
// or already queued behind that work.
void UrgentTests() {
  Gate gate;std::mutex mutex;std::vector<std::string> order;
  NativeShaderPrewarmQueue queue([&](const auto& job){
    if(job.key=="block")gate.Enter();
    std::lock_guard lock(mutex);order.push_back(job.key);
  },1);
  Release release{gate};
  Require(queue.TryEnqueue({"block",Source(1),false}),"Urgent block admission failed");gate.Wait(1);
  for(const char* key:{"p1","p2","p3"})Require(queue.TryEnqueue({key,Source(1),false}),"Prewarm admission failed");
  Require(queue.TryEnqueue({"draw",Source(1),false,false,true}),"Urgent admission failed");
  Require(!queue.TryEnqueue({"p3",Source(1),false,false,true}),"Urgent duplicate was admitted twice");
  gate.Open();queue.Drain();
  const std::vector<std::string> expected{"block","p3","draw","p1","p2"};
  Require(order==expected,"Urgent jobs did not run ahead of queued prewarm work");
  Require(queue.statistics().expedited==2,"Urgent jobs were not counted");
}
int main() {
  try {QueueTests();DiagnosticTests();SnapshotTests();SpeculativeTests();UrgentTests();std::cout<<"shader prewarm admission, lifetime, mode, bounds, failure, shutdown, snapshot and bounded diagnostics tests passed\n";}
  catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
