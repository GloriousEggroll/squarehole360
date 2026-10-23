#include <cstdint>
#include "fullscreen_spv.h"
#include "fused_packed_atlas_resolve_spv.h"
#include "renderer/native/fused_packed_atlas_resolve.h"
#include "atlas_depth_clear_spv.h"
#include "atlas_depth_clear_host_spv.h"
#include "memory_store_rgba_stencil_spv.h"
#include "packed_primary_stencil_clear_spv.h"
#include "packed_secondary_stencil_clear_spv.h"
#include "atlas_depth_clear_oracle.h"
#include "renderer/native/atlas_depth_clear.h"
#include "gpu_helpers.h"
#include "memory_load_spv.h"
#include "memory_store_spv.h"
#include "query_test_spv.h"
#include "renderer/native/fsi_device.h"
#include "renderer/native/gpu_work_queue.h"
#include "renderer/native/pass_transfer.h"
#include "renderer/native/query_tracker.h"
#include "renderer/native/resolve_memory.h"
#include "renderer/native/transfer_batch.h"
#include "resolve_spv.h"
#include "restore_depth_spv.h"
#include "restore_depth_stencil_spv.h"
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <exception>
#include <functional>
#include <iostream>
#include <iomanip>
#include <limits>
#include <memory>
using namespace plume;
using namespace gow3::renderer;
namespace {
constexpr uint32_t width = 256, height = 16, edram_bytes = 2048 * 5120;
size_t checks = 0;
void Require(bool value, const char *message) {
  ++checks;
  if (!value)
    throw std::runtime_error(message);
}
struct AttachmentBytes {
  std::vector<uint8_t> depth, stencil;
  bool operator==(const AttachmentBytes &) const = default;
};
AttachmentBytes ReadDepthStencil(RenderDevice &device,
                                 RenderCommandQueue &queue,
                                 RenderTexture &texture) {
  const auto &description = static_cast<VulkanTexture &>(texture).desc;
  const uint32_t width = description.width, height = description.height;
  Require(width && height && description.format == RenderFormat::D32_FLOAT_S8_UINT,
          "Unexpected depth/stencil readback image");
  auto buffer =
      device.createBuffer(RenderBufferDesc::ReadbackBuffer(width * height * 5));
  auto commands = queue.createCommandList();
  auto fence = device.createCommandFence();
  commands->begin();
  commands->barriers(
      RenderBarrierStage::COPY,
      RenderTextureBarrier(&texture, RenderTextureLayout::COPY_SOURCE));
  std::array<VkBufferImageCopy, 2> copies{};
  copies[0].imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
  copies[1].imageSubresource = {VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, 1};
  copies[1].bufferOffset = width * height * 4;
  for (auto &copy : copies)
    copy.imageExtent = {width, height, 1};
  const auto command = static_cast<VulkanCommandList &>(*commands).vk;
  auto &vk_buffer = static_cast<VulkanBuffer &>(*buffer);
  vkCmdCopyImageToBuffer(command, static_cast<VulkanTexture &>(texture).vk,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, vk_buffer.vk,
                         uint32_t(copies.size()), copies.data());
  VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, nullptr, 0,
                       nullptr);
  commands->end();
  queue.executeCommandLists(commands.get(), fence.get());
  queue.waitForCommandFence(fence.get());
  const auto data = static_cast<const uint8_t *>(buffer->map());
  Require(vmaInvalidateAllocation(vk_buffer.device->allocator,
                                  vk_buffer.allocation, 0,
                                  VK_WHOLE_SIZE) == VK_SUCCESS,
          "Cannot read restored attachment");
  AttachmentBytes result{
      {data, data + width * height * 4},
      {data + width * height * 4, data + width * height * 5}};
  buffer->unmap();
  return result;
}


// Exact fused-clear primitive proof. Scene cache/token orchestration and natural
// capture eligibility are separate root-owned replay checks.
struct FusedAtlasResult {
  uint64_t packed_sources=0,host_sources=0,whole_edram_compares=0,
           receipt_restores=0,receipt_owner_witnesses=0,preencode_difference_pixels=0,
           accepted_work=0,completed_work=0,host_descriptor_hits=0;
};
FusedAtlasResult RunFusedAtlasClear(RenderDevice &device,RenderCommandQueue &queue,
                                  bool export_stencil,uint32_t capacity) {
  using namespace gow3::renderer::validation;
  constexpr uint32_t w=1120,h=688;
  Require(std::endian::native==std::endian::little,"Packed oracle requires actual little-endian host");
  const SurfaceLayout surface{w,h,w,1440,0,true};
  const RenderRect full(0,0,w,h);
  const std::array<RenderRect,4> rects{RenderRect(0,0,160,96),RenderRect(41,17,179,91),
      RenderRect(79,15,82,18),RenderRect(1046,614,1120,688)};
  auto reference=device.createTexture(RenderTextureDesc::DepthTarget(w,h,RenderFormat::D32_FLOAT_S8_UINT));
  auto candidate_host=device.createTexture(RenderTextureDesc::DepthTarget(w,h,RenderFormat::D32_FLOAT_S8_UINT));
  auto restored_receipt=device.createTexture(RenderTextureDesc::DepthTarget(w,h,RenderFormat::D32_FLOAT_S8_UINT));
  Require(reference&&candidate_host&&restored_receipt,"Fused atlas fixture attachment allocation failed");
  NativeGpuWorkQueue work(device,queue,{},true,false,capacity);
  NativeTransferBatch batch(device,queue,&work);
  PassTransfer pass(device,queue,fullscreen_spv,resolve_spv,restore_depth_spv,&batch,
      export_stencil?std::span<const uint32_t>(restore_depth_stencil_spv):std::span<const uint32_t>{});
  ResolveMemory baseline(device,queue,memory_store_spv,memory_load_spv,edram_bytes,0,&batch);
  ResolveMemory candidate(device,queue,memory_store_spv,memory_load_spv,edram_bytes,0,&batch);
  baseline.SetDescriptorPoolEnabled(true);candidate.SetDescriptorPoolEnabled(true);
  Require(candidate.EnableAtlasDepthClear(atlas_depth_clear_spv,atlas_depth_clear_host_spv),
          "Fused atlas pipelines were unavailable");
  struct DrainOnExit {NativeTransferBatch& batch;~DrainOnExit(){try{batch.Flush();}catch(...){std::terminate();}}} drain{batch};
  FusedAtlasResult result;
  constexpr std::array<uint32_t,16> depths{0,1,0x3ffffe,0x3fffff,0x400000,0x400001,
      0x400002,0x400003,0x7ffffe,0x7fffff,0x800000,0x800001,0xbfffff,0xfffffd,0xfffffe,0xffffff};
  std::vector<uint8_t> seed(edram_bytes);
  for(size_t i=0;i<seed.size();++i)seed[i]=uint8_t(i*29+(i>>8));
  const auto read_word=[](const std::vector<uint8_t>& b,uint32_t at){uint32_t word;std::memcpy(&word,b.data()+at,4);return word;};
  const auto put_word=[](std::vector<uint8_t>& b,uint32_t at,uint32_t word){std::memcpy(b.data()+at,&word,4);};
  for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x){
    const uint32_t d=depths[(x+y*3)%depths.size()];
    put_word(seed,AtlasDepthOffsetOracle(x,y),(d<<8)|((x*17+y*29)&255));
  }
  const auto inside=[](const RenderRect&r,uint32_t x,uint32_t y){return x>=uint32_t(r.left)&&x<uint32_t(r.right)&&y>=uint32_t(r.top)&&y<uint32_t(r.bottom);};
  const auto restore_receipt=[&](ResolveMemory::AtlasDepthClearReceipt& receipt,RenderTexture& destination){
    Require(receipt.source&&receipt.packed_version==candidate.SurfaceVersion(surface),"Receipt does not match its actual new packed token");
    const auto& native=static_cast<const VulkanTexture&>(*receipt.source);
    const uint64_t bytes=native.allocationInfo.size;
    Require(native.ownership&&native.allocation&&bytes&&bytes<=kNativeAtlasDepthClearMaxReceiptBytes,"Receipt actual allocation escaped its bound");
    std::weak_ptr<RenderTexture> weak=receipt.source;
    // First generation's original packet has genuinely retired. The delayed
    // consumer must retain a NEW packet owner before reading this receipt.
    batch.Flush();
    Require(!work.pending()&&work.accepted_work()==work.completed_work()&&!weak.expired(),"Externally owned receipt did not survive its original covering fence");
    const auto accepted_before=work.accepted_work();
    batch.Run([&]{batch.KeepImage(receipt.source,bytes);pass.RestoreDepth(*receipt.source,destination);receipt.source.reset();receipt.packed_version=0;});
    Require(work.pending()&&work.accepted_work()>accepted_before&&!weak.expired(),"Delayed receipt was not retained by its actual consumer packet");
    batch.Flush();
    Require(work.accepted_work()==work.completed_work()&&!work.pending()&&weak.expired(),"Receipt consumer owner did not retire at its actual fence");
    ++result.receipt_restores;++result.receipt_owner_witnesses;
  };
  for(size_t ordinal=0;ordinal<rects.size();++ordinal){
    const auto& rect=rects[ordinal];baseline.Initialize(0,seed);candidate.Initialize(0,seed);
    batch.Run([&]{auto image=baseline.LoadSurfaceShared(surface);pass.RestoreDepth(*image,*reference);
        pass.Clear(nullptr,reference.get(),false,true,false,rect,{},1.f,0);baseline.StoreSurface(*reference,surface);});
    batch.Flush();
    auto reference_bits=ReadDepthStencil(device,queue,*reference);
    auto expected=seed;
    for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x){const auto at=AtlasDepthOffsetOracle(x,y);const auto word=read_word(seed,at);
        put_word(expected,at,((inside(rect,x,y)?0xffffffu:AtlasPackedRoundTripOracle(word>>8))<<8)|(word&255));}
    Require(baseline.ReadBytes(0,edram_bytes)==expected,"Canonical clear+full store disagrees with independent whole-memory integer oracle");
    const auto token=candidate.SurfaceVersion(surface),accepted_before=work.accepted_work();
    ResolveMemory::AtlasDepthClearReceipt receipt;
    batch.Run([&]{Require(candidate.ClearAtlasDepthOneFromPacked(surface,rect,receipt),"Packed-source fused atlas clear refused eligible input");});
    Require(receipt.packed_version==token+1&&work.pending()&&work.accepted_work()>accepted_before,"Fused packed clear did not define exactly one accepted full-surface generation");
    // A pre-record refusal must not alter metadata or append accepted work.
    const auto after_accept=work.accepted_work();
    ResolveMemory::AtlasDepthClearReceipt refusal;
    Require(!candidate.ClearAtlasDepthOneFromPacked(surface,RenderRect(-1,0,10,10),refusal)&&!refusal.source&&
        work.accepted_work()==after_accept&&candidate.SurfaceVersion(surface)==receipt.packed_version,"Unsupported rectangle was not a side-effect-free refusal");
    restore_receipt(receipt,*candidate_host);
    const auto actual=candidate.ReadBytes(0,edram_bytes);
    Require(actual==expected&&actual==baseline.ReadBytes(0,edram_bytes),"Fused packed clear lost full10MiB/stencil/outside equality");
    Require(ReadDepthStencil(device,queue,*candidate_host)==reference_bits,"Packed receipt did not preserve canonical PRE-encode host bits");
    ++result.packed_sources;++result.whole_edram_compares;
    uint64_t changed_outside=0;
    for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x)if(!inside(rect,x,y)){
        uint32_t bits;std::memcpy(&bits,reference_bits.depth.data()+(size_t(y)*w+x)*4,4);
        const auto d=read_word(actual,AtlasDepthOffsetOracle(x,y))>>8;
        changed_outside+=bits!=std::bit_cast<uint32_t>(float(d+(d>>23))/16777216.f);
    }
    Require(changed_outside>0,"Pre-encode host preservation witness was vacuous");result.preencode_difference_pixels+=changed_outside;
    // Baseline now has a clean/current D32 image whose floats intentionally
    // differ from Decode(packed) outside the prior rectangle. Read those REAL
    // source floats for this second clear, never substitute packed decoding.
    // Include real finite D32 values not assumed to originate in packed decode.
    // Canonical stores make both source images clean/current before host input.
    const std::array<float,3> host_values{.625f,std::nextafter(.5f,1.f),0.f};
    batch.Run([&]{for(unsigned i=0;i<host_values.size();++i){
      const RenderRect probe(300+int32_t(i)*13,240,309+int32_t(i)*13,245);
      pass.Clear(nullptr,reference.get(),false,true,false,probe,{},host_values[i],0);
      pass.Clear(nullptr,candidate_host.get(),false,true,false,probe,{},host_values[i],0);
    }baseline.StoreSurface(*reference,surface);candidate.StoreSurface(*candidate_host,surface);});
    batch.Flush();reference_bits=ReadDepthStencil(device,queue,*reference);
    Require(ReadDepthStencil(device,queue,*candidate_host)==reference_bits,"Clean-host arbitrary float sources were not identical");
    const auto& host_rect=rects[(ordinal+1)%rects.size()];
    auto host_expected=candidate.ReadBytes(0,edram_bytes);
    for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x){const size_t pixel=size_t(y)*w+x;uint32_t bits;
        std::memcpy(&bits,reference_bits.depth.data()+pixel*4,4);
        const auto d=inside(host_rect,x,y)?0xffffffu:AtlasHostQuantizeOracle(bits);
        put_word(host_expected,AtlasDepthOffsetOracle(x,y),(d<<8)|reference_bits.stencil[pixel]);}
    batch.Run([&]{pass.Clear(nullptr,reference.get(),false,true,false,host_rect,{},1.f,0);baseline.StoreSurface(*reference,surface);});
    batch.Flush();
    const auto before_host=candidate.SurfaceVersion(surface);
    batch.Run([&]{Require(candidate.ClearAtlasDepthOneFromPacked(surface,host_rect,receipt,candidate_host.get()),"Clean-host fused atlas clear refused eligible source");});
    Require(receipt.packed_version==before_host+1,"Host-source fused clear did not define exactly one full-surface generation");
    restore_receipt(receipt,*restored_receipt);
    Require(ReadDepthStencil(device,queue,*candidate_host)==reference_bits,"Host-source fused clear altered its read-only source image");
    Require(candidate.ReadBytes(0,edram_bytes)==host_expected&&baseline.ReadBytes(0,edram_bytes)==host_expected,"Host-source fused clear lost whole10MiB/exact actual-float oracle equality");
    Require(ReadDepthStencil(device,queue,*restored_receipt)==ReadDepthStencil(device,queue,*reference),"Host receipt did not reproduce canonical depth/stencil float state");
    ++result.host_sources;++result.whole_edram_compares;
  }
  // Drop the only external receipt owner BEFORE its original compute fence:
  // this independent witness proves the first packet itself owns its output.
  auto ownership_expected=candidate.ReadBytes(0,edram_bytes);
  for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x){const auto at=AtlasDepthOffsetOracle(x,y);const auto old=read_word(ownership_expected,at);
    put_word(ownership_expected,at,((inside(rects[0],x,y)?0xffffffu:AtlasPackedRoundTripOracle(old>>8))<<8)|(old&255));}
  ResolveMemory::AtlasDepthClearReceipt dropped;
  std::weak_ptr<RenderTexture> weak;
  batch.Run([&]{Require(candidate.ClearAtlasDepthOneFromPacked(surface,rects[0],dropped),"Ownership-witness clear failed");weak=dropped.source;dropped.source.reset();});
  Require(work.pending()&&!weak.expired(),"Compute packet did not pin its dropped receipt source");
  batch.Flush();Require(weak.expired(),"Dropped receipt remained owned after its real covering fence");++result.receipt_owner_witnesses;
  Require(candidate.ReadBytes(0,edram_bytes)==ownership_expected,"Compute-only ownership clear lost full10MiB oracle equality");
  ++result.packed_sources;++result.whole_edram_compares;
  const auto descriptors=candidate.AtlasDepthClearHostDescriptorStatistics();
  Require(descriptors.enabled&&descriptors.maximum_sets==16&&descriptors.requests==descriptors.hits+descriptors.misses&&
      descriptors.hits>0&&descriptors.factory_failures==0&&descriptors.live_sets==descriptors.idle_sets+descriptors.leased_sets&&
      descriptors.live_sets<=16&&descriptors.leased_sets==0,"Host descriptor leases did not conserve/reuse at actual fences");
  result.host_descriptor_hits=descriptors.hits;result.accepted_work=work.accepted_work();result.completed_work=work.completed_work();
  Require(result.accepted_work>0&&result.accepted_work==result.completed_work&&!work.pending()&&!work.inflight_batches()&&!work.failed(),"Fused atlas fixture left actual accepted work pending");
  return result;
}


