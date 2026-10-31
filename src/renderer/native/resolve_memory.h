#pragma once
#include "texture_data.h"
#include "cpu_upload_receipts.h"
#include "gpu_alias_state.h"
#include "exact_lru_cache.h"
#include "page-version-ranges.h"
#include "page_version_table.h"
#include "retired_image_pool.h"
#include "retired_descriptor_pool.h"
#include "plume_render_interface.h"
#include "state_transfer.h"
#include <map>
#include <functional>
#include <optional>
#include "surface_version_pages.h"
#include "primary_surface_write_receipt.h"
namespace gow3::renderer {
class NativeTransferBatch;
struct NativeSynchronousGpuTimingContext;
struct SurfaceLayout {
    uint32_t width,height,pitch,base,format;
    bool depth;
};
struct TextureOverlayStats {
    // Per-call totals. Readback includes aligned/coalesced padding; owned bytes
    // count only exact fragments copied to the CPU base and mip snapshots.
    uint64_t copy_regions=0,readback_bytes=0,owned_bytes=0;
    // Exact sampled unions per CPU base/mip provenance. A full overlay means
    // owned_region_bytes[i] == sampled_bytes[i] for both regions, even when
    // their physical allocations overlap or include unrelated padding.
    std::array<uint64_t,2> sampled_bytes{},owned_region_bytes{},footprint_ranges{};
    std::array<uint32_t,2> footprint_first{},footprint_end{};
};
// Console EDRAM uses 80x16 32-bit tiles (40x16 at 64 bits), wraps at
// 2048 tiles, and exchanges the two horizontal half-tiles for depth/stencil.
uint32_t SurfaceByteOffset(const SurfaceLayout&,uint32_t x,uint32_t y);
// GPU-resident resolved texture bytes. Different fetch layouts can view the
// same address space, including views with different base addresses. Offsets
// are byte addresses relative to this allocation. Initial bytes are unknown.
// Queue submissions are synchronous in this validation implementation.
class ResolveMemory {
public:
    ResolveMemory(plume::RenderDevice&,plume::RenderCommandQueue&,
                  std::span<const uint32_t> store_shader,std::span<const uint32_t> load_shader,
                  uint32_t capacity=8*1024*1024,uint32_t descriptor_window=0,NativeTransferBatch* batch=nullptr,
                  bool compact_cpu_texture_version_pages=false,bool block_page_versions=false,
                  bool range_texture_versions=false,bool coalesced_texture_ranges=false,bool reuse_defined_ranges=false);
    ~ResolveMemory();
    struct DefinitionStatistics {bool enabled=false;uint64_t calls=0,ranges=0,covered=0;};
    const DefinitionStatistics& definition_statistics() const {return definition_stats_;}
    void Store(plume::RenderTexture&,const TextureLayout&,const plume::RenderRect&,uint32_t address=0);
    // Deferred stores: the image already holds the bytes it would store and
    // stays immutable while held here. Versions and definitions advance now;
    // the store runs only before an overlapping read, copy or write. A later
    // deferred store of the identical footprint supersedes it unexecuted.
    void SetDeferredStoresEnabled(bool enabled) {deferred_stores_=enabled;}
    bool DeferStore(std::shared_ptr<plume::RenderTexture>,const TextureLayout&,const plume::RenderRect&,uint32_t address);
    static std::array<uint32_t,10> StoreFootprintKey(const TextureLayout&,const plume::RenderRect&,uint32_t address);
    // Pending stores holding this image, which the caller is about to change.
    size_t PendingStoreReferences(const plume::RenderTexture* image) const {
        size_t count=0;for(const auto& p:pending_stores_)count+=p.image.get()==image;return count;
    }
    // Before overwriting an image: pending stores of it with the footprint the
    // caller is about to store again are dropped; any other runs first.
    void ReleaseDeferredImage(const plume::RenderTexture& image,const std::array<uint32_t,10>& next_footprint);
    struct DeferredStoreStatistics {uint64_t deferred=0,superseded=0,materialized=0,pending=0,peak_pending=0;};
    DeferredStoreStatistics deferred_store_statistics() const {
        auto stats=deferred_store_stats_;stats.pending=pending_stores_.size();return stats;
    }
    // Optional startup-only full format26/28/31 store plus canonical RGBA32F
    // sampled output. The physical store remains authoritative. A receipt is
    // usable only after the enclosing transfer scope succeeds and while
    // CanonicalStoreReceiptMatches authenticates its exact physical version.
    struct CanonicalStoreReceipt {
        std::shared_ptr<plume::RenderTexture> image;
        uint64_t version=0;
    private:
        friend class ResolveMemory;
        std::shared_ptr<const uint8_t> owner;
        std::array<uint64_t,20> key{};
        uint64_t epoch=0;
    };
    bool EnableCanonicalStoreReceipt(std::span<const uint32_t> shader);
    bool SupportsCanonicalStoreReceipt() const {return bool(canonical_store_pipeline_);}
    // nullopt records NO store: caller uses the ordinary Native Store. Once
    // recording starts, failures throw and must never replay the store.
    // Source ownership is the caller's existing Store contract; output,
    // descriptors and views are retained by the actual accepted packet.
    std::optional<CanonicalStoreReceipt> TryStoreCanonical(plume::RenderTexture&,
        const TextureLayout&,const plume::RenderRect&,uint32_t address=0);
    bool CanonicalStoreReceiptMatches(const CanonicalStoreReceipt&,
        const TextureLayout&,uint32_t address=0) const;
    struct CanonicalStoreStatistics {
        bool configured=false,supported=false;
        uint64_t requests=0,selected=0,refused=0,pixels=0;
    };
    const CanonicalStoreStatistics& canonical_store_statistics() const {return canonical_store_stats_;}
    // Store resolved depth plus the source surface's actual stencil byte. The
    // offset maps destination texel coordinates to the source depth surface.
    void StoreDepth(plume::RenderTexture&,const TextureLayout&,const plume::RenderRect&,uint32_t address,
                    plume::RenderTexture& depth_stencil,int32_t source_x=0,int32_t source_y=0);
    // Optional startup-only variant. Preserve the original resolved-depth store
    // and reconstruct stencil from the ORIGINAL canonical RGBA source alpha.
    // Caller must keep a shared source in the current transfer packet before
    // Resolve/Store; cached destination owners follow the original fence rules.
    bool EnableAtlasRgbaDepthStore(std::span<const uint32_t> shader);
    bool SupportsAtlasRgbaDepthStore() const {return bool(atlas_rgba_depth_store_pipeline_);}
    // false is strictly before this primitive records anything. If Resolve was
    // already accepted, the caller must treat false as fatal, never fall back.
    bool StoreDepthFromRgbaStencil(plume::RenderTexture& resolved_depth,const TextureLayout&,
        const plume::RenderRect&,uint32_t address,plume::RenderTexture& original_rgba,
        int32_t source_x=0,int32_t source_y=0);
    // Optional startup-only exact primaryFP24 stencil clear. Scene must prove
    // the original path uses packed absent/stale clean source and no query.
    bool EnablePackedStencilClear(std::span<const uint32_t> shader);
    bool SupportsPackedStencilClear() const {return bool(packed_stencil_clear_pipeline_);}
    // Full depth normalization reproduces original D32[0,1] restore clamping;
    // stencil changes only inside rectangle. false precedes any commands.
    bool ClearPrimaryStencilFromPacked(const SurfaceLayout&,const plume::RenderRect&,uint32_t stencil);
    // Separate exact secondary 640x360 specialization. Primary behavior and
    // resources remain unchanged; all final-tile padding stays untouched.
    bool EnablePackedSecondaryStencilClear(std::span<const uint32_t> shader);
    bool SupportsPackedSecondaryStencilClear() const {return bool(packed_secondary_stencil_clear_pipeline_);}
    bool ClearSecondaryStencilFromPacked(const SurfaceLayout&,const plume::RenderRect&,uint32_t stencil);
    // Returns an RGBA32F decoded image, before fetch swizzle/gamma/exponent.
    std::unique_ptr<plume::RenderTexture> Load(const TextureLayout&,uint32_t address=0,
        const NativeSynchronousGpuTimingContext* presentation_timing=nullptr);
    // Shared variants retain their images before recording a deferred write.
    std::shared_ptr<plume::RenderTexture> LoadShared(const TextureLayout&,uint32_t address=0);
    // Startup-only bounded stacked-volume decode on the existing transfer FIFO.
    // CanQueue validates shape/address admission, independently of an active
    // scope. LoadShared validates the exact footprints before recording work.
    static constexpr uint32_t kMaxQueuedVolumeLayers=32;
    static constexpr uint64_t kMaxQueuedVolumeImageBytes=64ull*1024*1024;
    struct QueuedVolumeLoadStatistics {
        bool enabled=false;
        uint64_t requests=0,selected=0,refusals=0,synchronous_preflight=0,
                 decoded_slices=0,assemblies=0;
    };
    void SetQueuedVolumeLoadsEnabled(bool enabled);
    bool CanQueueVolumeLoad(const TextureLayout&,uint32_t address=0) const;
    const QueuedVolumeLoadStatistics& queued_volume_load_statistics() const {
        return queued_volume_load_stats_;
    }
    // Optional reuse of fully retired, otherwise-identical RGBA32F 2D decode
    // outputs. This adds bounded idle residency; active owners stay unchanged.
    using ImagePool=RetiredImagePool<plume::RenderTexture>;
    void SetImagePoolEnabled(bool enabled);
    ImagePool::Statistics ImagePoolStatistics() const;
    // Startup-only: cache only descriptor backings returned by the actual
    // transfer packet. Synchronous dispatches keep the original allocation path.
    using DescriptorPool=RetiredDescriptorPool<plume::RenderDescriptorSet>;
    void SetDescriptorPoolEnabled(bool enabled);
    DescriptorPool::Statistics DescriptorPoolStatistics() const;
    // Startup-only. Internal single-subresource load outputs may use their
    // owned identity image view; stores and unsupported outputs stay explicit.
    struct IdentityViewReuseStatistics {
        bool enabled=false;
        uint64_t load_requests=0,load_reused=0,load_explicit_views=0,
                 load_refusals=0,store_bypasses=0;
    };
    void SetIdentityViewReuseEnabled(bool enabled);
    const IdentityViewReuseStatistics& identity_view_reuse_statistics() const {return identity_view_reuse_stats_;}
    // Startup only. Preserve float32 values while omitting channels the fetch
    // view cannot expose. Unsupported variants retain the RGBA32F path.
    void EnableCompactLoadFormats(std::span<const uint32_t> r32_shader,
                                  std::span<const uint32_t> rg32_shader,
                                  bool storage_image_extended_formats_enabled);
    struct CompactLoadStatistics {
        bool r32_supported=false,rg32_supported=false;
        uint64_t r32_loads=0,rg32_loads=0,rgba32_loads=0;
        uint64_t logical_output_bytes=0,logical_bytes_avoided=0;
    };
    const CompactLoadStatistics& compact_load_statistics() const {return compact_load_stats_;}
    // Residency survives presentation. Versions invalidate cached image views
    // after any overlapping write, even when the writing view has another base.
    uint64_t Version(const TextureLayout&,uint32_t address=0) const;
    bool FullyDefined(const TextureLayout&,uint32_t address=0) const;
    bool AnyDefined(const TextureLayout&,uint32_t address=0) const;
    // Opt-in caller: one exact full-footprint state, reusable only until the
    // authoritative physical serial changes. No synchronization or GPU owners.
    NativeGpuAliasState InspectTextureState(const TextureLayout&,uint32_t address=0) const;
    const NativeGpuAliasStateStatistics& gpu_alias_state_statistics() const {return gpu_alias_state_stats_;}
    // Restore an explicitly captured GPU checkpoint. This is not a CPU
    // texture snapshot path; callers must establish the checkpoint provenance.
    void Initialize(uint32_t address,std::span<const uint8_t> bytes);
    void Invalidate(uint32_t address,uint32_t bytes);
    // Explicit CPU-update path only. Immutable page-sized data is copied into
    // an upload owner before return; standalone initialization stays synchronous.
    void InitializeCpuWrite(uint32_t address,std::span<const uint8_t> bytes,bool allow_queue);
    void InvalidateCpuWrite(uint32_t address,uint32_t bytes,bool allow_queue);
    // Startup only. One 4 KiB owner, used only by checked synchronous CPU
    // updates after their preceding actual fence. Queued/ordinary writes bypass.
    void SetCpuWriteUploadReuseEnabled(bool enabled);
    // Startup-only CPU copies of completed full-page uploads. Receipts never
    // alter native ownership; exact page versions guard an optional readback skip.
    void SetCpuUploadReceiptForwardingEnabled(bool enabled);
    const NativeCpuUploadReceipts::Statistics& cpu_upload_receipt_statistics() const {
        return cpu_upload_receipts_.statistics();
    }
    uint32_t cpu_upload_receipt_slots() const {return cpu_upload_receipts_.slots();}
    uint64_t cpu_upload_receipt_payload_bytes() const {return cpu_upload_receipts_.maximum_payload_bytes();}
    struct CpuWriteUploadReuseStatistics {
        bool enabled=false,checked_queue=false,poisoned=false;
        uint64_t allocations=0,reuses=0,uploads=0,uploaded_bytes=0,
                 submissions=0,waits=0,queued_bypasses=0,
                 unsupported_bypasses=0,unchecked_queue_bypasses=0;
    };
    const CpuWriteUploadReuseStatistics& cpu_write_upload_reuse_statistics() const {
        return cpu_write_upload_reuse_stats_;
    };
    struct CpuWriteStatistics {
        uint64_t queued_uploads=0,queued_invalidations=0,synchronous_uploads=0,
                 synchronous_invalidations=0,uploaded_bytes=0;
    };
    const CpuWriteStatistics& cpu_write_statistics() const {return cpu_write_stats_;}
    bool BytesDefined(uint32_t address,uint32_t count) const;
    // Native buffer operations share the same physical storage and invalidate
    // every overlapping texture view. Reading undefined history is rejected.
    void Copy(uint32_t source,uint32_t destination,uint32_t count);
    // Copy, then read the destination back in the same submission: one fence
    // wait instead of two when the result is published to guest memory.
    std::vector<uint8_t> CopyAndRead(uint32_t source,uint32_t destination,uint32_t count);
    // The same copy when every source page has a current CPU-upload receipt:
    // the result is known without the GPU, so the copy is only recorded into
    // the active transfer batch. False, with nothing changed, otherwise.
    bool CopyFromReceipts(uint32_t source,uint32_t destination,uint32_t count,std::vector<uint8_t>& result);
    // CPU snapshots are inputs, not native-owned source history. Upload only
    // the destination while retaining the guest memcpy range validation.
    void CopyFromCpu(uint32_t source,uint32_t destination,std::span<const uint8_t> data,bool queued=false);
    // Compose immutable CPU bytes with exact captured GPU-owned [first,end)
    // fragments. Undefined required GPU bytes are still fatal. Source history
    // and CPU-owned holes retain their original ownership.
    void CopyFromMixed(uint32_t source,uint32_t destination,std::span<const uint8_t> data,
                       std::span<const std::pair<uint32_t,uint32_t>> gpu_ranges);
    std::vector<uint8_t> ReadBytes(uint32_t address,uint32_t count);
    // CPU-backed textures may have GPU-owned mip levels or partial blocks.
    // Overlay only defined sampled bytes; this never publishes CPU snapshots
    // to GPU physical storage or changes ownership of padding.
    // gpu_bytes, when given, reports whether any sampled byte is GPU-defined,
    // i.e. whether OverlayCpuTexture can change the CPU payload at all.
    uint64_t CpuTextureVersion(const TextureLayout&,bool* gpu_bytes=nullptr) const;
    const NativePageVersionTable& PageVersions() const {return page_versions_;}
    struct CpuFootprintStatistics {
        uint64_t hits=0,misses=0,admissions=0,evictions=0;
        size_t entries=0,peak_entries=0,maximum_entries=2048;
    };
    CpuFootprintStatistics CpuTextureFootprintStatistics() const;
    struct CpuVersionStatistics {
        bool page_union_enabled=false,range_tracking_enabled=false;
        uint64_t calls=0,memo_hits=0,rescans=0,page_union_rescans=0,
                 baseline_page_visits=0,performed_page_visits=0,range_hits=0;
    };
    CpuVersionStatistics CpuTextureVersionStatistics() const {
        auto result=cpu_version_stats_;result.page_union_enabled=compact_cpu_texture_version_pages_;
        result.range_tracking_enabled=page_versions_.tracks_changes();return result;
    }
    // An optional completion callback defers synchronization until an exact
    // GPU-owned sampled byte needs readback. It must only complete queued work,
    // never mutate the texture's ownership or the supplied CPU snapshots.
    uint64_t OverlayCpuTexture(const TextureLayout&,std::span<uint8_t> base,std::span<uint8_t> mips,
                               TextureOverlayStats* stats=nullptr,
                               const std::function<void()>& before_readback={});
    // Narrow original-format read: exact observed linear4096x1 RGBA32F row,
    // fully owned sampled bytes, immutable destination and integer-only endian.
    // Configure once at startup. Unsupported layouts retain their CPU path.
    void EnableRawFloatCpuTextures(std::span<const uint32_t> shader);
    bool CanLoadRawFloatCpuTexture(const TextureLayout&,size_t cpu_payload_bytes) const;
    void LoadRawFloatCpuTexture(const std::shared_ptr<plume::RenderTexture>&,const TextureLayout&);
    struct RawFloatCpuStatistics {
        bool configured=false,supported=false;
        uint64_t copies=0,queued=0,synchronous=0,logical_bytes=0,scratch_allocation_bytes=0;
    };
    const RawFloatCpuStatistics& raw_float_cpu_statistics() const {return raw_float_cpu_stats_;}
    void StoreSurface(plume::RenderTexture&,const SurfaceLayout&);
    // Exact full-surface D24FS8 zero clear. Caller commits dirty working aliases first.
    void ClearPackedDepthZero(const SurfaceLayout&);
    // Optional startup-only fused UNORM24 atlas load/restore/clear/store replacement.
    // Canonical pre-encode host floats remain in an immutable receipt. The Scene
    // must consume that receipt only while its exact packed version is current.
    struct AtlasDepthClearReceipt {
        std::shared_ptr<plume::RenderTexture> source;
        uint64_t packed_version=0;
    };
    bool EnableAtlasDepthClear(std::span<const uint32_t> packed_shader,
                              std::span<const uint32_t> host_shader={});
    bool ClearAtlasDepthOneFromPacked(const SurfaceLayout&,const plume::RenderRect&,
                                     AtlasDepthClearReceipt&,plume::RenderTexture* clean_host_source=nullptr);
    DescriptorPool::Statistics AtlasDepthClearHostDescriptorStatistics() const;

