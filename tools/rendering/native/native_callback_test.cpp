#include <rex/graphics/native_callback.h>
#include <rex/graphics/native_completion.h>

#include <array>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <new>
#include <stdexcept>
#include <vector>

namespace {
thread_local bool count_allocations = false;
thread_local bool fail_next_allocation = false;
thread_local size_t allocations = 0;
}

// Keep the global allocation oracle out of line. Inlining a matched replacement
// new/delete pair confuses GCC's mismatched-new-delete heuristic.
#if defined(_MSC_VER)
#define NATIVE_CALLBACK_TEST_NOINLINE __declspec(noinline)
#else
#define NATIVE_CALLBACK_TEST_NOINLINE __attribute__((noinline))
#endif
NATIVE_CALLBACK_TEST_NOINLINE void* operator new(std::size_t size) {
  if (fail_next_allocation) {
    fail_next_allocation = false;
    throw std::bad_alloc();
  }
  if (count_allocations) ++allocations;
  if (void* result = std::malloc(size ? size : 1)) return result;
  throw std::bad_alloc();
}
NATIVE_CALLBACK_TEST_NOINLINE void operator delete(void* pointer) noexcept { std::free(pointer); }
NATIVE_CALLBACK_TEST_NOINLINE void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
NATIVE_CALLBACK_TEST_NOINLINE void* operator new[](std::size_t size) { return ::operator new(size); }
NATIVE_CALLBACK_TEST_NOINLINE void operator delete[](void* pointer) noexcept { ::operator delete(pointer); }
NATIVE_CALLBACK_TEST_NOINLINE void operator delete[](void* pointer, std::size_t) noexcept { ::operator delete(pointer); }

#undef NATIVE_CALLBACK_TEST_NOINLINE

