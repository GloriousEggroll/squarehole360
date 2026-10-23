#include "gpu_helpers.h"
#include "renderer/native/resolve_memory.h"
#include "renderer/native/transfer_batch.h"
#include "renderer/native/gpu_work_queue.h"
#include "memory_store_spv.h"
#include "memory_load_spv.h"
#include "memory_store_canonical_spv.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
using namespace plume;
using namespace gow3::renderer;
namespace {
uint64_t checks=0,physical_bytes=0,image_bytes=0,normalized_cpu_ulp_differences=0;
uint32_t normalized_cpu_max_ulp=0;
uint64_t half_cpu_rtz_differences=0;
void Require(bool value,const char* message) {++checks;if(!value)throw std::runtime_error(message);}
constexpr uint32_t kCapacity=4*1024*1024,kAddress=0x1000;
TextureLayout Layout(uint32_t format,uint32_t signs,uint32_t endian,bool tiled,
                     uint32_t width=256,uint32_t height=256,bool packed=false) {
    const uint32_t pitch=(width+31)/32;
    return DescribeTexture({2u|(signs<<2)|(pitch<<22)|(tiled?1u<<31:0u),
        kAddress|format|(endian<<6),(width-1)|((height-1)<<13),0,0,(1u<<9)|(packed?2048u:0u)});
}
float HalfToFloat(uint16_t half) {
    const uint32_t sign=uint32_t(half&0x8000)<<16,exponent=(half>>10)&31,mantissa=half&1023;
    if(exponent==31)return std::bit_cast<float>(sign|0x7f800000u|(mantissa<<13));
    if(exponent)return std::bit_cast<float>(sign|((exponent+112)<<23)|(mantissa<<13));
    if(!mantissa)return std::bit_cast<float>(sign);
    const float magnitude=std::ldexp(float(mantissa),-24);
    return sign?-magnitude:magnitude;
}
uint16_t FloatToHalf(float value) {
    const uint32_t bits=std::bit_cast<uint32_t>(value),sign=(bits>>16)&0x8000u;
    const uint32_t exponent=(bits>>23)&255u,mantissa=bits&0x7fffffu;
    if(exponent==255)return uint16_t(sign|0x7c00u|(mantissa?0x200u:0u));
    if(exponent>142)return uint16_t(sign|0x7c00u);
    if(exponent<102)return uint16_t(sign);
    const uint32_t shift=exponent<113?126-exponent:13;
    const uint32_t input=exponent<113?mantissa|0x800000u:mantissa;
    uint32_t rounded=input>>shift;
    const uint32_t remainder=input&((1u<<shift)-1),half=1u<<(shift-1);
    rounded+=remainder>half||(remainder==half&&(rounded&1));
    return uint16_t(sign|(exponent<113?rounded:((exponent-112)<<10)+rounded));
}
uint16_t FloatToHalfRtz(float value) {
    // Independent integer conversion: truncate discarded fraction bits and
    // saturate finite overflow. Infinities retain their distinct encoding.
    const uint32_t bits=std::bit_cast<uint32_t>(value),sign=(bits>>16)&0x8000u;
    const uint32_t exponent=(bits>>23)&255u,mantissa=bits&0x7fffffu;
    if(exponent==255)return uint16_t(sign|0x7c00u|(mantissa?0x200u:0u));
    if(exponent>142)return uint16_t(sign|0x7bffu);
    if(exponent<103)return uint16_t(sign);
    if(exponent<113)return uint16_t(sign|((mantissa|0x800000u)>>(126-exponent)));
    return uint16_t(sign|((exponent-112)<<10)|(mantissa>>13));
}
std::vector<std::array<float,4>> Pixels(const TextureLayout& layout,uint32_t generation) {
    std::vector<std::array<float,4>> result(size_t(layout.width)*layout.height);
    const float nan=std::bit_cast<float>(0x7fc12345u),snan=std::bit_cast<float>(0x7f812345u);
    const float inf=std::numeric_limits<float>::infinity();
    const float tie=(12345.0f+.5f)/65535.0f;
    const std::array<float,26> edge{0,-0.0f,1,-1,2,-2,nan,snan,inf,-inf,
        std::bit_cast<float>(1u),std::bit_cast<float>(0x80000001u),tie,
        std::nextafter(tie,0.0f),std::nextafter(tie,1.0f),.5f,-.5f,
        65504.0f,65520.0f,-65520.0f,1.00048828125f,
        std::nextafter(1.00048828125f,1.0f),std::nextafter(1.00048828125f,2.0f),
        0x1p-25f,std::nextafter(0x1p-25f,1.0f),-0x1p-25f};
    for(uint32_t i=0;i<result.size();++i) {
        const uint32_t code=(i+generation*131)&65535u;
        if(layout.format==26)result[i]={float(code)/65535.0f,
            (float(code)-32768.0f)/32767.0f,edge[(i+generation)%edge.size()],
            edge[(i*7+generation)%edge.size()]};
        else result[i]={HalfToFloat(uint16_t(code)),generation?edge[i%edge.size()]:HalfToFloat(uint16_t(code^0x8000)),
            edge[i%edge.size()],edge[(i+3)%edge.size()]};
    }
    return result;
}
std::shared_ptr<RenderTexture> Upload(RenderDevice& device,RenderCommandQueue& queue,
        const TextureLayout& layout,const std::vector<std::array<float,4>>& pixels) {
    const size_t bytes=pixels.size()*16;
    auto upload=device.createBuffer(RenderBufferDesc::UploadBuffer(bytes));
    std::memcpy(upload->map(),pixels.data(),bytes);validation::Flush(*upload);upload->unmap();
    std::shared_ptr<RenderTexture> image=device.createTexture(
        RenderTextureDesc::Texture2D(layout.width,layout.height,1,RenderFormat::R32G32B32A32_FLOAT));
    auto commands=queue.createCommandList();auto fence=device.createCommandFence();commands->begin();
    commands->barriers(RenderBarrierStage::COPY,RenderTextureBarrier(image.get(),RenderTextureLayout::COPY_DEST));
    VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
    copy.imageExtent={layout.width,layout.height,1};
    vkCmdCopyBufferToImage(static_cast<VulkanCommandList&>(*commands).vk,static_cast<VulkanBuffer&>(*upload).vk,
        static_cast<VulkanTexture&>(*image).vk,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
    commands->end();queue.executeCommandLists(commands.get(),fence.get());queue.waitForCommandFence(fence.get());return image;
}
struct PendingRead {
    std::shared_ptr<RenderTexture> image;
    std::unique_ptr<RenderBuffer> readback;
    std::unique_ptr<RenderCommandList> commands;
    size_t size=0;
    std::vector<uint8_t> Read() {
        auto& native=static_cast<VulkanBuffer&>(*readback);const auto* data=static_cast<const uint8_t*>(readback->map());
        Require(vmaInvalidateAllocation(native.device->allocator,native.allocation,0,VK_WHOLE_SIZE)==VK_SUCCESS,
            "Cannot invalidate canonical receipt readback");
        std::vector<uint8_t> bytes(data,data+size);readback->unmap();return bytes;
    }
};
std::shared_ptr<PendingRead> QueueRead(RenderDevice& device,RenderCommandQueue& queue,
        NativeGpuWorkQueue& work,const std::shared_ptr<RenderTexture>& image,uint64_t event) {
    auto result=std::make_shared<PendingRead>();result->image=image;
    const auto& desc=static_cast<VulkanTexture&>(*image).desc;result->size=uint64_t(desc.width)*desc.height*16;
    result->readback=device.createBuffer(RenderBufferDesc::ReadbackBuffer(result->size));
    result->commands=queue.createCommandList();work.Reserve(result->size);result->commands->begin();
    result->commands->barriers(RenderBarrierStage::COPY,RenderTextureBarrier(image.get(),RenderTextureLayout::COPY_SOURCE));
    VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={desc.width,desc.height,1};
    const auto commands=static_cast<VulkanCommandList&>(*result->commands).vk;
    vkCmdCopyImageToBuffer(commands,static_cast<VulkanTexture&>(*image).vk,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        static_cast<VulkanBuffer&>(*result->readback).vk,1,&copy);
    VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(commands,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);
    result->commands->end();work.EnqueueTransfer(*result->commands,result,result->size,event);return result;
}
uint32_t ReadGuestWord(const std::vector<uint8_t>& memory,uint32_t address,uint32_t endian) {
    constexpr uint32_t swaps[]{0,1,3,2};uint32_t result=0;
    for(uint32_t byte=0;byte<4;++byte)result|=uint32_t(memory.at((address+byte)^swaps[endian]))<<(byte*8);
    return result;
}
uint16_t Encode16(float input,bool signed_channel) {
    if(std::isnan(input))return 0;
    const float scaled=std::clamp(input,signed_channel?-1.0f:0.0f,1.0f)*(signed_channel?32767.0f:65535.0f);
    return uint16_t(int32_t(std::nearbyint(scaled)));
}
void Oracle(const TextureLayout& layout,const std::vector<std::array<float,4>>& input,
        const std::vector<uint8_t>& physical,const std::vector<uint8_t>& decoded) {
    const uint32_t signs=(layout.fetch[0]>>2)&255u;
    for(uint32_t y=0;y<layout.height;++y)for(uint32_t x=0;x<layout.width;++x) {
        const uint32_t i=y*layout.width+x;
        const uint32_t address=kAddress+uint32_t(TextureBlockOffset(layout,layout.levels.front(),x,y,0));
        std::array<uint32_t,4> actual{};std::memcpy(actual.data(),decoded.data()+size_t(i)*16,16);
        const auto low=ReadGuestWord(physical,address,layout.endian);
        if(layout.format==26) {
            const auto high=ReadGuestWord(physical,address+4,layout.endian);
            for(uint32_t c=0;c<4;++c) {
                const bool is_signed=((signs>>(c*2))&3)==1;const auto encoded=Encode16(input[i][c],is_signed);
                Require(((c<2?low:high)>>((c&1)*16)&65535u)==encoded,"Independent normalized physical oracle differs");
                const float value=is_signed?std::max(float(int16_t(encoded))/32767.0f,-1.0f):float(encoded)/65535.0f;
                const auto expected=std::bit_cast<uint32_t>(value);
                if(actual[c]!=expected) {
                    const auto distance=actual[c]>expected?actual[c]-expected:expected-actual[c];
                    // Hardware division may lower to reciprocal multiplication.
                    // This independent CPU check allows one ULP only, retaining
                    // exact sign, zero and saturation endpoints. The separate
                    // candidate-versus-baseline GPU image comparison is EXACT.
                    // Vulkan allows OpFDiv 2.5 ULP; this fixture is stricter:
                    // https://docs.vulkan.org/spec/latest/appendices/spirvenv.html#spirvenv-precision-operation
                    const bool within_bound=distance<=1&&((actual[c]^expected)&0x80000000u)==0&&
                        value!=0.0f&&std::abs(value)!=1.0f;
                    if(!within_bound||!normalized_cpu_ulp_differences) {
                        std::ostringstream witness;
                        witness<<"Independent normalized float oracle difference: pixel="<<i<<" component="<<c
                            <<" encoded="<<encoded<<" signed="<<is_signed<<" ULP="<<distance
                            <<" actual_bits=0x"<<std::hex<<actual[c]<<" expected_bits=0x"<<expected
                            <<" actual="<<std::hexfloat<<std::bit_cast<float>(actual[c])<<" expected="<<value
                            <<" reciprocal_form="<<(is_signed?float(int16_t(encoded))*(1.0f/32767.0f):float(encoded)*(1.0f/65535.0f));
                        if(!within_bound)throw std::runtime_error(witness.str());
                        std::cout<<witness.str()<<" (within CPU reference bound)"<<std::endl;
                    }
                    ++normalized_cpu_ulp_differences;
                    normalized_cpu_max_ulp=std::max(normalized_cpu_max_ulp,distance);
                }
                ++checks;
            }
        } else {
            for(uint32_t c=0;c<2;++c) {
                const uint16_t code=FloatToHalf(input[i][c]);
                if(!std::isnan(input[i][c])) {
                    const auto actual_half=uint16_t(low>>(c*16)),rtz=FloatToHalfRtz(input[i][c]);
                    // PackHalf2x16 has an implementation-defined rounding mode.
                    // Admit exactly the independent RNE or RTZ conversion, never
                    // a generic half-ULP tolerance. Decode the ACTUAL encoded
                    // value independently and require exact float bits.
                    const auto expected_decoded=std::bit_cast<uint32_t>(HalfToFloat(actual_half));
                    const bool valid=(actual_half==code||actual_half==rtz)&&actual[c]==expected_decoded;
                    const bool rtz_difference=actual_half!=code&&actual_half==rtz;
                    if(!valid||(rtz_difference&&!half_cpu_rtz_differences)) {
                        std::ostringstream witness;
                        witness<<"Independent finite/Inf half oracle rounding: pixel="<<i<<" component="<<c
                            <<" input_bits=0x"<<std::hex<<std::bit_cast<uint32_t>(input[i][c])
                            <<" expected_half_rne=0x"<<code<<" expected_half_rtz=0x"<<rtz<<" actual_half=0x"<<actual_half
                            <<" cpu_decode_actual_half_bits=0x"<<expected_decoded<<" actual_decode_bits=0x"<<actual[c]
                            <<" input="<<std::hexfloat<<input[i][c];
                        if(!valid)throw std::runtime_error(witness.str());
                        std::cout<<witness.str()<<" (exact RTZ conversion)"<<std::endl;
                    }
                    half_cpu_rtz_differences+=rtz_difference;
                    checks+=2;
                } else Require(std::isnan(std::bit_cast<float>(actual[c])),"Half NaN did not remain NaN");
            }
            Require(actual[2]==0&&actual[3]==0x3f800000u,"Half LOAD default z/w components changed");
        }
    }
}
void Case(RenderDevice& device,RenderCommandQueue& queue,uint32_t capacity,const TextureLayout& layout) try {
    NativeGpuWorkQueue work(device,queue,{},true,false,capacity);
    NativeTransferBatch batch(device,queue,&work);
    ResolveMemory baseline(device,queue,memory_store_spv,memory_load_spv,kCapacity);
    ResolveMemory candidate(device,queue,memory_store_spv,memory_load_spv,kCapacity,0,&batch);
    Require(candidate.EnableCanonicalStoreReceipt(memory_store_canonical_spv),"Canonical store startup preparation failed");
    candidate.SetDescriptorPoolEnabled(true);
    std::vector<uint8_t> seed(kCapacity);for(uint32_t i=0;i<seed.size();++i)seed[i]=uint8_t(i*37+(i>>8));
    baseline.Initialize(0,seed);candidate.Initialize(0,seed);
    const RenderRect full(0,0,layout.width,layout.height);
    std::array<std::shared_ptr<RenderTexture>,2> sources;
    std::array<std::vector<std::array<float,4>>,2> inputs;
    std::array<std::vector<uint8_t>,2> expected_images,expected_physical;
    for(uint32_t i=0;i<2;++i) {
        inputs[i]=Pixels(layout,i);sources[i]=Upload(device,queue,layout,inputs[i]);
        baseline.Store(*sources[i],layout,full,kAddress);
        expected_physical[i]=baseline.ReadBytes(0,kCapacity);
        auto expected=baseline.LoadShared(layout,kAddress);
        expected_images[i]=validation::ReadImage(device,queue,*expected,16);
        Oracle(layout,inputs[i],expected_physical[i],expected_images[i]);
    }
    std::array<std::optional<ResolveMemory::CanonicalStoreReceipt>,2> receipts;
    std::array<std::shared_ptr<PendingRead>,2> reads;
    for(uint32_t i=0;i<2;++i) {
        batch.Run([&]{receipts[i]=candidate.TryStoreCanonical(*sources[i],layout,full,kAddress);},i*2+1);
        Require(receipts[i].has_value(),"Supported full store was refused");
        Require(candidate.CanonicalStoreReceiptMatches(*receipts[i],layout,kAddress),"Fresh receipt was not authenticated");
        Require(!baseline.CanonicalStoreReceiptMatches(*receipts[i],layout,kAddress),"Foreign allocation accepted receipt");
        auto changed_sign=layout;changed_sign.fetch[0]^=4;
        Require(!candidate.CanonicalStoreReceiptMatches(*receipts[i],changed_sign,kAddress),"Changed signs reused a receipt");
        auto changed_swizzle=layout;changed_swizzle.fetch[3]^=0x30;
        Require(candidate.CanonicalStoreReceiptMatches(*receipts[i],changed_swizzle,kAddress),"Binding-only swizzle incorrectly changes decode identity");
        Require(!candidate.CanonicalStoreReceiptMatches(*receipts[i],layout,kAddress+4096),"Changed address reused a receipt");
        // Consumer is queued before producer retirement. This explicit image
        // barrier must publish the fused compute image without another decode.
        reads[i]=QueueRead(device,queue,work,receipts[i]->image,i*2+2);
        work.SubmitPending();
        if(i)Require(!candidate.CanonicalStoreReceiptMatches(*receipts[0],layout,kAddress),"Overlapping store left old receipt current");
    }
    Require(work.pending(),"Ownership fixture unexpectedly retired all submitted generations");
    work.Drain();
    Require(candidate.ReadBytes(0,kCapacity)==expected_physical[1],"Complete physical memory/padding differs");
    physical_bytes+=kCapacity;
    for(uint32_t i=0;i<2;++i) {
        const auto actual=reads[i]->Read();Require(actual==expected_images[i],"Queued immutable canonical image differs bitwise");
        image_bytes+=actual.size();Oracle(layout,inputs[i],expected_physical[i],actual);
    }
    const auto stats=candidate.canonical_store_statistics();
    Require(stats.selected==2&&stats.refused==0&&stats.pixels==uint64_t(layout.width)*layout.height*2,
        "Canonical store selection accounting differs");
    const uint32_t first_texel=kAddress+uint32_t(TextureBlockOffset(layout,layout.levels.front(),0,0,0));
    candidate.Initialize(first_texel,{expected_physical[1].data()+first_texel,4});
    Require(!candidate.CanonicalStoreReceiptMatches(*receipts[1],layout,kAddress),"CPU Initialize left receipt current");
    batch.Run([&]{receipts[1]=candidate.TryStoreCanonical(*sources[1],layout,full,kAddress);});work.Drain();
    Require(receipts[1]&&candidate.CanonicalStoreReceiptMatches(*receipts[1],layout,kAddress),"Fresh receipt after CPU write failed");
    // Perform an actual external packed-buffer write, fence it, then exercise
    // the import publication API. Even equal replacement bytes invalidate it.
    auto commands=queue.createCommandList();auto fence=device.createCommandFence();uint32_t word;
    std::memcpy(&word,expected_physical[1].data()+first_texel,4);
    commands->begin();commands->barriers(RenderBarrierStage::COPY,
        RenderBufferBarrier(&candidate.storage_buffer(),RenderBufferAccess::WRITE));
    vkCmdFillBuffer(static_cast<VulkanCommandList&>(*commands).vk,
        static_cast<VulkanBuffer&>(candidate.storage_buffer()).vk,first_texel,4,word);
    commands->end();queue.executeCommandLists(commands.get(),fence.get());queue.waitForCommandFence(fence.get());
    candidate.RecordImportedBytes(first_texel,4);
    Require(!candidate.CanonicalStoreReceiptMatches(*receipts[1],layout,kAddress),"External imported write left receipt current");
    batch.Run([&]{receipts[1]=candidate.TryStoreCanonical(*sources[1],layout,full,kAddress);});work.Drain();
    Require(receipts[1]&&candidate.CanonicalStoreReceiptMatches(*receipts[1],layout,kAddress),"Fresh receipt after import failed");
    candidate.Invalidate(first_texel,1);
    Require(!candidate.CanonicalStoreReceiptMatches(*receipts[1],layout,kAddress),"Partial invalidation left receipt current");
}catch(const std::exception& error) {
    throw std::runtime_error("format="+std::to_string(layout.format)+" signs="+std::to_string((layout.fetch[0]>>2)&255u)+
        " endian="+std::to_string(layout.endian)+" tiled="+std::to_string(layout.tiled)+
        " extent="+std::to_string(layout.width)+"x"+std::to_string(layout.height)+": "+error.what());
}
void Refusals(RenderDevice& device,RenderCommandQueue& queue,uint32_t capacity) {
    NativeGpuWorkQueue work(device,queue,{},true,false,capacity);NativeTransferBatch batch(device,queue,&work);
    ResolveMemory candidate(device,queue,memory_store_spv,memory_load_spv,kCapacity,0,&batch);
    Require(candidate.EnableCanonicalStoreReceipt(memory_store_canonical_spv),"Refusal fixture startup failed");
    const auto layout=Layout(26,0,1,true,33,17);const RenderRect full(0,0,33,17);
    auto source=Upload(device,queue,layout,Pixels(layout,0));
    std::vector<uint8_t> seed(kCapacity,0xcd);candidate.Initialize(0,seed);
    const auto serial=candidate.physical_serial(),mutation=candidate.mutation_serial();
    const auto check=[&](const TextureLayout& t,const RenderRect& rect) {
        const auto stages=batch.statistics().stages;
        batch.Run([&]{Require(!candidate.TryStoreCanonical(*source,t,rect,kAddress),"Unsupported canonical store was accepted");});
        Require(candidate.physical_serial()==serial&&candidate.mutation_serial()==mutation&&batch.statistics().stages==stages,
            "Refusal recorded or mutated physical memory");
    };
    Require(!candidate.TryStoreCanonical(*source,layout,full,kAddress),"Out-of-scope store was accepted");
    check(layout,{0,0,32,17});check(Layout(32,0,1,true,33,17),full);
    auto forged=layout;forged.levels[0].pitch_blocks=32;check(forged,full);
    auto overlapping=DescribeTexture({2u|(1u<<22)|(1u<<31),kAddress|26u|(1u<<6),32u|(16u<<13),0,0,1u<<9});
    check(overlapping,full);
    auto multilayer=layout;multilayer.layers=2;check(multilayer,full);
    ResolveMemory disabled(device,queue,memory_store_spv,memory_load_spv,kCapacity,0,&batch);
    batch.Run([&]{Require(!disabled.TryStoreCanonical(*source,layout,full,kAddress),"Default-off store was accepted");});
    ResolveMemory narrow(device,queue,memory_store_spv,memory_load_spv,kCapacity,1024,&batch);
    Require(narrow.EnableCanonicalStoreReceipt(memory_store_canonical_spv),"Window refusal startup failed");
    batch.Run([&]{Require(!narrow.TryStoreCanonical(*source,layout,full,kAddress),"Split descriptor window was accepted");});
    ResolveMemory synchronous(device,queue,memory_store_spv,memory_load_spv,kCapacity);
    Require(!synchronous.EnableCanonicalStoreReceipt(memory_store_canonical_spv),"Unowned synchronous candidate was enabled");
    Require(candidate.ReadBytes(0,kCapacity)==seed,"Refusal changed complete physical allocation");
    // Scratch pressure may complete earlier work and select the ordinary
    // synchronous path, but must decline before recording this optional store.
    const auto stages=batch.statistics().stages;
    batch.Run([&] {
        batch.KeepImage(source,NativeTransferBatch::kMaxImageBytes);
        Require(!candidate.TryStoreCanonical(*source,layout,full,kAddress),"Scratch-pressure candidate failed to decline");
    });
    Require(candidate.physical_serial()==serial&&candidate.mutation_serial()==mutation&&batch.statistics().stages==stages,
        "Scratch pressure replayed or recorded a candidate store");
    work.Drain();
}
void AcceptedFailure(RenderDevice& device,RenderCommandQueue& queue,uint32_t capacity) {
    NativeGpuWorkQueue work(device,queue,{},true,false,capacity);NativeTransferBatch batch(device,queue,&work);
    ResolveMemory baseline(device,queue,memory_store_spv,memory_load_spv,kCapacity);
    ResolveMemory candidate(device,queue,memory_store_spv,memory_load_spv,kCapacity,0,&batch);
    Require(candidate.EnableCanonicalStoreReceipt(memory_store_canonical_spv),"Accepted-prefix fixture startup failed");
    const auto layout=Layout(28,0x55,3,true,17,9);const RenderRect full(0,0,17,9);
    auto source=Upload(device,queue,layout,Pixels(layout,1));std::vector<uint8_t> seed(kCapacity,0xa7);
    baseline.Initialize(0,seed);candidate.Initialize(0,seed);baseline.Store(*source,layout,full,kAddress);
    auto canonical=baseline.LoadShared(layout,kAddress);const auto expected=validation::ReadImage(device,queue,*canonical,16);
    const auto mutation=candidate.mutation_serial();std::optional<ResolveMemory::CanonicalStoreReceipt> receipt;
    bool propagated=false;
    try {batch.Run([&] {
        receipt=candidate.TryStoreCanonical(*source,layout,full,kAddress);
        Require(receipt.has_value(),"Accepted-prefix store was refused");
        throw std::runtime_error("Injected after canonical store recording");
    });}catch(const NativeTransferBatchError& error) {
        propagated=std::string(error.what())=="Injected after canonical store recording";
    }
    Require(propagated&&!work.pending()&&work.accepted_work()==1&&work.completed_work()==1,
        "Accepted-prefix error lost its actual fence or replayed the store");
    Require(candidate.mutation_serial()==mutation+1&&candidate.canonical_store_statistics().selected==1,
        "Accepted-prefix error changed store/version count");
    Require(candidate.ReadBytes(0,kCapacity)==baseline.ReadBytes(0,kCapacity),"Accepted-prefix physical bytes/padding differ");
    Require(receipt&&validation::ReadImage(device,queue,*receipt->image,16)==expected,
        "Accepted-prefix immutable output was destroyed or changed");
}
}
int main(int argc,char** argv)try {
    const uint32_t capacity=argc>1?uint32_t(std::stoul(argv[1])):64;
    Require(capacity==64||capacity==128,"Test capacity must be 64 or 128");
    if(memory_store_canonical_spv.empty()) {
        // canonical_store_shader.cmake writes an empty stub when the reference
        // resolve_memory.comp no longer matches its reviewed hash.
        std::cout<<"SKIP: canonical store shader disabled (unreviewed reference shader)"<<std::endl;
        return 0;
    }
    auto interface=CreateVulkanInterface();auto device=interface->createDevice();Require(bool(device),"Vulkan unavailable");
    const auto& properties=static_cast<VulkanDevice&>(*device).physicalDeviceProperties;
    std::cout<<"Vulkan device="<<properties.deviceName<<" vendor_id="<<properties.vendorID
        <<" device_id="<<properties.deviceID<<" driver_version="<<properties.driverVersion<<std::endl;
    auto queue=device->createCommandQueue(RenderCommandListType::DIRECT);
    for(uint32_t endian=0;endian<4;++endian) {
        Case(*device,*queue,capacity,Layout(26,endian==0?0:endian==1?0x55:endian==2?0x11:0xff,endian,endian%2));
        Case(*device,*queue,capacity,Layout(endian%2?28:31,0x55,endian,endian%2));
    }
    Case(*device,*queue,capacity,Layout(26,0,1,true,322,182));
    Case(*device,*queue,capacity,Layout(31,0x55,2,true,640,360));
    Case(*device,*queue,capacity,Layout(26,0,3,true,3,5,true));
    Case(*device,*queue,capacity,Layout(28,0x55,1,false,5,3,true));
    Refusals(*device,*queue,capacity);
    AcceptedFailure(*device,*queue,capacity);
    std::cout<<"PASS: canonical store capacity="<<capacity<<" checks="<<checks<<" physical_bytes="<<physical_bytes
        <<" image_bytes="<<image_bytes<<" normalized_cpu_ulp_differences="<<normalized_cpu_ulp_differences
        <<" normalized_cpu_max_ulp="<<normalized_cpu_max_ulp<<" half_cpu_rtz_differences="<<half_cpu_rtz_differences
        <<"; exact candidate/baseline images and physical bytes; all normalized/half codes, rounding/special values, packed offsets, endian, padding, two generations, queued consumers and refusal ownership\n";
}catch(const std::exception& error){std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}