// The only omitted operation in the direct packed-color source candidate is
// this full RGBA32F identity graphics copy. Scene provenance/refusal and actual
// destination packing are qualified separately, not inferred from this proof.
struct PrimaryColorCopyResult {
  uint64_t pixels=0,compared_float_components=0,whole_edram_compares=0,
      source_owner_witnesses=0,accepted_work=0,completed_work=0;
  std::array<uint32_t,4> unique_codes{};
};
PrimaryColorCopyResult RunPrimaryColorCopy(RenderDevice& device,RenderCommandQueue& queue,
                                         uint32_t capacity) {
  constexpr uint32_t w=1280,h=720;
  Require(std::endian::native==std::endian::little,"Primary color seed requires a little-endian host");
  const SurfaceLayout surface{w,h,w,720,2,false};
  const RenderRect full(0,0,w,h);
  NativeGpuWorkQueue work(device,queue,{},true,false,capacity);
  NativeTransferBatch batch(device,queue,&work);
  PassTransfer pass(device,queue,fullscreen_spv,resolve_spv,{},&batch);
  ResolveMemory edram(device,queue,memory_store_spv,memory_load_spv,edram_bytes,0,&batch);
  edram.SetDescriptorPoolEnabled(true);
  auto host=device.createTexture(RenderTextureDesc::ColorTarget(w,h,RenderFormat::R32G32B32A32_FLOAT));
  Require(bool(host),"Primary color host allocation failed");
  struct DrainOnExit {NativeTransferBatch& batch;~DrainOnExit(){try{batch.Flush();}catch(...){std::terminate();}}}drain{batch};
  std::vector<uint8_t> seed(edram_bytes);
  for(size_t i=0;i<seed.size();++i)seed[i]=uint8_t(i*29+(i>>8));
  const auto word_at=[](uint32_t x,uint32_t y) {
    const uint32_t i=y*w+x;
    return (i&1023u)|(((i*37u+129u)&1023u)<<10)|(((i*73u+511u)&1023u)<<20)|(((i>>10)&3u)<<30);
  };
  for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x) {
    // Closed, independent 32-bit primary-color address expression. All tiles
    // are within the 10MiB arena; no production offset helper builds the seed.
    const uint32_t at=(720+(y/16)*16+x/80)*5120+(y%16)*320+(x%80)*4;
    const auto word=word_at(x,y);std::memcpy(seed.data()+at,&word,4);
  }
  edram.Initialize(0,seed);
  const auto version=edram.SurfaceVersion(surface);
  Require(version&&edram.SurfaceFullyDefined(surface),"Primary color seed lacks full packed provenance");
  std::shared_ptr<RenderTexture> source;
  batch.Run([&] {
    source=edram.LoadSurfaceShared(surface);
    batch.Seal();
    // This is the unchanged call Surface() makes when restoring color fmt2.
    pass.Resolve(*source,*host,full,full);
  });
  batch.Flush();
  const auto direct=::validation::ReadImage(device,queue,*source,16);
  const auto copied=::validation::ReadImage(device,queue,*host,16);
  Require(direct.size()==uint64_t(w)*h*16&&copied==direct,
          "Actual identity graphics copy changed canonical UNORM10/2 float bits");
  PrimaryColorCopyResult result;result.pixels=uint64_t(w)*h;
  result.compared_float_components=result.pixels*4;
  std::array<std::array<bool,1024>,3> seen_rgb{};
  std::array<bool,4> seen_alpha{};
  for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x) {
    const auto word=word_at(x,y);const size_t pixel=size_t(y)*w+x;
    for(unsigned c=0;c<4;++c) {
      const uint32_t mask=c==3?3u:1023u,code=(word>>(c*10))&mask;
      float value;std::memcpy(&value,direct.data()+(pixel*4+c)*4,4);
      Require(std::isfinite(value)&&value>=0.f&&value<=1.f&&
          uint32_t(std::lround(double(value)*mask))==code,
          "Canonical primary color decode did not recover its seeded channel code");
      if(c==3)seen_alpha[code]=true;else seen_rgb[c][code]=true;
    }
  }
  for(unsigned c=0;c<3;++c)for(bool present:seen_rgb[c])result.unique_codes[c]+=present;
  for(bool present:seen_alpha)result.unique_codes[3]+=present;
  Require(result.unique_codes==std::array<uint32_t,4>{1024,1024,1024,4},
          "Primary color identity witness did not cover every RGB/alpha code");
  Require(edram.ReadBytes(0,edram_bytes)==seed&&edram.SurfaceVersion(surface)==version,
          "Primary color decode/copy changed full packed EDRAM or its token");
  ++result.whole_edram_compares;
  source.reset();

  // Prove the real LoadSurfaceShared owner survives explicit Seal, then actual
  // producer retirement, and is copied into the subsequent consumer packet.
  std::weak_ptr<RenderTexture> weak;
  const auto accepted_before=work.accepted_work();
  batch.Run([&] {
    auto image=edram.LoadSurfaceShared(surface);weak=image;
    batch.Seal();
    const auto producer=work.accepted_work();
    Require(producer==accepted_before+1&&work.pending()&&!weak.expired(),
            "Primary source producer was not genuinely sealed and owned");
    work.SubmitPending();work.WaitThrough(producer);
    Require(work.completed_work()==producer&&!work.pending()&&!weak.expired(),
            "Primary source was lost when its original packet actually retired");
    pass.Resolve(*image,*host,full,full);
    image.reset();
  });
  Require(work.accepted_work()==accepted_before+2&&work.pending()&&!weak.expired(),
          "Primary consumer packet did not independently own its decoded image");
  work.SubmitPending();
  Require(work.inflight_batches()>0&&!weak.expired(),
          "Primary decoded image was released before its consumer fence");
  batch.Flush();
  Require(weak.expired()&&work.accepted_work()==work.completed_work()&&!work.pending()&&
      !work.inflight_batches()&&!work.failed(),"Primary source did not retire at its actual final fence");
  ++result.source_owner_witnesses;
  Require(::validation::ReadImage(device,queue,*host,16)==direct,
          "Post-producer-retirement identity copy changed complete float bits");
  result.compared_float_components+=result.pixels*4;
  Require(edram.ReadBytes(0,edram_bytes)==seed&&edram.SurfaceVersion(surface)==version,
          "Ownership witness changed full packed EDRAM or its token");
  ++result.whole_edram_compares;
  Require(!batch.statistics().synchronous_fallbacks,"Primary ownership witness silently used synchronous fallback");
  result.accepted_work=work.accepted_work();result.completed_work=work.completed_work();
  return result;
}

std::vector<uint8_t> ReadR32Atlas(RenderDevice& device,RenderCommandQueue& queue,RenderTexture& texture) {
  const auto& d=static_cast<const VulkanTexture&>(texture).desc;
  Require(d.format==RenderFormat::R32_FLOAT&&d.width==1120&&d.height==688,"R32 atlas readback shape mismatch");
  const uint64_t bytes=uint64_t(d.width)*d.height*4;
  auto readback=device.createBuffer(RenderBufferDesc::ReadbackBuffer(bytes));
  auto commands=queue.createCommandList();auto fence=device.createCommandFence();commands->begin();
  commands->barriers(RenderBarrierStage::COPY,RenderTextureBarrier(&texture,RenderTextureLayout::COPY_SOURCE));
  VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={d.width,d.height,1};
  auto& buffer=static_cast<VulkanBuffer&>(*readback);
  vkCmdCopyImageToBuffer(static_cast<VulkanCommandList&>(*commands).vk,static_cast<VulkanTexture&>(texture).vk,
      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buffer.vk,1,&copy);
  VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
  vkCmdPipelineBarrier(static_cast<VulkanCommandList&>(*commands).vk,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);
  commands->end();queue.executeCommandLists(commands.get(),fence.get());queue.waitForCommandFence(fence.get());
  const auto* data=static_cast<const uint8_t*>(readback->map());
  Require(vmaInvalidateAllocation(buffer.device->allocator,buffer.allocation,0,VK_WHOLE_SIZE)==VK_SUCCESS,"R32 atlas readback invalidate failed");
  std::vector<uint8_t> result(data,data+bytes);readback->unmap();return result;
}

