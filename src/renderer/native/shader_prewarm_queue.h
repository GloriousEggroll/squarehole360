#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include "background_thread_priority.h"

namespace gow3::renderer {

// CPU-only speculation. Admission never waits for compilation or free space.
// The callback must not access guest memory, capture locks, or GPU objects.
// A queued job deliberately does not reserve a compiler future: a real draw
// may compile it immediately instead of waiting behind unrelated queued jobs.
class NativeShaderPrewarmQueue {
 public:
  static constexpr size_t kMaxJobs = 4096, kMaxBytes = 64u * 1024 * 1024;
  static constexpr unsigned kMaxWorkers = 32;
  static constexpr size_t kMaxFailureRecords = 32, kMaxFailureReasonBytes = 512,
                          kMaxDiagnosticKeyBytes = 128;
  struct Job {
    std::string key;  // Exact source SHA-256 plus compilation mode.
    std::shared_ptr<const std::vector<uint8_t>> source;
    bool fragment_interlock = false;
    // Speculative neighbours (block discovery) run only on the single
    // lowest-priority worker, never on the workers serving real requests.
    bool speculative = false;
    // A draw is waiting for this shader (async shaders): it runs before any
    // queued prewarm job instead of behind them.
    bool urgent = false;
  };
  struct Statistics {
    uint64_t attempted=0, admitted=0, duplicate=0, full=0, stopped=0;
    uint64_t started=0, completed=0, failures=0, cancelled=0;
    size_t outstanding=0, retained_bytes=0, peak_jobs=0, peak_bytes=0;
    unsigned workers=0, urgent_helpers=0;
    uint64_t speculative_admitted=0, speculative_started=0, promoted=0, expedited=0;
    bool speculative_worker=false;
  };
  enum class JobStatus { Unknown, Queued, Running, Completed, Failed };
  struct JobInspection {
    JobStatus status=JobStatus::Unknown;
    // False after any admission refusal, cancellation, diagnostic eviction or
    // metadata loss. Unknown never independently means "never discovered".
    bool coverage_complete=true;
    uint64_t queued_ns=0, started_ns=0, finished_ns=0;
  };
  struct FailureRecord {
    std::string key, reason;
    uint64_t queued_ns=0, started_ns=0, finished_ns=0;
    bool key_truncated=false, reason_truncated=false;
  };
  struct Diagnostics {
    size_t entries=0, maximum_entries=0;
    bool coverage_complete=true;
    uint64_t history_evictions=0, metadata_failures=0, key_refusals=0,
             failure_records_dropped=0;
    std::vector<FailureRecord> failures;
  };
  using Compile = std::function<void(const Job&)>;

  explicit NativeShaderPrewarmQueue(Compile compile, unsigned workers=0,
      size_t max_jobs=kMaxJobs, size_t max_bytes=kMaxBytes, bool speculative_worker=false)
      : compile_(std::move(compile)), max_jobs_(max_jobs), max_bytes_(max_bytes) {
    // Default sizing also adds helpers for jobs a draw is waiting on.
    const unsigned urgent_helpers=workers ? 0u : std::min(UrgentCompileHelperThreads(),kMaxWorkers);
    if (!compile_ || !max_jobs || max_jobs>kMaxJobs || !max_bytes || max_bytes>kMaxBytes)
      throw std::invalid_argument("Invalid native shader prewarm queue bounds");
    workers=std::clamp(workers ? workers : BackgroundCompileThreads(),1u,kMaxWorkers);
    workers=unsigned(std::min<size_t>(workers,max_jobs));
    threads_.reserve(workers);
    try {
      for(unsigned i=0;i<workers;++i) threads_.emplace_back([this]{Worker();});
      statistics_.workers=workers;
      for(unsigned i=0;i<urgent_helpers;++i) threads_.emplace_back([this]{UrgentHelper();});
      statistics_.urgent_helpers=urgent_helpers;
      if(speculative_worker) {
        threads_.emplace_back([this]{SpeculativeWorker();});
        statistics_.speculative_worker=true;
      }
    } catch (...) {
      StopAndJoin();
      throw;
    }
  }
  ~NativeShaderPrewarmQueue() { StopAndJoin(); }
  NativeShaderPrewarmQueue(const NativeShaderPrewarmQueue&)=delete;
  NativeShaderPrewarmQueue& operator=(const NativeShaderPrewarmQueue&)=delete;

