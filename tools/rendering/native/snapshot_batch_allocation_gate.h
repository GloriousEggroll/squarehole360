#pragma once
// Dedicated fixture executables only. A thread-local gate never changes SDK
// production allocation and is disabled before assertions or fault dispatch.
#include <cstdlib>
#include <new>
#include <cstddef>
namespace snapshot_batch_fixture {
inline thread_local std::ptrdiff_t allocation_fail_after = -1;
inline thread_local size_t reached_allocations = 0;
inline void ArmAllocationFailure(size_t index) noexcept {
  reached_allocations = 0;
  allocation_fail_after = static_cast<std::ptrdiff_t>(index);
}
inline void DisarmAllocationFailure() noexcept { allocation_fail_after = -1; }
inline void* Allocate(size_t size) {
  if (allocation_fail_after >= 0) {
    ++reached_allocations;
    if (allocation_fail_after-- == 0) throw std::bad_alloc();
  }
  if (auto* pointer = std::malloc(size ? size : 1)) return pointer;
  throw std::bad_alloc();
}
}
void* operator new(std::size_t size) { return snapshot_batch_fixture::Allocate(size); }
void* operator new[](std::size_t size) { return snapshot_batch_fixture::Allocate(size); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }
