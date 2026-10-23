// Includes a hash-pinned, complete real SDK implementation, never a copied Wait.
// Every owner-death result is produced by a real robust pthread mutex.
#include GOW3_ROBUST_CONDITION_SDK_SOURCE

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <sys/resource.h>
#include <sys/wait.h>

#if !REX_PLATFORM_LINUX
#error This fixture exercises Linux robust pthread mutexes only.
#endif

namespace {
using rex::thread::WaitResult;
using namespace std::chrono_literals;

void Require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}

template <class Predicate>
void Until(Predicate predicate, const char* message) {
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (!predicate()) {
    Require(std::chrono::steady_clock::now() < deadline, message);
    std::this_thread::yield();
  }
}

// The only seam is access to the SDK's existing protected native objects.
// All waits, robust initialization, post_execution dispatch and mutex ownership
// are provided by the exact included SDK PosixConditionBase.
class Probe final : public rex::thread::PosixConditionBase {
 public:
  bool Signal() override {
    std::unique_lock<std::mutex> lock(mutex_);
    count_ += 1;
    cond_.notify_all();
    return true;
  }
  pthread_mutex_t* Mutex() { return mutex_.native_handle(); }
  void NotifyLocked() { cond_.notify_all(); }
  void SetLocked(unsigned count) { count_ = count; }
  std::atomic<unsigned> predicates{0};
  unsigned consumed = 0;
 protected:
  bool signaled() const override {
    const_cast<Probe*>(this)->predicates.fetch_add(1, std::memory_order_release);
    return count_ != 0;
  }
  void post_execution() override { --count_; ++consumed; }
 private:
  unsigned count_ = 0;
};

struct WaitContext {
  Probe* probe;
  std::chrono::milliseconds timeout;
  WaitResult result = WaitResult::kFailed;
  std::atomic<bool> returned{false};
};
void* WaitThread(void* opaque) {
  auto& context = *static_cast<WaitContext*>(opaque);
  context.result = context.probe->Wait(context.timeout);
  context.returned.store(true, std::memory_order_release);
  return nullptr;
}

enum class Death { Exit, Cancel, AsyncCancel };
struct OwnerContext {
  Probe* probe;
  unsigned count;
  Death death;
  std::atomic<bool> locked{false};
  std::atomic<unsigned> predicate_at_lock{0};
  sem_t cancel_point;
};
void* OwnerThread(void* opaque) {
  auto& context = *static_cast<OwnerContext*>(opaque);
  // pthread_mutex_lock can succeed only after the actual SDK wait has atomically
  // released this mutex into pthread_cond_*wait; its initial predicate is false.
  const int result = pthread_mutex_lock(context.probe->Mutex());
  if (result != 0) std::_Exit(91);
  context.predicate_at_lock.store(context.probe->predicates.load(std::memory_order_acquire),
                                  std::memory_order_release);
  context.probe->SetLocked(context.count);
  context.probe->NotifyLocked();
  if (context.death == Death::AsyncCancel) {
    if (pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, nullptr) != 0) std::_Exit(92);
  }
  context.locked.store(true, std::memory_order_release);
  if (context.death == Death::Exit) pthread_exit(nullptr);
  if (context.death == Death::AsyncCancel) {
    for (;;) sched_yield();
  }
  // A genuine cancellation point while owning the raw robust mutex. No RAII
  // unlock is installed: owner termination must produce actual owner death.
  while (sem_wait(&context.cancel_point) == -1 && errno == EINTR) {}
  std::_Exit(93);
}

void JoinOwner(pthread_t owner, OwnerContext& context) {
  Until([&] { return context.locked.load(std::memory_order_acquire); }, "owner never acquired released wait mutex");
  if (context.death != Death::Exit) Require(pthread_cancel(owner) == 0, "owner cancellation failed");
  void* outcome = nullptr;
  Require(pthread_join(owner, &outcome) == 0, "owner join failed");
  Require(outcome == (context.death == Death::Exit ? nullptr : PTHREAD_CANCELED),
          "wrong real owner termination outcome");
  Require(sem_destroy(&context.cancel_point) == 0, "owner semaphore destruction failed");
}