// Compare the actual fused graphics shader against the unchanged full canonical
// RGBA decode -> Resolve -> physical Store, including independent integer oracles.
struct FusedPackedAtlasResult {
  uint64_t cases=0,depth_codes=0,stencil_codes=0,raw_pixels=0,full_source_compares=0,
      full_destination_compares=0,owner_witnesses=0,refusals=0,accepted_work=0,completed_work=0,
      all24_chunks=0,all24_codes=0,all24_raw_pixels=0,all24_nonidentity_codes=0;
};
FusedPackedAtlasResult RunFusedPackedAtlasResolve(RenderDevice& device,RenderCommandQueue& queue,uint32_t capacity) {
  using namespace gow3::renderer::validation;
  constexpr uint32_t w=1120,h=688,address=0x10000;
  Require(std::endian::native==std::endian::little,"Fused atlas oracle requires actual little-endian host");
  const SurfaceLayout surface{w,h,w,1440,0,true};const RenderRect full(0,0,w,h);
  const std::array<RenderRect,5> rectangles{RenderRect(0,0,544,352),RenderRect(0,0,544,288),
      RenderRect(0,0,96,96),RenderRect(0,0,160,96),RenderRect(0,0,160,64)};
  constexpr std::array<uint32_t,16> codes{0,1,0x3ffffe,0x3fffff,0x400000,0x400001,0x400002,0x400003,
      0x7ffffe,0x7fffff,0x800000,0x800001,0xbfffff,0xfffffd,0xfffffe,0xffffff};
  auto original=device.createTexture(RenderTextureDesc::ColorTarget(w,h,RenderFormat::R32_FLOAT));
  auto candidate=device.createTexture(RenderTextureDesc::ColorTarget(w,h,RenderFormat::R32_FLOAT));
  Require(original&&candidate,"Fused atlas fixture image allocation failed");
  NativeGpuWorkQueue work(device,queue,{},true,false,capacity);NativeTransferBatch batch(device,queue,&work);
  PassTransfer pass(device,queue,fullscreen_spv,resolve_spv,restore_depth_spv,&batch);
  NativeFusedPackedAtlasResolve fused(device,queue,fullscreen_spv,fused_packed_atlas_resolve_spv,&batch);
  Require(fused.supported(),"Fused atlas module was unavailable");
  ResolveMemory source(device,queue,memory_store_spv,memory_load_spv,edram_bytes,0,&batch);
  ResolveMemory baseline(device,queue,memory_store_spv,memory_load_spv,edram_bytes,0,&batch);
  ResolveMemory output(device,queue,memory_store_spv,memory_load_spv,edram_bytes,0,&batch);
  Require(baseline.EnableAtlasRgbaDepthStore(memory_store_rgba_stencil_spv)&&
      output.EnableAtlasRgbaDepthStore(memory_store_rgba_stencil_spv),"Canonical atlas physical store unavailable");
  baseline.SetDescriptorPoolEnabled(true);output.SetDescriptorPoolEnabled(true);
  struct DrainOnExit {NativeTransferBatch& batch;~DrainOnExit(){try{batch.Flush();}catch(...){std::terminate();}}}drain{batch};
  const std::array<uint32_t,6> fetch{2u|(35u<<22)|0x80000000u,address|22u|(2u<<6),
      (w-1)|((h-1)<<13),0,0,1u<<9};
  const auto layout=DescribeTexture(fetch);
  Require(layout.format==22&&layout.endian==2&&layout.tiled&&layout.width==w&&layout.height==h,
      "Fused atlas destination fixture layout changed");
  std::vector<uint8_t> seed(edram_bytes),poison(edram_bytes);
  for(size_t n=0;n<seed.size();++n){seed[n]=uint8_t(n*31+(n>>9));poison[n]=uint8_t(n*43+(n>>7)+0x5d);}
  const auto word_at=[](const std::vector<uint8_t>& b,size_t at){uint32_t word;std::memcpy(&word,b.data()+at,4);return word;};
  for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x) {
    const uint32_t pixel=y*w+x,word=(codes[pixel%codes.size()]<<8)|((pixel/codes.size())&255u);
    std::memcpy(seed.data()+AtlasDepthOffsetOracle(x,y),&word,4);
  }
  source.Initialize(0,seed);const auto source_version=source.SurfaceVersion(surface);
  Require(source_version&&source.SurfaceFullyDefined(surface),"Fused atlas packed source lacks full provenance");
  // Independent tiled physical address and endian oracle, not Transfer's helper.
  const auto physical=[](uint32_t x,uint32_t y){
    const uint64_t macro=((x/32)+(y/32)*35ull)*512;
    const uint64_t micro=((x%8)+((y&14)*4))*4;
    const uint64_t o=macro+((micro/16)*32)+(micro%16)+(y%2)*16;
    return address+((o/512)*4096)+(y&16)*128+((o&448)*4)+((((y&8)/4)+(x/8))%4)*64+(o%64);
  };
  FusedPackedAtlasResult result;std::array<bool,256> seen_stencil{};std::array<bool,16> seen_depth{};
  std::shared_ptr<RenderTexture> scratch;
  const auto before=work.accepted_work();const auto stage_before=batch.statistics().stages;
  Require(!fused.Resolve(source.storage_buffer(),*candidate,rectangles[0],scratch)&&!scratch,
      "Fused atlas accepted outside its owning transfer scope");++result.refusals;
  batch.Run([&]{for(const auto& bad:std::array<RenderRect,4>{RenderRect(1,0,97,96),RenderRect(0,0,128,128),
      RenderRect(0,0,0,96),RenderRect(0,0,1121,688)}) {
    Require(!fused.Resolve(source.storage_buffer(),*candidate,bad,scratch)&&!scratch,"Unqualified fused rectangle accepted");
    ++result.refusals;
  }});
  Require(work.accepted_work()==before&&batch.statistics().stages==stage_before,
      "Fused pre-recording refusal accepted a GPU stage");
  for(size_t n=0;n<rectangles.size();++n) {
    const auto& rect=rectangles[n];baseline.Initialize(0,poison);output.Initialize(0,poison);
    batch.Run([&]{
      auto rgba=source.LoadSurfaceShared(surface);
      pass.Clear(original.get(),nullptr,true,false,false,full,{-.375f,0,0,0},0);
      pass.Resolve(*rgba,*original,rect,rect,0,PassTransfer::Conversion::DepthUNorm24);
      Require(baseline.StoreDepthFromRgbaStencil(*original,layout,rect,address,*rgba,0,0),
          "Canonical atlas physical store refused after baseline Resolve");
    });batch.Flush();
    const auto reference=ReadR32Atlas(device,queue,*original);
    // Repeated partial writes start from a distinct complete destination image.
    // Scratch itself is deliberately NOT initialized outside the copied region.
    batch.Run([&]{pass.Clear(candidate.get(),nullptr,true,false,false,full,{-.375f,0,0,0},0);});batch.Flush();
    const auto accepted=work.accepted_work();std::weak_ptr<RenderTexture> weak;
    batch.Run([&]{
      Require(fused.Resolve(source.storage_buffer(),*candidate,rect,scratch)&&scratch,
          "Qualified packed atlas MRT refused");weak=scratch;
      batch.Seal();
      Require(work.accepted_work()==accepted+1&&work.pending()&&!weak.expired(),
          "MRT scratch has no actual producer packet owner");
    });
    if(n==0) {
      // Producer truly retires; external owner retains only the initialized
      // rectangle until the subsequent physical consumer takes its own lease.
      work.SubmitPending();work.WaitThrough(accepted+1);
      Require(work.completed_work()==accepted+1&&!work.pending()&&!weak.expired(),
          "MRT scratch failed actual producer-retirement witness");
    }
    batch.Run([&]{
      batch.KeepImage(scratch,static_cast<const VulkanTexture&>(*scratch).allocationInfo.size);
      Require(output.StoreDepthFromRgbaStencil(*candidate,layout,rect,address,*scratch,0,0),
          "MRT physical store refused after accepted graphics write");scratch.reset();
    });
    Require(work.accepted_work()==accepted+2&&work.pending()&&!weak.expired(),
        "Actual physical consumer does not independently retain MRT scratch");
    work.SubmitPending();Require(work.inflight_batches()&&!weak.expired(),"Scratch was released before consumer fence");
    batch.Flush();Require(weak.expired()&&!work.pending()&&work.accepted_work()==work.completed_work(),
        "MRT scratch ownership did not retire at final covering fence");
    if(n==0)++result.owner_witnesses;
    const auto actual=ReadR32Atlas(device,queue,*candidate);
    Require(actual==reference&&actual.size()==uint64_t(w)*h*4,"MRT changed exact full R32 Resolve image");
    auto expected=poison;
    for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x) {
      const auto bits=word_at(actual,(size_t(y)*w+x)*4);
      if(x>=uint32_t(rect.right)||y>=uint32_t(rect.bottom)) {
        Require(bits==std::bit_cast<uint32_t>(-.375f),"MRT changed untouched destination pixel");continue;
      }
      const auto input=word_at(seed,AtlasDepthOffsetOracle(x,y));const uint32_t depth=input>>8,stencil=input&255;
      const auto first=AtlasPackedRoundTripOracle(depth);
      const auto expected_bits=std::bit_cast<uint32_t>(float(first+(first>>23))/16777216.f);
      Require(bits==expected_bits,"MRT first Resolve quantizer disagrees with independent integer oracle");
      const uint32_t packed=(AtlasHostQuantizeOracle(expected_bits)<<8)|stencil;
      const auto at=physical(x,y);Require(at+4<=expected.size(),"MRT physical oracle escaped storage");
      for(unsigned byte=0;byte<4;++byte)expected[at+byte]=uint8_t(packed>>(8*(3-byte)));
      seen_stencil[stencil]=true;seen_depth[(y*w+x)%codes.size()]=true;++result.raw_pixels;
    }
    Require(baseline.ReadBytes(0,edram_bytes)==expected&&output.ReadBytes(0,edram_bytes)==expected,
        "MRT changed full physical memory, original second quantizer, stencil or padding");
    ++result.full_destination_compares;
    Require(source.ReadBytes(0,edram_bytes)==seed&&source.SurfaceVersion(surface)==source_version,
        "Read-only MRT changed full packed source or original Surface token");
    ++result.full_source_compares;++result.cases;
  }
  for(bool seen:seen_stencil)result.stencil_codes+=seen;
  for(bool seen:seen_depth)result.depth_codes+=seen;
  Require(result.stencil_codes==256&&result.depth_codes==codes.size()&&result.cases==5&&result.owner_witnesses==1,
      "Fused atlas GPU fixture precision/rect/ownership coverage is incomplete");
  // Every possible packed UNORM24 code passes through BOTH actual GPU stages
  // in the baseline. This catches compute-vs-fragment compiler differences;
  // no CPU float expression substitutes for the original load or Resolve.
  constexpr uint32_t logical_pixels=w*h,depth_codes=1u<<24;
  for(uint32_t first=0;first<depth_codes;first+=logical_pixels) {
    for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x) {
      const uint32_t pixel=y*w+x,depth=(first+pixel)&0xffffffu;
      const uint32_t word=(depth<<8)|((pixel*73u+first)&255u);
      std::memcpy(seed.data()+AtlasDepthOffsetOracle(x,y),&word,4);
    }
    source.Initialize(0,seed);const auto version=source.SurfaceVersion(surface);
    batch.Run([&]{
      auto rgba=source.LoadSurfaceShared(surface);
      pass.Resolve(*rgba,*original,full,full,0,PassTransfer::Conversion::DepthUNorm24);
      Require(fused.Resolve(source.storage_buffer(),*candidate,full,scratch)&&scratch,
          "Full exact-layout GPU quantizer witness refused");scratch.reset();
    });batch.Flush();
    const auto old_bits=ReadR32Atlas(device,queue,*original);
    const auto new_bits=ReadR32Atlas(device,queue,*candidate);
    Require(old_bits==new_bits&&old_bits.size()==uint64_t(logical_pixels)*4,
        "Fused fragment differs from actual original compute-load/Resolve float bits");
    for(uint32_t pixel=0;pixel<logical_pixels;++pixel) {
      const uint32_t code=(first+pixel)&0xffffffu,quantized=AtlasPackedRoundTripOracle(code);
      const uint32_t expected=std::bit_cast<uint32_t>(float(quantized+(quantized>>23))/16777216.f);
      Require(word_at(new_bits,size_t(pixel)*4)==expected,
          "Actual exhaustive GPU quantizer disagrees with independent integer code oracle");
      ++result.all24_raw_pixels;
      if(first+pixel<depth_codes) {
        ++result.all24_codes;result.all24_nonidentity_codes+=code!=quantized;
      }
    }
    Require(source.SurfaceVersion(surface)==version,"Read-only exhaustive MRT changed packed source token");
    ++result.all24_chunks;
  }
  Require(result.all24_chunks==22&&result.all24_codes==16777216&&result.all24_raw_pixels==16952320&&
      result.all24_nonidentity_codes==2097152,"Exhaustive actual GPU code coverage differs");
  Require(!batch.statistics().synchronous_fallbacks&&!work.failed()&&!work.pending()&&!work.inflight_batches(),
      "Fused atlas fixture silently fell back or retained pending work");
  result.accepted_work=work.accepted_work();result.completed_work=work.completed_work();return result;
}