    std::unique_ptr<plume::RenderTexture> LoadSurface(const SurfaceLayout&);
    std::shared_ptr<plume::RenderTexture> LoadSurfaceShared(const SurfaceLayout&);
    // Startup only. Decode packed color straight into its attachment with one
    // fullscreen draw (the load shader built as a fragment shader).
    void EnableDirectColorRestore(std::span<const uint32_t> vertex,std::span<const uint32_t> pixel);
    // Same values as LoadSurfaceShared followed by an identity Resolve into
    // the target. Recorded only inside an active transfer batch; false means
    // not configured, a depth surface or no batch, and the caller keeps the
    // load-and-copy path.
    bool RestoreColorSurface(plume::RenderTexture& target,const SurfaceLayout&);
    uint64_t direct_color_restores() const {return direct_color_restores_;}
    // Transfer-only source scratch: pixels outside rectangle are undefined.
    // The caller must restrict every consumer to this exact initialized region.
    // This is never a sampled alias generation or a full surface certificate.
    std::shared_ptr<plume::RenderTexture> LoadSurfaceSharedRegion(const SurfaceLayout&,
        const plume::RenderRect& rectangle);
    uint64_t SurfaceVersion(const SurfaceLayout&) const;
    // Exact page granularity used by RecordSurfaceWrite, including shared
    // pages of adjacent 5120-byte tiles. Returned by value; no cache reference.
    using SurfacePageMask=NativeSurfacePageMask;
    SurfacePageMask SurfacePages(const SurfaceLayout&) const;
    // CP-thread-only observation, no GPU read or ownership certificate.
    uint64_t physical_serial() const noexcept {return serial_;}
    bool SurfaceFullyDefined(const SurfaceLayout&) const;
    // Native fragment raster operations write this same packed EDRAM allocation.
    // The caller supplies GPU barriers and waits for submission completion.
    plume::RenderBuffer& storage_buffer() {return *bytes_;}
    struct PrimarySurfaceWriteReceipt {
    private:
        friend class ResolveMemory;
        NativePrimarySurfaceWriteStamp stamp_;
    };
    // Optional, CP-thread-only draw-local output. It borrows this ResolveMemory
    // lifetime and is minted only after every original page assignment succeeds.
    // Other callers preserve the same writes and do not request a receipt.
    void RecordSurfaceWrite(const SurfaceLayout&,PrimarySurfaceWriteReceipt* receipt=nullptr);
    std::optional<std::array<uint64_t,2>> PrimarySurfaceWriteVersions(
        const PrimarySurfaceWriteReceipt& color,const PrimarySurfaceWriteReceipt& depth)const noexcept;
    // Only after an owned raw packed image-export stage has been accepted.
    void RecordPackedSurfaceCommit(const SurfaceLayout&);
    NativeMemoryState ExportState() const;
    // Caller must have fenced a completed external write of exactly this range.
    void RecordImportedBytes(uint32_t address,uint32_t bytes);
    uint64_t mutation_serial() const {return mutation_serial_;}
private:
    QueuedVolumeLoadStatistics queued_volume_load_stats_;
    bool queued_volume_load_configured_=false;
    std::shared_ptr<plume::RenderTexture> TryLoadQueuedVolume(const TextureLayout&,uint32_t address);
    IdentityViewReuseStatistics identity_view_reuse_stats_;
    bool identity_view_reuse_configured_=false;
    std::unique_ptr<DescriptorPool> descriptor_pool_;
    bool descriptor_pool_configured_=false,descriptor_dispatch_started_=false;
    uint64_t descriptor_synchronous_bypasses_=0;
    std::unique_ptr<ImagePool> image_pool_;
    CompactLoadStatistics compact_load_stats_;
    bool load_formats_configured_=false,shared_load_started_=false;
    NativeTransferBatch* batch_;
    plume::RenderDevice& device_;
    plume::RenderCommandQueue& queue_;
    uint32_t capacity_;
    const bool compact_cpu_texture_version_pages_;
    const bool coalesced_texture_ranges_;
    uint32_t descriptor_range_,descriptor_alignment_;
    uint64_t serial_=0;
    uint64_t mutation_serial_=0;
    NativePageVersionTable page_versions_;
    std::map<uint32_t,uint32_t> defined_;
    DefinitionStatistics definition_stats_;
    std::unique_ptr<plume::RenderBuffer> bytes_;
    // Reused by CopyAndRead; every use completes its fence before returning.
    std::unique_ptr<plume::RenderBuffer> copy_readback_;uint64_t copy_readback_bytes_=0;
    void CopyBuffer(uint32_t source,uint32_t destination,uint32_t count,std::vector<uint8_t>* read);
    std::unique_ptr<plume::RenderTexture> empty_stencil_;
    std::unique_ptr<plume::RenderCommandList> commands_;
    std::unique_ptr<plume::RenderCommandFence> fence_;
    std::array<std::unique_ptr<plume::RenderShader>,4> shaders_;
    std::array<std::unique_ptr<plume::RenderPipelineLayout>,2> layouts_;
    std::array<std::unique_ptr<plume::RenderPipeline>,4> pipelines_;
    void Transfer(plume::RenderTexture&,const TextureLayout&,const plume::RenderRect&,bool store,uint32_t address,
                  plume::RenderTexture* stencil=nullptr,int32_t source_x=0,int32_t source_y=0,
                  const NativeSynchronousGpuTimingContext* presentation_timing=nullptr,bool rgba_stencil=false);
    using Ranges=std::vector<std::pair<uint32_t,uint32_t>>;
    mutable std::map<std::array<uint32_t,11>,NativeGpuAliasFootprint> footprints_;
    // Exact-key lookaside into footprints_. Map nodes are stable until the
    // capacity clear, which also empties every slot.
    struct FootprintSlot {std::array<uint32_t,11> key{};NativeGpuAliasFootprint* entry=nullptr;};
    mutable std::array<FootprintSlot,512> footprint_lookaside_{};
    mutable NativeGpuAliasStateStatistics gpu_alias_state_stats_;
    NativeGpuAliasFootprint& CachedFootprint(const TextureLayout&,const plume::RenderRect&,uint32_t address) const;
    void AdvancePhysicalSerial();
    struct CpuTextureFootprint {
        std::array<Ranges,2> ranges;
        NativePageVersionRanges version_pages;
        uint64_t baseline_page_visits=0,version_page_visits=0;
        uint64_t version_serial=0,version=0;
        uint64_t range_revision=0;
        std::array<NativePageChangeTracker<>::Range,2> guard_pages{};
        bool version_valid=false,gpu_bytes=true;
    };
    // CPU-only exact layouts and version memoization. A reference is consumed
    // synchronously before another lookup; no guest or GPU pointer is retained.
    mutable ExactLruCache<std::array<uint32_t,6>,CpuTextureFootprint,2048> cpu_texture_footprints_;
    mutable CpuVersionStatistics cpu_version_stats_;
    mutable std::map<std::array<uint32_t,6>,Ranges> surface_footprints_;
    // Exact while page_versions_.mutations() is unchanged. RecordSurfaceWrite
    // may carry an entry across its own Assigns when the entry's page hull
    // [page_first,page_end) is disjoint from every page it wrote, and sets the
    // written layout's own entry to the serial it just assigned to all its pages.
    struct SurfaceVersionMemo {std::array<uint32_t,6> key{};uint64_t mutations=0,version=0;
        uint32_t page_first=0,page_end=0;bool valid=false;};
    mutable std::array<SurfaceVersionMemo,64> surface_version_memo_{};
    static size_t SurfaceVersionSlot(const SurfaceLayout& s) noexcept {
        return (s.base*7u^s.width*31u^s.height*13u^s.pitch*3u^s.format*17u^uint32_t(s.depth))%64u;
    }
    // Scalar compare: avoids a vector load of a freshly stored key array
    // (store-to-load forwarding failure on the per-draw hit path).
    static bool SurfaceVersionKeyEquals(const std::array<uint32_t,6>& k,const SurfaceLayout& s) noexcept {
        return k[3]==s.base&&k[0]==s.width&&k[1]==s.height&&k[2]==s.pitch&&k[4]==s.format&&k[5]==uint32_t(s.depth);
    }
    std::unique_ptr<plume::RenderShader> restore_vertex_,restore_pixel_;
    std::unique_ptr<plume::RenderPipelineLayout> restore_layout_;
    std::map<plume::RenderFormat,std::unique_ptr<plume::RenderPipeline>> restore_pipelines_;
    // The restore binding never changes (the whole EDRAM window of bytes_), so
    // one set serves every restore; it is never updated after creation.
    std::unique_ptr<plume::RenderDescriptorSet> restore_descriptors_;
    uint64_t direct_color_restores_=0;
    const std::array<Ranges,2>& CpuTextureFootprints(const TextureLayout&) const;
    CpuTextureFootprint& CachedCpuTextureFootprint(const TextureLayout&) const;
    // References remain valid until the next cache lookup. Callers consume the
    // ranges immediately; recursive transfer splitting retains only endpoints.
    const Ranges& Footprint(const TextureLayout&,const plume::RenderRect&,uint32_t address) const;
    void Define(const Ranges&);
    struct Parameters {
        uint32_t width,height,x,y,packed_x,packed_y,pitch,log_bytes,tiled,endian,format,signs,offset,words;
        int32_t source_x,source_y;
    };
    void Dispatch(plume::RenderTexture&,Parameters,bool store,plume::RenderTexture* stencil,
                  uint32_t first_byte,uint32_t byte_count,
                  const NativeSynchronousGpuTimingContext* presentation_timing=nullptr,bool rgba_stencil=false);
    struct PendingStore {
        std::shared_ptr<plume::RenderTexture> image;
        Parameters push;
        uint32_t first_byte=0,byte_count=0;
        std::array<uint32_t,10> footprint{};  // Address, layout and rectangle of the deferred Store.
    };
    std::vector<PendingStore> pending_stores_;
    bool deferred_stores_=false;
    std::shared_ptr<plume::RenderTexture> deferring_image_;
    std::array<uint32_t,10> deferring_footprint_{};
    DeferredStoreStatistics deferred_store_stats_;
    // Runs, in order, every pending store overlapping [first, last).
    void MaterializeDeferredStores(uint32_t first,uint32_t last);
    void MaterializeDeferredStores(const Ranges& ranges) {
        if(!pending_stores_.empty()&&!ranges.empty())MaterializeDeferredStores(ranges.front().first,ranges.back().second);
    }
    void TransferSurface(plume::RenderTexture&,const SurfaceLayout&,bool store);
    RawFloatCpuStatistics raw_float_cpu_stats_;
    std::unique_ptr<plume::RenderShader> raw_float_cpu_shader_;
    std::unique_ptr<plume::RenderPipelineLayout> raw_float_cpu_layout_;
    std::unique_ptr<plume::RenderPipeline> raw_float_cpu_pipeline_;
    bool atlas_rgba_depth_store_configured_=false;
    CanonicalStoreStatistics canonical_store_stats_;
    std::shared_ptr<const uint8_t> canonical_store_owner_;
    uint64_t canonical_store_epoch_=0;
    std::unique_ptr<plume::RenderShader> canonical_store_shader_;
    std::unique_ptr<plume::RenderPipelineLayout> canonical_store_layout_;
    std::unique_ptr<plume::RenderPipeline> canonical_store_pipeline_;
    std::unique_ptr<DescriptorPool> canonical_store_descriptor_pool_;
    std::unique_ptr<plume::RenderShader> atlas_rgba_depth_store_shader_;
    std::unique_ptr<plume::RenderPipeline> atlas_rgba_depth_store_pipeline_;
    bool packed_stencil_clear_configured_=false;
    std::unique_ptr<plume::RenderShader> packed_stencil_clear_shader_;
    std::unique_ptr<plume::RenderPipelineLayout> packed_stencil_clear_layout_;
    std::unique_ptr<plume::RenderPipeline> packed_stencil_clear_pipeline_;
    // Immutable after startup: no per-clear descriptor writes or allocation.
    std::unique_ptr<plume::RenderDescriptorSet> packed_stencil_clear_descriptors_;
    bool packed_secondary_stencil_clear_configured_=false;
    std::unique_ptr<plume::RenderShader> packed_secondary_stencil_clear_shader_;
    std::unique_ptr<plume::RenderPipelineLayout> packed_secondary_stencil_clear_layout_;
    std::unique_ptr<plume::RenderPipeline> packed_secondary_stencil_clear_pipeline_;
    std::unique_ptr<plume::RenderDescriptorSet> packed_secondary_stencil_clear_descriptors_;
    bool atlas_depth_clear_configured_=false;
    std::array<std::unique_ptr<plume::RenderShader>,2> atlas_depth_clear_shaders_;
    std::array<std::unique_ptr<plume::RenderPipelineLayout>,2> atlas_depth_clear_layouts_;
    std::array<std::unique_ptr<plume::RenderPipeline>,2> atlas_depth_clear_pipelines_;
    std::unique_ptr<DescriptorPool> atlas_depth_host_descriptor_pool_;
    uint64_t atlas_depth_host_synchronous_bypasses_=0;
    CpuWriteStatistics cpu_write_stats_;
    NativeCpuUploadReceipts cpu_upload_receipts_;
    // Diagnostic: CPU texture overlay readbacks, and how many of their bytes
    // already matched the CPU snapshot they replace.
    uint64_t overlay_readbacks_=0,overlay_owned_bytes_=0,overlay_identical_bytes_=0,overlay_next_report_=64ull<<20;
public:
    // GPU memory copies by how their result reached the runtime.
    struct CopyPublicationStatistics {
        uint64_t receipts=0,readbacks=0,inactive_batch=0,ineligible=0,receipt_misses=0;
    };
    const CopyPublicationStatistics& copy_publication_statistics() const {return copy_publication_stats_;}
private:
    CopyPublicationStatistics copy_publication_stats_;
    bool cpu_upload_receipts_configured_=false;
    CpuWriteUploadReuseStatistics cpu_write_upload_reuse_stats_;
    bool cpu_write_upload_reuse_configured_=false,cpu_write_upload_reuse_uncertain_=false;
    std::unique_ptr<plume::RenderBuffer> cpu_write_upload_;
    void InitializeCpuWriteReusable(uint32_t address,std::span<const uint8_t> data);
    void InitializeBytes(uint32_t address,std::span<const uint8_t> data,bool queued);
    void InvalidateBytes(uint32_t address,uint32_t bytes,bool queued);
    const Ranges& SurfaceFootprint(const SurfaceLayout&) const;
};
}