void AssertConsistent(Probe& probe) {
  Require(pthread_mutex_lock(probe.Mutex()) == 0, "recovered mutex is not consistent");
  Require(pthread_mutex_unlock(probe.Mutex()) == 0, "recovered mutex unlock failed");
}

void Reacquire(std::chrono::milliseconds timeout, Death death, unsigned count,
               bool before_timed_poison = false) {
  Probe probe;
  WaitContext waiter{&probe, timeout};
  pthread_t wait_thread;
  Require(pthread_create(&wait_thread, nullptr, WaitThread, &waiter) == 0, "waiter creation failed");
  Until([&] { return probe.predicates.load(std::memory_order_acquire) != 0; }, "initial SDK predicate not reached");
  OwnerContext owner_context{&probe, count, death};
  Require(sem_init(&owner_context.cancel_point, 0, 0) == 0, "cancel semaphore init failed");
  pthread_t owner;
  Require(pthread_create(&owner, nullptr, OwnerThread, &owner_context) == 0, "owner creation failed");
  JoinOwner(owner, owner_context);
  if (count == 0 && timeout == std::chrono::milliseconds::max()) {
    const auto previous = owner_context.predicate_at_lock.load(std::memory_order_acquire);
    Until([&] { return probe.predicates.load(std::memory_order_acquire) > previous; },
          "recovered false predicate not rechecked");
    Require(!waiter.returned.load(std::memory_order_acquire), "spurious owner-death wake returned success");
    Require(probe.Signal(), "later real signal failed");
  }
  void* wait_outcome = nullptr;
  Require(pthread_join(wait_thread, &wait_outcome) == 0 && wait_outcome == nullptr, "waiter join failed");
  Require(waiter.returned.load(std::memory_order_acquire), "SDK waiter did not return");
  const bool success = count != 0 || timeout == std::chrono::milliseconds::max();
  Require(waiter.result == (success ? WaitResult::kSuccess : WaitResult::kTimeout), "wrong SDK wait result");
  Require(probe.consumed == (success ? 1u : 0u), "wrong protected post_execution count");
  if (before_timed_poison) {
    // Before's timed wrapper drops the POSIX return and never repairs it.
    Require(pthread_mutex_lock(probe.Mutex()) == ENOTRECOVERABLE,
            "old timed path did not leave real mutex irrecoverable");
  } else {
    AssertConsistent(probe);
    Require(probe.Wait(0ms) == WaitResult::kTimeout, "recovered wait duplicated consumption");
  }
}

void InitialOwnerDeath(bool poison) {
  Probe probe;
  struct Context { Probe* probe; } context{&probe};
  pthread_t owner;
  Require(pthread_create(&owner, nullptr, [](void* opaque) -> void* {
    auto& value = *static_cast<Context*>(opaque);
    if (pthread_mutex_lock(value.probe->Mutex()) != 0) std::_Exit(94);
    value.probe->SetLocked(1);
    pthread_exit(nullptr);
  }, &context) == 0, "initial owner create failed");
  Require(pthread_join(owner, nullptr) == 0, "initial owner join failed");
  if (poison) {
    Require(pthread_mutex_lock(probe.Mutex()) == EOWNERDEAD, "initial real owner-death status absent");
    Require(pthread_mutex_unlock(probe.Mutex()) == 0, "poison unlock failed");
    Require(probe.Wait(0ms) == WaitResult::kFailed, "irrecoverable initial lock did not fail");
    Require(probe.consumed == 0, "irrecoverable state consumed a predicate");
    Require(pthread_mutex_lock(probe.Mutex()) == ENOTRECOVERABLE, "initial poison was repaired unexpectedly");
  } else {
    Require(probe.Wait(0ms) == WaitResult::kSuccess, "original initial owner-death recovery changed");
    Require(probe.consumed == 1, "initial recovery did not consume once");
    AssertConsistent(probe);
  }
}

