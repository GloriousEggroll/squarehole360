#include "texture_upload.h"
#include "texture_upload_conversion.h"
#include "plume_vulkan.h"
#include <algorithm>
#include <bit>
#include <cstring>
#include <stdexcept>

namespace gow3::renderer {
using namespace plume;
namespace {
RenderFormat Format(uint32_t format, bool snorm) {
  switch (format) {
    case 2: case 8: case 9: return snorm ? RenderFormat::R8_SNORM : RenderFormat::R8_UNORM;
    case 6: case 14: case 50: return snorm ? RenderFormat::R8G8B8A8_SNORM : RenderFormat::R8G8B8A8_UNORM;
    case 10: return snorm ? RenderFormat::R8G8_SNORM : RenderFormat::R8G8_UNORM;
    case 22: case 23: return RenderFormat::R32_FLOAT;
    case 24: return snorm ? RenderFormat::R16_SNORM : RenderFormat::R16_UNORM;
    case 25: return snorm ? RenderFormat::R16G16_SNORM : RenderFormat::R16G16_UNORM;
    case 26: return snorm ? RenderFormat::R16G16B16A16_SNORM : RenderFormat::R16G16B16A16_UNORM;
    case 27: case 30: return RenderFormat::R16_FLOAT;
    case 28: case 31: return RenderFormat::R16G16_FLOAT;
    case 29: case 32: return RenderFormat::R16G16B16A16_FLOAT;
    case 36: return RenderFormat::R32_FLOAT;
    case 37: return RenderFormat::R32G32_FLOAT;
    case 38: return RenderFormat::R32G32B32A32_FLOAT;
    case 49: return snorm ? RenderFormat::BC5_SNORM : RenderFormat::BC5_UNORM;
    case 59: return snorm ? RenderFormat::BC4_SNORM : RenderFormat::BC4_UNORM;
    default: break;
  }
  if (!snorm) switch (format) {
    // Plume doesn't expose RGB10A2. Keep unpacked normalized values in float
    // storage; CPU upload and GPU resolve both preserve the 10/10/10/2 grid.
    case 7: case 54: return RenderFormat::R32G32B32A32_FLOAT;
    case 18: case 51: return RenderFormat::BC1_UNORM;
    case 19: case 52: return RenderFormat::BC2_UNORM;
    case 20: case 53: return RenderFormat::BC3_UNORM;
  }
  throw std::runtime_error("Texture upload format/sign mode is not supported: " + std::to_string(format));
}

RenderTextureAddressMode Address(uint32_t mode) {
  switch (mode) {
    case 0: return RenderTextureAddressMode::WRAP;
    case 1: return RenderTextureAddressMode::MIRROR;
    case 2: return RenderTextureAddressMode::CLAMP;
    case 3: return RenderTextureAddressMode::MIRROR_ONCE;
    case 6: return RenderTextureAddressMode::BORDER;
    default: throw std::runtime_error("Texture halfway/mirrored-border clamp needs shader emulation");
  }
}

int32_t SignedField(uint32_t word, uint32_t shift, uint32_t bits) {
  const uint32_t v = (word >> shift) & ((1u << bits) - 1);
  return int32_t(v ^ (1u << (bits - 1))) - int32_t(1u << (bits - 1));
}
bool MixedSigns(const TextureLayout& t) {
  const uint32_t signs=(t.fetch[0]>>2)&255;
  bool signed_channel=false,unsigned_channel=false;
  for(unsigned c=0;c<4;++c) {
    signed_channel|=((signs>>(c*2))&3)==1;
    unsigned_channel|=((signs>>(c*2))&3)!=1;
  }
  return signed_channel&&unsigned_channel;
}
bool DecodeNormalizedToFloat(const TextureLayout& t) {
  if(t.format==7||t.format==54)return true;
  return MixedSigns(t)&&(t.format==6||t.format==14||t.format==50||t.format==26);
}
} // namespace

RenderFormat NativeTextureFormat(const TextureLayout& t) {
  const auto& f = t.fetch;
  const uint32_t signs = (f[0] >> 2) & 255;
  bool has_signed = false, has_unsigned = false;
  for (uint32_t c = 0; c < 4; ++c) {
    has_signed |= ((signs >> (c * 2)) & 3) == 1;
    has_unsigned |= ((signs >> (c * 2)) & 3) != 1;
  }
  const bool depth = t.format == 22 || t.format == 23;
  const bool floating = depth || (t.format >= 27 && t.format <= 32) || (t.format >= 36 && t.format <= 38);
  if (has_signed && has_unsigned && !floating && !DecodeNormalizedToFloat(t))
    throw std::runtime_error("Mixed signed/unsigned texture channels require multiple native views");
  const bool compressed = t.block.block_width > 1;
  if ((f[3] & 1) && !floating && !compressed)
    throw std::runtime_error("Integer fixed-point texture number format is not supported");
  if (t.layers > 1 && t.dimension == 1) {
    // A base-only stack with identical XY/Z filtering is
    // equivalent to a native volume. Other stacks need array sampling with
    // independent layer filtering rather than silently changing semantics.
    const uint32_t mag=(f[3]>>19)&3,min=(f[3]>>21)&3;
    if(t.max_mip||t.min_mip||mag!=(f[4]&1)||min!=((f[4]>>1)&1)||((f[3]>>25)&7)>1)
      throw std::runtime_error("Stacked texture requires independent layer/mip filtering");
  }
  return DecodeNormalizedToFloat(t)?RenderFormat::R32G32B32A32_FLOAT:Format(t.format, has_signed);
}

TextureBinding BindTexture(RenderDevice& device, RenderTexture& image, const TextureLayout& t,bool decoded_float,
                           int32_t anisotropy_override,uint32_t max_anisotropy) {
  const auto& f = t.fetch;
  const uint32_t signs = (f[0] >> 2) & 255;
  const auto format = static_cast<VulkanTexture&>(image).desc.format;
  auto validated=t;
  if(decoded_float) {
    const bool full=format==RenderFormat::R32G32B32A32_FLOAT;
    const bool compact=t.dimension==1&&t.layers==1&&
        ((t.block.components==1&&format==RenderFormat::R32_FLOAT)||
         (t.block.components==2&&format==RenderFormat::R32G32_FLOAT));
    if(!full&&!compact)
      throw std::runtime_error("Decoded GPU texture binding requires matching float32 channels");
    // The GPU decoder has already applied per-channel signed normalization.
    // Float32 storage also represents mixed signed/unsigned channels. Compact
    // images omit only channels unreachable after the existing component clamp;
    // swizzle, constant selection, signs and exponent stay exactly the same.
    validated.fetch[0]&=~(255u<<2);
  }
  NativeTextureFormat(validated);
  TextureBinding result;
  result.dimension = t.dimension==1&&t.layers>1?2:t.dimension;
  auto view = result.dimension == 2 ? RenderTextureViewDesc::Texture3D(format) : t.dimension == 3
            ? RenderTextureViewDesc::TextureCube(format) : RenderTextureViewDesc::Texture2D(format);
  std::array<RenderSwizzle, 4> mapping;
  constexpr RenderSwizzle components[]{RenderSwizzle::R, RenderSwizzle::G, RenderSwizzle::B, RenderSwizzle::A};
  for (uint32_t c = 0; c < 4; ++c) {
    const uint32_t swizzle = (f[3] >> (1 + c * 3)) & 7;
    if (swizzle > 5) throw std::runtime_error("Invalid texture channel swizzle");
    mapping[c] = swizzle == 4 ? RenderSwizzle::ZERO : swizzle == 5 ? RenderSwizzle::ONE
                              : components[std::min(swizzle, t.block.components - 1)];
    const uint32_t sign = swizzle < 4 ? ((signs >> (swizzle * 2)) & 3) : 0;
    result.properties.signs |= sign << (c * 2);
  }
  result.properties.exponent = SignedField(f[3], 13, 6);
  view.componentMapping = {mapping[0], mapping[1], mapping[2], mapping[3]};
  result.view = image.createTextureView(view);
  result.sampler = device.createSampler(TextureSampler(t,anisotropy_override,max_anisotropy));
  if (!result.view || !result.sampler) throw std::runtime_error("Failed to create texture view/sampler");
  return result;
}

RenderSamplerDesc TextureSampler(const TextureLayout& t,int32_t anisotropy_override,uint32_t max_anisotropy) {
  if(anisotropy_override < -1 || anisotropy_override > 5)
    throw std::runtime_error("Invalid texture anisotropy override or device limit");
  max_anisotropy=std::bit_floor(std::min(16u,max_anisotropy));
  const auto& f=t.fetch;
  RenderSamplerDesc sampler;
  const uint32_t mag = (f[3] >> 19) & 3, min = (f[3] >> 21) & 3, mip = (f[3] >> 23) & 3;
  if (mag > 1 || min > 1 || mip > 2) throw std::runtime_error("Invalid texture filtering state");
  sampler.magFilter = mag ? RenderFilter::LINEAR : RenderFilter::NEAREST;
  sampler.minFilter = min ? RenderFilter::LINEAR : RenderFilter::NEAREST;
  sampler.mipmapMode = mip == 1 ? RenderMipmapMode::LINEAR : RenderMipmapMode::NEAREST;
  sampler.addressU = Address((f[0] >> 10) & 7);
  sampler.addressV = Address((f[0] >> 13) & 7);
  // Stack layer indices clamp independently of the sampler's unused W mode.
  sampler.addressW = t.dimension==1&&t.layers>1?RenderTextureAddressMode::CLAMP:Address((f[0] >> 16) & 7);
  sampler.minLOD = float(t.min_mip);
  sampler.maxLOD = float(mip == 2 ? t.min_mip : t.max_mip);
  sampler.mipLODBias = SignedField(f[4], 12, 10) / 32.0f;
  uint32_t aniso = (f[3] >> 25) & 7;
  if (aniso > 5) throw std::runtime_error("Invalid texture anisotropy");
  if(anisotropy_override>=0 && t.max_mip>t.min_mip && mip!=2 && mag==1 && min==1)
    aniso=uint32_t(anisotropy_override);
  if(!max_anisotropy)aniso=0;
  // Anisotropic sampling also forces trilinear filtering, including the 1x
  // mode. Keep this behavior even when the host limits anisotropy to 1x.
  if(aniso) {
    sampler.magFilter=sampler.minFilter=RenderFilter::LINEAR;
    sampler.mipmapMode=RenderMipmapMode::LINEAR;
  }
  sampler.maxAnisotropy=std::min(aniso>1?1u<<(aniso-1):1u,std::max(1u,max_anisotropy));
  sampler.anisotropyEnabled=sampler.maxAnisotropy>1;
  if ((f[5] & 3) > 1) throw std::runtime_error("YUV border colors are not supported");
  sampler.borderColor = (f[5] & 3) ? RenderBorderColor::OPAQUE_WHITE : RenderBorderColor::TRANSPARENT_BLACK;
  return sampler;
}

NativeTexture UploadTexture(RenderDevice& device, RenderCommandList& commands, const TextureData& data,
                            int32_t anisotropy_override,uint32_t max_anisotropy) {
  const auto& t = data.layout;
  const auto format = NativeTextureFormat(t);
  NativeTexture result;
  const bool stacked=t.dimension==1&&t.layers>1;
  auto desc = t.dimension == 2 || stacked ? RenderTextureDesc::Texture3D(t.width, t.height, stacked?t.layers:t.depth, t.max_mip + 1, format)
                             : RenderTextureDesc::Texture2D(t.width, t.height, t.max_mip + 1, format);
  if (t.dimension == 3) { desc.arraySize = 6; desc.flags |= RenderTextureFlag::CUBE; }
  result.image = device.createTexture(desc);
  if (!result.image) throw std::runtime_error("Failed to create native texture");
  result.binding = BindTexture(device, *result.image, t,false,anisotropy_override,max_anisotropy);
  const bool depth = t.format == 22 || t.format == 23;
  const bool decode_normalized=DecodeNormalizedToFloat(t);
  std::vector<uint64_t> offsets;
  offsets.reserve(data.levels.size());
  uint64_t bytes = 0;size_t scratch_bytes=0;
  for (const auto& level : data.levels) {
    const size_t converted_bytes=depth||decode_normalized?
        TextureUploadConversionSize(t,level.bytes.size()):level.bytes.size();
    if(bytes>UINT64_MAX-511||converted_bytes>UINT64_MAX-((bytes+511)&~511ull))
      throw std::runtime_error("Texture staging extent overflow");
    bytes = (bytes + 511) & ~511ull;
    offsets.push_back(bytes);
    bytes += converted_bytes;
    if(depth||decode_normalized)scratch_bytes=std::max(scratch_bytes,std::min<size_t>(converted_bytes,64u*1024));
  }
  // Upload allocations may be write-combined or device-local host mappings.
  // Reading them back per depth texel stalls badly on such memory. Convert
  // only cached source bytes, then issue sequential bulk writes to staging.
  // The scratch bound is independent of the image and mip-chain dimensions.
  std::vector<uint8_t> converted(scratch_bytes);
  result.staging = device.createBuffer(RenderBufferDesc::UploadBuffer(bytes));
  if (!result.staging) throw std::runtime_error("Failed to allocate texture staging buffer");
  auto* mapped = static_cast<uint8_t*>(result.staging->map());
  if (!mapped) throw std::runtime_error("Failed to map texture staging buffer");
  for (size_t i = 0; i < data.levels.size(); ++i) {
    if (!depth&&!decode_normalized) {
      std::memcpy(mapped + offsets[i], data.levels[i].bytes.data(), data.levels[i].bytes.size());
      continue;
    }
    const auto& source = data.levels[i].bytes;
    const size_t output_stride=depth?4:16;
    for(size_t input=0,output=0;input<source.size();) {
      const size_t input_bytes=std::min(source.size()-input,converted.size()/output_stride*t.block.bytes);
      const size_t output_bytes=input_bytes/t.block.bytes*output_stride;
      ConvertTextureUploadTexels(t,std::span(source).subspan(input,input_bytes),
          std::span(converted).first(output_bytes));
      std::memcpy(mapped+offsets[i]+output,converted.data(),output_bytes);
      input+=input_bytes;output+=output_bytes;
    }
  }
  auto* vulkan = static_cast<VulkanBuffer*>(result.staging.get());
  if (vmaFlushAllocation(vulkan->device->allocator, vulkan->allocation, 0, VK_WHOLE_SIZE) != VK_SUCCESS)
    throw std::runtime_error("Failed to flush texture staging buffer");
  result.staging->unmap();
  commands.barriers(RenderBarrierStage::COPY, RenderTextureBarrier(result.image.get(), RenderTextureLayout::COPY_DEST));
  for (size_t i = 0; i < data.levels.size(); ++i) {
    const auto& l = data.levels[i].layout;
    commands.copyTextureRegion(RenderTextureCopyLocation::Subresource(result.image.get(), l.mip, stacked?0:l.layer),
        RenderTextureCopyLocation::PlacedFootprint(result.staging.get(), format, l.width, l.height, l.depth, l.width, offsets[i]),
        0, 0, stacked?l.layer:0, nullptr);
  }
  commands.barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(result.image.get(), RenderTextureLayout::SHADER_READ));
  return result;
}
} // namespace gow3::renderer