struct RgbaAtlasStoreResult {
  uint64_t resolves=0,decoded_sources=0,receipt_sources=0,whole_memory_compares=0,
      original_alpha_witness_pixels=0,source_owner_witnesses=0,preencode_difference_pixels=0,
      accepted_work=0,completed_work=0,descriptor_hits=0;
};
RgbaAtlasStoreResult RunRgbaAtlasDepthStore(RenderDevice& device,RenderCommandQueue& queue,
                                         bool export_stencil,uint32_t capacity) {
  using namespace gow3::renderer::validation;
  constexpr uint32_t w=1120,h=688,address=0x10000;
  Require(std::endian::native==std::endian::little,"Atlas store oracle requires little-endian host");
  const SurfaceLayout surface{w,h,w,1440,0,true};const RenderRect full(0,0,w,h),clear(41,17,179,91);
  const std::array<RenderRect,3> sources{RenderRect(0,0,160,96),RenderRect(41,17,179,91),RenderRect(1046,614,1120,688)};
  const std::array<RenderRect,3> destinations{RenderRect(7,11,167,107),RenderRect(13,9,151,83),RenderRect(1001,599,1075,673)};
  auto depth=device.createTexture(RenderTextureDesc::DepthTarget(w,h,RenderFormat::D32_FLOAT_S8_UINT));
  auto original=device.createTexture(RenderTextureDesc::ColorTarget(w,h,RenderFormat::R32_FLOAT));
  auto direct=device.createTexture(RenderTextureDesc::ColorTarget(w,h,RenderFormat::R32_FLOAT));
  Require(depth&&original&&direct,"Atlas store fixture allocation failed");
  NativeGpuWorkQueue work(device,queue,{},true,false,capacity);NativeTransferBatch batch(device,queue,&work);
  PassTransfer pass(device,queue,fullscreen_spv,resolve_spv,restore_depth_spv,&batch,
      export_stencil?std::span<const uint32_t>(restore_depth_stencil_spv):std::span<const uint32_t>{});
  ResolveMemory source_memory(device,queue,memory_store_spv,memory_load_spv,edram_bytes,0,&batch);
  ResolveMemory reference_memory(device,queue,memory_store_spv,memory_load_spv,edram_bytes,0,&batch);
  ResolveMemory candidate_memory(device,queue,memory_store_spv,memory_load_spv,edram_bytes,0,&batch);
  source_memory.SetDescriptorPoolEnabled(true);reference_memory.SetDescriptorPoolEnabled(true);candidate_memory.SetDescriptorPoolEnabled(true);
  Require(source_memory.EnableAtlasDepthClear(atlas_depth_clear_spv,atlas_depth_clear_host_spv),"Atlas receipt fixture pipelines unavailable");
  Require(candidate_memory.EnableAtlasRgbaDepthStore(memory_store_rgba_stencil_spv)&&candidate_memory.SupportsAtlasRgbaDepthStore(),"Atlas RGBA stencil store unavailable");
  struct DrainOnExit{NativeTransferBatch& batch;~DrainOnExit(){try{batch.Flush();}catch(...){std::terminate();}}}drain{batch};
  RgbaAtlasStoreResult result;
  constexpr std::array<uint32_t,16> codes{0,1,0x3ffffe,0x3fffff,0x400000,0x400001,0x400002,0x400003,
      0x7ffffe,0x7fffff,0x800000,0x800001,0xbfffff,0xfffffd,0xfffffe,0xffffff};
  std::vector<uint8_t> seed(edram_bytes),poison(edram_bytes);
  for(size_t at=0;at<seed.size();++at){seed[at]=uint8_t(at*31+(at>>9));poison[at]=uint8_t(at*43+(at>>7)+0x5d);}
  const auto word_at=[](const std::vector<uint8_t>& b,uint32_t at){uint32_t v;std::memcpy(&v,b.data()+at,4);return v;};
  for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x){const uint32_t v=(codes[(x+y*3)%codes.size()]<<8)|((x*17+y*29)&255);
      std::memcpy(seed.data()+AtlasDepthOffsetOracle(x,y),&v,4);}
  // Independent linear and Xenos32x32 macro/micro/bank addressing oracle.
  const auto offset=[](uint32_t x,uint32_t y,bool tiled){
    if(!tiled)return uint64_t(y*1120+x)*4;
    const uint64_t macro=((x/32)+(y/32)*35ull)*512;
    const uint64_t micro=((x%8)+((y&14)*4))*4;
    const uint64_t o=macro+((micro/16)*32)+(micro%16)+(y%2)*16;
    return ((o/512)*4096)+(y&16)*128+((o&448)*4)+((((y&8)/4)+(x/8))%4)*64+(o%64);
  };
  const auto in=[](const RenderRect&r,uint32_t x,uint32_t y){return x>=uint32_t(r.left)&&x<uint32_t(r.right)&&y>=uint32_t(r.top)&&y<uint32_t(r.bottom);};
  for(bool tiled:{false,true})for(bool receipt_source:{false,true}){
    source_memory.Initialize(0,seed);std::shared_ptr<RenderTexture> rgba;
    batch.Run([&]{
      if(receipt_source){ResolveMemory::AtlasDepthClearReceipt receipt;
        Require(source_memory.ClearAtlasDepthOneFromPacked(surface,clear,receipt),"Original preencode receipt fixture refused");
        rgba=std::move(receipt.source);Require(receipt.packed_version==source_memory.SurfaceVersion(surface),"Source receipt token mismatch");
      }else rgba=source_memory.LoadSurfaceShared(surface);
      pass.RestoreDepth(*rgba,*depth);
    });batch.Flush();
    const auto source_bits=ReadDepthStencil(device,queue,*depth);
    const auto source_packed=source_memory.ReadBytes(0,edram_bytes);
    const auto source_version=source_memory.SurfaceVersion(surface);
    uint64_t preencode_difference=0;
    for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x){const size_t pixel=size_t(y)*w+x;const auto word=word_at(seed,AtlasDepthOffsetOracle(x,y));
      const float value=receipt_source&&in(clear,x,y)?1.f:float((word>>8)+((word>>8)>>23))/16777216.f;
      uint32_t actual;std::memcpy(&actual,source_bits.depth.data()+pixel*4,4);
      Require(actual==std::bit_cast<uint32_t>(value)&&source_bits.stencil[pixel]==(word&255),"Original canonical RGBA restoration failed source pixel oracle");
      const auto d=word_at(source_packed,AtlasDepthOffsetOracle(x,y))>>8;
      preencode_difference+=actual!=std::bit_cast<uint32_t>(float(d+(d>>23))/16777216.f);
    }
    Require(receipt_source?preencode_difference>0:preencode_difference==0,"Source decoded/preencode provenance witness incorrect");
    result.preencode_difference_pixels+=preencode_difference;
    const std::array<uint32_t,6> fetch{2u|(35u<<22)|(tiled?0x80000000u:0),address|22u|(2u<<6),(w-1)|((h-1)<<13),0,0,1u<<9};
    const auto layout=DescribeTexture(fetch);Require(layout.format==22&&layout.endian==2&&layout.width==w&&layout.height==h,"Destination layout does not match actual atlas resolve");
    for(size_t n=0;n<sources.size();++n){const auto& src=sources[n];const auto& dst=destinations[n];const int32_t dx=src.left-dst.left,dy=src.top-dst.top;
      Require(dx&&dy,"Source offset fixture unexpectedly zero");
      reference_memory.Initialize(0,poison);candidate_memory.Initialize(0,poison);
      const auto accepted=work.accepted_work();
      Require(!candidate_memory.StoreDepthFromRgbaStencil(*direct,layout,dst,address,*rgba,std::numeric_limits<int32_t>::max(),dy)&&
          work.accepted_work()==accepted&&candidate_memory.ReadBytes(0,edram_bytes)==poison,"Out-of-bounds source refusal changed accepted work or poisoned destination");
      batch.Run([&]{pass.Clear(original.get(),nullptr,true,false,false,full,{-.375f,0,0,0},0,0);
        pass.Resolve(*depth,*original,src,dst,0,PassTransfer::Conversion::DepthUNorm24);
        reference_memory.StoreDepth(*original,layout,dst,address,*depth,dx,dy);});batch.Flush();
      std::weak_ptr<RenderTexture> weak=rgba;const bool last=n+1==sources.size();const auto before=work.accepted_work();
      batch.Run([&]{batch.KeepImage(rgba,static_cast<const VulkanTexture&>(*rgba).allocationInfo.size);
        pass.Clear(direct.get(),nullptr,true,false,false,full,{-.375f,0,0,0},0,0);
        pass.Resolve(*rgba,*direct,src,dst,0,PassTransfer::Conversion::DepthUNorm24);
        Require(candidate_memory.StoreDepthFromRgbaStencil(*direct,layout,dst,address,*rgba,dx,dy),"Eligible RGBA stencil store refused after accepted Resolve");
        if(last)rgba.reset();});
      Require(work.accepted_work()>before&&work.pending()&&!weak.expired(),"Direct source did not survive its real accepted packet");
      batch.Flush();Require(!work.pending()&&work.accepted_work()==work.completed_work()&&(!last||weak.expired()),"Direct source packet owners did not retire at covering fence");
      if(last)++result.source_owner_witnesses;
      const auto old_image=ReadR32Atlas(device,queue,*original),new_image=ReadR32Atlas(device,queue,*direct);
      Require(old_image==new_image,"Direct RGBA Resolve changed exact full R32 output image");
      auto expected=poison;uint64_t alpha_witness=0;
      for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x){const size_t pixel=size_t(y)*w+x;uint32_t actual;std::memcpy(&actual,new_image.data()+pixel*4,4);
        if(!in(dst,x,y)){Require(actual==std::bit_cast<uint32_t>(-.375f),"Resolve changed outside poisoned image pixels");continue;}
        const size_t source_pixel=size_t(int32_t(y)+dy)*w+uint32_t(int32_t(x)+dx);uint32_t z;
        std::memcpy(&z,source_bits.depth.data()+source_pixel*4,4);
        const auto first=AtlasHostQuantizeOracle(z);const auto resolved=std::bit_cast<uint32_t>(float(first+(first>>23))/16777216.f);
        Require(actual==resolved,"Unchanged first Resolve quantization disagreed with independent source bit oracle");
        const uint32_t stencil=source_bits.stencil[source_pixel],packed=(AtlasHostQuantizeOracle(resolved)<<8)|stencil;
        const uint64_t at=address+offset(x,y,tiled);Require(at+4<=expected.size(),"Destination physical oracle escaped full buffer");
        for(unsigned byte=0;byte<4;++byte)expected[at+byte]=uint8_t(packed>>(8*(3-byte))); // actual endian2
        alpha_witness+=stencil!=255;
      }
      Require(alpha_witness>0,"Original source alpha stencil proof was vacuous");result.original_alpha_witness_pixels+=alpha_witness;
      Require(reference_memory.ReadBytes(0,edram_bytes)==expected&&candidate_memory.ReadBytes(0,edram_bytes)==expected,"Direct RGBA store changed full10MiB/offset/endian/stencil/depth oracle");
      Require(source_memory.SurfaceVersion(surface)==source_version&&source_memory.ReadBytes(0,edram_bytes)==source_packed,"Read-only direct Resolve altered packed source authority");
      ++result.resolves;++result.whole_memory_compares;if(receipt_source)++result.receipt_sources;else ++result.decoded_sources;
    }
  }
  const auto restores=pass.restore_statistics();
  Require(restores.calls==4&&restores.export_failures==0&&restores.draws==(export_stencil?4u:36u)&&
      restores.export_calls==(export_stencil?4u:0u)&&restores.legacy_calls==(export_stencil?0u:4u)&&
      restores.stencil_clears==(export_stencil?0u:4u),"RGBA stencil fixture did not use its exact original restore path");
  Require(result.resolves==12&&result.decoded_sources==6&&result.receipt_sources==6&&
      result.whole_memory_compares==12&&result.source_owner_witnesses==4,
      "RGBA stencil fixture did not cover every source/offset/layout case");
  const auto stats=candidate_memory.DescriptorPoolStatistics();Require(stats.hits>0&&stats.factory_failures==0&&stats.leased_sets==0&&
      stats.requests==stats.hits+stats.misses&&stats.live_sets==stats.idle_sets+stats.leased_sets&&stats.live_sets<=128,"Original store-family descriptor reuse/lease invariant failed");
  result.descriptor_hits=stats.hits;result.accepted_work=work.accepted_work();result.completed_work=work.completed_work();
  Require(result.accepted_work>0&&result.accepted_work==result.completed_work&&!work.pending()&&!work.inflight_batches()&&!work.failed(),"RGBA stencil fixture left accepted work pending");
  return result;
}

