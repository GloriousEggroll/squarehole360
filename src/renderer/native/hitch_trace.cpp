#include "hitch_trace.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <winternl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>
#include <rex/logging.h>

namespace gow3::renderer::hitch_trace {
namespace {
// While a thread is suspended only its registers and the top of its stack are
// copied into preallocated storage: unwinding, logging or allocating there
// could wait on a lock the suspended thread holds. The copy is scanned for
// return addresses into known modules after the thread resumes.
constexpr size_t kStackBytes = 4096, kFrames = 8, kMaxThreads = 512, kMaxSamples = 60;
constexpr int kMaxReports = 50;

struct Module {
  uintptr_t begin = 0, end = 0;
  std::string name;
};
struct ThreadSample {
  DWORD tid = 0;
  std::array<uint64_t, kFrames> frames{};  // frames[0] is the instruction pointer
  uint8_t count = 0;
};

std::atomic<int64_t> last_frame_ns{0};
std::atomic<bool> running{false};
std::thread watchdog;
uint32_t threshold_ms = 100;
std::vector<Module> modules;
std::map<DWORD, uintptr_t> stack_bases;  // Thread id to its stack base (TEB).
alignas(16) uint8_t stack_copy[kStackBytes];

int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void FindModules() {
  for (const char* name : {"gow3.exe", "rexruntime.dll", "mimalloc.dll", "ntdll.dll", "kernelbase.dll",
                           "kernel32.dll", "vcruntime140.dll", "msvcp140.dll", "ucrtbase.dll", "win32u.dll",
                           "winevulkan.dll", "vulkan-1.dll", "dxcompiler.dll", "amd_fidelityfx_vk.dll"}) {
    const auto handle = GetModuleHandleA(name);
    if (!handle) continue;
    const auto base = reinterpret_cast<uintptr_t>(handle);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    modules.push_back({base, base + nt->OptionalHeader.SizeOfImage, name});
  }
}

const Module* ModuleOf(uint64_t address) {
  for (const auto& module : modules)
    if (address >= module.begin && address < module.end) return &module;
  return nullptr;
}

uintptr_t StackBase(HANDLE thread) {
  using Query = NTSTATUS(NTAPI*)(HANDLE, int, PVOID, ULONG, PULONG);
  static const auto query = reinterpret_cast<Query>(
      GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread"));
  struct BasicInformation {
    NTSTATUS exit_status;
    PVOID teb;
    CLIENT_ID client;
    KAFFINITY affinity;
    LONG priority, base_priority;
  } info{};
  if (!query || query(thread, 0, &info, sizeof(info), nullptr) < 0 || !info.teb) return 0;
  return reinterpret_cast<uintptr_t>(static_cast<NT_TIB*>(info.teb)->StackBase);
}

std::string ThreadName(DWORD tid) {
  using Describe = HRESULT(WINAPI*)(HANDLE, PWSTR*);
  static const auto describe = reinterpret_cast<Describe>(
      GetProcAddress(GetModuleHandleA("kernelbase.dll"), "GetThreadDescription"));
  std::string name;
  HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
  PWSTR wide = nullptr;
  if (thread && describe && SUCCEEDED(describe(thread, &wide)) && wide) {
    char buffer[128] = {};
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, buffer, sizeof(buffer) - 1, nullptr, nullptr);
    name = buffer;
    LocalFree(wide);
  }
  if (thread) CloseHandle(thread);
  return name;
}

// One pass over every thread but this one.
void SampleThreads(std::vector<ThreadSample>& out) {
  std::vector<DWORD> tids;
  tids.reserve(kMaxThreads);
  const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return;
  THREADENTRY32 entry{sizeof(entry)};
  const DWORD process = GetCurrentProcessId(), self = GetCurrentThreadId();
  for (BOOL more = Thread32First(snapshot, &entry); more && tids.size() < kMaxThreads;
       more = Thread32Next(snapshot, &entry))
    if (entry.th32OwnerProcessID == process && entry.th32ThreadID != self) tids.push_back(entry.th32ThreadID);
  CloseHandle(snapshot);
  for (const DWORD tid : tids) {
    HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!thread) continue;
    auto base = stack_bases.find(tid);
    if (base == stack_bases.end()) base = stack_bases.emplace(tid, StackBase(thread)).first;
    ThreadSample sample;
    sample.tid = tid;
    size_t copied = 0;
    if (SuspendThread(thread) != DWORD(-1)) {
      CONTEXT context{};
      context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
      if (GetThreadContext(thread, &context)) {
        sample.frames[0] = context.Rip;
        sample.count = 1;
        if (base->second > context.Rsp) {
          copied = std::min<size_t>(kStackBytes, base->second - context.Rsp);
          std::memcpy(stack_copy, reinterpret_cast<const void*>(context.Rsp), copied);
        }
      }
      ResumeThread(thread);
    }
    CloseHandle(thread);
    if (!sample.count) continue;
    for (size_t offset = 0; offset + 8 <= copied && sample.count < kFrames; offset += 8) {
      uint64_t value;
      std::memcpy(&value, stack_copy + offset, 8);
      if (ModuleOf(value)) sample.frames[sample.count++] = value;
    }
    out.push_back(sample);
  }
}

std::string Describe(uint64_t address) {
  if (const auto* module = ModuleOf(address))
    return fmt::format("{}+{:x}", module->name, address - module->begin);
  return fmt::format("{:x}", address);
}

void Report(int64_t stall_ns, const std::vector<std::vector<ThreadSample>>& passes) {
  // Per thread, the most frequent stack and how many passes saw it.
  std::map<DWORD, std::map<std::vector<uint64_t>, int>> stacks;
  for (const auto& pass : passes)
    for (const auto& sample : pass)
      ++stacks[sample.tid][std::vector<uint64_t>(sample.frames.begin(), sample.frames.begin() + sample.count)];
  REXLOG_WARN("Hitch trace: no frame for {} ms; {} samples of {} threads", stall_ns / 1000000, passes.size(),
              stacks.size());
  for (const auto& [tid, counts] : stacks) {
    const auto best = std::max_element(counts.begin(), counts.end(),
                                       [](const auto& a, const auto& b) { return a.second < b.second; });
    // Threads whose every frame is outside the game's own modules are idle
    // runtime or driver threads; skip them to keep the report readable.
    bool ours = false;
    for (const auto address : best->first)
      if (const auto* module = ModuleOf(address); module && (module->name == "gow3.exe" || module->name == "rexruntime.dll"))
        ours = true;
    if (!ours) continue;
    std::string text;
    for (const auto address : best->first) text += (text.empty() ? "" : " <- ") + Describe(address);
    REXLOG_WARN("Hitch trace: tid {} '{}' {}/{}: {}", tid, ThreadName(tid), best->second, passes.size(), text);
  }
}

void Watch() {
  FindModules();
  int reports = 0;
  while (running.load(std::memory_order_acquire)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const int64_t last = last_frame_ns.load(std::memory_order_acquire);
    if (!last || NowNs() - last < int64_t(threshold_ms) * 1000000 || reports >= kMaxReports) continue;
    std::vector<std::vector<ThreadSample>> passes;
    while (running.load(std::memory_order_acquire) && last_frame_ns.load(std::memory_order_acquire) == last &&
           passes.size() < kMaxSamples) {
      passes.emplace_back();
      passes.back().reserve(kMaxThreads);
      SampleThreads(passes.back());
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    // Wait for the frame that ends the stall to measure it.
    while (running.load(std::memory_order_acquire) && last_frame_ns.load(std::memory_order_acquire) == last)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    Report(last_frame_ns.load(std::memory_order_acquire) - last, passes);
    ++reports;
  }
}
}  // namespace

void Start(uint32_t threshold) {
  if (running.exchange(true)) return;
  threshold_ms = std::max<uint32_t>(threshold, 20);
  watchdog = std::thread(Watch);
  REXLOG_INFO("Hitch trace enabled: stalls over {} ms are sampled", threshold_ms);
}
void FrameCompleted() { last_frame_ns.store(NowNs(), std::memory_order_release); }
void Stop() {
  if (!running.exchange(false)) return;
  if (watchdog.joinable()) watchdog.join();
}
}  // namespace gow3::renderer::hitch_trace

#else
namespace gow3::renderer::hitch_trace {
void Start(uint32_t) {}
void FrameCompleted() {}
void Stop() {}
}  // namespace gow3::renderer::hitch_trace
#endif
