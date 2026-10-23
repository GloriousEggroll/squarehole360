#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace gow3::renderer {
// Idle resources only. A shared lease returns its resource after the LAST
// transfer/draw/cache owner releases it. The caller supplies the actual device
// allocation size and must use one otherwise-identical image description.
// Format participates in identity; all formats share one idle memory budget.
// No device, recorder, parent object or image layout is captured or changed.
template<class Resource>
class RetiredImagePool {
public:
    static constexpr uint32_t kMaxImages = 16;
    static constexpr uint64_t kMaxBytes = 128ull * 1024 * 1024;

    struct Statistics {
        uint64_t hits = 0, misses = 0, returns = 0;
        uint64_t admitted = 0, evictions = 0, rejected = 0;
        uint64_t idle_images = 0, idle_bytes = 0;
        uint64_t peak_idle_images = 0, peak_idle_bytes = 0;
    };

private:
    struct Slot {
        std::unique_ptr<Resource> image;
        uint32_t width = 0, height = 0, format = 0;
        uint64_t bytes = 0;
    };

    struct State {
        std::mutex mutex;
        std::array<Slot, kMaxImages> idle;
        uint32_t count = 0, max_images;
        uint64_t max_bytes;
        Statistics stats;

        State(uint32_t images, uint64_t bytes) : max_images(images), max_bytes(bytes) {}

        std::unique_ptr<Resource> Remove(uint32_t index) {
            auto result = std::move(idle[index].image);
            stats.idle_bytes -= idle[index].bytes;
            for(uint32_t i = index + 1; i < count; ++i) {
                idle[i - 1] = std::move(idle[i]);
            }
            idle[--count] = Slot{};
            stats.idle_images = count;
            return result;
        }

        void Return(std::unique_ptr<Resource> image, uint32_t width, uint32_t height,
                    uint64_t bytes, uint32_t format) noexcept {
            // Destruct evictions after unlocking, including resources whose
            // destructor performs device bookkeeping. No allocation on return.
            std::array<std::unique_ptr<Resource>, kMaxImages> evicted;
            try {
                std::lock_guard lock(mutex);
                ++stats.returns;
                if(!max_images || !bytes || bytes > max_bytes) {
                    ++stats.rejected;
                    return;
                }
                uint32_t discarded = 0;
                while(count && (count == max_images || bytes > max_bytes - stats.idle_bytes)) {
                    evicted[discarded++] = Remove(0);
                    ++stats.evictions;
                }
                idle[count++] = {std::move(image), width, height, format, bytes};
                ++stats.admitted;
                stats.idle_images = count;
                stats.idle_bytes += bytes;
                if(count > stats.peak_idle_images) {
                    stats.peak_idle_images = count;
                }
                if(stats.idle_bytes > stats.peak_idle_bytes) {
                    stats.peak_idle_bytes = stats.idle_bytes;
                }
            } catch(...) {
                // Shared-owner destruction cannot throw. If host locking ever
                // fails, destroy this already-retired resource normally.
            }
        }
    };
    std::shared_ptr<State> state_;

public:
    explicit RetiredImagePool(uint32_t max_images = kMaxImages, uint64_t max_bytes = kMaxBytes) {
        if(max_images > kMaxImages || max_bytes > kMaxBytes) {
            throw std::invalid_argument("Retired image pool exceeds hard limits");
        }
        state_ = std::make_shared<State>(max_images, max_bytes);
    }
    RetiredImagePool(const RetiredImagePool&) = delete;
    RetiredImagePool& operator=(const RetiredImagePool&) = delete;

    std::unique_ptr<Resource> Take(uint32_t width, uint32_t height, uint32_t format = 0) {
        std::lock_guard lock(state_->mutex);
        for(uint32_t i = 0; i < state_->count; ++i) {
            const auto& candidate = state_->idle[i];
            if(candidate.width == width && candidate.height == height && candidate.format == format) {
                ++state_->stats.hits;
                return state_->Remove(i);
            }
        }
        ++state_->stats.misses;
        return {};
    }

    std::shared_ptr<Resource> Share(std::unique_ptr<Resource> image, uint32_t width,
                                    uint32_t height, uint64_t actual_bytes, uint32_t format = 0) {
        if(!image) {
            throw std::invalid_argument("Cannot lease a null retired image");
        }
        // The pool retains unique resources, never leases, so no ownership
        // cycle exists. A lease may safely outlive the pool/ResolveMemory.
        const std::weak_ptr<State> weak = state_;
        return std::shared_ptr<Resource>(image.release(),
            [weak, width, height, actual_bytes, format](Resource* pointer) noexcept {
                std::unique_ptr<Resource> resource(pointer);
                if(auto state = weak.lock()) {
                    state->Return(std::move(resource), width, height, actual_bytes, format);
                }
            });
    }

    Statistics statistics() const {
        std::lock_guard lock(state_->mutex);
        return state_->stats;
    }
};
} // namespace gow3::renderer