  bool TryEnqueue(Job job) {
    if(!job.source || job.source->empty() || job.key.empty())
      throw std::invalid_argument("Shader prewarm requires an owned source and identity");
    std::lock_guard lock(mutex_);
    ++statistics_.attempted;
    if(stopping_) {++statistics_.stopped;diagnostic_coverage_complete_=false;return false;}
    if(pending_.contains(job.key)||completed_.contains(job.key)) {
      // A real request for a still-queued speculative job moves it ahead.
      if(!job.speculative) {
        const auto found=std::find_if(speculative_.begin(),speculative_.end(),
            [&](const Job& queued){return queued.key==job.key;});
        if(found!=speculative_.end()) {
          jobs_.push_back(std::move(*found));speculative_.erase(found);
          jobs_.back().speculative=false;++statistics_.promoted;changed_.notify_all();
        }
      }
      // A waiting draw moves its still-queued job to the front.
      if(job.urgent) {
        const auto found=std::find_if(jobs_.begin(),jobs_.end(),
            [&](const Job& queued){return queued.key==job.key;});
        if(found!=jobs_.begin()&&found!=jobs_.end()) {
          Job moved=std::move(*found);jobs_.erase(found);moved.urgent=true;
          jobs_.push_front(std::move(moved));++statistics_.expedited;changed_.notify_all();
        }
      }
      ++statistics_.duplicate;return false;
    }
    const size_t bytes=job.source->size();
    if(statistics_.outstanding>=max_jobs_ || bytes>max_bytes_ ||
        statistics_.retained_bytes>max_bytes_-bytes) {
      ++statistics_.full;diagnostic_coverage_complete_=false;return false;
    }
    auto [entry,inserted]=pending_.insert(job.key);
    const bool speculative=job.speculative&&statistics_.speculative_worker;
    const bool urgent=job.urgent&&!speculative;
    auto& queue=speculative?speculative_:jobs_;
    try { if(urgent) queue.push_front(std::move(job)); else queue.push_back(std::move(job)); }
    catch (...) {pending_.erase(entry);throw;}
    statistics_.expedited+=urgent;
    statistics_.speculative_admitted+=speculative;
    ++statistics_.admitted;++statistics_.outstanding;
    statistics_.retained_bytes+=bytes;
    statistics_.peak_jobs=std::max(statistics_.peak_jobs,statistics_.outstanding);
    statistics_.peak_bytes=std::max(statistics_.peak_bytes,statistics_.retained_bytes);
    DiagnoseAdmission(urgent?queue.front().key:queue.back().key);
    // Both worker kinds wait on one condition; wake all so the right one runs.
    changed_.notify_all();
    return true;
  }

  // Safe on the command thread: discard unused queued work, but never wait for
  // a worker whose exact shader may currently be needed by a real draw.
  void StopAccepting() {
    std::lock_guard lock(mutex_);
    stopping_=true;
    for(auto* queue:{&jobs_,&speculative_}) {
      for(const auto& job:*queue) {
        pending_.erase(job.key);
        diagnostic_jobs_.erase(job.key);diagnostic_coverage_complete_=false;
        --statistics_.outstanding;statistics_.retained_bytes-=job.source->size();
        ++statistics_.cancelled;
      }
      queue->clear();
    }
    changed_.notify_all();
  }
  // Owner must call after detaching discovery, outside the bridge capture lock.
  // No callback takes this lock while compiling, and compilation is CPU-only.
  void StopAndJoin() {
    StopAccepting();
    for(auto& thread:threads_) if(thread.joinable()) thread.join();
  }
  // Diagnostic/tests only. Gameplay never drains speculative work at a frame
  // or query boundary; real Get retains its ordinary dependency semantics.
  void Drain() {
    std::unique_lock lock(mutex_);
    changed_.wait(lock,[&]{return !statistics_.outstanding;});
  }
  Statistics statistics() const {std::lock_guard lock(mutex_);return statistics_;}
  // Cold lookup/shutdown diagnostics only. No waiting on compilation and no
  // source ownership; the existing pending/completed sets remain authoritative.
  JobInspection Inspect(const std::string& key) const {
    std::lock_guard lock(mutex_);
    const auto found=diagnostic_jobs_.find(key);
    auto result=found==diagnostic_jobs_.end()?JobInspection{}:found->second;
    result.coverage_complete=diagnostic_coverage_complete_;return result;
  }
  Diagnostics diagnostics() const {
    std::lock_guard lock(mutex_);
    return {diagnostic_jobs_.size(),max_jobs_,diagnostic_coverage_complete_,
        diagnostic_history_evictions_,diagnostic_metadata_failures_,diagnostic_key_refusals_,
        failure_records_dropped_,{failure_records_.begin(),failure_records_.end()}};
  }