void IrrecoverableReacquire() {
  Probe probe;
  WaitContext waiter{&probe, 500ms};
  pthread_t wait_thread;
  Require(pthread_create(&wait_thread, nullptr, WaitThread, &waiter) == 0, "poison waiter create failed");
  Until([&] { return probe.predicates.load(std::memory_order_acquire) != 0; }, "poison predicate not reached");
  // First owner acquires only after the waiter sleeps; no notification yet.
  pthread_t owner;
  Require(pthread_create(&owner, nullptr, [](void* opaque) -> void* {
    auto& value = *static_cast<Probe*>(opaque);
    if (pthread_mutex_lock(value.Mutex()) != 0) std::_Exit(95);
    pthread_exit(nullptr);
  }, &probe) == 0, "poison owner create failed");
  Require(pthread_join(owner, nullptr) == 0, "poison owner join failed");
  Require(pthread_mutex_lock(probe.Mutex()) == EOWNERDEAD, "poison repair owner status absent");
  probe.NotifyLocked();
  Require(pthread_mutex_unlock(probe.Mutex()) == 0, "irrecoverable wake unlock failed");
  Require(pthread_join(wait_thread, nullptr) == 0, "poison waiter join failed");
  Require(waiter.result == WaitResult::kFailed && probe.consumed == 0,
          "irrecoverable reacquire must fail without consumption");
  Require(pthread_mutex_lock(probe.Mutex()) == ENOTRECOVERABLE, "irrecoverable reacquire unexpectedly repaired mutex");
}

void CancelWaiter(std::chrono::milliseconds timeout) {
  Probe probe;
  WaitContext waiter{&probe, timeout};
  pthread_t thread;
  Require(pthread_create(&thread, nullptr, WaitThread, &waiter) == 0, "cancellable waiter create failed");
  Until([&] { return probe.predicates.load(std::memory_order_acquire) != 0; }, "cancellable predicate not reached");
  // Lock success proves the SDK wait released its mutex before cancellation.
  Require(pthread_mutex_lock(probe.Mutex()) == 0, "cancellable waiter did not release mutex");
  Require(pthread_mutex_unlock(probe.Mutex()) == 0, "cancellable probe unlock failed");
  Require(pthread_cancel(thread) == 0, "waiter cancellation failed");
  void* outcome = nullptr;
  Require(pthread_join(thread, &outcome) == 0 && outcome == PTHREAD_CANCELED, "waiter was not cancelled cleanly");
  Require(!waiter.returned.load(std::memory_order_acquire) && probe.consumed == 0,
          "cancelled waiter returned/consumed a predicate");
  AssertConsistent(probe);
  Require(probe.Signal() && probe.Wait(0ms) == WaitResult::kSuccess, "cancelled waiter leaked mutex ownership");
}

void OrdinaryPredicates() {
  rex::thread::PosixCondition<rex::thread::Event> automatic(false, true);
  Require(automatic.Wait(0ms) == WaitResult::kSuccess && automatic.Wait(0ms) == WaitResult::kTimeout,
          "actual auto-reset event consumption changed");
  Require(automatic.Signal() && automatic.Wait(0ms) == WaitResult::kSuccess,
          "actual event signal failed");
  rex::thread::PosixCondition<rex::thread::Event> manual(true, true);
  Require(manual.Wait(0ms) == WaitResult::kSuccess && manual.Wait(0ms) == WaitResult::kSuccess,
          "actual manual event state changed");
  manual.Reset();
  Require(manual.Wait(0ms) == WaitResult::kTimeout, "actual event reset failed");
  rex::thread::PosixCondition<rex::thread::Semaphore> semaphore(2, 3);
  Require(semaphore.Wait(0ms) == WaitResult::kSuccess && semaphore.Wait(0ms) == WaitResult::kSuccess &&
          semaphore.Wait(0ms) == WaitResult::kTimeout, "actual semaphore initial consumption changed");
  int previous = -1;
  Require(semaphore.Release(2, &previous) && previous == 0 && !semaphore.Release(2, nullptr),
          "actual semaphore release/max semantics changed");
  Require(semaphore.Wait(0ms) == WaitResult::kSuccess && semaphore.Wait(0ms) == WaitResult::kSuccess &&
          semaphore.Wait(0ms) == WaitResult::kTimeout, "actual semaphore release consumption changed");
  Probe false_predicate;
  const auto start = std::chrono::steady_clock::now();
  Require(false_predicate.Wait(20ms) == WaitResult::kTimeout, "ordinary finite wait did not time out");
  const auto elapsed = std::chrono::steady_clock::now() - start;
  Require(elapsed >= 20ms && elapsed < 2s && false_predicate.consumed == 0,
          "finite steady timeout/post_execution semantics changed");
  Require(false_predicate.Wait(-1ms) == WaitResult::kTimeout, "negative timeout did not return immediately");
  Require(false_predicate.Signal() && false_predicate.Wait(-1ms) == WaitResult::kSuccess,
          "signaled predicate lost priority to expired timeout");
}

