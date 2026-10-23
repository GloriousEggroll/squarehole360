#pragma once
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace gow3::renderer {
// Observation only. A receipt cannot complete a query or authorize an early
// marker write. Every receipt is attached to the existing exact API generation.
struct QueryReadinessReceipt {
    uint64_t generation = 0;
    uint32_t frame = 0, address = 0;
    bool published = false;
    bool operator==(const QueryReadinessReceipt&) const = default;
};

class QueryReadinessCensus {
public:
    explicit QueryReadinessCensus(size_t capacity = 8192) : capacity_(capacity) {}
    void Begin(uint32_t handle, uint32_t address) {
        std::lock_guard lock(mutex_);
        Erase(handle);
        EraseAddress(address);
    }
    void End(uint32_t handle, QueryReadinessReceipt receipt) {
        std::lock_guard lock(mutex_);
        Erase(handle);
        EraseAddress(receipt.address);
        if (!capacity_) return;
        if (handles_.size() >= capacity_) { handles_.clear(); generations_.clear(); addresses_.clear(); }
        receipt.published = false;
        // Duplicate generation observations are deliberately not evidence.
        if (const auto old = generations_.find(receipt.generation); old != generations_.end()) {
            const auto previous_handle = old->second;
            Erase(previous_handle);
            return;
        }
        handles_.emplace(handle, receipt);
        generations_.emplace(receipt.generation, handle);
        addresses_.emplace(receipt.address, handle);
    }
    bool Publish(uint32_t frame, uint64_t generation, uint32_t address) {
        std::lock_guard lock(mutex_);
        const auto key = generations_.find(generation);
        if (key == generations_.end()) return false;
        auto& receipt = handles_.at(key->second);
        if (receipt.frame != frame || receipt.address != address) return false;
        receipt.published = true;
        return true;
    }
    std::optional<QueryReadinessReceipt> Read(uint32_t handle) const {
        std::lock_guard lock(mutex_);
        const auto found = handles_.find(handle);
        return found == handles_.end() ? std::nullopt : std::optional(found->second);
    }
private:
    void Erase(uint32_t handle) {
        const auto found = handles_.find(handle);
        if (found != handles_.end()) {
            generations_.erase(found->second.generation);
            addresses_.erase(found->second.address);
            handles_.erase(found);
        }
    }
    void EraseAddress(uint32_t address) {
        const auto found = addresses_.find(address);
        if (found != addresses_.end()) Erase(found->second);
    }
    const size_t capacity_;
    mutable std::mutex mutex_;
    std::unordered_map<uint32_t, QueryReadinessReceipt> handles_;
    std::unordered_map<uint64_t, uint32_t> generations_;
    std::unordered_map<uint32_t, uint32_t> addresses_;
};
}