 private:
  Compile compile_;
  const size_t max_jobs_,max_bytes_;
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::deque<Job> jobs_,speculative_;
  std::unordered_set<std::string> pending_,completed_;
  std::deque<std::string> completed_order_;
  std::vector<std::thread> threads_;
  bool stopping_=false;
  Statistics statistics_;
  std::unordered_map<std::string,JobInspection> diagnostic_jobs_;
  std::deque<FailureRecord> failure_records_;
  bool diagnostic_coverage_complete_=true;
  uint64_t diagnostic_history_evictions_=0,diagnostic_metadata_failures_=0,
           diagnostic_key_refusals_=0,failure_records_dropped_=0;

  static uint64_t NowNs() noexcept {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
  }
  // Called under mutex_. All diagnostic allocation failures are contained and
  // cannot reject an otherwise admitted job or change successful deduplication.
  void DiagnoseAdmission(const std::string& key) noexcept {
    if(key.size()>kMaxDiagnosticKeyBytes) {
      diagnostic_coverage_complete_=false;++diagnostic_key_refusals_;return;
    }
    try {
      auto found=diagnostic_jobs_.find(key);
      if(found==diagnostic_jobs_.end()) {
        if(diagnostic_jobs_.size()==max_jobs_) {
          auto oldest=diagnostic_jobs_.end();
          for(auto it=diagnostic_jobs_.begin();it!=diagnostic_jobs_.end();++it) {
            if(it->second.status!=JobStatus::Completed&&it->second.status!=JobStatus::Failed)continue;
            if(oldest==diagnostic_jobs_.end()||it->second.queued_ns<oldest->second.queued_ns)oldest=it;
          }
          // Never discard current queued/running state to retain history.
          if(oldest==diagnostic_jobs_.end()) {
            diagnostic_coverage_complete_=false;++diagnostic_metadata_failures_;return;
          }
          diagnostic_jobs_.erase(oldest);++diagnostic_history_evictions_;
          diagnostic_coverage_complete_=false;
        }
        found=diagnostic_jobs_.try_emplace(key).first;
      }
      found->second={JobStatus::Queued,true,NowNs(),0,0};
    }catch(...) {diagnostic_coverage_complete_=false;++diagnostic_metadata_failures_;}
  }
  void RememberFailure(const Job& job,const JobInspection& times,
                       const char* reason,size_t reason_bytes,bool reason_truncated) noexcept {
    try {
      FailureRecord record{job.key.substr(0,kMaxDiagnosticKeyBytes),std::string(reason,reason_bytes),
          times.queued_ns,times.started_ns,times.finished_ns,
          job.key.size()>kMaxDiagnosticKeyBytes,reason_truncated};
      if(failure_records_.size()==kMaxFailureRecords) {
        failure_records_.pop_front();++failure_records_dropped_;
      }
      failure_records_.push_back(std::move(record));
    }catch(...) {++failure_records_dropped_;}
  }

