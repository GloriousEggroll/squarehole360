#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace gow3::renderer {
// Descriptor backings only: no image, buffer, parent or device owner is cached.
// A caller must overwrite EVERY binding before recording and retain its lease
// through the actual fence. Returning the last lease is the only reuse path.
// Active plus idle cached backings are bounded; pressure uses an uncached set.
template<class Descriptor>
class RetiredDescriptorPool {
public:
    static constexpr uint32_t kMaximumSets = 128;
    static constexpr uint32_t kLayoutFamilies = 2;
    struct Statistics {
        bool enabled = false;
        uint64_t requests = 0, hits = 0, misses = 0;
        uint64_t allocations = 0, factory_failures = 0, capacity_fallbacks = 0;
        uint64_t returns = 0, uncached_destructions = 0;
        uint64_t live_sets = 0, leased_sets = 0, idle_sets = 0, peak_live_sets = 0;
        uint64_t maximum_sets = kMaximumSets;
        // ResolveMemory populates this observer; synchronous and non-unified
        // dispatches retain their original allocation and wait behavior.
        uint64_t synchronous_bypasses = 0;
    };
private:
    struct Slot {
        std::unique_ptr<Descriptor> resource;
        uint32_t family = 0;
    };
    struct State {
        std::mutex mutex;
        std::array<Slot, kMaximumSets> slots;
        const uint32_t maximum;
        Statistics stats;
        explicit State(uint32_t count) : maximum(count) {
            stats.enabled = true;
            stats.maximum_sets = count;
        }
        void FactoryFailed(bool reserved) {
            std::lock_guard lock(mutex);
            ++stats.factory_failures;
            if(reserved) {
                --stats.live_sets;
                --stats.leased_sets;
            }
        }
        void Allocated() {
            std::lock_guard lock(mutex);
            ++stats.allocations;
        }
        void Return(std::unique_ptr<Descriptor> resource, uint32_t family) noexcept {
            try {
                std::lock_guard lock(mutex);
                ++stats.returns;
                --stats.leased_sets;
                for(auto& slot : slots) {
                    if(!slot.resource) {
                        slot.family = family;
                        slot.resource = std::move(resource);
                        ++stats.idle_sets;
                        return;
                    }
                }
                // A reserved backing always has one slot; if the invariant is
                // broken, discard this retired backing rather than reuse it.
                --stats.live_sets;
            } catch(...) {
                // Lease destruction must not throw. A host-lock failure leaves
                // conservative accounting and destroys an already-retired set.
            }
        }
        void UncachedDestroyed() noexcept {
            try {
                std::lock_guard lock(mutex);
                ++stats.uncached_destructions;
            } catch(...) {}
        }
    };
    std::shared_ptr<State> state_;
public:
    explicit RetiredDescriptorPool(uint32_t maximum = kMaximumSets) {
        if(maximum > kMaximumSets)
            throw std::invalid_argument("Retired descriptor pool exceeds its backing bound");
        state_ = std::make_shared<State>(maximum);
    }
    RetiredDescriptorPool(const RetiredDescriptorPool&) = delete;
    RetiredDescriptorPool& operator=(const RetiredDescriptorPool&) = delete;

    template<class Factory>
    std::shared_ptr<Descriptor> Acquire(uint32_t family, Factory&& factory) {
        if(family >= kLayoutFamilies)
            throw std::invalid_argument("Unknown retired descriptor layout family");
        std::unique_ptr<Descriptor> resource;
        bool pooled = false;
        {
            std::lock_guard lock(state_->mutex);
            auto& stats = state_->stats;
            ++stats.requests;
            for(auto& slot : state_->slots) {
                if(slot.resource && slot.family == family) {
                    resource = std::move(slot.resource);
                    --stats.idle_sets;
                    ++stats.leased_sets;
                    ++stats.hits;
                    pooled = true;
                    break;
                }
            }
            if(!resource) {
                ++stats.misses;
                if(stats.live_sets < state_->maximum) {
                    ++stats.live_sets;
                    ++stats.leased_sets;
                    if(stats.live_sets > stats.peak_live_sets)
                        stats.peak_live_sets = stats.live_sets;
                    pooled = true;
                } else {
                    ++stats.capacity_fallbacks;
                }
            }
        }
        if(!resource) {
            try {
                resource = std::forward<Factory>(factory)();
                if(!resource)
                    throw std::runtime_error("Retired descriptor factory returned null");
                state_->Allocated();
            } catch(...) {
                state_->FactoryFailed(pooled);
                throw;
            }
        }
        const std::weak_ptr<State> weak = state_;
        // shared_ptr calls the deleter if control-block allocation fails. This
        // returns the unused backing safely, without losing its reservation.
        return std::shared_ptr<Descriptor>(resource.release(),
            [weak, family, pooled](Descriptor* pointer) noexcept {
                std::unique_ptr<Descriptor> owned(pointer);
                if(auto state = weak.lock()) {
                    if(pooled) state->Return(std::move(owned), family);
                    else state->UncachedDestroyed();
                }
            });
    }
    Statistics statistics() const {
        std::lock_guard lock(state_->mutex);
        return state_->stats;
    }
};
} // namespace gow3::renderer