using Case = void (*)();
bool ChildCase(const char* name, Case body, int expected_signal = 0) {
  const pid_t child = fork();
  if (child < 0) throw std::runtime_error("fork failed");
  if (child == 0) {
    rlimit limit{0, 0};
    setrlimit(RLIMIT_CORE, &limit);
    alarm(5);
    try { body(); std::_Exit(0); }
    catch (const std::exception& error) { std::fprintf(stderr, "%s: %s\n", name, error.what()); std::_Exit(1); }
  }
  int status = 0;
  while (waitpid(child, &status, 0) == -1) {
    if (errno != EINTR) throw std::runtime_error("waitpid failed");
  }
  const bool passed = expected_signal ? WIFSIGNALED(status) && WTERMSIG(status) == expected_signal
                                      : WIFEXITED(status) && WEXITSTATUS(status) == 0;
  std::cout << "case=" << name << " passed=" << (passed ? "true" : "false")
            << " signal=" << (WIFSIGNALED(status) ? WTERMSIG(status) : 0)
            << " exit=" << (WIFEXITED(status) ? WEXITSTATUS(status) : -1) << '\n';
  return passed;
}
}  // namespace

int main() {
  std::cout << "sdk_source_sha256=" << GOW3_ROBUST_CONDITION_SDK_SHA256
            << " sdk_before=" << GOW3_ROBUST_CONDITION_EXPECT_BEFORE
#ifdef _GLIBCXX_USE_PTHREAD_COND_CLOCKWAIT
            << " timed_clock=monotonic\n";
#else
            << " timed_clock=realtime-converted-from-steady\n";
#endif
  std::cout.flush();
  unsigned total = 0, failed = 0;
  auto run = [&](const char* name, Case body, int signal = 0) {
    ++total;
    if (!ChildCase(name, body, signal)) ++failed;
  };
#if GOW3_ROBUST_CONDITION_EXPECT_BEFORE
  run("original-infinite-real-owner-death-aborts", [] { Reacquire(std::chrono::milliseconds::max(), Death::Exit, 1); }, SIGABRT);
  run("original-timed-success-leaves-real-mutex-poisoned", [] { Reacquire(500ms, Death::Exit, 1, true); });
#else
  run("infinite-real-owner-exit", [] { Reacquire(std::chrono::milliseconds::max(), Death::Exit, 1); });
  run("timed-real-owner-exit", [] { Reacquire(500ms, Death::Exit, 1); });
  run("infinite-real-owner-cancel", [] { Reacquire(std::chrono::milliseconds::max(), Death::Cancel, 1); });
  run("timed-real-owner-cancel", [] { Reacquire(500ms, Death::Cancel, 1); });
  run("infinite-real-owner-async-cancel", [] { Reacquire(std::chrono::milliseconds::max(), Death::AsyncCancel, 1); });
  run("timed-real-owner-async-cancel", [] { Reacquire(500ms, Death::AsyncCancel, 1); });
  run("infinite-owner-death-false-predicate-later-signal", [] { Reacquire(std::chrono::milliseconds::max(), Death::Exit, 0); });
  run("timed-owner-death-false-predicate-times-out", [] { Reacquire(500ms, Death::Exit, 0); });
  run("initial-owner-death-original-recovery", [] { InitialOwnerDeath(false); });
  run("initial-irrecoverable-real-mutex", [] { InitialOwnerDeath(true); });
  run("condition-reacquire-irrecoverable-real-mutex", IrrecoverableReacquire);
  run("infinite-waiter-cancellation-unlocks", [] { CancelWaiter(std::chrono::milliseconds::max()); });
  run("timed-waiter-cancellation-unlocks", [] { CancelWaiter(500ms); });
  run("actual-events-semaphore-timeouts", OrdinaryPredicates);
#endif
  std::cout << "PASS_CASES=" << (total - failed) << " TOTAL_CASES=" << total
            << " fixture_runtime_executed=true crash_attribution_proved=false\n";
  return failed ? 1 : 0;
}
