#pragma once
#include <atomic>
#include <functional>
#include <future>
#include <utility>

namespace gow3::renderer {
// Single admission token for optional CP startup work. Cancellation may win
// only before Run admits the callback. Once running, its caller must wait until
// completion so captured progress/device state cannot expire underneath it.
class NativePipelineStartupTask {
  public:
    explicit NativePipelineStartupTask(std::function<void()> work) : work_(std::move(work)) {}
    std::future<void> Future() { return completion_.get_future(); }
    bool CancelPending() {
        auto expected = Phase::Pending;
        if (!phase_.compare_exchange_strong(expected, Phase::Cancelled))
            return false;
        // The CAS loser never reads work_. Release CPU shader owners and the
        // progress reference even if a paused CP keeps its empty task queued.
        work_ = {};
        completion_.set_value();
        return true;
    }
    void Run() noexcept {
        auto expected = Phase::Pending;
        if (!phase_.compare_exchange_strong(expected, Phase::Running))
            return;
        try {
            work_();
            work_ = {};
            phase_.store(Phase::Complete);
            completion_.set_value();
        } catch (...) {
            work_ = {};
            phase_.store(Phase::Complete);
            completion_.set_exception(std::current_exception());
        }
    }

  private:
    enum class Phase { Pending, Running, Cancelled, Complete };
    std::atomic<Phase> phase_{Phase::Pending};
    std::function<void()> work_;
    std::promise<void> completion_;
};
} // namespace gow3::renderer