// All actual original FP24 decode->D32 viewport clamp->StoreSurface anchors.
// This validates dynamic GLSL exp2 on the real driver, not a CPU ldexp proxy.
struct PackedStencilResult {
  uint64_t unique_codes=0,chunks=0,full_edram_pairs=0,host_image_pairs=0,
      source_owner_witnesses=0,above_unit_pixels=0,accepted_work=0,completed_work=0;
};
uint32_t PrimaryFp24DecodeBitsOracle(uint32_t d) {
  const uint32_t e=d>>20,m=d&0xfffff;
  if(e)return ((e+112)<<23)|(m<<3);
  if(!m)return 0;
  const uint32_t k=31u-std::countl_zero(m);
  return ((k+93)<<23)|((m-(1u<<k))<<(23-k));
}
PackedStencilResult RunPackedPrimaryStencilClear(RenderDevice& device,RenderCommandQueue& queue,
                                                bool export_stencil,uint32_t capacity) {
  constexpr uint32_t w=1280,h=720,pixels=w*h,codes=0x1000000;
  const SurfaceLayout surface{w,h,w,0,1,true};const RenderRect full(0,0,w,h);
  const std::array<RenderRect,4> rectangles{full,RenderRect(41,17,179,91),
      RenderRect(79,15,82,18),RenderRect(1206,646,1280,720)};
  auto original=device.createTexture(RenderTextureDesc::DepthTarget(w,h,RenderFormat::D32_FLOAT_S8_UINT));
  auto future=device.createTexture(RenderTextureDesc::DepthTarget(w,h,RenderFormat::D32_FLOAT_S8_UINT));
  Require(original&&future,"Primary stencil fixture attachment allocation failed");
  NativeGpuWorkQueue work(device,queue,{},true,false,capacity);NativeTransferBatch batch(device,queue,&work);
  PassTransfer pass(device,queue,fullscreen_spv,resolve_spv,restore_depth_spv,&batch,
      export_stencil?std::span<const uint32_t>(restore_depth_stencil_spv):std::span<const uint32_t>{});
  ResolveMemory baseline(device,queue,memory_store_spv,memory_load_spv,edram_bytes,0,&batch);
  ResolveMemory candidate(device,queue,memory_store_spv,memory_load_spv,edram_bytes,0,&batch);
  baseline.SetDescriptorPoolEnabled(true);candidate.SetDescriptorPoolEnabled(true);
  Require(candidate.EnablePackedStencilClear(packed_primary_stencil_clear_spv)&&candidate.SupportsPackedStencilClear(),
          "Packed primary stencil pipeline unavailable");
  struct DrainOnExit{NativeTransferBatch& batch;~DrainOnExit(){try{batch.Flush();}catch(...){std::terminate();}}}drain{batch};
  const auto word_index=[](uint32_t x,uint32_t y){const uint32_t column=x%80;
    return ((y/16)*16+x/80)*1280+(y%16)*80+(column<40?column+40:column-40);};
  const auto inside=[](const RenderRect& r,uint32_t x,uint32_t y){return x>=uint32_t(r.left)&&x<uint32_t(r.right)&&y>=uint32_t(r.top)&&y<uint32_t(r.bottom);};
  const auto word=[](const std::vector<uint8_t>& data,uint32_t at){uint32_t value;std::memcpy(&value,data.data()+at,4);return value;};
  const auto put=[](std::vector<uint8_t>& data,uint32_t at,uint32_t value){std::memcpy(data.data()+at,&value,4);};
  Require(std::endian::native==std::endian::little,"Primary packed stencil oracle requires little-endian host");
  const auto initial_accepted=work.accepted_work();
  Require(!candidate.ClearPrimaryStencilFromPacked(surface,full,0)&&work.accepted_work()==initial_accepted,
          "Undefined primary stencil clear was not a pre-record refusal");
  PackedStencilResult result;
  for(uint32_t first=0,ordinal=0;first<codes;first+=pixels,++ordinal){
    const auto unique=std::min(pixels,codes-first);const auto& r=rectangles[ordinal%rectangles.size()];
    const uint32_t stencil=(ordinal*53+0xa5)&255;
    std::vector<uint8_t> seed(edram_bytes);
    for(size_t i=0;i<seed.size();++i)seed[i]=uint8_t(i*29+(i>>8)+ordinal*17);
    for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x){const uint32_t p=y*w+x;
      const uint32_t d=first+std::min(p,unique-1),s=(p*17+(p>>11)*29+ordinal*53)&255;
      put(seed,word_index(x,y)*4,(d<<8)|s);
    }
    baseline.Initialize(0,seed);candidate.Initialize(0,seed);
    auto expected=seed;
    for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x){const auto at=word_index(x,y)*4,previous=word(seed,at);
      const uint32_t d=previous>>8;const auto normalized=d<=0xf00000?d:0xf00000;
      put(expected,at,(normalized<<8)|(inside(r,x,y)?stencil:(previous&255)));
      result.above_unit_pixels+=d>0xf00000;
    }
    std::weak_ptr<RenderTexture> original_source;
    batch.Run([&]{auto source=baseline.LoadSurfaceShared(surface);original_source=source;
      pass.RestoreDepth(*source,*original);
      pass.Clear(nullptr,original.get(),false,false,true,r,{},0.f,stencil);
      baseline.StoreSurface(*original,surface);});
    Require(work.pending()&&!original_source.expired(),"Original primary decode owner did not survive accepted packet");
    batch.Flush();Require(original_source.expired(),"Original primary decode owner did not retire at full fence");
    ++result.source_owner_witnesses;
    const auto token=candidate.SurfaceVersion(surface),accepted=work.accepted_work();
    Require(!candidate.ClearPrimaryStencilFromPacked(surface,RenderRect(-1,0,1,1),stencil)&&
        candidate.SurfaceVersion(surface)==token&&work.accepted_work()==accepted&&
        candidate.ReadBytes(0,edram_bytes)==seed,"Invalid stencil rectangle changed accepted work/token/bytes");
    batch.Run([&]{Require(candidate.ClearPrimaryStencilFromPacked(surface,r,stencil),"Eligible packed stencil clear refused");});
    Require(work.pending()&&work.accepted_work()>accepted&&candidate.SurfaceVersion(surface)==token+1,
        "Packed stencil clear did not publish one actual full-footprint accepted version");
    batch.Flush();
    Require(baseline.ReadBytes(0,edram_bytes)==expected&&candidate.ReadBytes(0,edram_bytes)==expected,
        "Actual FP24 exp2/clamp/encode or packed clear disagrees with exhaustive whole10MiB oracle");
    // A later genuine host consumer must obtain the SAME post-clear D32/S8
    // bits without any receipt or premature whole-host version promotion.
    std::weak_ptr<RenderTexture> future_source;
    batch.Run([&]{auto source=candidate.LoadSurfaceShared(surface);future_source=source;pass.RestoreDepth(*source,*future);});
    Require(work.pending()&&!future_source.expired(),"Future packed decode owner did not survive its consumer packet");
    batch.Flush();Require(future_source.expired(),"Future packed decode owner survived its full covering fence");
    ++result.source_owner_witnesses;
    const auto before=ReadDepthStencil(device,queue,*original),after=ReadDepthStencil(device,queue,*future);
    Require(before==after,"Deferred no-receipt primary host consumer differs from original cached host bits");
    for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x){const auto p=size_t(y)*w+x;const auto value=word(expected,word_index(x,y)*4);
      const auto bits=PrimaryFp24DecodeBitsOracle(value>>8);uint32_t actual;std::memcpy(&actual,after.depth.data()+p*4,4);
      Require(actual==bits&&after.stencil[p]==(value&255),"Actual future host bits differ from independent clamped FP24/S8 oracle");
    }
    Require(candidate.ReadBytes(0,edram_bytes)==expected,"Future primary host read altered packed authority");
    result.unique_codes+=unique;++result.chunks;++result.full_edram_pairs;++result.host_image_pairs;
  }
  const auto restores=pass.restore_statistics();
  Require(result.unique_codes==codes&&result.chunks==19&&result.above_unit_pixels>0&&
      restores.calls==38&&restores.export_failures==0&&restores.draws==(export_stencil?38u:342u)&&
      restores.export_calls==(export_stencil?38u:0u)&&restores.legacy_calls==(export_stencil?0u:38u)&&
      restores.stencil_clears==(export_stencil?0u:38u),"Actual exhaustive primary stencil restore path/coverage incomplete");
  result.accepted_work=work.accepted_work();result.completed_work=work.completed_work();
  Require(result.accepted_work>0&&result.accepted_work==result.completed_work&&!work.pending()&&!work.inflight_batches()&&!work.failed(),
      "Primary stencil fixture left accepted work pending");
  return result;
}

// Secondary address/padding qualification; same previously exhaustive FP24 math.
PackedStencilResult RunPackedSecondaryStencilClear(RenderDevice& device,RenderCommandQueue& queue,
                                                bool export_stencil,uint32_t capacity) {
  constexpr uint32_t w=640,h=360,pixels=w*h,codes=128;
  const SurfaceLayout surface{w,h,w,1440,1,true};const RenderRect full(0,0,w,h);
  const std::array<RenderRect,4> rectangles{full,RenderRect(41,17,179,91),
      RenderRect(39,15,82,18),RenderRect(79,351,640,360)};
  auto original=device.createTexture(RenderTextureDesc::DepthTarget(w,h,RenderFormat::D32_FLOAT_S8_UINT));
  auto future=device.createTexture(RenderTextureDesc::DepthTarget(w,h,RenderFormat::D32_FLOAT_S8_UINT));
  Require(original&&future,"Secondary stencil fixture attachment allocation failed");
  NativeGpuWorkQueue work(device,queue,{},true,false,capacity);NativeTransferBatch batch(device,queue,&work);
  PassTransfer pass(device,queue,fullscreen_spv,resolve_spv,restore_depth_spv,&batch,
      export_stencil?std::span<const uint32_t>(restore_depth_stencil_spv):std::span<const uint32_t>{});
  ResolveMemory baseline(device,queue,memory_store_spv,memory_load_spv,edram_bytes,0,&batch);
  ResolveMemory candidate(device,queue,memory_store_spv,memory_load_spv,edram_bytes,0,&batch);
  baseline.SetDescriptorPoolEnabled(true);candidate.SetDescriptorPoolEnabled(true);
  Require(candidate.EnablePackedSecondaryStencilClear(packed_secondary_stencil_clear_spv)&&candidate.SupportsPackedSecondaryStencilClear(),
          "Packed secondary stencil pipeline unavailable");
  struct DrainOnExit{NativeTransferBatch& batch;~DrainOnExit(){try{batch.Flush();}catch(...){std::terminate();}}}drain{batch};
  const auto word_index=[](uint32_t x,uint32_t y){const uint32_t column=x%80;
    return (1440+(y/16)*8+x/80)*1280+(y%16)*80+(column<40?column+40:column-40);};
  const auto inside=[](const RenderRect& r,uint32_t x,uint32_t y){return x>=uint32_t(r.left)&&x<uint32_t(r.right)&&y>=uint32_t(r.top)&&y<uint32_t(r.bottom);};
  const auto word=[](const std::vector<uint8_t>& data,uint32_t at){uint32_t value;std::memcpy(&value,data.data()+at,4);return value;};
  const auto put=[](std::vector<uint8_t>& data,uint32_t at,uint32_t value){std::memcpy(data.data()+at,&value,4);};
  Require(std::endian::native==std::endian::little,"Secondary packed stencil oracle requires little-endian host");
  const auto initial_accepted=work.accepted_work();
  Require(!candidate.ClearSecondaryStencilFromPacked(surface,full,0)&&work.accepted_work()==initial_accepted,
          "Undefined secondary stencil clear was not a pre-record refusal");
  PackedStencilResult result;
  constexpr std::array<uint32_t,8> mantissas{0,1,2,0x3ffff,0x7ffff,0xffffd,0xffffe,0xfffff};
  std::array<bool,codes> seen_codes{};std::array<bool,256> seen_stencil{};
  const SurfaceLayout overlapping_atlas{1120,688,1120,1440,0,true};
  const SurfaceLayout adjacent_color{640,360,640,1624,6,false};
  for(uint32_t ordinal=0;ordinal<rectangles.size();++ordinal){
    const auto& r=rectangles[ordinal];
    const uint32_t stencil=(ordinal*53+0xa5)&255;
    std::vector<uint8_t> seed(edram_bytes);
    for(size_t i=0;i<seed.size();++i)seed[i]=uint8_t(i*29+(i>>8)+ordinal*17);
    for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x){const uint32_t p=y*w+x;
      const uint32_t anchor=(p+ordinal*13)&127u;
      const uint32_t d=((anchor>>3)<<20)|mantissas[anchor&7],s=(p*17+(p>>11)*29+ordinal*53)&255;
      seen_codes[anchor]=true;seen_stencil[s]=true;
      put(seed,word_index(x,y)*4,(d<<8)|s);
    }
    baseline.Initialize(0,seed);candidate.Initialize(0,seed);
    auto expected=seed;
    for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x){const auto at=word_index(x,y)*4,previous=word(seed,at);
      const uint32_t d=previous>>8;const auto normalized=d<=0xf00000?d:0xf00000;
      put(expected,at,(normalized<<8)|(inside(r,x,y)?stencil:(previous&255)));
      result.above_unit_pixels+=d>0xf00000;
    }
    std::weak_ptr<RenderTexture> original_source;
    batch.Run([&]{auto source=baseline.LoadSurfaceShared(surface);original_source=source;
      pass.RestoreDepth(*source,*original);
      pass.Clear(nullptr,original.get(),false,false,true,r,{},0.f,stencil);
      baseline.StoreSurface(*original,surface);});
    Require(work.pending()&&!original_source.expired(),"Original secondary decode owner did not survive accepted packet");
    batch.Flush();Require(original_source.expired(),"Original secondary decode owner did not retire at full fence");
    ++result.source_owner_witnesses;
    const auto token=candidate.SurfaceVersion(surface),accepted=work.accepted_work();
    const auto atlas_token=candidate.SurfaceVersion(overlapping_atlas);
    const auto adjacent_token=candidate.SurfaceVersion(adjacent_color);
    Require(atlas_token==token,"Initial secondary/overlapping-atlas token witness differs");
    Require(!candidate.ClearSecondaryStencilFromPacked(surface,RenderRect(-1,0,1,1),stencil)&&
        candidate.SurfaceVersion(surface)==token&&work.accepted_work()==accepted&&
        candidate.ReadBytes(0,edram_bytes)==seed,"Invalid stencil rectangle changed accepted work/token/bytes");
    batch.Run([&]{Require(candidate.ClearSecondaryStencilFromPacked(surface,r,stencil),"Eligible packed stencil clear refused");});
    Require(work.pending()&&work.accepted_work()>accepted&&candidate.SurfaceVersion(surface)==token+1&&
        candidate.SurfaceVersion(overlapping_atlas)==atlas_token+1&&candidate.SurfaceVersion(adjacent_color)==adjacent_token,
        "Packed stencil clear did not publish one actual full-footprint accepted version");
    batch.Flush();
    Require(baseline.ReadBytes(0,edram_bytes)==expected&&candidate.ReadBytes(0,edram_bytes)==expected,
        "Secondary FP24 clear differs from actual original or full10MiB oracle including tailpadding/outside");
    // A later genuine host consumer must obtain the SAME post-clear D32/S8
    // bits without any receipt or premature whole-host version promotion.
    std::weak_ptr<RenderTexture> future_source;
    batch.Run([&]{auto source=candidate.LoadSurfaceShared(surface);future_source=source;pass.RestoreDepth(*source,*future);});
    Require(work.pending()&&!future_source.expired(),"Future packed decode owner did not survive its consumer packet");
    batch.Flush();Require(future_source.expired(),"Future packed decode owner survived its full covering fence");
    ++result.source_owner_witnesses;
    const auto before=ReadDepthStencil(device,queue,*original),after=ReadDepthStencil(device,queue,*future);
    Require(before==after,"Deferred no-receipt secondary host consumer differs from original cached host bits");
    for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x){const auto p=size_t(y)*w+x;const auto value=word(expected,word_index(x,y)*4);
      const auto bits=PrimaryFp24DecodeBitsOracle(value>>8);uint32_t actual;std::memcpy(&actual,after.depth.data()+p*4,4);
      Require(actual==bits&&after.stencil[p]==(value&255),"Actual future host bits differ from independent clamped FP24/S8 oracle");
    }
    Require(candidate.ReadBytes(0,edram_bytes)==expected,"Future secondary host read altered packed authority");
    ++result.chunks;++result.full_edram_pairs;++result.host_image_pairs;
  }
  for(bool seen:seen_codes){Require(seen,"Secondary anchored FP24 code coverage incomplete");result.unique_codes+=seen;}
  for(bool seen:seen_stencil)Require(seen,"Secondary source stencil code coverage incomplete");
  const auto restores=pass.restore_statistics();
  Require(result.unique_codes==codes&&result.chunks==4&&result.above_unit_pixels>0&&
      restores.calls==8&&restores.export_failures==0&&restores.draws==(export_stencil?8u:72u)&&
      restores.export_calls==(export_stencil?8u:0u)&&restores.legacy_calls==(export_stencil?0u:8u)&&
      restores.stencil_clears==(export_stencil?0u:8u),"Actual secondary anchored restore path/coverage incomplete");
  result.accepted_work=work.accepted_work();result.completed_work=work.completed_work();
  Require(result.accepted_work>0&&result.accepted_work==result.completed_work&&!work.pending()&&!work.inflight_batches()&&!work.failed(),
      "Secondary stencil fixture left accepted work pending");
  return result;
}

