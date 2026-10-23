#pragma once
#include "texture_data.h"
#include "shader_abi.h"
#include "plume_render_interface.h"

namespace gow3::renderer {
// Keep this object (including its staging buffer) alive until the submission
// fence completes. It owns all objects referenced by the descriptor sets.
struct TextureBinding {
  std::unique_ptr<plume::RenderTextureView> view;
  std::unique_ptr<plume::RenderSampler> sampler;
  SharedShaderConstants::TextureProperties properties{};
  uint32_t dimension = 1;
  // Scene-local immutable view/sampler pair identity; never serialized or
  // recycled. Numeric descriptor metadata retains no additional resource owner.
  uint64_t descriptor_generation = 0;
};
struct NativeTexture {
  std::unique_ptr<plume::RenderTexture> image;
  // The optional GPU raw-copy output shares ownership with its transfer packet.
  // Preserve the ordinary CPU image's unique ownership and destruction order.
  std::shared_ptr<plume::RenderTexture> raw_gpu_image;
  TextureBinding binding;
  std::unique_ptr<plume::RenderBuffer> staging;
  plume::RenderTexture* GetImage() const {return raw_gpu_image?raw_gpu_image.get():image.get();}
};
plume::RenderFormat NativeTextureFormat(const TextureLayout& layout);
// Override applies only to eligible filtered mip chains, matching the guest
// backend's graphics setting. The limit reflects actually enabled host support.
plume::RenderSamplerDesc TextureSampler(const TextureLayout& layout,
                                       int32_t anisotropy_override=-1,uint32_t max_anisotropy=16);
TextureBinding BindTexture(plume::RenderDevice& device, plume::RenderTexture& image,
                           const TextureLayout& layout,bool decoded_float=false,
                           int32_t anisotropy_override=-1,uint32_t max_anisotropy=16);
NativeTexture UploadTexture(plume::RenderDevice& device, plume::RenderCommandList& commands,
                            const TextureData& data,int32_t anisotropy_override=-1,
                            uint32_t max_anisotropy=16);
} // namespace gow3::renderer
