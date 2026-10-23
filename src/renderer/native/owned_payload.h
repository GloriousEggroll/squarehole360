#pragma once
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace gow3::renderer {
// A CPU-only view with explicit ownership. Live input shares an immutable
// snapshot; legacy providers and file replay move their returned bytes here.
// Derive the span on demand so moves never leave a view into the old object.
class OwnedPayload {
public:
    using Bytes=std::vector<uint8_t>;
    using Snapshot=std::shared_ptr<const Bytes>;
    explicit OwnedPayload(Bytes bytes):bytes_(std::move(bytes)) {}
    explicit OwnedPayload(Snapshot snapshot):snapshot_(std::move(snapshot)) {
        if(!snapshot_)throw std::runtime_error("Missing immutable native payload snapshot");
    }
    OwnedPayload(OwnedPayload&&)=default;
    OwnedPayload& operator=(OwnedPayload&&)=default;
    OwnedPayload(const OwnedPayload&)=delete;
    OwnedPayload& operator=(const OwnedPayload&)=delete;
    std::span<const uint8_t> bytes() const & {
        return snapshot_?std::span<const uint8_t>(*snapshot_):std::span<const uint8_t>(bytes_);
    }
    std::span<const uint8_t> bytes() const &&=delete;
    // Immutable index caches may retain this owner through their resource's
    // fenced lifetime. Legacy/file payloads become owned without copying bytes.
    const Snapshot& snapshot() const & noexcept {return snapshot_;}
    Snapshot TakeSnapshot() && {
        if(snapshot_)return std::move(snapshot_);
        return std::make_shared<const Bytes>(std::move(bytes_));
    }
    // Texture overlays need private writable bytes. Existing vector providers
    // retain their move-only path; shared snapshots are copied exactly once.
    Bytes TakeMutable() && {
        if(snapshot_)return Bytes(snapshot_->begin(),snapshot_->end());
        return std::move(bytes_);
    }
private:
    Bytes bytes_;
    Snapshot snapshot_;
};
}
