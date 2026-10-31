#include "defined_byte_ranges.h"
#include "resolve_memory.h"
#include "edram_extent.h"
#include "synchronous_gpu_timing.h"
#include "cpu_write_policy.h"
#include "live_device.h"
#include "raw_float_texture.h"
#include "packed_zero_depth_clear.h"
#include "compact_alias_format.h"
#include "atlas_depth_region.h"
#include "atlas_depth_clear.h"
#include "atlas_rgba_stencil_depth_store.h"
#include "packed_primary_stencil_clear_kernel.h"
#include "packed_secondary_stencil_clear_kernel.h"
#include "footprint_ranges.h"
#include "transfer_batch.h"
#include "plume_vulkan.h"
// Standalone GPU tests build this file without the SDK and its logging.
#if __has_include(<rex/logging.h>)
#include <rex/logging.h>
#define GOW3_RESOLVE_MEMORY_LOG(...) REXLOG_INFO(__VA_ARGS__)
#else
#define GOW3_RESOLVE_MEMORY_LOG(...) ((void)0)
#endif
#include <bit>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <exception>
namespace gow3::renderer {
using namespace plume;
namespace {
uint32_t SurfaceStorageFormat(const SurfaceLayout& s) {
    if(s.depth) {
        if(s.format>1)throw std::runtime_error("Invalid EDRAM depth format");
        return s.format?23:22;
    }
    switch(s.format) {
    case 0:return 6;
    case 2:case 10:return 7;
    case 3:case 12:return 64;
    case 4:return 65;
    case 5:return 66;
    case 6:return 31;
    case 7:return 32;
    case 14:return 36;
    case 15:return 37;
    default:throw std::runtime_error("Unsupported EDRAM color format");
    }
}
bool Surface64(const SurfaceLayout& s) {return !s.depth&&(s.format==5||s.format==7||s.format==15);}
auto DescriptorRanges(bool store) {
    std::vector<RenderDescriptorRange> ranges{{RenderDescriptorRangeType::READ_WRITE_BYTE_ADDRESS_BUFFER,0,1},
        {store?RenderDescriptorRangeType::TEXTURE:RenderDescriptorRangeType::READ_WRITE_TEXTURE,1,1}};
    if(store)ranges.emplace_back(RenderDescriptorRangeType::TEXTURE,2,1);
    return ranges;
}
auto CanonicalStoreDescriptorRanges() {
    return std::array<RenderDescriptorRange,3>{{
        {RenderDescriptorRangeType::READ_WRITE_BYTE_ADDRESS_BUFFER,0,1},
        {RenderDescriptorRangeType::TEXTURE,1,1},
        {RenderDescriptorRangeType::READ_WRITE_TEXTURE,2,1}}};
}
std::optional<std::array<uint64_t,20>> CanonicalStoreKey(const TextureLayout& t,uint32_t address) {
    if((t.format!=26&&t.format!=28&&t.format!=31)||t.dimension!=1||t.depth!=1||t.layers!=1||
       !t.width||!t.height||t.width>8192||t.height>8192||t.min_mip||t.max_mip||
       t.levels.size()!=1||t.endian>3||t.block.block_width!=1||t.block.block_height!=1||
       t.block.bytes!=(t.format==26?8u:4u)||t.block.components!=(t.format==26?4u:2u))return std::nullopt;
    const auto& l=t.levels.front();
    if(l.mip||l.layer||l.depth!=1||l.z||l.mips||l.width!=t.width||l.height!=t.height||
       uint64_t(l.x)+t.width>l.pitch_blocks||uint64_t(l.y)+t.height>l.slice_rows||
       l.offset>UINT32_MAX||((uint64_t(address)+l.offset)%(t.block.bytes)))return std::nullopt;
    // Footprint memoization keys the original fetch. Refuse a hand-constructed
    // layout that disagrees with those bytes, including its packed offsets.
    TextureLayout decoded;
    try {decoded=DescribeTexture(t.fetch);}catch(const std::exception&){return std::nullopt;}
    if(decoded.width!=t.width||decoded.height!=t.height||decoded.depth!=t.depth||decoded.layers!=t.layers||
       decoded.dimension!=t.dimension||decoded.format!=t.format||decoded.endian!=t.endian||
       decoded.min_mip!=t.min_mip||decoded.max_mip!=t.max_mip||decoded.tiled!=t.tiled||
       decoded.levels.size()!=1)return std::nullopt;
    const auto& expected=decoded.levels.front();
    if(expected.pitch_blocks!=l.pitch_blocks||expected.slice_rows!=l.slice_rows||
       expected.x!=l.x||expected.y!=l.y||expected.z!=l.z||expected.offset!=l.offset||expected.mips!=l.mips)
        return std::nullopt;
    // Sampler, swizzle, gamma and exponent act AFTER canonical decoding. They
    // deliberately do not identify these immutable pre-binding float texels.
    return std::array<uint64_t,20>{t.width,t.height,t.depth,t.layers,t.dimension,t.format,t.endian,
        t.min_mip,t.max_mip,uint64_t(t.tiled),t.block.bytes,t.block.components,
        l.pitch_blocks,l.x,l.y,l.offset,address,(t.fetch[0]>>2)&255u,l.slice_rows,l.depth};
}
bool LoadIdentityViewEquivalent(const VulkanTexture& texture,const VulkanDevice& device) noexcept {
    // Dispatch's load images originate in this ResolveMemory's owned output
    // paths. The constructor's identity view is never replaced by those paths,
    // including retired-image reuse. Do not apply this to external store images.
    const auto& d=texture.desc;
    if(!texture.ownership||texture.device!=&device||!device.vk||!texture.vk||!texture.imageView||
       d.dimension!=RenderTextureDimension::TEXTURE_2D||!d.width||!d.height||d.depth!=1||
       d.mipLevels!=1||d.arraySize!=1||d.multisampling.sampleCount!=RenderSampleCount::COUNT_1||
       d.multisampling.sampleLocationsEnabled||
       (d.flags&(RenderTextureFlag::DEPTH_TARGET|RenderTextureFlag::CUBE))||
       !(d.flags&RenderTextureFlag::STORAGE))return false;
    VkFormat format=VK_FORMAT_UNDEFINED;
    switch(d.format) {
    case RenderFormat::R32G32B32A32_FLOAT:format=VK_FORMAT_R32G32B32A32_SFLOAT;break;
    case RenderFormat::R32_FLOAT:format=VK_FORMAT_R32_SFLOAT;break;
    case RenderFormat::R32G32_FLOAT:format=VK_FORMAT_R32G32_SFLOAT;break;
    default:return false;
    }
    const auto& r=texture.imageSubresourceRange;
    return texture.imageFormat==format&&r.aspectMask==VK_IMAGE_ASPECT_COLOR_BIT&&
        r.baseMipLevel==0&&r.levelCount==1&&r.baseArrayLayer==0&&r.layerCount==1;
}
uint32_t SurfaceByteOffsetImpl(const SurfaceLayout& s,uint32_t x,uint32_t y) {
    // Addressing derived from Xenia's FSI_LoadEdramOffsets. BSD attribution:
    // texture_data.LICENSE in this directory.
    if(!s.width||!s.height||!s.pitch||s.pitch>16383||s.base>=NativeEdramTiles()||x>=s.width||y>=s.height)
        throw std::runtime_error("Invalid EDRAM surface coordinates");
    const bool wide=Surface64(s);
    const uint32_t tile=(y/16)*((s.pitch+79)/80)*(wide?2:1)+x/(wide?40:80);
    uint32_t column=x%(wide?40:80);
    if(s.depth)column=column<40?column+40:column-40;
    const uint32_t word=((s.base+tile)*1280+(y%16)*80+column*(wide?2:1))%((NativeEdramTiles()*1280));
    return word*4;
}
// Plume's public depth view selects the depth aspect. Create an additional
// stencil-only view in consumer code, without modifying the Plume dependency.
std::unique_ptr<VulkanTextureView> StencilView(RenderTexture& image) {
    auto& texture=static_cast<VulkanTexture&>(image);
    if(texture.desc.format!=RenderFormat::D32_FLOAT_S8_UINT)
        throw std::runtime_error("Stencil resolve requires a depth/stencil surface");
    auto view=std::make_unique<VulkanTextureView>(&texture,RenderTextureViewDesc::Texture2D(texture.desc.format));
    if(view->vk)vkDestroyImageView(texture.device->vk,view->vk,nullptr);
    view->vk=VK_NULL_HANDLE;
    VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    info.image=texture.vk;info.viewType=VK_IMAGE_VIEW_TYPE_2D;info.format=VK_FORMAT_D32_SFLOAT_S8_UINT;
    info.subresourceRange={VK_IMAGE_ASPECT_STENCIL_BIT,0,1,0,1};
    if(vkCreateImageView(texture.device->vk,&info,nullptr,&view->vk)!=VK_SUCCESS)
        throw std::runtime_error("Cannot create stencil resolve view");
    return view;
}
}
uint32_t SurfaceByteOffset(const SurfaceLayout& s,uint32_t x,uint32_t y) {return SurfaceByteOffsetImpl(s,x,y);}
ResolveMemory::ResolveMemory(RenderDevice& device,RenderCommandQueue& queue,std::span<const uint32_t> store_shader,
                             std::span<const uint32_t> load_shader,uint32_t capacity,uint32_t descriptor_window,NativeTransferBatch* batch,
                             bool compact_cpu_texture_version_pages,bool block_page_versions,bool range_texture_versions,bool coalesced_texture_ranges,bool reuse_defined_ranges)
    :batch_(batch),device_(device),queue_(queue),capacity_(capacity),
     compact_cpu_texture_version_pages_(compact_cpu_texture_version_pages),coalesced_texture_ranges_(coalesced_texture_ranges) {
    definition_stats_.enabled=reuse_defined_ranges;
    if(!capacity||capacity%4||capacity>512*1024*1024)throw std::runtime_error("Invalid resolve storage capacity");
    const auto& limits=static_cast<VulkanDevice&>(device).physicalDeviceProperties.limits;
    descriptor_range_=std::min(capacity,limits.maxStorageBufferRange)&~3u;
    if(descriptor_window)descriptor_range_=std::min(descriptor_range_,descriptor_window&~3u);
    descriptor_alignment_=uint32_t(std::max<VkDeviceSize>(4,limits.minStorageBufferOffsetAlignment));
    if(descriptor_range_<descriptor_alignment_+16)
        throw std::runtime_error("Resolve storage descriptor window is too small");
    page_versions_.Reset((capacity+4095)/4096,block_page_versions,range_texture_versions);
    bytes_=device_.createBuffer(RenderBufferDesc::DefaultBuffer(capacity,RenderBufferFlag::STORAGE|RenderBufferFlag::UNORDERED_ACCESS));
    empty_stencil_=device_.createTexture(RenderTextureDesc::DepthTarget(1,1,RenderFormat::D32_FLOAT_S8_UINT));
    commands_=queue_.createCommandList();fence_=device_.createCommandFence();
    const RenderPushConstantRange push(0,0,0,64,RenderShaderStageFlag::COMPUTE);
    for(unsigned i=0;i<2;++i) {
        auto code=i?load_shader:store_shader;
        shaders_[i]=device_.createShader(code.data(),code.size_bytes(),"main",RenderShaderFormat::SPIRV);
        auto ranges=DescriptorRanges(i==0);RenderDescriptorSetDesc descriptor(ranges.data(),ranges.size());
        layouts_[i]=device_.createPipelineLayout(RenderPipelineLayoutDesc(&push,1,&descriptor,1));
        pipelines_[i]=device_.createComputePipeline(RenderComputePipelineDesc(layouts_[i].get(),shaders_[i].get(),8,8,1));
        if(!pipelines_[i]||!static_cast<VulkanComputePipeline*>(pipelines_[i].get())->vk)
            throw std::runtime_error("Resolve memory pipeline creation failed");
    }
}
ResolveMemory::~ResolveMemory() {
    // A failed checked submit/wait leaves this standalone command list outside
    // the shared FIFO. Never free its cached source, target, command or fence
    // while completion is uncertain. Native failure is terminal, not replay.
    if(cpu_write_upload_reuse_uncertain_)std::terminate();
}
void ResolveMemory::SetCpuUploadReceiptForwardingEnabled(bool enabled) {
    if(cpu_upload_receipts_configured_||mutation_serial_)
        throw std::logic_error("CPU upload receipts must be configured once before use");
    cpu_upload_receipts_configured_=true;
    // 128 MB of pages: one texture streaming burst uploads and moves tens of
    // MB that the following draws sample; a smaller direct-mapped table
    // evicted most of a burst before its draws.
    cpu_upload_receipts_.Configure(enabled,dynamic_cast<NativeCheckedQueueCompletion*>(&queue_)!=nullptr,32768);
}
void ResolveMemory::SetCpuWriteUploadReuseEnabled(bool enabled) {
    if(cpu_write_upload_reuse_configured_||mutation_serial_)
        throw std::logic_error("CPU upload reuse must be configured once before use");
    cpu_write_upload_reuse_configured_=true;
    cpu_write_upload_reuse_stats_.enabled=enabled;
    cpu_write_upload_reuse_stats_.checked_queue=dynamic_cast<NativeCheckedQueueCompletion*>(&queue_)!=nullptr;
}
const ResolveMemory::Ranges& ResolveMemory::Footprint(const TextureLayout& t,const RenderRect& r,uint32_t address) const {
    return CachedFootprint(t,r,address).ranges;
}
NativeGpuAliasFootprint& ResolveMemory::CachedFootprint(const TextureLayout& t,const RenderRect& r,uint32_t address) const {
    if(address%4||t.levels.size()!=t.layers||!t.layers||t.min_mip||t.max_mip||t.block.block_width!=1||t.dimension!=1||
       r.isEmpty()||r.left<0||r.top<0||r.right>int32_t(t.width)||r.bottom>int32_t(t.height))
        throw std::runtime_error("Unsupported storage footprint");
    // Only the layout bits of the fetch (type, pitch, tiling; format, endian,
    // stacking, base; extent; mip range; dimension, packed, mip base): the
    // footprint is the same for every sampler state of a texture.
    const std::array<uint32_t,6> fetch{t.fetch[0]&0xffc00003u,t.fetch[1],t.fetch[2],0,
        t.fetch[4]&((15u<<2)|(15u<<6)),t.fetch[5]&0xfffffe00u};
    // Slot choice only; equality below is exact over all eleven key words.
    uint64_t h=0x9e3779b97f4a7c15ull;
    for(const auto word:fetch)h=(h^word)*0xff51afd7ed558ccdull;
    for(const uint32_t word:{address,uint32_t(r.left),uint32_t(r.top),uint32_t(r.right),uint32_t(r.bottom)})
        h=(h^word)*0xff51afd7ed558ccdull;
    h^=h>>33;
    auto& slot=footprint_lookaside_[h%footprint_lookaside_.size()];
    if(slot.entry&&slot.key[6]==address&&slot.key[7]==uint32_t(r.left)&&slot.key[8]==uint32_t(r.top)&&
       slot.key[9]==uint32_t(r.right)&&slot.key[10]==uint32_t(r.bottom)&&
       std::equal(fetch.begin(),fetch.end(),slot.key.begin()))
        return *slot.entry;
    std::array<uint32_t,11> key;
    std::copy(fetch.begin(),fetch.end(),key.begin());
    key[6]=address;key[7]=r.left;key[8]=r.top;key[9]=r.right;key[10]=r.bottom;
    if(auto it=footprints_.find(key);it!=footprints_.end()) {
        slot={key,&it->second};return it->second;
    }
    Ranges ranges;
    // Tiled/endian traversal is not globally ordered; the exact final
    // sort/union below remains authoritative.
    for(const auto& level:t.levels)
        AppendTextureLevelFootprint(ranges,t,level,uint32_t(r.left),uint32_t(r.top),uint32_t(r.right),
            uint32_t(r.bottom),address,capacity_);
    std::sort(ranges.begin(),ranges.end());
    size_t count=0;
    for(const auto& r:ranges) {
        if(count&&r.first<=ranges[count-1].second)ranges[count-1].second=std::max(ranges[count-1].second,r.second);
        else ranges[count++]=r;
    }
    ranges.resize(count);
    ReleaseFootprintRangeCapacity(ranges);
    if(footprints_.size()>=16384) {
        footprint_lookaside_.fill({});
        footprints_.clear();if(gpu_alias_state_stats_.requests)++gpu_alias_state_stats_.cache_clears;
    }
    auto& stored=footprints_.emplace(key,NativeGpuAliasFootprint{std::move(ranges),{}}).first->second;
    slot={key,&stored};return stored;
}
void ResolveMemory::AdvancePhysicalSerial() {
    if(AdvanceNativeGpuAliasStateSerial(serial_,footprints_)) {
        cpu_upload_receipts_.ClearOnSerialWrap();
        if(gpu_alias_state_stats_.requests)++gpu_alias_state_stats_.serial_wraps;
        // Page maxima can coincide again after serial wrap. No receipt from an
        // earlier epoch may then authenticate merely by matching that maximum.
        if(++canonical_store_epoch_==0)canonical_store_owner_.reset();
    }
}
NativeGpuAliasState ResolveMemory::InspectTextureState(const TextureLayout& t,uint32_t address) const {
    auto& entry=CachedFootprint(t,{0,0,int32_t(t.width),int32_t(t.height)},address);
    return ReadNativeGpuAliasStateRanges(entry,serial_,defined_,page_versions_.size(),
        [&](uint32_t first,uint32_t end){return page_versions_.Max(first,end);},gpu_alias_state_stats_);
}
void ResolveMemory::Define(const Ranges& ranges) {
    AdvancePhysicalSerial();
    if(definition_stats_.enabled){++definition_stats_.calls;definition_stats_.ranges+=ranges.size();}
    // Exact covered-hull fast path. defined_ is canonical: Define merges both
    // overlapping and touching intervals and InvalidateBytes only removes bytes,
    // so no two stored intervals touch. If one stored interval covers the hull
    // of every (nonempty) range, each DefineNativeByteRange below would only
    // erase and reinsert that same interval (or return early when reuse is on),
    // leaving an identical map. Page versions are still assigned per range in
    // the original order, so versions, mutations and change tracking match.
    if(!ranges.empty()) {
        uint32_t lo=UINT32_MAX,hi=0;bool nonempty=true;
        for(const auto& [first,last]:ranges) {
            nonempty&=first<last;lo=std::min(lo,first);hi=std::max(hi,last);
        }
        const auto it=nonempty?defined_.upper_bound(lo):defined_.begin();
        if(nonempty&&it!=defined_.begin()&&std::prev(it)->second>=hi) {
            for(const auto& [first,last]:ranges)page_versions_.Assign(first/4096,(last-1)/4096+1,serial_);
            // Same per-range answer DefineNativeByteRange(...,true) returns:
            // the hull interval is the unique predecessor of every range start.
            if(definition_stats_.enabled)definition_stats_.covered+=ranges.size();
            return;
        }
    }
    for(auto [first,last]:ranges) {
        // Every write still updates all touched pages and the alias serial.
        // An unchanged byte coverage map does not mean unchanged contents.
        page_versions_.Assign(first/4096,(last-1)/4096+1,serial_);
        const bool covered=DefineNativeByteRange(defined_,first,last,definition_stats_.enabled);
        if(definition_stats_.enabled)definition_stats_.covered+=covered;
    }
}

uint64_t ResolveMemory::Version(const TextureLayout& t,uint32_t address) const {
    uint64_t version=0;
    for(const auto& [first,last]:Footprint(t,{0,0,int32_t(t.width),int32_t(t.height)},address))
        version=std::max(version,page_versions_.Max(first/4096,(last-1)/4096+1));
    return version;
}
bool ResolveMemory::FullyDefined(const TextureLayout& t,uint32_t address) const {
    for(const auto& [first,last]:Footprint(t,{0,0,int32_t(t.width),int32_t(t.height)},address)) {
        auto it=defined_.upper_bound(first);
        if(it==defined_.begin()||std::prev(it)->second<last)return false;
    }
    return true;
}
bool ResolveMemory::AnyDefined(const TextureLayout& t,uint32_t address) const {
    for(const auto& [first,last]:Footprint(t,{0,0,int32_t(t.width),int32_t(t.height)},address)) {
        auto it=defined_.upper_bound(first);
        if(it!=defined_.begin()&&std::prev(it)->second>first)return true;
        if(it!=defined_.end()&&it->first<last)return true;
    }
    return false;
}
void ResolveMemory::Initialize(uint32_t address,std::span<const uint8_t> data) {
    InitializeBytes(address,data,false);
}
void ResolveMemory::InitializeCpuWrite(uint32_t address,std::span<const uint8_t> data,bool allow_queue) {
    const bool queued=batch_&&batch_->active()&&
        NativeCpuWriteQueueEligible(allow_queue,true,address,uint32_t(data.size()))&&data.size()<=4096;
    const auto stages_before=batch_?batch_->statistics().stages:0;
    auto& reuse=cpu_write_upload_reuse_stats_;
    if(reuse.enabled&&reuse.poisoned)
        throw std::runtime_error("CPU upload reuse is poisoned after a failed write");
    bool selected=false;
    if(reuse.enabled) {
        if(queued)++reuse.queued_bypasses;
        else if(data.size()!=4096||address%4096||uint64_t(address)+data.size()>capacity_)
            ++reuse.unsupported_bypasses;
        else if(!reuse.checked_queue)++reuse.unchecked_queue_bypasses;
        else selected=true;
    }
    if(selected)InitializeCpuWriteReusable(address,data);
    else InitializeBytes(address,data,queued);
    const bool recorded=queued&&batch_->statistics().stages!=stages_before;
    if(recorded)++cpu_write_stats_.queued_uploads;else ++cpu_write_stats_.synchronous_uploads;
    cpu_write_stats_.uploaded_bytes+=data.size();
    // Define has already succeeded, so the page version names exactly these
    // bytes. A queued upload has not run yet, but in operation order the
    // buffer holds them from here on: any reader that would read the page
    // back first flushes this batch, and any later write retires the receipt.
    cpu_upload_receipts_.Remember(address,data,true,
        [this](uint32_t page){return page_versions_.Max(page,page+1);});
}
void ResolveMemory::InitializeCpuWriteReusable(uint32_t address,std::span<const uint8_t> data) {
    auto& stats=cpu_write_upload_reuse_stats_;
    try {
        if(!data.empty())MaterializeDeferredStores(address,uint32_t(std::min<uint64_t>(uint64_t(address)+data.size(),capacity_)));
        if(batch_)batch_->Flush();
        if(stats.poisoned||!stats.enabled||!stats.checked_queue||data.size()!=4096||
           address%4096||uint64_t(address)+data.size()>capacity_)
            throw std::logic_error("Invalid synchronous CPU upload reuse admission");
        if(cpu_write_upload_)++stats.reuses;
        else {
            cpu_write_upload_=device_.createBuffer(RenderBufferDesc::UploadBuffer(4096));
            if(!cpu_write_upload_||!static_cast<const VulkanBuffer&>(*cpu_write_upload_).vk)
                throw std::runtime_error("Cannot allocate reusable CPU upload buffer");
            ++stats.allocations;
        }
        auto* mapped=cpu_write_upload_->map();
        if(!mapped)throw std::runtime_error("Cannot map reusable CPU upload buffer");
        std::memcpy(mapped,data.data(),data.size());
        const auto& buffer=static_cast<const VulkanBuffer&>(*cpu_write_upload_);
        const auto flush=vmaFlushAllocation(buffer.device->allocator,buffer.allocation,0,VK_WHOLE_SIZE);
        cpu_write_upload_->unmap();
        if(flush!=VK_SUCCESS)throw std::runtime_error("Cannot flush reusable CPU upload buffer");
        commands_->begin();
        ++mutation_serial_;
        commands_->barriers(RenderBarrierStage::COPY,RenderBufferBarrier(bytes_.get(),RenderBufferAccess::WRITE));
        commands_->copyBufferRegion(RenderBufferReference(bytes_.get(),address),RenderBufferReference(cpu_write_upload_.get()),data.size());
        commands_->end();
        // The checked queue can throw even after submission. Set uncertainty
        // before entering it and retain every owner until actual wait/reset.
        cpu_write_upload_reuse_uncertain_=true;
        queue_.executeCommandLists(commands_.get(),fence_.get());++stats.submissions;
        queue_.waitForCommandFence(fence_.get());++stats.waits;
        cpu_write_upload_reuse_uncertain_=false;
        Define({{address,uint32_t(address+data.size())}});
        ++stats.uploads;stats.uploaded_bytes+=data.size();
    } catch(...) {stats.poisoned=true;throw;}
}
void ResolveMemory::InitializeBytes(uint32_t address,std::span<const uint8_t> data,bool queued) {
    if(!data.empty())MaterializeDeferredStores(address,uint32_t(std::min<uint64_t>(uint64_t(address)+data.size(),capacity_)));
    if(!queued&&batch_)batch_->Flush();
    if(data.empty()||address%4||data.size()%4||uint64_t(address)+data.size()>capacity_)
        throw std::runtime_error("Invalid GPU history byte range");
    auto upload=device_.createBuffer(RenderBufferDesc::UploadBuffer(data.size()));
    std::memcpy(upload->map(),data.data(),data.size());
    const auto& b=static_cast<VulkanBuffer&>(*upload);
    if(vmaFlushAllocation(b.device->allocator,b.allocation,0,VK_WHOLE_SIZE)!=VK_SUCCESS)
        throw std::runtime_error("Cannot flush GPU history upload");
    upload->unmap();
    if(queued)queued=batch_->ReserveUploadBytes(data.size());
    auto* commands=queued?&batch_->BeginStage():commands_.get();
    // Ownership enters the packet before any command references this buffer.
    auto* source=queued?batch_->Keep(std::move(upload)):upload.get();
    if(!queued)commands->begin();
    ++mutation_serial_;
    commands->barriers(RenderBarrierStage::COPY,RenderBufferBarrier(bytes_.get(),RenderBufferAccess::WRITE));
    commands->copyBufferRegion(RenderBufferReference(bytes_.get(),address),RenderBufferReference(source),data.size());
    if(!queued) {
        commands->end();queue_.executeCommandLists(commands,fence_.get());queue_.waitForCommandFence(fence_.get());
    }
    // Like Store, logical ownership advances in operation order. Queue barriers
    // establish the bytes before any later decode; CPU readers still fence.
    Define({{address,uint32_t(address+data.size())}});
}
void ResolveMemory::Invalidate(uint32_t address,uint32_t count) {
    InvalidateBytes(address,count,false);
}
void ResolveMemory::InvalidateCpuWrite(uint32_t address,uint32_t count,bool allow_queue) {
    const bool queued=batch_&&batch_->active()&&NativeCpuInvalidationQueueEligible(allow_queue,true,address,count);
    InvalidateBytes(address,count,queued);
    if(queued)++cpu_write_stats_.queued_invalidations;else ++cpu_write_stats_.synchronous_invalidations;
}
void ResolveMemory::InvalidateBytes(uint32_t address,uint32_t count,bool queued) {
    if(count)MaterializeDeferredStores(address,uint32_t(std::min<uint64_t>(uint64_t(address)+count,capacity_)));
    if(!queued&&batch_)batch_->Flush();
    if(!count||uint64_t(address)+count>capacity_)throw std::runtime_error("Invalid GPU memory invalidation");
    const uint32_t last=address+count;AdvancePhysicalSerial();++mutation_serial_;
    page_versions_.Assign(address/4096,(last-1)/4096+1,serial_);
    auto it=defined_.lower_bound(address);
    if(it!=defined_.begin()&&std::prev(it)->second>address)--it;
    while(it!=defined_.end()&&it->first<last) {
        const auto [first,end]=*it;it=defined_.erase(it);
        if(first<address)defined_.emplace(first,address);
        if(end>last){defined_.emplace(last,end);break;}
    }
}
bool ResolveMemory::BytesDefined(uint32_t address,uint32_t count) const {
    if(!count||uint64_t(address)+count>capacity_)return false;
    const auto it=defined_.upper_bound(address);
    return it!=defined_.begin()&&std::prev(it)->second>=uint64_t(address)+count;
}
void ResolveMemory::Copy(uint32_t source,uint32_t destination,uint32_t count) {
    CopyBuffer(source,destination,count,nullptr);
}
std::vector<uint8_t> ResolveMemory::CopyAndRead(uint32_t source,uint32_t destination,uint32_t count) {
    std::vector<uint8_t> result;
    CopyBuffer(source,destination,count,&result);
    ++copy_publication_stats_.readbacks;
    return result;
}
bool ResolveMemory::CopyFromReceipts(uint32_t source,uint32_t destination,uint32_t count,std::vector<uint8_t>& result) {
    auto& stats=copy_publication_stats_;
    if(!batch_||!batch_->active()){++stats.inactive_batch;return false;}
    if(!count||((source|destination|count)&3)||uint64_t(source)+count>capacity_||
       uint64_t(destination)+count>capacity_||(uint64_t(source)+count>destination&&uint64_t(destination)+count>source)||
       !BytesDefined(source,count)){++stats.ineligible;return false;}
    // Pending stores into either range change their page versions first.
    MaterializeDeferredStores(source,source+count);
    MaterializeDeferredStores(destination,destination+count);
    result.resize(count);
    const NativeCpuUploadReceipts::Copy copy{source,source+count,result.data()};
    if(!cpu_upload_receipts_.TryForward(std::span(&copy,1),
        [this](uint32_t page){return page_versions_.Max(page,page+1);})){++stats.receipt_misses;return false;}
    // In operation order this copy reads exactly the forwarded bytes; batch
    // barriers order it after every earlier write, like a queued upload.
    auto& commands=batch_->BeginStage();
    ++mutation_serial_;
    commands.barriers(RenderBarrierStage::COPY,RenderBufferBarrier(bytes_.get(),RenderBufferAccess::READ|RenderBufferAccess::WRITE));
    // Consecutive copies in one stage chain (A to B, then B to C); the state
    // tracker sees no transition between them, so order them explicitly.
    VkMemoryBarrier ordered{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    ordered.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT|VK_ACCESS_SHADER_WRITE_BIT;
    ordered.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(static_cast<VulkanCommandList&>(commands).vk,
        VK_PIPELINE_STAGE_TRANSFER_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,1,&ordered,0,nullptr,0,nullptr);
    commands.copyBufferRegion(RenderBufferReference(bytes_.get(),destination),RenderBufferReference(bytes_.get(),source),count);
    Define({{destination,destination+count}});
    cpu_upload_receipts_.RememberPages(destination,result,
        [this](uint32_t page){return page_versions_.Max(page,page+1);});
    ++stats.receipts;
    return true;
}
void ResolveMemory::CopyBuffer(uint32_t source,uint32_t destination,uint32_t count,std::vector<uint8_t>* read) {
    if(count) {
        MaterializeDeferredStores(source,uint32_t(std::min<uint64_t>(uint64_t(source)+count,capacity_)));
        MaterializeDeferredStores(destination,uint32_t(std::min<uint64_t>(uint64_t(destination)+count,capacity_)));
    }
    if(batch_)batch_->Flush();
    if(!count||((source|destination|count)&3)||uint64_t(destination)+count>capacity_||
        !BytesDefined(source,count))throw std::runtime_error("Undefined or invalid native buffer copy: source="+
            std::to_string(source)+" destination="+std::to_string(destination)+" bytes="+
            std::to_string(count)+" source_defined="+std::to_string(BytesDefined(source,count)));
    if(uint64_t(source)+count>destination&&uint64_t(destination)+count>source)
        throw std::runtime_error("Overlapping native buffer copy");
    commands_->begin();++mutation_serial_;
    commands_->barriers(RenderBarrierStage::COPY,RenderBufferBarrier(bytes_.get(),RenderBufferAccess::READ|RenderBufferAccess::WRITE));
    commands_->copyBufferRegion(RenderBufferReference(bytes_.get(),destination),RenderBufferReference(bytes_.get(),source),count);
    if(read) {
        if(!copy_readback_||copy_readback_bytes_<count) {
            copy_readback_bytes_=std::max<uint64_t>(count,64*1024);
            copy_readback_=device_.createBuffer(RenderBufferDesc::ReadbackBuffer(copy_readback_bytes_));
            if(!copy_readback_)throw std::runtime_error("Cannot allocate native copy readback");
        }
        // The copy's write must land before its readback in this same list.
        const auto vk_commands=static_cast<VulkanCommandList&>(*commands_).vk;
        VkMemoryBarrier written{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        written.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;written.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(vk_commands,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&written,0,nullptr,0,nullptr);
        commands_->copyBufferRegion(RenderBufferReference(copy_readback_.get()),RenderBufferReference(bytes_.get(),destination),count);
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(vk_commands,VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,nullptr,0,nullptr);
    }
    commands_->end();queue_.executeCommandLists(commands_.get(),fence_.get());queue_.waitForCommandFence(fence_.get());
    Define({{destination,destination+count}});
    if(read) {
        const auto& buffer=static_cast<VulkanBuffer&>(*copy_readback_);
        const void* mapped=copy_readback_->map();
        if(!mapped||vmaInvalidateAllocation(buffer.device->allocator,buffer.allocation,0,VK_WHOLE_SIZE)!=VK_SUCCESS)
            throw std::runtime_error("Cannot read native copy result");
        read->resize(count);std::memcpy(read->data(),mapped,count);copy_readback_->unmap();
        // The copy completed and Define succeeded: the destination's pages
        // now hold exactly these bytes until a later write changes them.
        cpu_upload_receipts_.RememberPages(destination,*read,
            [this](uint32_t page){return page_versions_.Max(page,page+1);});
    }
}
void ResolveMemory::CopyFromCpu(uint32_t source,uint32_t destination,std::span<const uint8_t> data,bool queued) {
    if(data.empty()||data.size()>capacity_||((source|destination|data.size())&3)||
        uint64_t(source)+data.size()>capacity_||uint64_t(destination)+data.size()>capacity_)
        throw std::runtime_error("Invalid native CPU buffer copy");
    if(uint64_t(source)+data.size()>destination&&uint64_t(destination)+data.size()>source)
        throw std::runtime_error("Overlapping native CPU buffer copy");
    // Seeding source as well would make an unwatched CPU allocation eligible
    // to overwrite later texture snapshots through GPU-history overlays.
    // Queued copies record into the active transfer batch (falling back to the
    // synchronous path when its upload budget is exhausted).
    InitializeBytes(destination,data,queued&&batch_&&batch_->active());
    // As a CPU write's upload: the destination holds these bytes from here on.
    cpu_upload_receipts_.RememberPages(destination,data,
        [this](uint32_t page){return page_versions_.Max(page,page+1);});
}
void ResolveMemory::CopyFromMixed(uint32_t source,uint32_t destination,std::span<const uint8_t> data,
    std::span<const std::pair<uint32_t,uint32_t>> gpu_ranges) {
    if(data.empty()||data.size()>8*1024*1024||((source|destination|data.size())&3)||
       uint64_t(source)+data.size()>capacity_||uint64_t(destination)+data.size()>capacity_||gpu_ranges.empty())
        throw std::runtime_error("Invalid native mixed buffer copy");
    if(uint64_t(source)+data.size()>destination&&uint64_t(destination)+data.size()>source)
        throw std::runtime_error("Overlapping native mixed buffer copy");
    // Validate the entire captured ownership before any destination mutation.
    // Readback alignment may include padding, but only certified bytes below
    // may replace the immutable CPU snapshot. Padding never becomes owned.
    Ranges aligned;uint32_t previous=source;
    for(const auto& [first,last]:gpu_ranges) {
        if(first<previous||first>=last||uint64_t(last)>uint64_t(source)+data.size()||!BytesDefined(first,last-first))
            throw std::runtime_error("Undefined or invalid native mixed source fragment: first="+
                std::to_string(first)+" end="+std::to_string(last));
        previous=last;
        const uint32_t begin=first&~3u,end=(last+3u)&~3u;
        if(!aligned.empty()&&begin<=aligned.back().second)aligned.back().second=std::max(aligned.back().second,end);
        else aligned.emplace_back(begin,end);
    }
    MaterializeDeferredStores(source,uint32_t(uint64_t(source)+data.size()));
    if(batch_)batch_->Flush();
    std::vector<uint8_t> composed(data.begin(),data.end());
    auto readback=device_.createBuffer(RenderBufferDesc::ReadbackBuffer(data.size()));
    commands_->begin();
    commands_->barriers(RenderBarrierStage::COPY,RenderBufferBarrier(bytes_.get(),RenderBufferAccess::READ));
    for(const auto& [first,last]:aligned)
        commands_->copyBufferRegion(RenderBufferReference(readback.get(),first-source),
            RenderBufferReference(bytes_.get(),first),last-first);
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(static_cast<VulkanCommandList&>(*commands_).vk,VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,nullptr,0,nullptr);
    commands_->end();queue_.executeCommandLists(commands_.get(),fence_.get());queue_.waitForCommandFence(fence_.get());
    const auto& buffer=static_cast<VulkanBuffer&>(*readback);
    const auto* mapped=static_cast<const uint8_t*>(readback->map());
    if(vmaInvalidateAllocation(buffer.device->allocator,buffer.allocation,0,VK_WHOLE_SIZE)!=VK_SUCCESS)
        throw std::runtime_error("Cannot invalidate native mixed buffer readback");
    for(const auto& [first,last]:gpu_ranges)
        std::memcpy(composed.data()+first-source,mapped+first-source,last-first);
    readback->unmap();
    CopyFromCpu(source,destination,composed);
}
std::vector<uint8_t> ResolveMemory::ReadBytes(uint32_t address,uint32_t count) {
    if(count)MaterializeDeferredStores(address,uint32_t(std::min<uint64_t>(uint64_t(address)+count,capacity_)));
    if(batch_)batch_->Flush();
    if(((address|count)&3)||!BytesDefined(address,count))throw std::runtime_error("Undefined native buffer readback");
    auto readback=device_.createBuffer(RenderBufferDesc::ReadbackBuffer(count));
    commands_->begin();
    commands_->barriers(RenderBarrierStage::COPY,RenderBufferBarrier(bytes_.get(),RenderBufferAccess::READ));
    commands_->copyBufferRegion(RenderBufferReference(readback.get()),RenderBufferReference(bytes_.get(),address),count);
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(static_cast<VulkanCommandList&>(*commands_).vk,VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,nullptr,0,nullptr);
    commands_->end();queue_.executeCommandLists(commands_.get(),fence_.get());queue_.waitForCommandFence(fence_.get());
    const auto& buffer=static_cast<VulkanBuffer&>(*readback);
    const void* mapped=readback->map();
    if(vmaInvalidateAllocation(buffer.device->allocator,buffer.allocation,0,VK_WHOLE_SIZE)!=VK_SUCCESS)
        throw std::runtime_error("Cannot invalidate native buffer readback");
    std::vector<uint8_t> result(count);std::memcpy(result.data(),mapped,count);readback->unmap();return result;
}
NativeMemoryState ResolveMemory::ExportState() const {
    if(batch_)batch_->Flush();
    NativeMemoryState state{static_cast<VulkanBuffer&>(*bytes_).vk,capacity_,{}};
    state.ranges.reserve(defined_.size());
    for(const auto& [first,last]:defined_)state.ranges.emplace_back(first,last-first);
    return state;
}
const std::array<ResolveMemory::Ranges,2>& ResolveMemory::CpuTextureFootprints(const TextureLayout& t) const {
    return CachedCpuTextureFootprint(t).ranges;
}
ResolveMemory::CpuFootprintStatistics ResolveMemory::CpuTextureFootprintStatistics() const {
    const auto stats=cpu_texture_footprints_.statistics();
    return {stats.hits,stats.misses,stats.admissions,stats.evictions,
            stats.entries,stats.peak_entries,decltype(cpu_texture_footprints_)::kCapacity};
}
ResolveMemory::CpuTextureFootprint& ResolveMemory::CachedCpuTextureFootprint(const TextureLayout& t) const {
    return cpu_texture_footprints_.GetOrCreate(t.fetch,[&] {
        std::array<Ranges,2> ranges;
        for(unsigned region=0;region<2;++region) {
            ranges[region]=TextureMemoryRanges(t,region!=0,coalesced_texture_ranges_);
            for(auto& [first,count]:ranges[region]) {
                if(uint64_t(first)+count>capacity_)throw std::runtime_error("CPU texture exceeds physical storage");
                count+=first; // Internal ranges consistently store exclusive ends.
            }
        }
        CpuTextureFootprint result;
        if(page_versions_.tracks_changes())for(unsigned region=0;region<2;++region) {
            // One conservative cover per independently located base/mip region.
            // Original exact sparse ranges below still determine the version.
            uint32_t first=UINT32_MAX,end=0;
            for(const auto& range:ranges[region]) {
                first=std::min(first,range.first/4096);
                end=std::max(end,(range.second-1)/4096+1);
            }
            if(end)result.guard_pages[region]={first,end};
        }
        result.baseline_page_visits=NativeByteRangePageVisits(ranges);
        if(compact_cpu_texture_version_pages_) {
            result.version_pages=BuildNativePageVersionRanges(ranges);
            result.version_page_visits=NativePageRangeVisits(result.version_pages);
        } else result.version_page_visits=result.baseline_page_visits;
        result.ranges=std::move(ranges);return result;
    });
}
uint64_t ResolveMemory::CpuTextureVersion(const TextureLayout& t,bool* gpu_bytes) const {
    auto& cached=CachedCpuTextureFootprint(t);
    // The optional independent write tracker permits reuse across mutations
    // outside conservative base/mip covers. All original page-version writers
    // pass through Assign; changes to version values need not be monotonic.
    ++cpu_version_stats_.calls;
    const bool tracked=page_versions_.tracks_changes();
    if(cached.version_valid&&(tracked?
        page_versions_.UnchangedSince(cached.range_revision,cached.guard_pages):cached.version_serial==serial_)) {
        if(cached.version_serial==serial_)++cpu_version_stats_.memo_hits;
        else ++cpu_version_stats_.range_hits;
        cached.version_serial=serial_;cached.range_revision=page_versions_.revision();
        if(gpu_bytes)*gpu_bytes=cached.gpu_bytes;
        return cached.version;
    }
    const auto scan=[&] {
        ++cpu_version_stats_.rescans;
        cpu_version_stats_.baseline_page_visits+=cached.baseline_page_visits;
        cpu_version_stats_.performed_page_visits+=cached.version_page_visits;
        if(compact_cpu_texture_version_pages_) {
            ++cpu_version_stats_.page_union_rescans;
            uint64_t version=0;
            for(const auto& [first,last]:cached.version_pages)version=std::max(version,page_versions_.Max(first,last));
            return version;
        }
        uint64_t version=0;
        for(const auto& region:cached.ranges)for(const auto& [first,last]:region)
            version=std::max(version,page_versions_.Max(first/4096,(last-1)/4096+1));
        return version;
    };
    // Publish only after a successful exact scan. Exhausted tracking stamps
    // stay invalid, so even a repeated physical serial cannot skip this scan.
    const auto version=scan();
    // Every defined_ change (including deferred stores, defined when queued)
    // assigns its pages, so this is current exactly while the version memo is.
    bool defined=false;
    for(const auto& region:cached.ranges)for(const auto& [first,last]:region) {
        auto it=defined_.upper_bound(first);
        if(it!=defined_.begin()&&std::prev(it)->second>first)--it;
        defined|=it!=defined_.end()&&it->first<last;
    }
    cached.version=version;cached.gpu_bytes=defined;cached.version_serial=serial_;cached.version_valid=true;
    cached.range_revision=page_versions_.revision();
    if(gpu_bytes)*gpu_bytes=defined;
    return version;
}
uint64_t ResolveMemory::OverlayCpuTexture(const TextureLayout& t,std::span<uint8_t> base,std::span<uint8_t> mips,
                                         TextureOverlayStats* stats,const std::function<void()>& before_readback) {
    if(!pending_stores_.empty())for(const auto& footprint:CpuTextureFootprints(t))MaterializeDeferredStores(footprint);
    if(!before_readback&&batch_)batch_->Flush();
    if(stats)*stats={};
    using Copy=NativeCpuUploadReceipts::Copy;
    std::vector<Copy> copies;
    uint64_t owned_bytes=0;
    const auto& footprints=CpuTextureFootprints(t);
    for(unsigned region=0;region<2;++region) {
        const auto data=region?mips:base;
        const uint32_t address=region?t.mip_address:t.base_address;
        if(stats) {
            stats->footprint_ranges[region]=footprints[region].size();
            if(!footprints[region].empty()) {
                stats->footprint_first[region]=footprints[region].front().first;
                stats->footprint_end[region]=footprints[region].back().second;
            }
        }
        for(const auto& [first,last]:footprints[region]) {
            if(stats)stats->sampled_bytes[region]+=last-first;
            if(first<address||uint64_t(last)-address>data.size())throw std::runtime_error("CPU texture payload misses sampled blocks");
            auto it=defined_.upper_bound(first);
            if(it!=defined_.begin()&&std::prev(it)->second>first)--it;
            for(;it!=defined_.end()&&it->first<last;++it) {
                const uint32_t start=std::max(first,it->first),end=std::min(last,it->second);
                if(start>=end)continue;
                copies.push_back({start,end,data.data()+start-address});
                owned_bytes+=end-start;
                if(stats)stats->owned_region_bytes[region]+=end-start;
            }
        }
    }
    if(copies.empty())return 0;
    if(before_readback) {
        before_readback();
        if(batch_)batch_->Flush();
    }
    // Only completed CPU-upload bytes can replace a readback. Validate AFTER
    // the original callbacks/flushes, which may materialize native writes.
    // A single missing/stale page preserves the entire original GPU path.
    if(cpu_upload_receipts_.TryForward(copies,
        [this](uint32_t page){return page_versions_.Max(page,page+1);})) {
        if(stats)stats->owned_bytes=owned_bytes;
        return owned_bytes;
    }
    // Separate physical transfers from ownership masks. A transfer may include
    // nearby CPU-owned bytes or allocation padding, but only the exact ranges
    // in copies may replace the caller's CPU snapshot. Base and mip allocations
    // may overlap or appear in either address order.
    Ranges spans;spans.reserve(copies.size());
    for(const auto& c:copies)spans.emplace_back(c.first&~3u,(c.last+3)&~3u);
    std::sort(spans.begin(),spans.end());
    size_t merged=0;
    for(const auto& span:spans) {
        if(merged&&uint64_t(span.first)<=uint64_t(spans[merged-1].second)+256)
            spans[merged-1].second=std::max(spans[merged-1].second,span.second);
        else spans[merged++]=span;
    }
    spans.resize(merged);
    std::vector<VkBufferCopy> regions;
    uint64_t size=0;
    for(const auto& [first,last]:spans)for(uint32_t offset=first;offset<last;) {
        const uint32_t count=std::min(last-offset,1u<<20);
        if(size+count>512u*1024*1024)throw std::runtime_error("CPU texture overlay exceeds safety limit");
        regions.push_back({offset,size,count});size+=count;offset+=count;
    }
    auto readback=device_.createBuffer(RenderBufferDesc::ReadbackBuffer(size));
    commands_->begin();
    commands_->barriers(RenderBarrierStage::COPY,RenderBufferBarrier(bytes_.get(),RenderBufferAccess::READ));
    // One Vulkan command carries all disjoint destination ranges instead of one
    // driver call per owned fragment. This is a consumer-side API use; Plume's
    // command list and buffer ownership remain unchanged.
    const auto vk_commands=static_cast<VulkanCommandList&>(*commands_).vk;
    const auto& buffer=static_cast<VulkanBuffer&>(*readback);
    vkCmdCopyBuffer(vk_commands,static_cast<VulkanBuffer&>(*bytes_).vk,buffer.vk,
                    uint32_t(regions.size()),regions.data());
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(vk_commands,VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,nullptr,0,nullptr);
    commands_->end();queue_.executeCommandLists(commands_.get(),fence_.get());queue_.waitForCommandFence(fence_.get());
    const auto* mapped=static_cast<const uint8_t*>(readback->map());
    if(!mapped)throw std::runtime_error("Cannot map texture overlay readback");
    if(vmaInvalidateAllocation(buffer.device->allocator,buffer.allocation,0,VK_WHOLE_SIZE)!=VK_SUCCESS)
        throw std::runtime_error("Cannot invalidate texture overlay readback");
    for(const auto& c:copies) {
        auto region=std::lower_bound(regions.begin(),regions.end(),c.first,
            [](const VkBufferCopy& r,uint32_t first){return r.srcOffset+r.size<=first;});
        uint32_t source=c.first;
        while(source<c.last) {
            if(region==regions.end()||source<region->srcOffset)
                throw std::logic_error("Texture overlay readback missed an owned range");
            const uint32_t count=uint32_t(std::min<uint64_t>(c.last-source,region->srcOffset+region->size-source));
            const auto* fresh=mapped+region->dstOffset+(source-region->srcOffset);
            auto* snapshot=c.destination+(source-c.first);
            for(uint32_t at=0;at<count;at+=4096) {
                const uint32_t piece=std::min(count-at,4096u);
                if(!std::memcmp(snapshot+at,fresh+at,piece))overlay_identical_bytes_+=piece;
            }
            std::memcpy(snapshot,fresh,count);
            source+=count;++region;
        }
    }
    readback->unmap();
    ++overlay_readbacks_;overlay_owned_bytes_+=owned_bytes;
    if(overlay_owned_bytes_>=overlay_next_report_) {
        overlay_next_report_=overlay_owned_bytes_+(64ull<<20);
        GOW3_RESOLVE_MEMORY_LOG("Texture overlay readbacks: {} reads, {} MB owned by the GPU copy, {} MB of it equal to the CPU snapshot",
            overlay_readbacks_,overlay_owned_bytes_>>20,overlay_identical_bytes_>>20);
    }
    if(stats) {stats->copy_regions=regions.size();stats->readback_bytes=size;stats->owned_bytes=owned_bytes;}
    return owned_bytes;
}
void ResolveMemory::RecordImportedBytes(uint32_t address,uint32_t count) {
    if(!count||uint64_t(address)+count>capacity_)throw std::runtime_error("Invalid imported native memory range");
    ++mutation_serial_;Define({{address,address+count}});
}
void ResolveMemory::Store(RenderTexture& image,const TextureLayout& layout,const RenderRect& rectangle,uint32_t address) {
    Transfer(image,layout,rectangle,true,address);
}
std::array<uint32_t,10> ResolveMemory::StoreFootprintKey(const TextureLayout& layout,const RenderRect& rectangle,
                                                        uint32_t address) {
    const uint32_t pitch=layout.levels.size()==1?layout.levels.front().pitch_blocks:UINT32_MAX;
    return {address,layout.width,layout.height,layout.format,uint32_t(layout.tiled)|(layout.endian<<1),pitch,
        uint32_t(rectangle.left),uint32_t(rectangle.top),uint32_t(rectangle.right),uint32_t(rectangle.bottom)};
}
void ResolveMemory::ReleaseDeferredImage(const RenderTexture& image,const std::array<uint32_t,10>& next_footprint) {
    if(pending_stores_.empty())return;
    const auto before=pending_stores_.size();
    std::erase_if(pending_stores_,[&](const PendingStore& p){return p.image.get()==&image&&p.footprint==next_footprint;});
    deferred_store_stats_.superseded+=before-pending_stores_.size();
    std::vector<PendingStore> due;
    std::erase_if(pending_stores_,[&](PendingStore& p) {
        if(p.image.get()!=&image)return false;
        due.push_back(std::move(p));return true;
    });
    for(auto& p:due) {
        if(batch_&&batch_->active()) {
            const auto& desc=static_cast<const VulkanTexture&>(*p.image).desc;
            batch_->KeepImage(p.image,uint64_t(desc.width)*desc.height*16);
        }
        // An older store may run inside a newer one's deferral; run it for real.
        auto deferring=std::exchange(deferring_image_,nullptr);
        try {Dispatch(*p.image,p.push,true,nullptr,p.first_byte,p.byte_count);}
        catch(...) {deferring_image_=std::move(deferring);throw;}
        deferring_image_=std::move(deferring);
        ++deferred_store_stats_.materialized;
    }
}
bool ResolveMemory::DeferStore(std::shared_ptr<RenderTexture> image,const TextureLayout& layout,
                               const RenderRect& rectangle,uint32_t address) {
    if(!deferred_stores_||!image||!batch_||!batch_->active()||layout.levels.size()!=1)return false;
    const auto footprint=StoreFootprintKey(layout,rectangle,address);
    // Bytes the new store rewrites in full; never executing the old one is
    // equivalent. Different footprints keep order through materialization.
    const auto before=pending_stores_.size();
    std::erase_if(pending_stores_,[&](const PendingStore& p){return p.footprint==footprint;});
    deferred_store_stats_.superseded+=before-pending_stores_.size();
    deferring_image_=std::move(image);deferring_footprint_=footprint;
    try {Transfer(*deferring_image_,layout,rectangle,true,address);}
    catch(...) {deferring_image_.reset();throw;}
    deferring_image_.reset();
    ++deferred_store_stats_.deferred;
    deferred_store_stats_.peak_pending=std::max<uint64_t>(deferred_store_stats_.peak_pending,pending_stores_.size());
    return true;
}
void ResolveMemory::MaterializeDeferredStores(uint32_t first,uint32_t last) {
    if(pending_stores_.empty()||first>=last)return;
    std::vector<PendingStore> due;
    std::erase_if(pending_stores_,[&](PendingStore& p) {
        if(p.first_byte>=last||uint64_t(p.first_byte)+p.byte_count<=first)return false;
        due.push_back(std::move(p));return true;
    });
    for(auto& p:due) {
        // The packet must own the source image until its dispatch retires.
        if(batch_&&batch_->active()) {
            const auto& desc=static_cast<const VulkanTexture&>(*p.image).desc;
            batch_->KeepImage(p.image,uint64_t(desc.width)*desc.height*16);
        }
        // An older store may run inside a newer one's deferral; run it for real.
        auto deferring=std::exchange(deferring_image_,nullptr);
        try {Dispatch(*p.image,p.push,true,nullptr,p.first_byte,p.byte_count);}
        catch(...) {deferring_image_=std::move(deferring);throw;}
        deferring_image_=std::move(deferring);
        ++deferred_store_stats_.materialized;
    }
}
bool ResolveMemory::EnableCanonicalStoreReceipt(std::span<const uint32_t> code) {
    if(canonical_store_stats_.configured||mutation_serial_||descriptor_dispatch_started_)
        throw std::logic_error("Canonical store receipt must be configured once before work");
    canonical_store_stats_.configured=true;
    if(code.size()<5||code.front()!=0x07230203||code.size_bytes()>1024*1024||
       !batch_||!batch_->has_work_queue())return false;
    try {
        const auto ranges=CanonicalStoreDescriptorRanges();
        const RenderDescriptorSetDesc descriptor(ranges.data(),ranges.size());
        const RenderPushConstantRange push(0,0,0,64,RenderShaderStageFlag::COMPUTE);
        auto shader=device_.createShader(code.data(),code.size_bytes(),"main",RenderShaderFormat::SPIRV);
        auto layout=device_.createPipelineLayout(RenderPipelineLayoutDesc(&push,1,&descriptor,1));
        if(!shader||!layout||!static_cast<VulkanShader&>(*shader).vk||
           !static_cast<VulkanPipelineLayout&>(*layout).vk)return false;
        auto pipeline=device_.createComputePipeline(RenderComputePipelineDesc(layout.get(),shader.get(),8,8,1));
        if(!pipeline||!static_cast<VulkanComputePipeline&>(*pipeline).vk)return false;
        auto pool=std::make_unique<DescriptorPool>();
        auto owner=std::make_shared<const uint8_t>(0);
        canonical_store_shader_=std::move(shader);canonical_store_layout_=std::move(layout);
        canonical_store_pipeline_=std::move(pipeline);canonical_store_descriptor_pool_=std::move(pool);
        canonical_store_owner_=std::move(owner);canonical_store_stats_.supported=true;return true;
    }catch(const std::exception&){return false;} // Preparation only; no recorded work.
}
bool ResolveMemory::CanonicalStoreReceiptMatches(const CanonicalStoreReceipt& receipt,
        const TextureLayout& layout,uint32_t address) const {
    const auto key=CanonicalStoreKey(layout,address);
    return receipt.image&&canonical_store_owner_&&receipt.owner==canonical_store_owner_&&
        receipt.epoch==canonical_store_epoch_&&key&&receipt.key==*key&&
        FullyDefined(layout,address)&&receipt.version==Version(layout,address);
}
std::optional<ResolveMemory::CanonicalStoreReceipt> ResolveMemory::TryStoreCanonical(
        RenderTexture& image,const TextureLayout& layout,const RenderRect& rectangle,uint32_t address) {
    ++canonical_store_stats_.requests;
    const auto refuse=[&]() -> std::optional<CanonicalStoreReceipt> {
        ++canonical_store_stats_.refused;return std::nullopt;
    };
    const auto key=CanonicalStoreKey(layout,address);
    if(!canonical_store_pipeline_||!canonical_store_owner_||!key||!batch_||!batch_->active()||
       !batch_->has_work_queue()||rectangle.left||rectangle.top||
       rectangle.right!=int32_t(layout.width)||rectangle.bottom!=int32_t(layout.height))return refuse();
    const auto& source=static_cast<const VulkanTexture&>(image);const auto& desc=source.desc;
    const auto* device=static_cast<const VulkanDevice*>(&device_);
    if(source.device!=device||!source.vk||!source.ownership||!source.allocation||
       desc.dimension!=RenderTextureDimension::TEXTURE_2D||desc.width!=layout.width||desc.height!=layout.height||
       desc.depth!=1||desc.arraySize!=1||desc.mipLevels!=1||
       desc.multisampling.sampleCount!=RenderSampleCount::COUNT_1||desc.multisampling.sampleLocationsEnabled||
       (desc.format!=RenderFormat::R32G32B32A32_FLOAT&&desc.format!=RenderFormat::R16G16_FLOAT&&
        desc.format!=RenderFormat::R16G16B16A16_UNORM&&desc.format!=RenderFormat::R16G16B16A16_SNORM))return refuse();
    const uint64_t image_bytes=uint64_t(layout.width)*layout.height*16;
    if(image_bytes>NativeTransferBatch::kMaxImageBytes)return refuse();
    const auto& footprint=Footprint(layout,rectangle,address);
    uint64_t owned_bytes=0;for(const auto& range:footprint)owned_bytes+=range.second-range.first;
    // Prove one distinct physical byte range per texel, including odd tiled
    // pitches and packed offsets. A racing overlapping layout must decline.
    if(footprint.empty()||owned_bytes!=uint64_t(layout.width)*layout.height*layout.block.bytes)return refuse();
    const uint32_t first=footprint.front().first&~(descriptor_alignment_-1);
    const uint32_t last=(footprint.back().second+3)&~3u;
    if(last-first>descriptor_range_)return refuse(); // No split-store receipts in v1.
    const auto& level=layout.levels.front();
    Parameters push{layout.width,layout.height,0,0,level.x,level.y,level.pitch_blocks,
        uint32_t(std::countr_zero(layout.block.bytes)),layout.tiled,layout.endian,layout.format==28?31u:layout.format,
        (layout.fetch[0]>>2)&255u,uint32_t(level.offset)+address-first,(last-first)/4,0,0};
    CanonicalStoreReceipt receipt;receipt.owner=canonical_store_owner_;receipt.key=*key;
    std::shared_ptr<RenderDescriptorSet> descriptors;
    std::unique_ptr<RenderTextureView> source_view,output_view;
    try {
        auto output_desc=RenderTextureDesc::Texture2D(layout.width,layout.height,1,RenderFormat::R32G32B32A32_FLOAT);
        output_desc.flags|=RenderTextureFlag::STORAGE|RenderTextureFlag::UNORDERED_ACCESS;
        receipt.image=device_.createTexture(output_desc);
        if(!receipt.image||!static_cast<VulkanTexture&>(*receipt.image).vk)return refuse();
        const auto ranges=CanonicalStoreDescriptorRanges();
        descriptors=canonical_store_descriptor_pool_->Acquire(0,[&] {
            auto created=device_.createDescriptorSet(RenderDescriptorSetDesc(ranges.data(),ranges.size()));
            if(!created||!static_cast<VulkanDescriptorSet&>(*created).vk)
                throw std::runtime_error("Cannot allocate canonical store descriptors");
            return created;
        });
        source_view=image.createTextureView(RenderTextureViewDesc::Texture2D(desc.format));
        output_view=receipt.image->createTextureView(RenderTextureViewDesc::Texture2D(RenderFormat::R32G32B32A32_FLOAT));
        if(!source_view||!output_view||!static_cast<VulkanTextureView&>(*source_view).vk||
           !static_cast<VulkanTextureView&>(*output_view).vk)return refuse();
        const RenderBufferStructuredView window(4,first/4);
        descriptors->setBuffer(0,bytes_.get(),last-first,&window);
        descriptors->setTexture(1,&image,RenderTextureLayout::SHADER_READ,source_view.get());
        descriptors->setTexture(2,receipt.image.get(),RenderTextureLayout::GENERAL,output_view.get());
    }catch(const std::exception&){return refuse();} // Still before any recording or physical mutation.
    // From here, a wait/record/acceptance failure is fatal. Do not catch and
    // replay. KeepImage can drain the prefix and select synchronous fallback.
    batch_->KeepImage(receipt.image,image_bytes);
    if(!batch_->active())return refuse();
    auto& commands=batch_->BeginStage();
    batch_->KeepLease(descriptors);batch_->Keep(std::move(source_view));batch_->Keep(std::move(output_view));
    descriptor_dispatch_started_=true;++mutation_serial_;
    commands.barriers(RenderBarrierStage::COMPUTE,RenderBufferBarrier(bytes_.get(),RenderBufferAccess::READ|RenderBufferAccess::WRITE));
    commands.barriers(RenderBarrierStage::COMPUTE,RenderTextureBarrier(&image,RenderTextureLayout::SHADER_READ));
    commands.barriers(RenderBarrierStage::COMPUTE,RenderTextureBarrier(receipt.image.get(),RenderTextureLayout::GENERAL));
    commands.setComputePipelineLayout(canonical_store_layout_.get());commands.setComputePushConstants(0,&push);
    commands.setComputeDescriptorSet(descriptors.get(),0);commands.setPipeline(canonical_store_pipeline_.get());
    commands.dispatch((layout.width+7)/8,(layout.height+7)/8,1);
    // Prefix retirement during KeepImage/BeginStage may invoke observers that
    // evict metadata caches. Reacquire the exact footprint after those waits.
    Define(Footprint(layout,rectangle,address));
    receipt.version=Version(layout,address);receipt.epoch=canonical_store_epoch_;
    ++canonical_store_stats_.selected;canonical_store_stats_.pixels+=uint64_t(layout.width)*layout.height;
    return receipt;
}
void ResolveMemory::StoreDepth(RenderTexture& image,const TextureLayout& layout,const RenderRect& rectangle,uint32_t address,
                                RenderTexture& stencil,int32_t source_x,int32_t source_y) {
    if(layout.format!=22&&layout.format!=23)throw std::runtime_error("Stencil bytes require a depth resolve format");
    const auto& desc=static_cast<VulkanTexture&>(stencil).desc;
    if(int64_t(rectangle.left)+source_x<0||int64_t(rectangle.top)+source_y<0||
       int64_t(rectangle.right)+source_x>desc.width||int64_t(rectangle.bottom)+source_y>desc.height)
        throw std::runtime_error("Stencil source rectangle is out of bounds");
    Transfer(image,layout,rectangle,true,address,&stencil,source_x,source_y);
}
bool ResolveMemory::EnableAtlasRgbaDepthStore(std::span<const uint32_t> code) {
    if(atlas_rgba_depth_store_configured_||mutation_serial_)
        throw std::logic_error("Atlas RGBA stencil store must be configured once before use");
    atlas_rgba_depth_store_configured_=true;
    if(code.size()<5||code.front()!=0x07230203||code.size_bytes()>1024*1024||
       !layouts_[0]||!static_cast<const VulkanPipelineLayout&>(*layouts_[0]).vk)return false;
    try {
        auto shader=device_.createShader(code.data(),code.size_bytes(),"main",RenderShaderFormat::SPIRV);
        if(!shader||!static_cast<const VulkanShader&>(*shader).vk)return false;
        auto pipeline=device_.createComputePipeline(RenderComputePipelineDesc(layouts_[0].get(),shader.get(),8,8,1));
        if(!pipeline||!static_cast<const VulkanComputePipeline&>(*pipeline).vk)return false;
        atlas_rgba_depth_store_shader_=std::move(shader);atlas_rgba_depth_store_pipeline_=std::move(pipeline);return true;
    }catch(...){return false;} // Startup preparation only, no recorded work.
}
bool ResolveMemory::StoreDepthFromRgbaStencil(RenderTexture& image,const TextureLayout& layout,
        const RenderRect& rectangle,uint32_t address,RenderTexture& source,int32_t dx,int32_t dy) {
    const auto& rgba=static_cast<const VulkanTexture&>(source);const auto& output=static_cast<const VulkanTexture&>(image);
    const auto& a=rgba.desc;const auto& b=output.desc;const auto* device=static_cast<const VulkanDevice*>(&device_);
    if(!SupportsAtlasRgbaDepthStore()||&source==&image||rgba.device!=device||output.device!=device||
       !rgba.vk||!output.vk||rgba.imageFormat!=VK_FORMAT_R32G32B32A32_SFLOAT||output.imageFormat!=VK_FORMAT_R32_SFLOAT||!rgba.ownership||!output.ownership||!rgba.allocation||!output.allocation||
       a.dimension!=RenderTextureDimension::TEXTURE_2D||b.dimension!=RenderTextureDimension::TEXTURE_2D||
       a.depth!=1||b.depth!=1||a.mipLevels!=1||b.mipLevels!=1||a.arraySize!=1||b.arraySize!=1||
       a.format!=RenderFormat::R32G32B32A32_FLOAT||b.format!=RenderFormat::R32_FLOAT||
       a.multisampling.sampleCount!=RenderSampleCount::COUNT_1||b.multisampling.sampleCount!=RenderSampleCount::COUNT_1||
       a.multisampling.sampleLocationsEnabled||b.multisampling.sampleLocationsEnabled||
       b.width!=layout.width||b.height!=layout.height||layout.dimension!=1||layout.layers!=1||
       layout.min_mip||layout.max_mip||layout.levels.size()!=1||layout.block.block_width!=1||
       layout.block.block_height!=1||layout.block.bytes!=4||layout.endian>3||
       !NativeAtlasRgbaStencilStoreRegionAllowed(layout.width,layout.height,layout.format,a.width,a.height,
          {rectangle.left,rectangle.top,rectangle.right,rectangle.bottom},dx,dy))return false;
    // Exactly the original address/window/subdivision/publication path. This
    // does NOT replace PassTransfer::Resolve or either depth quantization.
    Transfer(image,layout,rectangle,true,address,&source,dx,dy,nullptr,true);return true;
}
std::unique_ptr<RenderTexture> ResolveMemory::Load(const TextureLayout& layout,uint32_t address,
        const NativeSynchronousGpuTimingContext* presentation_timing) {
    NativeTransferBatch::Suspension synchronous(batch_);
    if(layout.dimension==1&&layout.layers>1) {
        Footprint(layout,{0,0,int32_t(layout.width),int32_t(layout.height)},address);
        auto volume=device_.createTexture(RenderTextureDesc::Texture3D(layout.width,layout.height,layout.layers,1,RenderFormat::R32G32B32A32_FLOAT));
        std::vector<std::unique_ptr<RenderTexture>> slices;
        for(const auto& level:layout.levels) {
            auto slice=layout;slice.layers=1;slice.fetch[1]&=~1024u;slice.fetch[2]&=0x3ffffff;
            slice.levels={level};slice.levels[0].offset=0;slice.levels[0].layer=0;
            slices.push_back(Load(slice,address+uint32_t(level.offset)));
        }
        commands_->begin();
        commands_->barriers(RenderBarrierStage::COPY,RenderTextureBarrier(volume.get(),RenderTextureLayout::COPY_DEST));
        for(unsigned z=0;z<slices.size();++z) {
            commands_->barriers(RenderBarrierStage::COPY,RenderTextureBarrier(slices[z].get(),RenderTextureLayout::COPY_SOURCE));
            commands_->copyTextureRegion(RenderTextureCopyLocation::Subresource(volume.get(),0),
                RenderTextureCopyLocation::Subresource(slices[z].get(),0),0,0,z,nullptr);
        }
        commands_->end();queue_.executeCommandLists(commands_.get(),fence_.get());queue_.waitForCommandFence(fence_.get());
        return volume;
    }
    auto desc=RenderTextureDesc::Texture2D(layout.width,layout.height,1,RenderFormat::R32G32B32A32_FLOAT);
    desc.flags|=RenderTextureFlag::STORAGE|RenderTextureFlag::UNORDERED_ACCESS;
    auto image=device_.createTexture(desc);
    Transfer(*image,layout,{0,0,int32_t(layout.width),int32_t(layout.height)},false,address,nullptr,0,0,presentation_timing);return image;
}
void ResolveMemory::SetQueuedVolumeLoadsEnabled(bool enabled) {
    if(queued_volume_load_configured_||shared_load_started_)
        throw std::logic_error("Queued volume loads must be configured once before shared loads");
    queued_volume_load_configured_=true;queued_volume_load_stats_.enabled=enabled;
}
bool ResolveMemory::CanQueueVolumeLoad(const TextureLayout& t,uint32_t address) const {
    if(!queued_volume_load_stats_.enabled||!batch_||!batch_->has_work_queue()||
       t.dimension!=1||t.layers<2||t.layers>kMaxQueuedVolumeLayers||
       !t.width||!t.height||t.width>8192||t.height>8192||t.depth!=1||
       t.min_mip||t.max_mip||t.levels.size()!=t.layers||t.endian>3||
       t.block.block_width!=1||t.block.block_height!=1||
       !std::has_single_bit(t.block.bytes)||t.block.bytes>16||address%4||address>=capacity_)
        return false;
    // Both the assembled volume and every exact RGBA32F decode scratch image
    // count against the original transfer budget, before any new GPU command.
    if(uint64_t(t.width)*t.height*t.layers*32>kMaxQueuedVolumeImageBytes)return false;
    uint32_t format=t.format;
    if(format>=27&&format<=29)format+=3;
    // 2_10_10_10_FLOAT_AS_16_16_16_16 stores raw 7e3 bits (64), as the
    // guest resolve does; titles also sample those bytes as UNorm 2_10_10_10.
    if(format==54)format=64;
    if(format!=2&&format!=6&&format!=7&&format!=22&&format!=23&&format!=24&&format!=25&&format!=26&&
       format!=30&&format!=31&&format!=32&&format!=36&&format!=37&&format!=38&&format!=64)return false;
    for(uint32_t z=0;z<t.layers;++z) {
        const auto& level=t.levels[z];
        if(level.mip||level.mips||level.layer!=z||level.width!=t.width||level.height!=t.height||
           level.depth!=1||level.z||!level.pitch_blocks||!level.slice_rows||
           level.offset%4||level.offset>=uint64_t(capacity_)-address)return false;
    }
    return true;
}
std::shared_ptr<RenderTexture> ResolveMemory::TryLoadQueuedVolume(const TextureLayout& layout,uint32_t address) {
    if(!batch_->active())return {};
    const RenderRect rectangle(0,0,layout.width,layout.height);
    Footprint(layout,rectangle,address);
    std::vector<TextureLayout> slice_layouts;slice_layouts.reserve(layout.layers);
    for(const auto& level:layout.levels) {
        auto slice=layout;slice.layers=1;slice.fetch[1]&=~1024u;slice.fetch[2]&=0x3ffffff;
        slice.levels={level};slice.levels[0].offset=0;slice.levels[0].layer=0;
        // All source bounds are proved before allocations/retention/recording.
        Footprint(slice,rectangle,address+uint32_t(level.offset));
        slice_layouts.push_back(std::move(slice));
    }
    std::shared_ptr<RenderTexture> volume=device_.createTexture(
        RenderTextureDesc::Texture3D(layout.width,layout.height,layout.layers,1,RenderFormat::R32G32B32A32_FLOAT));
    if(!volume)throw std::runtime_error("Cannot allocate native queued volume");
    auto desc=RenderTextureDesc::Texture2D(layout.width,layout.height,1,RenderFormat::R32G32B32A32_FLOAT);
    desc.flags|=RenderTextureFlag::STORAGE|RenderTextureFlag::UNORDERED_ACCESS;
    std::vector<std::shared_ptr<RenderTexture>> slices;slices.reserve(layout.layers);
    for(uint32_t z=0;z<layout.layers;++z) {
        std::shared_ptr<RenderTexture> image=device_.createTexture(desc);
        if(!image)throw std::runtime_error("Cannot allocate native queued volume slice");
        slices.push_back(std::move(image));
    }
    const uint64_t slice_bytes=uint64_t(layout.width)*layout.height*16;
    // KeepImage may fence an older prefix and choose synchronous execution.
    // Retain ALL outputs first, then test the whole-operation decision once.
    batch_->KeepImage(volume,slice_bytes*layout.layers);
    for(const auto& slice:slices)batch_->KeepImage(slice,slice_bytes);
    if(!batch_->active())return {};
    for(uint32_t z=0;z<layout.layers;++z) {
        // Do not recurse through LoadShared: compact R/RG images cannot be
        // copied as RGBA32F slices, and Load itself suspends/flushes batching.
        Transfer(*slices[z],slice_layouts[z],rectangle,false,address+uint32_t(layout.levels[z].offset));
        ++queued_volume_load_stats_.decoded_slices;
    }
    // BeginStage can seal at the existing64-stage bound. Every resulting packet
    // receives the scope's shared image owners and retires at its actual fence.
    auto& commands=batch_->BeginStage();
    commands.barriers(RenderBarrierStage::COPY,RenderTextureBarrier(volume.get(),RenderTextureLayout::COPY_DEST));
    for(uint32_t z=0;z<layout.layers;++z) {
        commands.barriers(RenderBarrierStage::COPY,RenderTextureBarrier(slices[z].get(),RenderTextureLayout::COPY_SOURCE));
        commands.copyTextureRegion(RenderTextureCopyLocation::Subresource(volume.get(),0),
            RenderTextureCopyLocation::Subresource(slices[z].get(),0),0,0,z,nullptr);
    }
    ++queued_volume_load_stats_.assemblies;++queued_volume_load_stats_.selected;
    return volume;
}
void ResolveMemory::SetImagePoolEnabled(bool enabled) {
    if(enabled) {if(!image_pool_)image_pool_=std::make_unique<ImagePool>();}
    else image_pool_.reset();
}
ResolveMemory::ImagePool::Statistics ResolveMemory::ImagePoolStatistics() const {
    return image_pool_?image_pool_->statistics():ImagePool::Statistics{};
}
void ResolveMemory::SetDescriptorPoolEnabled(bool enabled) {
    if(descriptor_pool_configured_||descriptor_dispatch_started_)
        throw std::logic_error("Resolve descriptor pool must be configured once before dispatch");
    auto prepared=enabled?std::make_unique<DescriptorPool>():nullptr;
    descriptor_pool_=std::move(prepared);descriptor_pool_configured_=true;
}
ResolveMemory::DescriptorPool::Statistics ResolveMemory::DescriptorPoolStatistics() const {
    auto stats=descriptor_pool_?descriptor_pool_->statistics():DescriptorPool::Statistics{};
    stats.synchronous_bypasses=descriptor_synchronous_bypasses_;return stats;
}
void ResolveMemory::SetIdentityViewReuseEnabled(bool enabled) {
    if(identity_view_reuse_configured_||descriptor_dispatch_started_)
        throw std::logic_error("Resolve identity view reuse must be configured once before dispatch");
    identity_view_reuse_stats_.enabled=enabled;identity_view_reuse_configured_=true;
}
void ResolveMemory::EnableCompactLoadFormats(std::span<const uint32_t> r32_shader,
                                            std::span<const uint32_t> rg32_shader,
                                            bool storage_image_extended_formats_enabled) {
    if(load_formats_configured_||shared_load_started_||mutation_serial_)
        throw std::logic_error("Compact resolve formats must be configured once before use");
    load_formats_configured_=true;
    const auto& device=static_cast<VulkanDevice&>(device_);
    const auto prepare=[&](unsigned index,VkFormat format,std::span<const uint32_t> code) {
        if(code.empty())return false;
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(device.physicalDevice,format,&properties);
        constexpr VkFormatFeatureFlags required=VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT|
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT|
            VK_FORMAT_FEATURE_TRANSFER_SRC_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        if((properties.optimalTilingFeatures&required)!=required)return false;
        if(code.size()<5||code.front()!=0x07230203)
            throw std::invalid_argument("Invalid compact resolve shader");
        auto shader=device_.createShader(code.data(),code.size_bytes(),"main",RenderShaderFormat::SPIRV);
        if(!shader||!static_cast<VulkanShader*>(shader.get())->vk)
            throw std::runtime_error("Compact resolve shader creation failed");
        auto pipeline=device_.createComputePipeline(RenderComputePipelineDesc(layouts_[1].get(),shader.get(),8,8,1));
        if(!pipeline||!static_cast<VulkanComputePipeline*>(pipeline.get())->vk)
            throw std::runtime_error("Compact resolve pipeline creation failed");
        shaders_[index]=std::move(shader);pipelines_[index]=std::move(pipeline);
        return true;
    };
    compact_load_stats_.r32_supported=prepare(2,VK_FORMAT_R32_SFLOAT,r32_shader);
    // rg32f declares StorageImageExtendedFormats. Physical format support does
    // not prove the borrowed host enabled that shader capability.
    compact_load_stats_.rg32_supported=storage_image_extended_formats_enabled&&
        prepare(3,VK_FORMAT_R32G32_SFLOAT,rg32_shader);
}
std::shared_ptr<RenderTexture> ResolveMemory::LoadShared(const TextureLayout& layout,uint32_t address) {
    shared_load_started_=true;
    if(layout.dimension==1&&layout.layers>1) {
        ++queued_volume_load_stats_.requests;
        if(CanQueueVolumeLoad(layout,address)) {
            if(auto image=TryLoadQueuedVolume(layout,address))return image;
            ++queued_volume_load_stats_.synchronous_preflight;
        } else ++queued_volume_load_stats_.refusals;
        // Original synchronous path is entered only before this volume has
        // recorded any work; accepted-prefix errors keep native failure rules.
        std::shared_ptr<RenderTexture> image=Load(layout,address);
        if(batch_)batch_->KeepImage(image,uint64_t(layout.width)*layout.height*layout.layers*16);
        return image;
    }
    const uint32_t channels=DecodedAliasChannels(layout.dimension,layout.layers,layout.block.components,
        {compact_load_stats_.r32_supported,compact_load_stats_.rg32_supported});
    const RenderFormat format=channels==1?RenderFormat::R32_FLOAT:
        channels==2?RenderFormat::R32G32_FLOAT:RenderFormat::R32G32B32A32_FLOAT;
    auto desc=RenderTextureDesc::Texture2D(layout.width,layout.height,1,format);
    desc.flags|=RenderTextureFlag::STORAGE|RenderTextureFlag::UNORDERED_ACCESS;
    std::shared_ptr<RenderTexture> image;
    if(image_pool_) {
        // The pool keys exact format and extent; flags, samples, layers and
        // mip count are otherwise identical. Preserve
        // tracked layout/barrier stages: the existing Transfer barriers order
        // a recycled image just like any other previously used image.
        auto storage=image_pool_->Take(layout.width,layout.height,uint32_t(format));
        if(!storage)storage=device_.createTexture(desc);
        if(!storage)throw std::runtime_error("Cannot allocate native resolve image");
        const uint64_t actual_bytes=static_cast<VulkanTexture&>(*storage).allocationInfo.size;
        image=image_pool_->Share(std::move(storage),layout.width,layout.height,actual_bytes,uint32_t(format));
    } else image=device_.createTexture(desc);
    if(batch_)batch_->KeepImage(image,uint64_t(layout.width)*layout.height*16);
    Transfer(*image,layout,{0,0,int32_t(layout.width),int32_t(layout.height)},false,address);
    if(channels==1)++compact_load_stats_.r32_loads;
    else if(channels==2)++compact_load_stats_.rg32_loads;
    else ++compact_load_stats_.rgba32_loads;
    const uint64_t pixels=uint64_t(layout.width)*layout.height;
    compact_load_stats_.logical_output_bytes+=pixels*channels*4;
    compact_load_stats_.logical_bytes_avoided+=pixels*(4-channels)*4;
    return image;
}
void ResolveMemory::Transfer(RenderTexture& image,const TextureLayout& t,const RenderRect& r,bool store,uint32_t address,
                              RenderTexture* stencil,int32_t source_x,int32_t source_y,
                              const NativeSynchronousGpuTimingContext* presentation_timing,bool rgba_stencil) {
    if(r.isEmpty()||r.left<0||r.top<0||r.right>int32_t(t.width)||r.bottom>int32_t(t.height)||t.dimension!=1||
       t.layers!=1||t.max_mip||t.min_mip||t.block.block_width!=1||t.levels.size()!=1)
        throw std::runtime_error("Unsupported resolved storage region/layout");
    uint32_t format=t.format;
    if(format>=27&&format<=29)format+=3;
    // 2_10_10_10_FLOAT_AS_16_16_16_16 stores raw 7e3 bits (64), as the
    // guest resolve does; titles also sample those bytes as UNorm 2_10_10_10.
    if(format==54)format=64;
    // 8_8 (10) only decodes: the game samples other resolves' bytes as 8_8 but
    // never resolves into it, so it has no encoder.
    if(format!=2&&format!=6&&format!=7&&(format!=10||store)&&format!=22&&format!=23&&format!=24&&format!=25&&
       format!=26&&format!=30&&format!=31&&format!=32&&format!=36&&format!=37&&format!=38&&format!=64)
        throw std::runtime_error("Unsupported resolved storage format "+std::to_string(t.format)+" ("+
            std::to_string(t.width)+"x"+std::to_string(t.height)+(t.tiled?" tiled":" linear")+", endian "+
            std::to_string(t.endian)+(store?", store":", load")+" at 0x"+[&]{char text[16];
            std::snprintf(text,sizeof text,"%08X",address);return std::string(text);}()+")");
    const auto& level=t.levels.front();
    // Validate all reachable addresses, including tiled swizzles at odd pitches.
    const auto& footprint=Footprint(t,r,address);
    const auto& desc=static_cast<VulkanTexture&>(image).desc;
    if(desc.width!=t.width||desc.height!=t.height)throw std::runtime_error("Resolve storage image extent mismatch");
    // A physical memory allocation may exceed maxStorageBufferRange (only
    // 128 MiB is guaranteed by Vulkan). Bind an aligned window containing this
    // dispatch, subdividing the rectangle if its tiled footprint is too wide.
    const uint32_t first=footprint.front().first&~(descriptor_alignment_-1);
    const uint32_t last=(footprint.back().second+3)&~3u;
    if(last-first>descriptor_range_) {
        auto low=r,high=r;
        if(r.bottom-r.top>1)low.bottom=high.top=r.top+(r.bottom-r.top)/2;
        else if(r.right-r.left>1)low.right=high.left=r.left+(r.right-r.left)/2;
        else throw std::runtime_error("Texture texel exceeds storage descriptor window");
        Transfer(image,t,low,store,address,stencil,source_x,source_y,presentation_timing,rgba_stencil);
        Transfer(image,t,high,store,address,stencil,source_x,source_y,presentation_timing,rgba_stencil);
        return;
    }
    const Parameters push{uint32_t(r.right-r.left),uint32_t(r.bottom-r.top),uint32_t(r.left),uint32_t(r.top),
        level.x,level.y,level.pitch_blocks,uint32_t(std::countr_zero(t.block.bytes)),t.tiled,t.endian,format|(stencil?0x80000000u:0),
        (t.fetch[0]>>2)&255,uint32_t(level.offset)+address,capacity_/4,source_x,source_y};
    Dispatch(image,push,store,stencil,first,last-first,presentation_timing,rgba_stencil);
    if(store)Define(footprint);
}
void ResolveMemory::Dispatch(RenderTexture& image,Parameters push,bool store,RenderTexture* stencil,
                             uint32_t first_byte,uint32_t byte_count,
                             const NativeSynchronousGpuTimingContext* presentation_timing,bool rgba_stencil) {
    if(!byte_count||first_byte%descriptor_alignment_||byte_count>descriptor_range_||uint64_t(first_byte)+byte_count>capacity_)
        throw std::runtime_error("Invalid resolve storage descriptor range");
    // Earlier deferred stores into these bytes run first, in order.
    MaterializeDeferredStores(first_byte,first_byte+byte_count);
    if(deferring_image_) {
        if(!store||stencil||rgba_stencil||&image!=deferring_image_.get())
            throw std::logic_error("Deferred resolve store accepts only its own plain store dispatches");
        pending_stores_.push_back({deferring_image_,push,first_byte,byte_count,deferring_footprint_});
        return;
    }
    // Unsigned arithmetic deliberately permits a base before the descriptor:
    // adding the validated texel offset produces the window-relative address.
    push.offset-=first_byte;push.words=byte_count/4;
    const auto& desc=static_cast<VulkanTexture&>(image).desc;
    const unsigned index=store?0:1;
    const unsigned pipeline_index=store?0:desc.format==RenderFormat::R32_FLOAT?2:
        desc.format==RenderFormat::R32G32_FLOAT?3:1;
    if(!pipelines_[pipeline_index])throw std::runtime_error("Unconfigured compact resolve storage format");
    auto* selected_pipeline=rgba_stencil?atlas_rgba_depth_store_pipeline_.get():pipelines_[pipeline_index].get();
    if(rgba_stencil&&(!selected_pipeline||!store||!stencil||(push.format&0x7fffffffu)!=22))
        throw std::runtime_error("Unconfigured or invalid atlas RGBA stencil store");
    descriptor_dispatch_started_=true;
    const bool batched=batch_&&batch_->active();
    auto ranges=DescriptorRanges(store);
    std::unique_ptr<RenderDescriptorSet> descriptors;
    std::shared_ptr<RenderDescriptorSet> descriptor_lease;
    if(descriptor_pool_&&batched&&batch_->has_work_queue()) {
        descriptor_lease=descriptor_pool_->Acquire(index,[&] {
            auto created=device_.createDescriptorSet(RenderDescriptorSetDesc(ranges.data(),ranges.size()));
            const auto* native=created?static_cast<const VulkanDescriptorSet*>(created.get()):nullptr;
            if(!native||!native->vk||!native->descriptorPool||!native->setLayout||!native->setLayout->vk)
                throw std::runtime_error("Resolve pooled descriptor allocation failed");
            return created;
        });
    } else {
        descriptors=device_.createDescriptorSet(RenderDescriptorSetDesc(ranges.data(),ranges.size()));
        if(descriptor_pool_)++descriptor_synchronous_bypasses_;
    }
    auto* descriptor_ptr=descriptor_lease?descriptor_lease.get():descriptors.get();
    // No binding contents are cached: previous resources may already be gone.
    // Every fixed-layout binding is overwritten while this lease is exclusive.
    const RenderBufferStructuredView window(4,first_byte/4);
    descriptor_ptr->setBuffer(0,bytes_.get(),byte_count,&window);
    bool reuse_identity_view=false;
    if(identity_view_reuse_stats_.enabled) {
        if(store)++identity_view_reuse_stats_.store_bypasses;
        else {
            ++identity_view_reuse_stats_.load_requests;
            reuse_identity_view=LoadIdentityViewEquivalent(static_cast<const VulkanTexture&>(image),
                static_cast<const VulkanDevice&>(device_));
            if(reuse_identity_view)++identity_view_reuse_stats_.load_reused;
            else {
                ++identity_view_reuse_stats_.load_explicit_views;
                ++identity_view_reuse_stats_.load_refusals;
            }
        }
    }
    // A null explicit view selects the existing owned imageView in Plume.
    std::unique_ptr<RenderTextureView> view;
    if(!reuse_identity_view)view=image.createTextureView(RenderTextureViewDesc::Texture2D(desc.format));
    descriptor_ptr->setTexture(1,&image,store?RenderTextureLayout::SHADER_READ:RenderTextureLayout::GENERAL,view.get());
    auto* stencil_binding=stencil?stencil:empty_stencil_.get();
    std::unique_ptr<RenderTextureView> stencil_view;
    if(store) {
        if(rgba_stencil) {
            stencil_view=stencil_binding->createTextureView(RenderTextureViewDesc::Texture2D(RenderFormat::R32G32B32A32_FLOAT));
            if(!stencil_view||!static_cast<const VulkanTextureView&>(*stencil_view).vk)
                throw std::runtime_error("Atlas RGBA stencil view allocation failed");
        }else stencil_view=StencilView(*stencil_binding);
    }
    if(store)descriptor_ptr->setTexture(2,stencil_binding,RenderTextureLayout::SHADER_READ,stencil_view.get());
    auto* commands=batched?&batch_->BeginStage():commands_.get();
    if(batched) {
        if(descriptor_lease)batch_->KeepLease(std::move(descriptor_lease));
        else batch_->Keep(std::move(descriptors));
        if(!reuse_identity_view)batch_->Keep(std::move(view));
        if(stencil_view)batch_->Keep(std::move(stencil_view));
    }
    if(store)++mutation_serial_;
    if(!batched){if(batch_)batch_->Flush();commands->begin();}
    NativeSynchronousGpuTimingScope presentation_scope(!batched&&!store?presentation_timing:nullptr,*commands,
        NativeSynchronousGpuPhase::FrontbufferDecode,push.width,push.height);
    commands->barriers(RenderBarrierStage::COMPUTE,RenderBufferBarrier(bytes_.get(),store?(RenderBufferAccess::READ|RenderBufferAccess::WRITE):RenderBufferAccess::READ));
    commands->barriers(RenderBarrierStage::COMPUTE,RenderTextureBarrier(&image,store?RenderTextureLayout::SHADER_READ:RenderTextureLayout::GENERAL));
    if(store)commands->barriers(RenderBarrierStage::COMPUTE,RenderTextureBarrier(stencil_binding,RenderTextureLayout::SHADER_READ));
    commands->setComputePipelineLayout(layouts_[index].get());commands->setComputePushConstants(0,&push);
    commands->setComputeDescriptorSet(descriptor_ptr,0);commands->setPipeline(selected_pipeline);
    commands->dispatch((push.width+7)/8,(push.height+7)/8,1);
    if(!batched){
        presentation_scope.End(*commands);commands->end();presentation_scope.BeforeSubmit();
        queue_.executeCommandLists(commands,fence_.get());presentation_scope.BeforeFenceWait();
        queue_.waitForCommandFence(fence_.get());presentation_scope.FenceCompleted();
    }
}
const ResolveMemory::Ranges& ResolveMemory::SurfaceFootprint(const SurfaceLayout& s) const {
    SurfaceStorageFormat(s);
    if(capacity_<NativeEdramBytes()||!s.width||!s.height||!s.pitch||s.pitch>16383||s.base>=NativeEdramTiles()||s.width>8192||s.height>8192)
        throw std::runtime_error("Invalid EDRAM backing/extent");
    // Layout is independent of contents and ownership. Raster writes update
    // page versions every draw, but cannot change these exact byte ranges.
    const std::array<uint32_t,6> key{s.width,s.height,s.pitch,s.base,s.format,uint32_t(s.depth)};
    if(const auto found=surface_footprints_.find(key);found!=surface_footprints_.end())return found->second;
    Ranges ranges;
    // Contiguous runs within a tile row; split depth's swapped half-tiles.
    const uint32_t width=Surface64(s)?40:s.depth?40:80,bytes=Surface64(s)?8:4;
    for(uint32_t y=0;y<s.height;++y)for(uint32_t x=0;x<s.width;x+=width) {
        const auto at=SurfaceByteOffset(s,x,y);
        ranges.emplace_back(at,at+std::min(width,s.width-x)*bytes);
    }
    std::sort(ranges.begin(),ranges.end());size_t count=0;
    for(const auto& r:ranges) {
        if(count&&r.first<=ranges[count-1].second)ranges[count-1].second=std::max(ranges[count-1].second,r.second);
        else ranges[count++]=r;
    }
    ranges.resize(count);
    ReleaseFootprintRangeCapacity(ranges);
    if(surface_footprints_.size()>=128)surface_footprints_.clear();
    return surface_footprints_.emplace(key,std::move(ranges)).first->second;
}
ResolveMemory::SurfacePageMask ResolveMemory::SurfacePages(const SurfaceLayout& s) const {
    return NativeSurfaceVersionPages(SurfaceFootprint(s));
}
uint64_t ResolveMemory::SurfaceVersion(const SurfaceLayout& s) const {
    static_assert(std::tuple_size_v<decltype(surface_version_memo_)> ==64);
    const auto mutations=page_versions_.mutations();
    auto& memo=surface_version_memo_[SurfaceVersionSlot(s)];
    if(memo.valid&&memo.mutations==mutations&&SurfaceVersionKeyEquals(memo.key,s))return memo.version;
    uint64_t version=0;
    const auto& footprint=SurfaceFootprint(s);
    for(const auto& [first,last]:footprint)
        version=std::max(version,page_versions_.Max(first/4096,(last-1)/4096+1));
    // Footprint is sorted, merged and nonempty (validated extent), so its
    // first start and last end bound every page Max visited above.
    memo={{s.width,s.height,s.pitch,s.base,s.format,uint32_t(s.depth)},mutations,version,
          footprint.front().first/4096,(footprint.back().second-1)/4096+1,true};
    return version;
}
bool ResolveMemory::SurfaceFullyDefined(const SurfaceLayout& s) const {
    for(const auto& [first,last]:SurfaceFootprint(s))if(!BytesDefined(first,last-first))return false;
    return true;
}
void ResolveMemory::RecordSurfaceWrite(const SurfaceLayout& s,PrimarySurfaceWriteReceipt* receipt) {
    if(receipt)*receipt={};
    // A draw may cover only part of the surface. Invalidate cached views without
    // claiming that previously unknown pixels were initialized by the draw.
    AdvancePhysicalSerial();
    const auto& footprint=SurfaceFootprint(s);
    const auto mutations_before=page_versions_.mutations();
    for(const auto& [first,last]:footprint)
        page_versions_.Assign(first/4096,(last-1)/4096+1,serial_);
    // Exact memo maintenance (reached only after every Assign succeeded).
    // The only page changes since mutations_before are the Assigns above, all
    // inside [write_first,write_end). An entry current at mutations_before whose
    // visited pages lie outside that hull has an unchanged Max. The written
    // layout's own pages now all hold serial_, so its Max is exactly serial_
    // (no monotonicity or wrap assumption). Every other entry stays stale.
    if(!footprint.empty()) {
        const auto mutations_after=page_versions_.mutations();
        const uint32_t write_first=footprint.front().first/4096,write_end=(footprint.back().second-1)/4096+1;
        for(auto& memo:surface_version_memo_)
            if(memo.valid&&memo.mutations==mutations_before&&
               (memo.page_end<=write_first||memo.page_first>=write_end))memo.mutations=mutations_after;
        surface_version_memo_[SurfaceVersionSlot(s)]={{s.width,s.height,s.pitch,s.base,s.format,uint32_t(s.depth)},
            mutations_after,serial_,write_first,write_end,true};
    }
    // This is the serial actually assigned above, never a guessed successor.
    // Validate the actual cached footprint; copy only its exact page span so
    // later footprint-cache eviction cannot invalidate the draw-local receipt.
    if(receipt) {
        const std::array<uint32_t,6> layout{s.width,s.height,s.pitch,s.base,s.format,uint32_t(s.depth)};
        if(const auto pages=NativePrimarySurfaceWritePages(layout,footprint,page_versions_.size()))
            receipt->stamp_={this,serial_,canonical_store_epoch_,pages->first,pages->second};
    }
}
std::optional<std::array<uint64_t,2>> ResolveMemory::PrimarySurfaceWriteVersions(
        const PrimarySurfaceWriteReceipt& color,const PrimarySurfaceWriteReceipt& depth)const noexcept {
    return NativePrimarySurfaceWriteVersions(color.stamp_,depth.stamp_,this,serial_,canonical_store_epoch_);
}
void ResolveMemory::RecordPackedSurfaceCommit(const SurfaceLayout& surface) {
    ++mutation_serial_;Define(SurfaceFootprint(surface));
}
void ResolveMemory::TransferSurface(RenderTexture& image,const SurfaceLayout& s,bool store) {
    const auto& footprint=SurfaceFootprint(s);
    const auto& desc=static_cast<VulkanTexture&>(image).desc;
    if(desc.width!=s.width||desc.height!=s.height)throw std::runtime_error("EDRAM image extent mismatch");
    const Parameters push{s.width,s.height,0,0,0,0,(s.pitch+79)/80,Surface64(s)?3u:2u,s.depth?3u:2u,0,
        SurfaceStorageFormat(s)|(s.depth&&store?0x80000000u:0),0,s.base*5120,(NativeEdramTiles()*1280),0,0};
    Dispatch(image,push,store,s.depth&&store?&image:nullptr,0,NativeEdramBytes());
    if(store)Define(footprint);
}
void ResolveMemory::EnableRawFloatCpuTextures(std::span<const uint32_t> code) {
    if(raw_float_cpu_stats_.configured||mutation_serial_)
        throw std::logic_error("Raw float CPU texture pipeline must be configured once before use");
    raw_float_cpu_stats_.configured=true;
    const auto& vk=static_cast<const VulkanDevice&>(device_);VkFormatProperties properties{};
    vkGetPhysicalDeviceFormatProperties(vk.physicalDevice,VK_FORMAT_R32G32B32A32_SFLOAT,&properties);
    constexpr VkFormatFeatureFlags required=VK_FORMAT_FEATURE_TRANSFER_DST_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    if((properties.optimalTilingFeatures&required)!=required)return;
    if(code.size()<5||code[0]!=0x07230203)throw std::invalid_argument("Invalid raw float CPU texture shader");
    const std::array<RenderDescriptorRange,2> ranges{{
        {RenderDescriptorRangeType::READ_WRITE_BYTE_ADDRESS_BUFFER,0,1},
        {RenderDescriptorRangeType::READ_WRITE_BYTE_ADDRESS_BUFFER,1,1}}};
    const RenderDescriptorSetDesc descriptor(ranges.data(),ranges.size());
    const RenderPushConstantRange push(0,0,0,16,RenderShaderStageFlag::COMPUTE);
    auto shader=device_.createShader(code.data(),code.size_bytes(),"main",RenderShaderFormat::SPIRV);
    auto layout=device_.createPipelineLayout(RenderPipelineLayoutDesc(&push,1,&descriptor,1));
    if(!shader||!static_cast<VulkanShader*>(shader.get())->vk||!layout)
        throw std::runtime_error("Raw float CPU texture shader/layout creation failed");
    auto pipeline=device_.createComputePipeline(RenderComputePipelineDesc(layout.get(),shader.get(),64,1,1));
    if(!pipeline||!static_cast<VulkanComputePipeline*>(pipeline.get())->vk)
        throw std::runtime_error("Raw float CPU texture pipeline creation failed");
    raw_float_cpu_shader_=std::move(shader);raw_float_cpu_layout_=std::move(layout);
    raw_float_cpu_pipeline_=std::move(pipeline);raw_float_cpu_stats_.supported=true;
}
bool ResolveMemory::CanLoadRawFloatCpuTexture(const TextureLayout& t,size_t cpu_bytes) const {
    if(!raw_float_cpu_pipeline_||!RawFloatCpuTextureLayout(t)||!RawFloatCpuTexturePayload(cpu_bytes))return false;
    // The admitted layout's sampled footprint is exactly this contiguous run,
    // excluding 31 unused slice rows. No allocation-wide ownership shortcut.
    if(!BytesDefined(t.base_address,kRawFloatCpuTextureBytes))return false;
    const uint32_t first=t.base_address&~(descriptor_alignment_-1);
    const uint64_t last=uint64_t(t.base_address)+kRawFloatCpuTextureBytes;
    return last<=capacity_&&last-first<=descriptor_range_;
}
void ResolveMemory::LoadRawFloatCpuTexture(const std::shared_ptr<RenderTexture>& image,const TextureLayout& t) {
    if(!pending_stores_.empty())for(const auto& footprint:CpuTextureFootprints(t))MaterializeDeferredStores(footprint);
    if(!image||!CanLoadRawFloatCpuTexture(t,kRawFloatCpuTextureBytes))
        throw std::runtime_error("Raw float CPU texture source lacks exact owned layout");
    const auto& target=static_cast<const VulkanTexture&>(*image);const auto& d=target.desc;
    if(target.device!=static_cast<const VulkanDevice*>(&device_)||!target.vk||
        d.dimension!=RenderTextureDimension::TEXTURE_2D||d.width!=4096||d.height!=1||d.depth!=1||
        d.mipLevels!=1||d.arraySize!=1||d.format!=RenderFormat::R32G32B32A32_FLOAT||d.multisampling.sampleCount!=1)
        throw std::runtime_error("Raw float CPU texture destination is incompatible");
    // Each result is a fresh immutable generation, including when later
    // physical writes advance the original CPU texture's version.
    if(image.use_count()!=1||target.textureLayout!=RenderTextureLayout::UNKNOWN||target.barrierStages!=RenderBarrierStage::NONE)
        throw std::runtime_error("Raw float CPU texture destination is not a fresh private generation");
    auto scratch=device_.createBuffer(RenderBufferDesc::DefaultBuffer(kRawFloatCpuTextureBytes,
        RenderBufferFlag::STORAGE|RenderBufferFlag::UNORDERED_ACCESS));
    if(!scratch||!static_cast<VulkanBuffer*>(scratch.get())->vk||!static_cast<VulkanBuffer*>(scratch.get())->allocation)
        throw std::runtime_error("Raw float CPU texture scratch allocation failed");
    const auto scratch_bytes=static_cast<const VulkanBuffer&>(*scratch).allocationInfo.size;
    const auto image_bytes=target.allocationInfo.size;
    if(scratch_bytes<kRawFloatCpuTextureBytes||!image_bytes)
        throw std::runtime_error("Raw float CPU texture allocation extent is invalid");
    const uint32_t first=t.base_address&~(descriptor_alignment_-1);
    const uint32_t window_bytes=t.base_address+kRawFloatCpuTextureBytes-first;
    const std::array<RenderDescriptorRange,2> ranges{{
        {RenderDescriptorRangeType::READ_WRITE_BYTE_ADDRESS_BUFFER,0,1},
        {RenderDescriptorRangeType::READ_WRITE_BYTE_ADDRESS_BUFFER,1,1}}};
    auto descriptors=device_.createDescriptorSet(RenderDescriptorSetDesc(ranges.data(),ranges.size()));
    if(!descriptors)throw std::runtime_error("Raw float CPU texture descriptors failed");
    const RenderBufferStructuredView window(4,first/4);
    descriptors->setBuffer(0,bytes_.get(),window_bytes,&window);
    descriptors->setBuffer(1,scratch.get(),kRawFloatCpuTextureBytes);
    bool batched=batch_&&batch_->active();
    if(batched) {
        batch_->KeepImage(image,image_bytes);
        batched=batch_->active()&&batch_->ReserveUploadBytes(scratch_bytes);
    }
    auto* commands=batched?&batch_->BeginStage():commands_.get();
    auto* scratch_ptr=scratch.get();auto* descriptor_ptr=descriptors.get();
    if(batched) {batch_->Keep(std::move(scratch));batch_->Keep(std::move(descriptors));}
    else {if(batch_)batch_->Flush();commands->begin();}
    const std::array<uint32_t,4> push{(t.base_address-first)/4,kRawFloatCpuTextureBytes/4,0,0};
    commands->barriers(RenderBarrierStage::COMPUTE,RenderBufferBarrier(bytes_.get(),RenderBufferAccess::READ));
    commands->barriers(RenderBarrierStage::COMPUTE,RenderBufferBarrier(scratch_ptr,RenderBufferAccess::WRITE));
    commands->setComputePipelineLayout(raw_float_cpu_layout_.get());commands->setComputePushConstants(0,push.data());
    commands->setComputeDescriptorSet(descriptor_ptr,0);commands->setPipeline(raw_float_cpu_pipeline_.get());
    commands->dispatch((push[1]+63)/64,1,1);
    commands->barriers(RenderBarrierStage::COPY,RenderBufferBarrier(scratch_ptr,RenderBufferAccess::READ));
    commands->barriers(RenderBarrierStage::COPY,RenderTextureBarrier(image.get(),RenderTextureLayout::COPY_DEST));
    commands->copyTextureRegion(RenderTextureCopyLocation::Subresource(image.get(),0),
        RenderTextureCopyLocation::PlacedFootprint(scratch_ptr,RenderFormat::R32G32B32A32_FLOAT,4096,1,1,4096,0));
    commands->barriers(RenderBarrierStage::GRAPHICS,RenderTextureBarrier(image.get(),RenderTextureLayout::SHADER_READ));
    if(!batched) {commands->end();queue_.executeCommandLists(commands,fence_.get());queue_.waitForCommandFence(fence_.get());}
    ++raw_float_cpu_stats_.copies;raw_float_cpu_stats_.queued+=batched;raw_float_cpu_stats_.synchronous+=!batched;
    raw_float_cpu_stats_.logical_bytes+=kRawFloatCpuTextureBytes;raw_float_cpu_stats_.scratch_allocation_bytes+=scratch_bytes;
}
void ResolveMemory::StoreSurface(RenderTexture& image,const SurfaceLayout& s) {TransferSurface(image,s,true);}
void ResolveMemory::EnableDirectColorRestore(std::span<const uint32_t> vertex,std::span<const uint32_t> pixel) {
    if(restore_layout_)throw std::logic_error("Direct color restore must be configured once");
    const RenderDescriptorRange range(RenderDescriptorRangeType::READ_WRITE_BYTE_ADDRESS_BUFFER,0,1);
    const RenderDescriptorSetDesc set(&range,1);
    const RenderPushConstantRange push(0,0,0,sizeof(Parameters),RenderShaderStageFlag::PIXEL);
    auto vs=device_.createShader(vertex.data(),vertex.size_bytes(),"main",RenderShaderFormat::SPIRV);
    auto ps=device_.createShader(pixel.data(),pixel.size_bytes(),"main",RenderShaderFormat::SPIRV);
    auto layout=device_.createPipelineLayout(RenderPipelineLayoutDesc(&push,1,&set,1));
    if(!vs||!ps||!layout||!static_cast<VulkanShader&>(*vs).vk||!static_cast<VulkanShader&>(*ps).vk)
        throw std::runtime_error("Direct color restore shader/layout creation failed");
    restore_vertex_=std::move(vs);restore_pixel_=std::move(ps);restore_layout_=std::move(layout);
}
bool ResolveMemory::RestoreColorSurface(RenderTexture& target,const SurfaceLayout& s) {
    if(!restore_layout_||s.depth||!batch_||!batch_->active())return false;
    (void)SurfaceFootprint(s);
    const auto& desc=static_cast<VulkanTexture&>(target).desc;
    if(desc.width!=s.width||desc.height!=s.height)throw std::runtime_error("EDRAM image extent mismatch");
    auto& pipeline=restore_pipelines_[desc.format];
    if(!pipeline) {
        RenderGraphicsPipelineDesc p;
        p.pipelineLayout=restore_layout_.get();p.vertexShader=restore_vertex_.get();p.pixelShader=restore_pixel_.get();
        p.renderTargetCount=1;p.renderTargetFormat[0]=desc.format;p.renderTargetBlend[0]=RenderBlendDesc::Copy();
        p.depthClipEnabled=true;
        auto created=device_.createGraphicsPipeline(p);
        if(!created||!static_cast<VulkanGraphicsPipeline*>(created.get())->vk) {
            restore_pipelines_.erase(desc.format);
            throw std::runtime_error("Direct color restore pipeline creation failed");
        }
        pipeline=std::move(created);
    }
    // Identical to TransferSurface's load parameters over the whole EDRAM window.
    const uint32_t bytes=NativeEdramBytes();
    const Parameters push{s.width,s.height,0,0,0,0,(s.pitch+79)/80,Surface64(s)?3u:2u,2u,0,
        SurfaceStorageFormat(s),0,s.base*5120,bytes/4,0,0};
    if(!restore_descriptors_) {
        const RenderDescriptorRange range(RenderDescriptorRangeType::READ_WRITE_BYTE_ADDRESS_BUFFER,0,1);
        auto created=device_.createDescriptorSet(RenderDescriptorSetDesc(&range,1));
        if(!created)throw std::runtime_error("Direct color restore descriptor allocation failed");
        const RenderBufferStructuredView window(4,0);
        created->setBuffer(0,bytes_.get(),bytes,&window);
        restore_descriptors_=std::move(created);
    }
    const RenderTexture* attachment=&target;
    RenderFramebufferDesc fd;fd.colorAttachments=&attachment;fd.colorAttachmentsCount=1;
    auto framebuffer=device_.createFramebuffer(fd);
    if(!framebuffer)throw std::runtime_error("Direct color restore framebuffer creation failed");
    auto* commands=&batch_->BeginStage();
    auto* descriptor_ptr=restore_descriptors_.get();auto* framebuffer_ptr=framebuffer.get();
    batch_->Keep(std::move(framebuffer));
    commands->barriers(RenderBarrierStage::GRAPHICS,RenderBufferBarrier(bytes_.get(),RenderBufferAccess::READ));
    commands->barriers(RenderBarrierStage::GRAPHICS,RenderTextureBarrier(&target,RenderTextureLayout::COLOR_WRITE));
    commands->setFramebuffer(framebuffer_ptr);
    commands->setViewports(RenderViewport(0,0,float(s.width),float(s.height)));
    commands->setScissors(RenderRect(0,0,int32_t(s.width),int32_t(s.height)));
    commands->setGraphicsPipelineLayout(restore_layout_.get());commands->setGraphicsPushConstants(0,&push);
    commands->setGraphicsDescriptorSet(descriptor_ptr,0);commands->setPipeline(pipeline.get());
    commands->drawInstanced(3,1,0,0);
    commands->barriers(RenderBarrierStage::GRAPHICS,RenderTextureBarrier(&target,RenderTextureLayout::SHADER_READ));
    ++direct_color_restores_;return true;
}
bool ResolveMemory::EnablePackedStencilClear(std::span<const uint32_t> code) {
    if(packed_stencil_clear_configured_||mutation_serial_)
        throw std::logic_error("Packed stencil clear pipeline must be configured once before use");
    packed_stencil_clear_configured_=true;
    const uint32_t bytes=NativeEdramBytes();
    if(code.size()<5||code[0]!=0x07230203||capacity_<bytes||descriptor_range_<bytes)return false;
    try {
        const RenderDescriptorRange range(RenderDescriptorRangeType::READ_WRITE_BYTE_ADDRESS_BUFFER,0,1);
        const RenderDescriptorSetDesc set(&range,1);
        const RenderPushConstantRange push(0,0,0,32,RenderShaderStageFlag::COMPUTE);
        auto shader=device_.createShader(code.data(),code.size_bytes(),"main",RenderShaderFormat::SPIRV);
        auto layout=device_.createPipelineLayout(RenderPipelineLayoutDesc(&push,1,&set,1));
        if(!shader||!layout||!static_cast<const VulkanShader&>(*shader).vk||
           !static_cast<const VulkanPipelineLayout&>(*layout).vk)
            throw std::runtime_error("Packed stencil clear shader/layout allocation failed");
        auto pipeline=device_.createComputePipeline(RenderComputePipelineDesc(layout.get(),shader.get(),8,8,1));
        auto descriptor=device_.createDescriptorSet(set);
        if(!pipeline||!static_cast<const VulkanComputePipeline&>(*pipeline).vk||!descriptor||
           !static_cast<const VulkanDescriptorSet&>(*descriptor).vk)
            throw std::runtime_error("Packed stencil clear pipeline/descriptor allocation failed");
        descriptor->setBuffer(0,bytes_.get(),bytes);
        packed_stencil_clear_shader_=std::move(shader);packed_stencil_clear_layout_=std::move(layout);
        packed_stencil_clear_pipeline_=std::move(pipeline);packed_stencil_clear_descriptors_=std::move(descriptor);
        return true;
    } catch(...) {return false;} // No commands or packed writes were recorded.
}
bool ResolveMemory::ClearPrimaryStencilFromPacked(const SurfaceLayout& s,const RenderRect& r,uint32_t stencil) {
    const std::array<int32_t,4> rectangle{r.left,r.top,r.right,r.bottom};
    if(!SupportsPackedStencilClear()||!packed_stencil_clear_descriptors_||
       !NativePackedPrimaryStencilClearLayoutAllowed(s.width,s.height,s.pitch,s.base,s.format,s.depth,rectangle)||
       !SurfaceFullyDefined(s))return false;
    const auto& footprint=SurfaceFootprint(s);
    for(const auto& [first,last]:footprint)
        if(first>=last||last>NativeEdramBytes()||(first&3)||(last&3))
            throw std::runtime_error("Packed stencil clear full footprint is invalid");
    const auto push=NativePackedPrimaryStencilClearParameters(rectangle,stencil);
    const bool queued=batch_&&batch_->active();
    auto* commands=queued?&batch_->BeginStage():commands_.get();
    if(!queued){if(batch_)batch_->Flush();commands->begin();}
    // Immutable descriptor/pipeline and stable bytes_ remain owned by this
    // ResolveMemory through its caller's original full covering fence.
    descriptor_dispatch_started_=true;++mutation_serial_;
    commands->barriers(RenderBarrierStage::COMPUTE,
        RenderBufferBarrier(bytes_.get(),RenderBufferAccess::READ|RenderBufferAccess::WRITE));
    commands->setComputePipelineLayout(packed_stencil_clear_layout_.get());
    commands->setComputePushConstants(0,push.data());
    commands->setComputeDescriptorSet(packed_stencil_clear_descriptors_.get(),0);
    commands->setPipeline(packed_stencil_clear_pipeline_.get());
    commands->dispatch(160,90,1);
    if(!queued){commands->end();queue_.executeCommandLists(commands,fence_.get());queue_.waitForCommandFence(fence_.get());}
    Define(footprint);return true;
}
bool ResolveMemory::EnablePackedSecondaryStencilClear(std::span<const uint32_t> code) {
    if(packed_secondary_stencil_clear_configured_||mutation_serial_)
        throw std::logic_error("Packed secondary stencil clear pipeline must be configured once before use");
    packed_secondary_stencil_clear_configured_=true;
    const uint32_t bytes=NativeEdramBytes();
    if(code.size()<5||code[0]!=0x07230203||capacity_<bytes||descriptor_range_<bytes)return false;
    try {
        const RenderDescriptorRange range(RenderDescriptorRangeType::READ_WRITE_BYTE_ADDRESS_BUFFER,0,1);
        const RenderDescriptorSetDesc set(&range,1);
        const RenderPushConstantRange push(0,0,0,32,RenderShaderStageFlag::COMPUTE);
        auto shader=device_.createShader(code.data(),code.size_bytes(),"main",RenderShaderFormat::SPIRV);
        auto layout=device_.createPipelineLayout(RenderPipelineLayoutDesc(&push,1,&set,1));
        if(!shader||!layout||!static_cast<const VulkanShader&>(*shader).vk||
           !static_cast<const VulkanPipelineLayout&>(*layout).vk)
            throw std::runtime_error("Packed secondary stencil clear shader/layout allocation failed");
        auto pipeline=device_.createComputePipeline(RenderComputePipelineDesc(layout.get(),shader.get(),8,8,1));
        auto descriptor=device_.createDescriptorSet(set);
        if(!pipeline||!static_cast<const VulkanComputePipeline&>(*pipeline).vk||!descriptor||
           !static_cast<const VulkanDescriptorSet&>(*descriptor).vk)
            throw std::runtime_error("Packed secondary stencil clear pipeline/descriptor allocation failed");
        descriptor->setBuffer(0,bytes_.get(),bytes);
        packed_secondary_stencil_clear_shader_=std::move(shader);packed_secondary_stencil_clear_layout_=std::move(layout);
        packed_secondary_stencil_clear_pipeline_=std::move(pipeline);packed_secondary_stencil_clear_descriptors_=std::move(descriptor);
        return true;
    } catch(...) {return false;} // No commands or packed writes were recorded.
}
bool ResolveMemory::ClearSecondaryStencilFromPacked(const SurfaceLayout& s,const RenderRect& r,uint32_t stencil) {
    const std::array<int32_t,4> rectangle{r.left,r.top,r.right,r.bottom};
    if(!SupportsPackedSecondaryStencilClear()||!packed_secondary_stencil_clear_descriptors_||
       !NativePackedSecondaryStencilClearLayoutAllowed(s.width,s.height,s.pitch,s.base,s.format,s.depth,rectangle)||
       !SurfaceFullyDefined(s))return false;
    const auto& footprint=SurfaceFootprint(s);
    for(const auto& [first,last]:footprint)
        if(first>=last||last>NativeEdramBytes()||(first&3)||(last&3))
            throw std::runtime_error("Packed secondary stencil clear full footprint is invalid");
    const auto push=NativePackedSecondaryStencilClearParameters(rectangle,stencil);
    const bool queued=batch_&&batch_->active();
    auto* commands=queued?&batch_->BeginStage():commands_.get();
    if(!queued){if(batch_)batch_->Flush();commands->begin();}
    // Immutable descriptor/pipeline and stable bytes_ remain owned by this
    // ResolveMemory through its caller's original full covering fence.
    descriptor_dispatch_started_=true;++mutation_serial_;
    commands->barriers(RenderBarrierStage::COMPUTE,
        RenderBufferBarrier(bytes_.get(),RenderBufferAccess::READ|RenderBufferAccess::WRITE));
    commands->setComputePipelineLayout(packed_secondary_stencil_clear_layout_.get());
    commands->setComputePushConstants(0,push.data());
    commands->setComputeDescriptorSet(packed_secondary_stencil_clear_descriptors_.get(),0);
    commands->setPipeline(packed_secondary_stencil_clear_pipeline_.get());
    commands->dispatch(80,45,1);
    if(!queued){commands->end();queue_.executeCommandLists(commands,fence_.get());queue_.waitForCommandFence(fence_.get());}
    Define(footprint);return true;
}
bool ResolveMemory::EnableAtlasDepthClear(std::span<const uint32_t> packed_code,std::span<const uint32_t> host_code) {
    if(atlas_depth_clear_configured_||mutation_serial_)
        throw std::logic_error("Atlas clear pipelines must be configured once before use");
    atlas_depth_clear_configured_=true;
    if(packed_code.empty()||host_code.empty()||capacity_<NativeEdramBytes()||descriptor_range_<NativeEdramBytes())return false;
    try {
        std::array<std::unique_ptr<RenderShader>,2> shaders;
        std::array<std::unique_ptr<RenderPipelineLayout>,2> layouts;
        std::array<std::unique_ptr<RenderPipeline>,2> pipelines;
        const RenderPushConstantRange push(0,0,0,32,RenderShaderStageFlag::COMPUTE);
        for(unsigned i=0;i<2;++i) {
            auto code=i?host_code:packed_code;
            shaders[i]=device_.createShader(code.data(),code.size_bytes(),"main",RenderShaderFormat::SPIRV);
            auto ranges=DescriptorRanges(false);
            if(i) {
                ranges.emplace_back(RenderDescriptorRangeType::TEXTURE,2,1);
                ranges.emplace_back(RenderDescriptorRangeType::TEXTURE,3,1);
            }
            const RenderDescriptorSetDesc set(ranges.data(),ranges.size());
            layouts[i]=device_.createPipelineLayout(RenderPipelineLayoutDesc(&push,1,&set,1));
            if(!shaders[i]||!layouts[i]||!static_cast<const VulkanShader&>(*shaders[i]).vk||
               !static_cast<const VulkanPipelineLayout&>(*layouts[i]).vk)
                throw std::runtime_error("Atlas clear shader/layout allocation failed");
            pipelines[i]=device_.createComputePipeline(RenderComputePipelineDesc(layouts[i].get(),shaders[i].get(),8,8,1));
            if(!pipelines[i]||!static_cast<VulkanComputePipeline*>(pipelines[i].get())->vk)
                throw std::runtime_error("Atlas clear compute pipeline allocation failed");
        }
        // Four-binding host descriptors are a distinct family from the original
        // store/load layouts. Cache only actual fence-returned backings, max16.
        std::unique_ptr<DescriptorPool> host_pool;
        if(descriptor_pool_)host_pool=std::make_unique<DescriptorPool>(16);
        atlas_depth_clear_shaders_=std::move(shaders);atlas_depth_clear_layouts_=std::move(layouts);
        atlas_depth_clear_pipelines_=std::move(pipelines);atlas_depth_host_descriptor_pool_=std::move(host_pool);
        return true;
    } catch(...) {return false;} // No commands or packed writes have been recorded.
}
ResolveMemory::DescriptorPool::Statistics ResolveMemory::AtlasDepthClearHostDescriptorStatistics() const {
    if(!atlas_depth_host_descriptor_pool_)return {};
    auto result=atlas_depth_host_descriptor_pool_->statistics();
    result.synchronous_bypasses=atlas_depth_host_synchronous_bypasses_;return result;
}
bool ResolveMemory::ClearAtlasDepthOneFromPacked(const SurfaceLayout& s,const RenderRect& r,
                                                AtlasDepthClearReceipt& receipt,RenderTexture* host_source) {
    const std::array<int32_t,4> rectangle{r.left,r.top,r.right,r.bottom};
    const unsigned index=host_source?1:0;
    if(!atlas_depth_clear_pipelines_[index]||receipt.source||receipt.packed_version||
       !NativeAtlasDepthClearLayoutAllowed(s.width,s.height,s.pitch,s.base,s.format,s.depth,rectangle)||
       capacity_<NativeEdramBytes()||descriptor_range_<NativeEdramBytes()||!SurfaceFullyDefined(s))return false;
    if(host_source) {
        const auto& native=static_cast<const VulkanTexture&>(*host_source);const auto& d=native.desc;
        if(native.device!=static_cast<const VulkanDevice*>(&device_)||!native.vk||!native.ownership||
           d.dimension!=RenderTextureDimension::TEXTURE_2D||d.width!=s.width||d.height!=s.height||
           d.depth!=1||d.mipLevels!=1||d.arraySize!=1||d.format!=RenderFormat::D32_FLOAT_S8_UINT||
           d.multisampling.sampleCount!=RenderSampleCount::COUNT_1||d.multisampling.sampleLocationsEnabled)return false;
    }
    const auto push=NativeAtlasDepthClearParameters(s.width,s.height,s.pitch,s.base,s.format,s.depth,rectangle);
    const auto& footprint=SurfaceFootprint(s);
    std::shared_ptr<RenderTexture> image;
    std::unique_ptr<RenderDescriptorSet> descriptors;
    std::shared_ptr<RenderDescriptorSet> descriptor_lease;
    std::unique_ptr<RenderTextureView> view,host_view;
    std::unique_ptr<VulkanTextureView> host_stencil_view;
    uint64_t image_bytes=0;
    const bool batched=batch_&&batch_->active();
    auto* pool=host_source?atlas_depth_host_descriptor_pool_.get():descriptor_pool_.get();
    try {
        auto desc=RenderTextureDesc::Texture2D(s.width,s.height,1,RenderFormat::R32G32B32A32_FLOAT);
        desc.flags|=RenderTextureFlag::STORAGE|RenderTextureFlag::UNORDERED_ACCESS;
        image=device_.createTexture(desc);
        if(!image)throw std::runtime_error("Atlas clear receipt allocation failed");
        const auto& native_image=static_cast<const VulkanTexture&>(*image);
        image_bytes=native_image.allocationInfo.size;
        if(native_image.device!=static_cast<const VulkanDevice*>(&device_)||!native_image.vk||
           !native_image.ownership||!native_image.allocation||!image_bytes||
           image_bytes>kNativeAtlasDepthClearMaxReceiptBytes)
            throw std::runtime_error("Atlas clear receipt allocation failed or exceeded its bound");
        auto ranges=DescriptorRanges(false);
        if(host_source) {
            ranges.emplace_back(RenderDescriptorRangeType::TEXTURE,2,1);
            ranges.emplace_back(RenderDescriptorRangeType::TEXTURE,3,1);
        }
        const auto factory=[&] {
            auto created=device_.createDescriptorSet(RenderDescriptorSetDesc(ranges.data(),ranges.size()));
            const auto* native=created?static_cast<const VulkanDescriptorSet*>(created.get()):nullptr;
            if(!native||!native->vk||!native->descriptorPool||!native->setLayout||!native->setLayout->vk)
                throw std::runtime_error("Atlas clear descriptor allocation failed");
            return created;
        };
        if(pool&&batched&&batch_->has_work_queue())descriptor_lease=pool->Acquire(host_source?0:1,factory);
        else {
            descriptors=factory();
            if(host_source&&pool)++atlas_depth_host_synchronous_bypasses_;
        }
        auto* set=descriptor_lease?descriptor_lease.get():descriptors.get();
        view=image->createTextureView(RenderTextureViewDesc::Texture2D(RenderFormat::R32G32B32A32_FLOAT));
        if(!view||!static_cast<const VulkanTextureView&>(*view).vk)
            throw std::runtime_error("Atlas clear receipt view allocation failed");
        set->setBuffer(0,bytes_.get(),NativeEdramBytes());
        set->setTexture(1,image.get(),RenderTextureLayout::GENERAL,view.get());
        if(host_source) {
            host_view=host_source->createTextureView(RenderTextureViewDesc::Texture2D(RenderFormat::D32_FLOAT_S8_UINT));
            host_stencil_view=StencilView(*host_source);
            if(!host_view||!host_stencil_view||!static_cast<const VulkanTextureView&>(*host_view).vk||!host_stencil_view->vk)
                throw std::runtime_error("Atlas clear source views failed");
            set->setTexture(2,host_source,RenderTextureLayout::SHADER_READ,host_view.get());
            set->setTexture(3,host_source,RenderTextureLayout::SHADER_READ,host_stencil_view.get());
        }
    } catch(...) {return false;} // Only resource preparation; canonical fallback remains untouched.
    // Pressure may fence an older packet. Any completion/recording failure from
    // this point propagates, never falls through after a potentially accepted write.
    if(batched)batch_->KeepImage(image,image_bytes);
    const bool queued=batch_&&batch_->active();
    auto* commands=queued?&batch_->BeginStage():commands_.get();
    auto* set=descriptor_lease?descriptor_lease.get():descriptors.get();
    if(queued) {
        if(descriptor_lease)batch_->KeepLease(std::move(descriptor_lease));
        else batch_->Keep(std::move(descriptors));
        batch_->Keep(std::move(view));
        if(host_view)batch_->Keep(std::move(host_view));
        if(host_stencil_view)batch_->Keep(std::move(host_stencil_view));
    } else {if(batch_)batch_->Flush();commands->begin();}
    descriptor_dispatch_started_=true;++mutation_serial_;
    commands->barriers(RenderBarrierStage::COMPUTE,
        RenderBufferBarrier(bytes_.get(),RenderBufferAccess::READ|RenderBufferAccess::WRITE));
    commands->barriers(RenderBarrierStage::COMPUTE,RenderTextureBarrier(image.get(),RenderTextureLayout::GENERAL));
    if(host_source)commands->barriers(RenderBarrierStage::COMPUTE,RenderTextureBarrier(host_source,RenderTextureLayout::SHADER_READ));
    commands->setComputePipelineLayout(atlas_depth_clear_layouts_[index].get());commands->setComputePushConstants(0,push.data());
    commands->setComputeDescriptorSet(set,0);commands->setPipeline(atlas_depth_clear_pipelines_[index].get());
    commands->dispatch((s.width+7)/8,(s.height+7)/8,1);
    if(!queued){commands->end();queue_.executeCommandLists(commands,fence_.get());queue_.waitForCommandFence(fence_.get());}
    Define(footprint);
    receipt.source=std::move(image);receipt.packed_version=SurfaceVersion(s);return true;
}
void ResolveMemory::ClearPackedDepthZero(const SurfaceLayout& s) {
    if(!PackedZeroDepthLayoutAllowed(s.width,s.height,s.pitch,s.base,s.format,s.depth))
        throw std::invalid_argument("Unsupported packed depth zero-clear layout");
    const auto& footprint=SurfaceFootprint(s);
    if(footprint.empty())throw std::runtime_error("Empty packed depth zero-clear footprint");
    for(const auto& [first,last]:footprint)
        if(first>=last||last>capacity_||(first&3)||(last&3))
            throw std::runtime_error("Invalid packed depth zero-clear range");
    const bool batched=batch_&&batch_->active();
    auto* commands=batched?&batch_->BeginStage():commands_.get();
    if(!batched){if(batch_)batch_->Flush();commands->begin();}
    ++mutation_serial_;
    // Uniform zero is the exact packed result of depth=0, stencil=0 in format1.
    // The union contains complete dwords and retains bytes outside the surface.
    // Keep the COPY tracking so later FSI/resolve work takes its full dependency.
    commands->barriers(RenderBarrierStage::COPY,
        RenderBufferBarrier(bytes_.get(),RenderBufferAccess::WRITE));
    const auto command=static_cast<VulkanCommandList&>(*commands).vk;
    const auto buffer=static_cast<VulkanBuffer&>(*bytes_).vk;
    for(const auto& [first,last]:footprint)vkCmdFillBuffer(command,buffer,first,last-first,0);
    if(!batched){commands->end();queue_.executeCommandLists(commands,fence_.get());queue_.waitForCommandFence(fence_.get());}
    Define(footprint);
}

std::unique_ptr<RenderTexture> ResolveMemory::LoadSurface(const SurfaceLayout& s) {
    NativeTransferBatch::Suspension synchronous(batch_);
    auto desc=RenderTextureDesc::Texture2D(s.width,s.height,1,RenderFormat::R32G32B32A32_FLOAT);
    desc.flags|=RenderTextureFlag::STORAGE|RenderTextureFlag::UNORDERED_ACCESS;
    auto image=device_.createTexture(desc);TransferSurface(*image,s,false);return image;
}
std::shared_ptr<RenderTexture> ResolveMemory::LoadSurfaceShared(const SurfaceLayout& s) {
    auto desc=RenderTextureDesc::Texture2D(s.width,s.height,1,RenderFormat::R32G32B32A32_FLOAT);
    desc.flags|=RenderTextureFlag::STORAGE|RenderTextureFlag::UNORDERED_ACCESS;
    std::shared_ptr<RenderTexture> image=device_.createTexture(desc);
    if(batch_)batch_->KeepImage(image,uint64_t(s.width)*s.height*16);
    TransferSurface(*image,s,false);return image;
}
std::shared_ptr<RenderTexture> ResolveMemory::LoadSurfaceSharedRegion(const SurfaceLayout& s,
                                                                    const RenderRect& rectangle) {
    // Keep the full allocation/coordinate space and the unchanged load shader.
    // Only push extent/origin shrink; canonical EDRAM tile/half-tile addressing,
    // UNORM24 decode and stencil decode retain their original implementation.
    (void)SurfaceFootprint(s);
    if(!s.depth||rectangle.isEmpty()||rectangle.left<0||rectangle.top<0||
       rectangle.right>int32_t(s.width)||rectangle.bottom>int32_t(s.height))
        throw std::invalid_argument("Invalid partial EDRAM depth restoration region");
    const auto values=NativeAtlasDepthRegionLoadParameters(s.width,s.height,s.pitch,s.base,s.format,s.depth,
        {rectangle.left,rectangle.top,rectangle.right,rectangle.bottom});
    auto desc=RenderTextureDesc::Texture2D(s.width,s.height,1,RenderFormat::R32G32B32A32_FLOAT);
    desc.flags|=RenderTextureFlag::STORAGE|RenderTextureFlag::UNORDERED_ACCESS;
    std::shared_ptr<RenderTexture> image=device_.createTexture(desc);
    if(batch_)batch_->KeepImage(image,uint64_t(s.width)*s.height*16);
    Parameters push;static_assert(sizeof(push)==sizeof(values));
    std::memcpy(&push,values.data(),sizeof(push));
    Dispatch(*image,push,false,nullptr,0,NativeEdramBytes());
    return image;
}

}