// This subcase exercises the actual production crop primitives. Full Scene
// replay separately validates stale whole-Surface-version orchestration.
struct AtlasResult {
  uint64_t rectangles = 0, cropped_pixels = 0, reference_pixels = 0;
  uint64_t accepted_work = 0, completed_work = 0, scratch_owner_witnesses = 0;
};
AtlasResult RunAtlasRegion(RenderDevice &device, RenderCommandQueue &queue,
                          bool export_stencil) {
  constexpr uint32_t w = 1120, h = 688;
  const SurfaceLayout surface{w, h, w, 1440, 0, true};
  const RenderRect full(0, 0, w, h);
  const std::array<RenderRect, 4> rectangles{
      RenderRect(0, 0, 160, 96), RenderRect(41, 17, 179, 91),
      RenderRect(79, 15, 82, 18), RenderRect(1046, 614, 1120, 688)};
  auto reference = device.createTexture(RenderTextureDesc::DepthTarget(
      w, h, RenderFormat::D32_FLOAT_S8_UINT));
  auto target = device.createTexture(RenderTextureDesc::DepthTarget(
      w, h, RenderFormat::D32_FLOAT_S8_UINT));
  Require(reference && target, "Cannot allocate atlas fixture attachments");
  NativeGpuWorkQueue work(device, queue, {}, true, false, 64);
  NativeTransferBatch batch(device, queue, &work);
  PassTransfer pass(device, queue, fullscreen_spv, resolve_spv, restore_depth_spv,
                    &batch, export_stencil
                                ? std::span<const uint32_t>(restore_depth_stencil_spv)
                                : std::span<const uint32_t>{});
  ResolveMemory edram(device, queue, memory_store_spv, memory_load_spv,
                      edram_bytes, 0, &batch);
  // Independent 80x16 depth half-tile address oracle. This exact atlas ends
  // before tile2048; no actual wrap coverage is claimed by this subcase.
  const auto offset = [](uint32_t x, uint32_t y) {
    const uint32_t tile = 1440 + (y / 16) * 14 + x / 80;
    return (tile * 1280 + (y % 16) * 80 + (x % 80 + 40) % 80) * 4;
  };
  constexpr std::array<uint32_t, 10> depths{
      0, 1, 0x3fffff, 0x400000, 0x400001, 0x7fffff, 0x800000,
      0x800001, 0xfffffe, 0xffffff};
  const auto word = [&](uint32_t x, uint32_t y) {
    return (depths[(x + y * 3) % depths.size()] << 8) |
           ((x * 17 + y * 29) & 255);
  };
  std::vector<uint8_t> seed(edram_bytes);
  for (size_t at = 0; at < seed.size(); ++at)
    seed[at] = uint8_t(at * 29 + (at >> 8));
  for (uint32_t y = 0; y < h; ++y)
    for (uint32_t x = 0; x < w; ++x) {
      const uint32_t value = word(x, y), address = offset(x, y);
      Require(address + 4 <= seed.size(), "Atlas oracle escaped packed memory");
      for (uint32_t byte = 0; byte < 4; ++byte)
        seed[address + byte] = uint8_t(value >> (byte * 8));
    }
  // Declared last so every accepted prefix is fenced before any image,
  // framebuffer, descriptor, packed buffer or pipeline owner is destroyed.
  struct DrainOnExit {
    NativeTransferBatch &batch;
    ~DrainOnExit() {
      try { batch.Flush(); } catch (...) { std::terminate(); }
    }
  } drain{batch};
  edram.Initialize(0, seed);
  AtlasResult result;
  const auto restore = [&](RenderTexture &destination, const RenderRect *region) {
    std::weak_ptr<RenderTexture> scratch;
    const auto accepted_before = work.accepted_work();
    batch.Run([&] {
      auto source = region ? edram.LoadSurfaceSharedRegion(surface, *region)
                           : edram.LoadSurfaceShared(surface);
      scratch = source;
      pass.RestoreDepth(*source, destination, region);
    });
    Require(work.accepted_work() > accepted_before && work.pending() &&
                work.completed_work() < work.accepted_work() && !scratch.expired(),
            "Atlas queued restore did not retain its source through the fence");
    batch.Flush();
    Require(work.accepted_work() == work.completed_work() && !work.pending() &&
                !work.inflight_batches() && scratch.expired(),
            "Atlas queued restore did not retire its actual source owner");
    ++result.scratch_owner_witnesses;
  };
  restore(*reference, nullptr);
  const auto canonical = ReadDepthStencil(device, queue, *reference);
  for (uint32_t y = 0; y < h; ++y)
    for (uint32_t x = 0; x < w; ++x) {
      const uint32_t d = word(x, y) >> 8;
      const uint32_t expected = std::bit_cast<uint32_t>(
          float(d + (d >> 23)) / 16777216.f);
      const size_t index = size_t(y) * w + x;
      uint32_t actual;
      std::memcpy(&actual, canonical.depth.data() + index * 4, 4);
      Require(actual == expected && canonical.stencil[index] == (word(x, y) & 255),
              "Full canonical atlas restore failed the independent pixel oracle");
    }
  constexpr float sentinel_depth = 0.625f;
  constexpr uint8_t sentinel_stencil = 0xa5;
  const auto sentinel_bits = std::bit_cast<uint32_t>(sentinel_depth);
  for (const auto &rectangle : rectangles) {
    batch.Run([&] {
      pass.Clear(nullptr, target.get(), false, true, true, full, {},
                 sentinel_depth, sentinel_stencil);
    });
    batch.Flush();
    const auto before = ReadDepthStencil(device, queue, *target);
    for (size_t index = 0; index < size_t(w) * h; ++index) {
      uint32_t bits;
      std::memcpy(&bits, before.depth.data() + index * 4, 4);
      Require(bits == sentinel_bits && before.stencil[index] == sentinel_stencil,
              "Atlas sentinel initialization was not exact");
    }
    restore(*target, &rectangle);
    const auto partial = ReadDepthStencil(device, queue, *target);
    uint64_t changed_inside = 0, preserved_outside = 0;
    for (uint32_t y = 0; y < h; ++y)
      for (uint32_t x = 0; x < w; ++x) {
        const size_t index = size_t(y) * w + x;
        const bool inside = x >= uint32_t(rectangle.left) &&
                            x < uint32_t(rectangle.right) &&
                            y >= uint32_t(rectangle.top) &&
                            y < uint32_t(rectangle.bottom);
        const auto &expected = inside ? canonical : before;
        Require(std::memcmp(partial.depth.data() + index * 4,
                            expected.depth.data() + index * 4, 4) == 0 &&
                    partial.stencil[index] == expected.stencil[index],
                "Cropped restore changed outside depth/stencil or lost inside texels");
        if (inside)
          changed_inside += std::memcmp(partial.depth.data() + index * 4,
                                        before.depth.data() + index * 4, 4) != 0 ||
                            partial.stencil[index] != before.stencil[index];
        else
          ++preserved_outside;
      }
    Require(changed_inside && preserved_outside,
            "Atlas inside/outside preservation witness was vacuous");
    // An outside partial clear deliberately leaves stale host pixels. A later
    // generic full source restoration must repair this SAME image everywhere.
    const RenderRect outside(500, 300, 517, 311);
    batch.Run([&] {
      pass.Clear(nullptr, target.get(), false, true, true, outside, {}, .875f, 0x3c);
    });
    batch.Flush();
    const auto after_clear = ReadDepthStencil(device, queue, *target);
    Require(after_clear != canonical, "Atlas stale-after-clear witness was vacuous");
    for (uint32_t y = 300; y < 311; ++y)
      for (uint32_t x = 500; x < 517; ++x) {
        uint32_t bits;
        const size_t index = size_t(y) * w + x;
        std::memcpy(&bits, after_clear.depth.data() + index * 4, 4);
        Require(bits == std::bit_cast<uint32_t>(.875f) &&
                    after_clear.stencil[index] == 0x3c,
                "Atlas outside partial clear did not actually alter the image");
      }
    restore(*target, nullptr);
    Require(ReadDepthStencil(device, queue, *target) == canonical,
            "Generic full restore did not repair the cropped image outside pixels");
    ++result.rectangles;
    result.cropped_pixels += uint64_t(rectangle.right - rectangle.left) *
                             (rectangle.bottom - rectangle.top);
    result.reference_pixels += uint64_t(w) * h;
  }
  Require(edram.ReadBytes(0, edram_bytes) == seed,
          "Atlas source crop/clear/repair changed packed history or untouched bytes");
  const auto stats = pass.restore_statistics();
  Require(stats.calls == 9 && stats.export_failures == 0 &&
              stats.draws == (export_stencil ? 9u : 81u) &&
              stats.export_calls == (export_stencil ? 9u : 0u) &&
              stats.legacy_calls == (export_stencil ? 0u : 9u) &&
              stats.stencil_clears == (export_stencil ? 0u : 9u),
          "Atlas fixture did not execute its exact queued restore path");
  result.accepted_work = work.accepted_work();
  result.completed_work = work.completed_work();
  Require(result.accepted_work && result.accepted_work == result.completed_work &&
              !work.pending() && !work.inflight_batches() && !work.failed(),
          "Atlas fixture left accepted GPU work incomplete");
  return result;
}
std::unique_ptr<RenderTexture>
Source(RenderDevice &device, RenderCommandQueue &queue, bool nonfinite) {
  std::array<float, height> depths{0.0f,
                                   -0.0f,
                                   std::numeric_limits<float>::denorm_min(),
                                   std::numeric_limits<float>::min(),
                                   std::nextafter(0.5f, 0.0f),
                                   0.5f,
                                   std::nextafter(0.5f, 1.0f),
                                   std::nextafter(1.0f, 0.0f),
                                   1.0f,
                                   0.25f,
                                   0.75f,
                                   1.f / 16777215.f,
                                   0.125f,
                                   0.375f,
                                   0.625f,
                                   0.875f};
  if (nonfinite) {
    depths[12] = -1.0f;
    depths[13] = std::numeric_limits<float>::infinity();
    depths[14] = -std::numeric_limits<float>::infinity();
    depths[15] = std::bit_cast<float>(0x7fc12345u);
  }
  auto image = device.createTexture(RenderTextureDesc::ColorTarget(
      width, height, RenderFormat::R32G32B32A32_FLOAT));
  auto upload =
      device.createBuffer(RenderBufferDesc::UploadBuffer(width * height * 16));
  auto bytes = static_cast<uint8_t *>(upload->map());
  for (uint32_t y = 0; y < height; ++y)
    for (uint32_t x = 0; x < width; ++x) {
      const std::array<float, 4> value{depths[y], 0, 0, float(x) / 255.f};
      std::memcpy(bytes + (y * width + x) * 16, value.data(), 16);
    }
  ::validation::Flush(*upload);
  upload->unmap();
  auto commands = queue.createCommandList();
  auto fence = device.createCommandFence();
  commands->begin();
  commands->barriers(
      RenderBarrierStage::COPY,
      RenderTextureBarrier(image.get(), RenderTextureLayout::COPY_DEST));
  commands->copyTextureRegion(
      RenderTextureCopyLocation::Subresource(image.get(), 0),
      RenderTextureCopyLocation::PlacedFootprint(
          upload.get(), RenderFormat::R32G32B32A32_FLOAT, width, height, 1,
          width),
      0, 0, 0, nullptr);
  commands->end();
  queue.executeCommandLists(commands.get(), fence.get());
  queue.waitForCommandFence(fence.get());
  return image;
}
std::vector<uint64_t> Probe(RenderDevice &device, RenderCommandQueue &queue,
                            RenderTexture &depth) {
  auto vertex = device.createShader(fullscreen_spv, sizeof(fullscreen_spv),
                                    "main", RenderShaderFormat::SPIRV);
  auto pixel = device.createShader(query_test_spv, sizeof(query_test_spv),
                                   "main", RenderShaderFormat::SPIRV);
  auto layout = device.createPipelineLayout(RenderPipelineLayoutDesc{});
  RenderFramebufferDesc fd;
  fd.depthAttachment = &depth;
  auto framebuffer = device.createFramebuffer(fd);
  auto commands = queue.createCommandList();
  auto fence = device.createCommandFence();
  NativeQueryTracker queries(device);
  std::vector<uint64_t> results;
  for (unsigned i = 0; i < 7; ++i) {
    const std::array<uint32_t, 5> references{0, 1, 0x55, 0xaa, 255};
    RenderGraphicsPipelineDesc p;
    p.pipelineLayout = layout.get();
    p.vertexShader = vertex.get();
    p.pixelShader = pixel.get();
    p.renderTargetCount = 0;
    p.depthTargetFormat = RenderFormat::D32_FLOAT_S8_UINT;
    p.depthEnabled = true;
    p.depthWriteEnabled = false;
    p.depthFunction = i < 5 ? RenderComparisonFunction::ALWAYS
                            : RenderComparisonFunction::EQUAL;
    p.stencilEnabled = i != 5;
    p.stencilReadMask = 255;
    p.stencilWriteMask = 0;
    p.stencilReference = i < 5 ? references[i] : 7;
    p.stencilFrontFace.compareFunction = RenderComparisonFunction::EQUAL;
    p.stencilBackFace = p.stencilFrontFace;
    auto pipeline = device.createGraphicsPipeline(p);
    Require(pipeline &&
                static_cast<VulkanGraphicsPipeline *>(pipeline.get())->vk,
            "Query probe pipeline failed");
    queries.Begin(0x1000, 0x2020);
    commands->begin();
    commands->barriers(
        RenderBarrierStage::GRAPHICS,
        RenderTextureBarrier(&depth, RenderTextureLayout::DEPTH_WRITE));
    commands->setFramebuffer(framebuffer.get());
    auto viewport = RenderViewport(0, 0, width, height);
    viewport.minDepth = viewport.maxDepth = 0.5f;
    commands->setViewports(viewport);
    commands->setScissors(RenderRect(0, 0, width, height));
    commands->setGraphicsPipelineLayout(layout.get());
    commands->setPipeline(pipeline.get());
    queries.ResetDraw(*commands);
    queries.BeginDraw(*commands);
    commands->drawInstanced(3, 1, 0, 0);
    queries.EndDraw(*commands);
    commands->end();
    queue.executeCommandLists(commands.get(), fence.get());
    queue.waitForCommandFence(fence.get());
    queries.CompleteDraw();
    const auto result = queries.End(0x1000, 0x2000);
    results.push_back(result.samples);
    Require(result.draws == 1 && result.samples == (i < 5    ? height
                                                    : i == 5 ? width
                                                             : 1),
            "Restored hardware depth/stencil did not produce exact expected "
            "query samples");
  }
  return results;
}
struct Result {
  AttachmentBytes attachment, after_partial;
  std::vector<uint8_t> packed;
  std::vector<uint64_t> queries;
  PassTransfer::RestoreStatistics restores;
  uint64_t submissions = 0;
};
Result Run(RenderDevice &device, RenderCommandQueue &queue, bool enabled,
           bool batched, bool nonfinite, unsigned format) {
  auto source = Source(device, queue, nonfinite);
  auto target = device.createTexture(RenderTextureDesc::DepthTarget(
      width, height, RenderFormat::D32_FLOAT_S8_UINT));
  NativeGpuWorkQueue work(device, queue, {}, true, false, 64);
  NativeTransferBatch batch(device, queue, &work);
  PassTransfer pass(device, queue, fullscreen_spv, resolve_spv,
                    restore_depth_spv, batched ? &batch : nullptr,
                    enabled
                        ? std::span<const uint32_t>(restore_depth_stencil_spv)
                        : std::span<const uint32_t>{});
  auto perform = [&](const std::function<void()> &fn) {
    if (batched) {
      batch.Run(fn);
      Require(work.pending(), "Batched restore did not retain GPU work");
      batch.Flush();
    } else
      fn();
  };
  // Distinct initial values prove the one-pass path replaces zero bits too;
  // retaining a stale stencil bit cannot hide behind zero initialization.
  pass.Clear(nullptr, target.get(), false, true, true, {0, 0, width, height},
             {}, 0.75f, 0xff);
  perform([&] { pass.RestoreDepth(*source, *target); });
  Result result;
  result.attachment = ReadDepthStencil(device, queue, *target);
  for (uint32_t y = 0; y < height; ++y)
    for (uint32_t x = 0; x < width; ++x)
      Require(result.attachment.stencil[y * width + x] == x,
              "Stencil export lost an exact 8-bit value");
  result.queries = Probe(device, queue, *target);
  perform([&] {
    pass.Clear(nullptr, target.get(), false, false, true, {13, 3, 141, 13}, {},
               0, 0xa9);
    pass.Clear(nullptr, target.get(), false, true, false, {127, 1, 231, 9}, {},
               0.625f, 0);
  });
  result.after_partial = ReadDepthStencil(device, queue, *target);
  ResolveMemory edram(device, queue, memory_store_spv, memory_load_spv,
                      edram_bytes, 0, batched ? &batch : nullptr);
  std::vector<uint8_t> seed(edram_bytes);
  for (size_t i = 0; i < seed.size(); ++i)
    seed[i] = uint8_t(i * 29 + (i >> 8));
  edram.Initialize(0, seed);
  const SurfaceLayout surface{width, height, 320, 2047, format, true};
  perform([&] { edram.StoreSurface(*target, surface); });
  result.packed = edram.ReadBytes(0, edram_bytes);
  result.restores = pass.restore_statistics();
  result.submissions = work.statistics().submissions;
  Require(result.restores.calls == 1 && result.restores.export_failures == 0 &&
              result.restores.draws == (enabled ? 1 : 9) &&
              result.restores.export_calls == uint64_t(enabled) &&
              result.restores.stencil_clears == uint64_t(!enabled),
          "Restore did not execute the selected1/9 draw implementation");
  // Existing malformed/aliased inputs stay rejected before attachment writes.
  bool rejected = false;
  try {
    pass.RestoreDepth(*target, *target);
  } catch (const std::runtime_error &) {
    rejected = true;
  }
  Require(rejected && pass.restore_statistics().calls == 1,
          "Aliased restore changed validation order");
  return result;
}
} // namespace
int main(int argc, char **argv) try {
  bool require = false, atlas = false, fused = false, rgba_store=false, primary_stencil=false, primary_color_copy=false, secondary_stencil=false, fused_packed_atlas=false;
  uint32_t fused_capacity=64;bool capacity_explicit=false;
  for (int i = 1; i < argc; ++i) {
    const std::string_view option = argv[i];
    if (option == "--require-stencil-export" && !require) require = true;
    else if (option == "--atlas-depth-region-fixture" && !atlas) atlas = true;
    else if (option == "--atlas-depth-clear-fixture" && !fused) fused=true;
    else if(option=="--atlas-rgba-depth-store-fixture"&&!rgba_store)rgba_store=true;
    else if(option=="--packed-primary-stencil-clear-fixture"&&!primary_stencil)primary_stencil=true;
    else if(option=="--primary-color-copy-fixture"&&!primary_color_copy)primary_color_copy=true;
    else if(option=="--packed-secondary-stencil-clear-fixture"&&!secondary_stencil)secondary_stencil=true;
    else if(option=="--fused-packed-atlas-resolve-fixture"&&!fused_packed_atlas)fused_packed_atlas=true;
    else if (!capacity_explicit&&(option=="--submission-capacity=64"||option=="--submission-capacity=128")) {
      capacity_explicit=true;fused_capacity=option.ends_with("128")?128:64;
    }
    else throw std::runtime_error("Usage: stencil-export-test [--require-stencil-export] [--atlas-depth-region-fixture | --atlas-depth-clear-fixture | --atlas-rgba-depth-store-fixture | --packed-primary-stencil-clear-fixture | --primary-color-copy-fixture | --packed-secondary-stencil-clear-fixture | --fused-packed-atlas-resolve-fixture] [--submission-capacity=64|128]");
  }
  Require(unsigned(atlas)+unsigned(fused)+unsigned(rgba_store)+unsigned(primary_stencil)+unsigned(primary_color_copy)+unsigned(secondary_stencil)+unsigned(fused_packed_atlas)<=1&&(!capacity_explicit||fused||rgba_store||primary_stencil||primary_color_copy||secondary_stencil||fused_packed_atlas),"Fixture capacity/selection combination unsupported");
  if(fused_packed_atlas) {
    FsiDevice fsi(true);auto& device=fsi.device();VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(static_cast<VulkanDevice&>(device).physicalDevice,&properties);
    Require(properties.deviceType!=VK_PHYSICAL_DEVICE_TYPE_CPU,"Fused packed atlas fixture refuses CPU Vulkan device");
    Require(!require||fsi.stencil_export_enabled(),"Requested stencil export device support is unavailable");
    auto queue=device.createCommandQueue(RenderCommandListType::DIRECT);
    const auto r=RunFusedPackedAtlasResolve(device,*queue,fused_capacity);
    std::cout<<"{\"passed\":true,\"fused_packed_atlas_resolve_fixture\":true,\"hardware_required\":true,\"software_device\":false,\"vendor_id\":"<<properties.vendorID
      <<",\"device_id\":"<<properties.deviceID<<",\"device_type\":"<<properties.deviceType<<",\"device_name\":"<<std::quoted(properties.deviceName)
      <<",\"submission_capacity\":"<<fused_capacity<<",\"checks\":"<<checks<<",\"source_layout\":[1120,688,1120,1440,0,1]"
      <<",\"cases\":"<<r.cases<<",\"anchored_depth_codes\":"<<r.depth_codes<<",\"stencil_codes\":"<<r.stencil_codes<<",\"compared_resolve_pixels\":"<<r.raw_pixels
      <<",\"whole_source_bytes\":"<<edram_bytes<<",\"whole_destination_bytes\":"<<edram_bytes<<",\"whole_source_compares\":"<<r.full_source_compares
      <<",\"whole_destination_compares\":"<<r.full_destination_compares<<",\"scratch_owner_witnesses\":"<<r.owner_witnesses<<",\"pre_recording_refusals\":"<<r.refusals
      <<",\"accepted_work\":"<<r.accepted_work<<",\"completed_work\":"<<r.completed_work
      <<",\"all24_codes_on_gpu\":true,\"all24_chunks\":"<<r.all24_chunks<<",\"all24_codes\":"<<r.all24_codes
      <<",\"all24_raw_pixels\":"<<r.all24_raw_pixels<<",\"all24_nonidentity_codes\":"<<r.all24_nonidentity_codes
      <<",\"active_query\":false,\"scene_orchestration_exercised\":false}\n";return 0;
  }
  if(secondary_stencil){
    FsiDevice fsi(true);auto& device=fsi.device();VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(static_cast<VulkanDevice&>(device).physicalDevice,&properties);
    Require(properties.deviceType!=VK_PHYSICAL_DEVICE_TYPE_CPU,"Secondary stencil hardware fixture refuses CPU Vulkan device");
    const bool supported=fsi.stencil_export_enabled();Require(!require||supported,"Stencil export is not enabled");
    auto queue=device.createCommandQueue(RenderCommandListType::DIRECT);
    const auto legacy=RunPackedSecondaryStencilClear(device,*queue,false,fused_capacity);
    PackedStencilResult exported;if(supported)exported=RunPackedSecondaryStencilClear(device,*queue,true,fused_capacity);
    std::cout<<"{\"passed\":true,\"packed_secondary_stencil_clear_fixture\":true,\"hardware_required\":true,\"software_device\":false,\"vendor_id\":"<<properties.vendorID
      <<",\"device_id\":"<<properties.deviceID<<",\"device_type\":"<<properties.deviceType<<",\"device_name\":"<<std::quoted(properties.deviceName)<<",\"submission_capacity\":"<<fused_capacity<<",\"extension_enabled\":"<<(supported?"true":"false")
      <<",\"checks\":"<<checks<<",\"source_layout\":[640,360,640,1440,1,1],\"legacy_anchored_fp24_codes\":"<<legacy.unique_codes<<",\"export_anchored_fp24_codes\":"<<exported.unique_codes
      <<",\"source_stencil_codes_per_arm\":256,\"legacy_cases\":"<<legacy.chunks<<",\"export_cases\":"<<exported.chunks<<",\"whole_edram_bytes\":"<<edram_bytes
      <<",\"padding_bytes_per_case\":20480,\"whole_edram_comparison_pairs\":"<<legacy.full_edram_pairs+exported.full_edram_pairs<<",\"host_image_pairs\":"<<legacy.host_image_pairs+exported.host_image_pairs
      <<",\"source_owner_witnesses\":"<<legacy.source_owner_witnesses+exported.source_owner_witnesses<<",\"above_unit_pixels\":"<<legacy.above_unit_pixels+exported.above_unit_pixels
      <<",\"accepted_work\":"<<legacy.accepted_work+exported.accepted_work<<",\"completed_work\":"<<legacy.completed_work+exported.completed_work
      <<",\"active_query\":false,\"all24_codes_on_this_layout\":false,\"scene_orchestration_exercised\":false}\n";return 0;
  }
  if(primary_color_copy) {
    FsiDevice fsi(true);auto& device=fsi.device();VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(static_cast<VulkanDevice&>(device).physicalDevice,&properties);
    Require(properties.deviceType!=VK_PHYSICAL_DEVICE_TYPE_CPU,"Primary color hardware fixture refuses CPU Vulkan device");
    const bool supported=fsi.stencil_export_enabled();Require(!require||supported,"Stencil export is not enabled");
    auto queue=device.createCommandQueue(RenderCommandListType::DIRECT);
    const auto result=RunPrimaryColorCopy(device,*queue,fused_capacity);
    std::cout<<"{\"passed\":true,\"primary_color_copy_fixture\":true,\"hardware_required\":true,\"software_device\":false,\"vendor_id\":"<<properties.vendorID
      <<",\"device_id\":"<<properties.deviceID<<",\"device_type\":"<<properties.deviceType<<",\"device_name\":"<<std::quoted(properties.deviceName)
      <<",\"submission_capacity\":"<<fused_capacity<<",\"extension_enabled\":"<<(supported?"true":"false")
      <<",\"checks\":"<<checks<<",\"source_layout\":[1280,720,1280,720,2,1],\"pixels\":"<<result.pixels
      <<",\"unique_red_codes\":"<<result.unique_codes[0]<<",\"unique_green_codes\":"<<result.unique_codes[1]
      <<",\"unique_blue_codes\":"<<result.unique_codes[2]<<",\"unique_alpha_codes\":"<<result.unique_codes[3]
      <<",\"compared_float_components\":"<<result.compared_float_components<<",\"whole_edram_bytes\":"<<edram_bytes
      <<",\"whole_edram_compares\":"<<result.whole_edram_compares<<",\"source_owner_witnesses\":"<<result.source_owner_witnesses
      <<",\"accepted_work\":"<<result.accepted_work<<",\"completed_work\":"<<result.completed_work
      <<",\"active_query\":false,\"scene_orchestration_exercised\":false,\"physical_destination_resolve_exercised\":false}\n";
    return 0;
  }
  if(primary_stencil){
    FsiDevice fsi(true);auto& device=fsi.device();VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(static_cast<VulkanDevice&>(device).physicalDevice,&properties);
    Require(properties.deviceType!=VK_PHYSICAL_DEVICE_TYPE_CPU,"Primary stencil hardware fixture refuses CPU Vulkan device");
    const bool supported=fsi.stencil_export_enabled();Require(!require||supported,"Stencil export is not enabled");
    auto queue=device.createCommandQueue(RenderCommandListType::DIRECT);
    const auto legacy=RunPackedPrimaryStencilClear(device,*queue,false,fused_capacity);
    PackedStencilResult exported;if(supported)exported=RunPackedPrimaryStencilClear(device,*queue,true,fused_capacity);
    std::cout<<"{\"passed\":true,\"packed_primary_stencil_clear_fixture\":true,\"hardware_required\":true,\"software_device\":false,\"vendor_id\":"<<properties.vendorID
      <<",\"device_id\":"<<properties.deviceID<<",\"device_type\":"<<properties.deviceType<<",\"device_name\":"<<std::quoted(properties.deviceName)<<",\"submission_capacity\":"<<fused_capacity<<",\"extension_enabled\":"<<(supported?"true":"false")
      <<",\"checks\":"<<checks<<",\"legacy_unique_fp24_codes\":"<<legacy.unique_codes<<",\"export_unique_fp24_codes\":"<<exported.unique_codes
      <<",\"legacy_chunks\":"<<legacy.chunks<<",\"export_chunks\":"<<exported.chunks<<",\"whole_edram_bytes\":"<<edram_bytes
      <<",\"whole_edram_comparison_pairs\":"<<legacy.full_edram_pairs+exported.full_edram_pairs<<",\"host_image_pairs\":"<<legacy.host_image_pairs+exported.host_image_pairs
      <<",\"source_owner_witnesses\":"<<legacy.source_owner_witnesses+exported.source_owner_witnesses<<",\"above_unit_pixels\":"<<legacy.above_unit_pixels+exported.above_unit_pixels
      <<",\"accepted_work\":"<<legacy.accepted_work+exported.accepted_work<<",\"completed_work\":"<<legacy.completed_work+exported.completed_work
      <<",\"active_query\":false,\"scene_orchestration_exercised\":false}\n";return 0;
  }
  if(rgba_store){
    FsiDevice fsi(true);auto& device=fsi.device();VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(static_cast<VulkanDevice&>(device).physicalDevice,&properties);
    Require(properties.deviceType!=VK_PHYSICAL_DEVICE_TYPE_CPU,"Atlas RGBA store hardware fixture refuses CPU Vulkan device");
    const bool supported=fsi.stencil_export_enabled();Require(!require||supported,"Stencil export is not enabled");
    auto queue=device.createCommandQueue(RenderCommandListType::DIRECT);
    const auto legacy=RunRgbaAtlasDepthStore(device,*queue,false,fused_capacity);
    RgbaAtlasStoreResult exported;if(supported)exported=RunRgbaAtlasDepthStore(device,*queue,true,fused_capacity);
    std::cout<<"{\"passed\":true,\"atlas_rgba_depth_store_fixture\":true,\"hardware_required\":true,\"software_device\":false,\"vendor_id\":"<<properties.vendorID
      <<",\"device_id\":"<<properties.deviceID<<",\"device_type\":"<<properties.deviceType<<",\"device_name\":"<<std::quoted(properties.deviceName)<<",\"submission_capacity\":"<<fused_capacity<<",\"extension_enabled\":"<<(supported?"true":"false")
      <<",\"checks\":"<<checks<<",\"legacy_resolves\":"<<legacy.resolves<<",\"export_resolves\":"<<exported.resolves<<",\"decoded_source_cases\":"<<legacy.decoded_sources+exported.decoded_sources
      <<",\"preencode_receipt_cases\":"<<legacy.receipt_sources+exported.receipt_sources<<",\"full_destination_bytes\":"<<edram_bytes<<",\"full_destination_compares\":"<<legacy.whole_memory_compares+exported.whole_memory_compares
      <<",\"original_alpha_witness_pixels\":"<<legacy.original_alpha_witness_pixels+exported.original_alpha_witness_pixels<<",\"preencode_difference_pixels\":"<<legacy.preencode_difference_pixels+exported.preencode_difference_pixels
      <<",\"source_owner_witnesses\":"<<legacy.source_owner_witnesses+exported.source_owner_witnesses<<",\"descriptor_hits\":"<<legacy.descriptor_hits+exported.descriptor_hits
      <<",\"accepted_work\":"<<legacy.accepted_work+exported.accepted_work<<",\"completed_work\":"<<legacy.completed_work+exported.completed_work<<",\"scene_orchestration_exercised\":false}\n";return 0;
  }
  if(fused) {
    FsiDevice fsi(true);auto& device=fsi.device();VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(static_cast<VulkanDevice&>(device).physicalDevice,&properties);
    Require(properties.deviceType!=VK_PHYSICAL_DEVICE_TYPE_CPU,"Fused atlas hardware fixture refuses CPU Vulkan device");
    const bool supported=fsi.stencil_export_enabled();Require(!require||supported,"Stencil export is not enabled");
    auto queue=device.createCommandQueue(RenderCommandListType::DIRECT);
    const auto legacy=RunFusedAtlasClear(device,*queue,false,fused_capacity);
    FusedAtlasResult exported;if(supported)exported=RunFusedAtlasClear(device,*queue,true,fused_capacity);
    std::cout<<"{\"passed\":true,\"atlas_depth_clear_fixture\":true,\"hardware_required\":true,\"software_device\":false,\"vendor_id\":"<<properties.vendorID
      <<",\"device_id\":"<<properties.deviceID<<",\"device_type\":"<<properties.deviceType<<",\"device_name\":"<<std::quoted(properties.deviceName)<<",\"submission_capacity\":"<<fused_capacity<<",\"extension_enabled\":"<<(supported?"true":"false")
      <<",\"checks\":"<<checks<<",\"legacy_packed_sources\":"<<legacy.packed_sources<<",\"legacy_host_sources\":"<<legacy.host_sources
      <<",\"export_packed_sources\":"<<exported.packed_sources<<",\"export_host_sources\":"<<exported.host_sources
      <<",\"whole_edram_bytes\":"<<edram_bytes<<",\"whole_edram_compares\":"<<legacy.whole_edram_compares+exported.whole_edram_compares
      <<",\"receipt_restores\":"<<legacy.receipt_restores+exported.receipt_restores<<",\"receipt_owner_witnesses\":"<<legacy.receipt_owner_witnesses+exported.receipt_owner_witnesses
      <<",\"preencode_difference_pixels\":"<<legacy.preencode_difference_pixels+exported.preencode_difference_pixels
      <<",\"host_descriptor_hits\":"<<legacy.host_descriptor_hits+exported.host_descriptor_hits
      <<",\"accepted_work\":"<<legacy.accepted_work+exported.accepted_work<<",\"completed_work\":"<<legacy.completed_work+exported.completed_work
      <<",\"scene_version_orchestration_exercised\":false}\n";
    return 0;
  }
  if (atlas) {
    FsiDevice fsi(true);
    auto &device = fsi.device();
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(static_cast<VulkanDevice &>(device).physicalDevice,
                                  &properties);
    Require(properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU,
            "Atlas hardware fixture refuses a software Vulkan device");
    const bool supported = fsi.stencil_export_enabled();
    Require(!require || supported, "Stencil export is not enabled on this device");
    auto queue = device.createCommandQueue(RenderCommandListType::DIRECT);
    const auto legacy = RunAtlasRegion(device, *queue, false);
    AtlasResult exported;
    if (supported) exported = RunAtlasRegion(device, *queue, true);
    std::cout << "{\"passed\":true,\"atlas_depth_region_fixture\":true,"
              << "\"hardware_required\":true,\"software_device\":false,\"vendor_id\":"
              << properties.vendorID << ",\"device_id\":" << properties.deviceID
              << ",\"extension_enabled\":" << (supported ? "true" : "false")
              << ",\"checks\":" << checks << ",\"queued_legacy_rectangles\":"
              << legacy.rectangles << ",\"queued_export_rectangles\":" << exported.rectangles
              << ",\"full_edram_bytes\":" << edram_bytes
              << ",\"cropped_pixels_per_path\":" << legacy.cropped_pixels
              << ",\"full_reference_pixels_per_path\":" << legacy.reference_pixels
              << ",\"scratch_owner_witnesses\":"
              << legacy.scratch_owner_witnesses + exported.scratch_owner_witnesses
              << ",\"accepted_work\":" << legacy.accepted_work + exported.accepted_work
              << ",\"completed_work\":" << legacy.completed_work + exported.completed_work
              << ",\"scene_version_orchestration_exercised\":false}\n";
    return 0;
  }
  {
    FsiDevice absent(false);
    Require(!absent.stencil_export_enabled(),
            "Disabled device unexpectedly enabled stencil export");
    auto queue =
        absent.device().createCommandQueue(RenderCommandListType::DIRECT);
    const auto result = Run(absent.device(), *queue, false, false, false, 1);
    Require(
        result.restores.legacy_calls == 1,
        "Actual capability absence did not retain Native nine-draw restore");
  }
  FsiDevice fsi(true);
  auto &device = fsi.device();
  auto queue = device.createCommandQueue(RenderCommandListType::DIRECT);
  const bool supported = fsi.stencil_export_enabled();
  Require(!require || supported,
          "Stencil export is not enabled on this device");
  uint64_t comparisons = 0, queries = 7, export_calls = 0;
  if (supported)
    for (bool batched : {false, true})
      for (bool nonfinite : {false, true})
        for (unsigned format : {0u, 1u}) {
          const auto baseline =
              Run(device, *queue, false, batched, nonfinite, format);
          const auto candidate =
              Run(device, *queue, true, batched, nonfinite, format);
          Require(baseline.attachment == candidate.attachment &&
                      baseline.after_partial == candidate.after_partial,
                  "One-pass restore changed exact D32 depth/stencil bits");
          Require(baseline.packed == candidate.packed,
                  "One-pass restore changed packed10MiB EDRAM");
          Require(
              baseline.queries == candidate.queries &&
                  baseline.submissions == candidate.submissions,
              "One-pass restore changed actual queries or submission schedule");
          ++comparisons;
          queries += 14;
          export_calls += candidate.restores.export_calls;
        }
  std::cout << "{\"passed\":true,\"extension_enabled\":"
            << (supported ? "true" : "false") << ",\"checks\":" << checks
            << ",\"paired_cases\":" << comparisons
            << ",\"actual_queries\":" << queries
            << ",\"one_pass_restores\":" << export_calls << "}\n";
} catch (const std::exception &error) {
  std::cerr << error.what() << '\n';
  return 1;
}