  void SpeculativeWorker() noexcept {
    LowerCurrentThreadPriority();
    for(;;) {
      Job job;
      {
        std::unique_lock lock(mutex_);
        changed_.wait(lock,[&]{return stopping_||!speculative_.empty();});
        if(speculative_.empty()) return;
        job=std::move(speculative_.front());speculative_.pop_front();
        ++statistics_.started;++statistics_.speculative_started;
        if(auto found=diagnostic_jobs_.find(job.key);found!=diagnostic_jobs_.end()) {
          found->second.status=JobStatus::Running;found->second.started_ns=NowNs();
        }
      }
      Run(std::move(job));
    }
  }
  // Takes only jobs a draw waits on; those sit at the front of jobs_.
  void UrgentHelper() noexcept {
    LowerCurrentThreadPriority();
    for(;;) {
      Job job;
      {
        std::unique_lock lock(mutex_);
        changed_.wait(lock,[&]{return stopping_||(!jobs_.empty()&&jobs_.front().urgent);});
        if(stopping_) return;
        job=std::move(jobs_.front());jobs_.pop_front();++statistics_.started;
        if(auto found=diagnostic_jobs_.find(job.key);found!=diagnostic_jobs_.end()) {
          found->second.status=JobStatus::Running;found->second.started_ns=NowNs();
        }
      }
      Run(std::move(job));
    }
  }
  void Worker() noexcept {
    // A new area can queue hundreds of compiles at once; the game's own
    // threads must keep running while they drain.
    LowerCurrentThreadPriority();
    for(;;) {
      Job job;
      {
        std::unique_lock lock(mutex_);
        changed_.wait(lock,[&]{return stopping_||!jobs_.empty();});
        if(jobs_.empty()) return;
        job=std::move(jobs_.front());jobs_.pop_front();++statistics_.started;
        if(auto found=diagnostic_jobs_.find(job.key);found!=diagnostic_jobs_.end()) {
          found->second.status=JobStatus::Running;found->second.started_ns=NowNs();
        }
      }
      Run(std::move(job));
    }
  }
  void Run(Job job) noexcept {
    {
      bool success=false;
      std::array<char,kMaxFailureReasonBytes+1> failure_reason{};
      size_t reason_bytes=0;bool reason_truncated=false;
      const auto remember_reason=[&](const char* reason) noexcept {
        if(!reason)reason="Exception without a reason";
        while(reason_bytes<kMaxFailureReasonBytes&&reason[reason_bytes]) {
          failure_reason[reason_bytes]=reason[reason_bytes];++reason_bytes;
        }
        reason_truncated=reason_bytes==kMaxFailureReasonBytes&&reason[reason_bytes];
      };
      try {CompileSlot slot(!job.urgent);compile_(job);success=true;}
      catch(const std::exception& error) {remember_reason(error.what());}
      catch (...) {
        // Existing exact Get remains the only rendering authority. Speculative
        // validation/translation failures do not disable the native renderer.
        remember_reason("Non-standard exception");
      }
      const auto finished_ns=NowNs();
      {
        std::lock_guard lock(mutex_);
        JobInspection times;times.finished_ns=finished_ns;
        if(auto found=diagnostic_jobs_.find(job.key);found!=diagnostic_jobs_.end()) {
          found->second.status=success?JobStatus::Completed:JobStatus::Failed;
          found->second.finished_ns=finished_ns;times=found->second;
        }
        if(success) {
          ++statistics_.completed;
          // Bound successful dedup metadata too. Failure to remember a success
          // merely permits another cheap call into the compiler's exact cache.
          try {
            if(completed_order_.size()==max_jobs_) {
              completed_.erase(completed_order_.front());completed_order_.pop_front();
            }
            auto [entry,inserted]=completed_.insert(job.key);
            if(inserted) {
              try {completed_order_.push_back(job.key);}
              catch (...) {completed_.erase(entry);throw;}
            }
          } catch (...) {}
        } else {
          ++statistics_.failures;
          RememberFailure(job,times,failure_reason.data(),reason_bytes,reason_truncated);
        }
        pending_.erase(job.key);
        --statistics_.outstanding;statistics_.retained_bytes-=job.source->size();
        // Release the last source owner before signalling a drained queue.
        job.source.reset();changed_.notify_all();
      }
    }
  }
};
} // namespace gow3::renderer
