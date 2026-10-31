#pragma once
#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <thread>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <sys/resource.h>
#endif

namespace gow3::renderer {
// Shader and pipeline compilation runs beside the game's own threads; a new
// area queues hundreds of compiles at once, and they must not take the CPU
// those threads need.
inline void LowerCurrentThreadPriority() noexcept {
#if defined(_WIN32)
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
#elif defined(__linux__)
  // On Linux a PRIO_PROCESS target of 0 changes only the calling thread.
  setpriority(PRIO_PROCESS, 0, 19);
#endif
}
// Lowering priority is not enough on its own: under Proton the request above
// is not applied, and compile threads then compete with the game's threads as
// equals (a new area's compiles took 12 of 16 hardware threads and starved
// the GPU command thread for half a second). So the number of compiles
// running at once is capped as well. These hardware threads always stay free
// for the game: GPU commands, planner, render and main threads, audio and
// wineserver.
inline unsigned ReservedGameThreads() noexcept {
  return std::max(4u, std::thread::hardware_concurrency() * 3 / 8);
}
// Compiles running at once, shader translation and pipeline creation alike.
inline unsigned CompileSlots() noexcept {
  const unsigned hardware = std::thread::hardware_concurrency(), reserved = ReservedGameThreads();
  return std::max(2u, hardware > reserved ? hardware - reserved : 0u);
}
// Of those, compiles running at once during gameplay. A new area's content
// arrives together with its compiles, and its first frames are the heaviest
// for the GPU command thread; that thread then keeps a core to itself instead
// of sharing one with a compile. The full CompileSlots are for frames held at
// a scene start (SetCompileBoost), when the screen is not moving anyway.
inline unsigned GameplayCompileSlots() noexcept {
  return std::clamp(std::thread::hardware_concurrency() / 4, 1u, CompileSlots());
}
// Of those, compiles no draw is waiting for (prewarm of discovered shaders).
inline unsigned BackgroundCompileSlots() noexcept {
  return std::clamp(std::thread::hardware_concurrency() / 8, 1u, GameplayCompileSlots());
}
// Shader workers serve any job; pipeline builders each run one driver compile.
inline unsigned BackgroundCompileThreads() noexcept { return GameplayCompileSlots(); }
inline unsigned BackgroundPipelineThreads() noexcept { return std::clamp(CompileSlots() * 2 / 5, 1u, 4u); }
// Extra shader threads that run only compiles a draw is waiting for. A new
// area's first frames wait on hundreds of those at once (and are held on
// screen meanwhile).
inline unsigned UrgentCompileHelperThreads() noexcept {
  return CompileSlots() > BackgroundCompileThreads() ? CompileSlots() - BackgroundCompileThreads() : 0;
}

// Holds one compile slot for the life of a compile: one of the
// GameplayCompileSlots, or of all CompileSlots while boosted, and for
// background work also one of the BackgroundCompileSlots. Never taken on the
// GPU command thread, which must not wait for compile threads.
class CompileSlot {
 public:
  explicit CompileSlot(bool background) : background_(background) {
    auto& s = Shared();
    std::unique_lock lock(s.mutex);
    s.changed.wait(lock, [&] {
      return s.in_use < (s.boost ? CompileSlots() : GameplayCompileSlots()) &&
             (!background_ || s.background_in_use < BackgroundCompileSlots());
    });
    ++s.in_use;
    s.background_in_use += background_;
  }
  ~CompileSlot() {
    auto& s = Shared();
    {
      std::lock_guard lock(s.mutex);
      --s.in_use;
      s.background_in_use -= background_;
    }
    s.changed.notify_all();
  }
  CompileSlot(const CompileSlot&) = delete;
  CompileSlot& operator=(const CompileSlot&) = delete;
  // The screen is held while a scene's compiles finish: use every slot.
  static void SetCompileBoost(bool boost) {
    auto& s = Shared();
    {
      std::lock_guard lock(s.mutex);
      if (s.boost == boost) return;
      s.boost = boost;
    }
    s.changed.notify_all();
  }

 private:
  struct State {
    std::mutex mutex;
    std::condition_variable changed;
    unsigned in_use = 0, background_in_use = 0;
    bool boost = false;
  };
  static State& Shared() {
    static State state;
    return state;
  }
  bool background_;
};
}  // namespace gow3::renderer
