#pragma once
#include "plume_vulkan.h"
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>

namespace gow3::renderer {
// Cached vertex data sub-allocated from large, persistently mapped upload
// buffers. Shaders fetch vertices through a device address, so a slice needs
// no buffer object of its own. Creating and destroying one buffer per vertex
// upload cost a create, bind, map and destroy for every new piece of dynamic
// geometry (about 80 per frame in gameplay, each crossing Wine on Windows).
//
// A slice must only be released once the GPU has finished with it: vertex
// cache entries are released at EndFrame, after the frame fence. A chunk is
// rewound only when it holds no live slices.
class NativeVertexArena {
  public:
    static constexpr uint64_t kChunkBytes = 8u * 1024 * 1024, kMaxSliceBytes = 1u * 1024 * 1024,
                              kAlignment = 256;
    static constexpr size_t kMaxChunks = 32;
    struct Statistics {
        uint64_t slices = 0, fallbacks = 0, chunk_creations = 0, rewinds = 0;
    };

  private:
    struct Chunk {
        std::unique_ptr<plume::RenderBuffer> buffer;
        uint8_t *mapped = nullptr;
        uint64_t address = 0, used = 0, live = 0;
        ~Chunk() {
            if (mapped)
                buffer->unmap();
        }
    };

  public:
    // Move-only owner of one allocation; releasing it frees the bytes for
    // the chunk's next rewind.
    class Slice {
      public:
        Slice() = default;
        Slice(Slice &&other) noexcept : chunk_(std::move(other.chunk_)), address_(other.address_), bytes_(other.bytes_) {}
        Slice &operator=(Slice &&other) noexcept {
            if (this != &other) {
                Release();
                chunk_ = std::move(other.chunk_);
                address_ = other.address_;
                bytes_ = other.bytes_;
            }
            return *this;
        }
        Slice(const Slice &) = delete;
        Slice &operator=(const Slice &) = delete;
        ~Slice() { Release(); }
        explicit operator bool() const noexcept { return bool(chunk_); }
        uint64_t address() const noexcept { return address_; }
        uint64_t bytes() const noexcept { return bytes_; }

      private:
        friend class NativeVertexArena;
        Slice(std::shared_ptr<Chunk> chunk, uint64_t address, uint64_t bytes)
            : chunk_(std::move(chunk)), address_(address), bytes_(bytes) {}
        void Release() noexcept {
            if (chunk_)
                --chunk_->live;
            chunk_.reset();
        }
        std::shared_ptr<Chunk> chunk_;
        uint64_t address_ = 0, bytes_ = 0;
    };

    explicit NativeVertexArena(plume::RenderDevice &device) : device_(device) {}

    // Copies data into the arena. An empty Slice means the caller must use a
    // buffer of its own: the data is too large or every chunk is in use.
    Slice Allocate(std::span<const uint8_t> data) {
        if (data.empty() || data.size() > kMaxSliceBytes) {
            ++statistics_.fallbacks;
            return {};
        }
        const uint64_t size = (data.size() + kAlignment - 1) & ~(kAlignment - 1);
        if (!current_ || current_->used + size > kChunkBytes) {
            current_ = Available();
            if (!current_) {
                ++statistics_.fallbacks;
                return {};
            }
        }
        auto &chunk = *current_;
        const uint64_t offset = chunk.used;
        std::memcpy(chunk.mapped + offset, data.data(), data.size());
        auto &vk = static_cast<plume::VulkanBuffer &>(*chunk.buffer);
        if (vmaFlushAllocation(vk.device->allocator, vk.allocation, offset, data.size()) != VK_SUCCESS)
            throw std::runtime_error("Vertex arena flush failed");
        chunk.used += size;
        ++chunk.live;
        ++statistics_.slices;
        return Slice(current_, chunk.address + offset, data.size());
    }
    const Statistics &statistics() const noexcept { return statistics_; }
    size_t chunks() const noexcept { return chunks_.size(); }

  private:
    // An empty chunk other than the current one, rewound; else a new chunk.
    std::shared_ptr<Chunk> Available() {
        for (auto &chunk : chunks_)
            if (chunk != current_ && !chunk->live) {
                chunk->used = 0;
                ++statistics_.rewinds;
                return chunk;
            }
        if (current_ && !current_->live) {
            current_->used = 0;
            ++statistics_.rewinds;
            return current_;
        }
        if (chunks_.size() >= kMaxChunks)
            return nullptr;
        auto chunk = std::make_shared<Chunk>();
        chunk->buffer = device_.createBuffer(plume::RenderBufferDesc::UploadBuffer(
            kChunkBytes, plume::RenderBufferFlag::CONSTANT | plume::RenderBufferFlag::DEVICE_ADDRESSABLE));
        const auto *vk = chunk->buffer ? &static_cast<const plume::VulkanBuffer &>(*chunk->buffer) : nullptr;
        if (!vk || vk->vk == VK_NULL_HANDLE || !vk->allocation)
            return nullptr;
        chunk->mapped = static_cast<uint8_t *>(chunk->buffer->map());
        if (!chunk->mapped)
            return nullptr;
        chunk->address = chunk->buffer->getDeviceAddress();
        chunks_.push_back(chunk);
        ++statistics_.chunk_creations;
        return chunk;
    }
    plume::RenderDevice &device_;
    std::vector<std::shared_ptr<Chunk>> chunks_;
    std::shared_ptr<Chunk> current_;
    Statistics statistics_;
};
} // namespace gow3::renderer