namespace {
using rex::graphics::NativeCallback;
using rex::graphics::NativeCallbackStorage;
using rex::graphics::NativeCompletionReason;
using rex::graphics::NativeCompletionState;
using IntCallback = NativeCallback<std::function<int(int)>>;
using VoidCallback = NativeCallback<std::function<void(int)>>;
constexpr auto copied = NativeCallbackStorage::CopyPerInvocation;
constexpr auto shared = NativeCallbackStorage::SharedImmutable;

size_t checks = 0;
void Check(bool condition, const char* message) {
  ++checks;
  if (!condition) throw std::runtime_error(message);
}
struct Marker { int value; };
struct Probe {
  size_t copies = 0;
  size_t calls = 0;
  bool throw_on_copy = false;
};
struct ImmutableCallable {
  std::shared_ptr<Probe> probe;
  std::array<unsigned char, 512> storage{};
  explicit ImmutableCallable(std::shared_ptr<Probe> p) : probe(std::move(p)) {}
  ImmutableCallable(const ImmutableCallable& other) : probe(other.probe), storage(other.storage) {
    if (probe->throw_on_copy) throw Marker{73};
    ++probe->copies;
  }
  ImmutableCallable(ImmutableCallable&&) = default;
  int operator()(int value) const {
    ++probe->calls;
    return value + 11;
  }
};

void CheckEmpty() {
  IntCallback callback;
  Check(!callback, "empty slot");
  auto invocation = callback.Snapshot();
  Check(!invocation, "empty snapshot");
  bool threw = false;
  try { invocation(0); } catch (const std::bad_function_call&) { threw = true; }
  Check(threw, "empty invocation preserves bad_function_call");
  callback.Set({}, shared);
  Check(!callback && !callback.Snapshot(), "empty shared install is empty");
}

void CheckCopiesAndAllocations() {
  for (auto mode : {copied, shared}) {
    auto probe = std::make_shared<Probe>();
    IntCallback callback;
    callback.Set(ImmutableCallable(probe), mode);
    probe->copies = 0;
    allocations = 0;
    count_allocations = true;
    int total = 0;
    for (int i = 0; i < 10000; ++i) {
      const auto invocation = callback.Snapshot();
      total += invocation(i);
    }
    count_allocations = false;
    const auto measured_allocations = allocations;
    Check(total == 50105000, "actual callback results preserved");
    Check(probe->calls == 10000, "exactly one call per invocation");
    Check(probe->copies == (mode == copied ? 10000u : 0u), "callable-copy policy exact");
    if (mode == shared) Check(measured_allocations == 0, "shared hot invocation allocates nothing");
    std::cout << (mode == shared ? "shared" : "copied") << " callbacks: allocations="
              << measured_allocations << ", copies=" << probe->copies << '\n';
  }
}

void CheckDefaultMutableTarget() {
  IntCallback callback;
  callback.Set([counter = 0](int increment) mutable { return counter += increment; });
  Check(callback.Snapshot()(3) == 3, "default snapshots clone mutable target");
  Check(callback.Snapshot()(3) == 3, "default does not mutate installed target");
  auto retained = callback.Snapshot();
  Check(retained(3) == 3 && retained(3) == 6, "one snapshot retains its own target state");
}

void CheckSelfRemovalAndReplacement(NativeCallbackStorage mode) {
  VoidCallback callback;
  std::vector<int> order;
  auto old_owner = std::make_shared<int>(17);
  auto replacement_owner = std::make_shared<int>(23);
  std::weak_ptr<int> old_weak = old_owner;
  std::weak_ptr<int> replacement_weak = replacement_owner;
  callback.Set([&, old_owner](int value) {
    Check(*old_owner == 17, "old callable remains alive");
    order.push_back(value);
    callback.Reset();
    Check(!old_weak.expired(), "self removal retains active snapshot");
    callback.Set([&, replacement_owner](int next) {
      Check(*replacement_owner == 23, "replacement callable remains alive");
      order.push_back(next);
      callback.Reset();
      Check(!replacement_weak.expired(), "nested self removal retains nested snapshot");
    }, mode);
    replacement_owner.reset();
    {
      const auto nested = callback.Snapshot();
      nested(value + 1);
    }
    Check(replacement_weak.expired(), "nested owner retires after nested call");
    order.push_back(value + 2);
  }, mode);
  old_owner.reset();
  {
    const auto invocation = callback.Snapshot();
    invocation(4);
    Check(!callback && !old_weak.expired(), "removed old callback pinned through return");
  }
  Check(old_weak.expired(), "last invocation retires old owner");
  Check(order == std::vector<int>({4, 5, 6}), "nested replacement invocation order");
}

void CheckExceptions(NativeCallbackStorage mode) {
  VoidCallback callback;
  auto owner = std::make_shared<int>(5);
  std::weak_ptr<int> weak = owner;
  callback.Set([&, owner](int value) {
    callback.Reset();
    Check(!weak.expired() && *owner == 5, "throwing callable retained after removal");
    throw Marker{value};
  }, mode);
  owner.reset();
  bool matched = false;
  try {
    const auto invocation = callback.Snapshot();
    invocation(91);
  } catch (const Marker& error) {
    matched = error.value == 91;
  }
  Check(matched && weak.expired() && !callback, "original exception and unwound lifetime");

  auto probe = std::make_shared<Probe>();
  IntCallback throwing_copy;
  throwing_copy.Set(ImmutableCallable(probe), mode);
  probe->throw_on_copy = true;
  bool copy_threw = false;
  try {
    auto invocation = throwing_copy.Snapshot();
    Check(invocation(2) == 13, "shared snapshot avoids throwing target copy");
  } catch (const Marker& error) {
    copy_threw = error.value == 73;
  }
  Check(copy_threw == (mode == copied), "default copy exception remains at snapshot boundary");
}

void CheckFailedAdmission() {
  IntCallback callback;
  callback.Set([](int value) { return value + 1; }, shared);
  std::function<int(int)> replacement = [](int value) { return value + 99; };
  fail_next_allocation = true;
  bool failed = false;
  try { callback.Set(std::move(replacement), shared); }
  catch (const std::bad_alloc&) { failed = true; }
  Check(failed && !fail_next_allocation, "shared holder allocation failure exercised");
  Check(callback.Snapshot()(3) == 4, "failed admission preserves previous provider");
}

void CheckCompletionAndHandback(NativeCallbackStorage mode) {
  NativeCompletionState accepted;
  VoidCallback draw;
  VoidCallback query;
  NativeCallback<std::function<void(NativeCompletionReason)>> completion;
  std::vector<int> order;
  auto owner = std::make_shared<int>(42);
  std::weak_ptr<int> weak = owner;
  query.Set([owner, &order](int generation) {
    Check(*owner == 42, "query uses original live owner");
    order.push_back(generation);
  }, mode);
  completion.Set([&, owner](NativeCompletionReason reason) {
    Check(reason == NativeCompletionReason::ReferenceDraw, "exact handback completion reason");
    Check(accepted.pending() && *owner == 42, "accepted ownership remains until completion");
    order.push_back(2);
    const auto query_invocation = query.Snapshot();
    query.Reset();
    query_invocation(3);
    completion.Reset();
    Check(!weak.expired(), "completion removal retains invocation and outer draw");
  }, mode);
  draw.Set([&, owner](int) {
    Check(*owner == 42, "draw owner alive");
    order.push_back(1);
    accepted.AcceptPending();
    accepted.Complete([&] {
      const auto invocation = completion.Snapshot();
      invocation(NativeCompletionReason::ReferenceDraw);
    });
    Check(!accepted.pending(), "successful completion retires accepted ownership");
    draw.Reset();
    Check(!weak.expired(), "handback cannot destroy currently executing draw");
    order.push_back(4);
  }, mode);
  owner.reset();
  { const auto invocation = draw.Snapshot(); invocation(0); }
  Check(weak.expired() && !draw && !query && !completion, "all handback owners retired");
  Check(order == std::vector<int>({1, 2, 3, 4}), "handback publishes before final draw return");
}

void CheckFailedCompletionAndShutdown(NativeCallbackStorage mode) {
  NativeCompletionState accepted;
  NativeCallback<std::function<void(NativeCompletionReason)>> completion;
  auto owner = std::make_shared<int>(9);
  std::weak_ptr<int> weak = owner;
  completion.Set([&, owner](NativeCompletionReason reason) {
    Check(reason == NativeCompletionReason::Shutdown && *owner == 9, "terminal reason unchanged");
    completion.Reset();
    Check(!weak.expired(), "shutdown callback owner lives while running");
    throw Marker{101};
  }, mode);
  owner.reset();
  accepted.AcceptPending();
  bool failed = false;
  try {
    accepted.Complete([&] {
      const auto invocation = completion.Snapshot();
      invocation(NativeCompletionReason::Shutdown);
    }, true);
  } catch (const Marker& error) { failed = error.value == 101; }
  Check(failed && accepted.pending(), "failed terminal completion cannot retire accepted GPU work");
  Check(weak.expired() && !completion, "throwing shutdown releases only callback lifetime");
}

void CheckModeReplacement() {
  IntCallback callback;
  callback.Set([](int value) { return value + 1; });
  const auto before = callback.Snapshot();
  callback.Set([](int value) { return value + 2; }, shared);
  const auto during = callback.Snapshot();
  callback.Set([](int value) { return value + 3; });
  const auto after = callback.Snapshot();
  callback.Reset();
  Check(before(1) == 2 && during(1) == 3 && after(1) == 4,
        "mode replacement preserves each already-taken provider identity");
}
}  // namespace

int main() {
  try {
    CheckEmpty();
    CheckCopiesAndAllocations();
    CheckDefaultMutableTarget();
    CheckFailedAdmission();
    CheckModeReplacement();
    for (auto mode : {copied, shared}) {
      CheckSelfRemovalAndReplacement(mode);
      CheckExceptions(mode);
      CheckCompletionAndHandback(mode);
      CheckFailedCompletionAndShutdown(mode);
    }
    std::cout << "PASS " << checks << " callback ownership/exception/completion checks\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  } catch (...) {
    std::cerr << "unexpected non-standard exception\n";
    return 1;
  }
}
