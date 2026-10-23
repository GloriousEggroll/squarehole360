// Geometry diagnostic: an actual Gears vertex shader and mesh captured at the
// D3D boundary, submitted directly to native Vulkan through plume. Material
// mode also replays CPU textures and raster state, with initially empty depth.
#include "plume_render_interface.h"
#include "plume_vulkan.h"
#include "renderer/native/shader_abi.h"
#include "renderer/native/texture_upload.h"
#include "renderer/native/draw_state.h"
#include <array>
#include <bit>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace plume { std::unique_ptr<RenderInterface> CreateVulkanInterface(); }
using namespace plume;

void Read(std::ifstream& input, void* data, size_t size) {
    input.read(static_cast<char*>(data), size);
    if (!input) throw std::runtime_error("Truncated replay data");
}
uint32_t Word(std::ifstream& input) {
    uint32_t value;
    Read(input, &value, 4);
    if constexpr (std::endian::native != std::endian::little) value = std::byteswap(value);
    return value;
}
std::vector<uint32_t> Spirv(const char* path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    const auto size = input.tellg();
    if (!input || size < 20 || size > 16 * 1024 * 1024 || size % 4)
        throw std::runtime_error("Invalid SPIR-V file");
    input.seekg(0);
    std::vector<uint32_t> result(size_t(size) / 4);
    for (auto& word : result) word = Word(input);
    if (result[0] != 0x07230203) throw std::runtime_error("Invalid SPIR-V magic");
    return result;
}
void Flush(RenderBuffer* buffer) {
    auto* vk = static_cast<VulkanBuffer*>(buffer);
    if (vmaFlushAllocation(vk->device->allocator, vk->allocation, 0, VK_WHOLE_SIZE) != VK_SUCCESS)
        throw std::runtime_error("Upload flush failed");
}

int main(int argc, char** argv) try {
    if (argc != 5) throw std::runtime_error("Usage: gow3-native-mesh-replay <mesh.g3m> <vertex.spv> <pixel.spv> <output.ppm>");
    std::ifstream input(argv[1], std::ios::binary);
    std::array<char, 8> magic{};
    Read(input, magic.data(), 8);
    const bool extended_raster = !std::memcmp(magic.data(), "G3MESH04", 8);
    const bool raster = extended_raster || !std::memcmp(magic.data(), "G3MESH03", 8);
    const bool material = raster || !std::memcmp(magic.data(), "G3MESH02", 8);
    if (!material && std::memcmp(magic.data(), "G3MESH01", 8)) throw std::runtime_error("Unknown replay format");
    const uint32_t width = Word(input), height = Word(input), primitive = Word(input);
    const uint32_t index_width = Word(input), index_count = Word(input), stream_count = Word(input);
    if (!width || width > 4096 || !height || height > 4096 ||
        (primitive != 4 && primitive != 6) || (index_width != 2 && index_width != 4) ||
        !index_count || index_count > 4 * 1024 * 1024 || stream_count > 96)
        throw std::runtime_error("Invalid replay dimensions/counts");
    gow3::renderer::DrawState draw_state;
    if (raster) {
        std::array<uint32_t, gow3::renderer::kDrawStateWords> words{};
        for (size_t i=0;i<(extended_raster?words.size():116);++i) words[i]=Word(input);
        if(!extended_raster && (words[(0x2948-0x2880)/4]&(7u<<11)))
            throw std::runtime_error("Re-export this mesh with polygon offset registers (G3MESH04)");
        draw_state = gow3::renderer::DecodeDrawState(words, width, height, primitive);
    }
    std::array<uint32_t, 1024> vertex_constants;
    for (auto& value : vertex_constants) value = Word(input);
    std::array<uint32_t, 1024> pixel_constants{};
    if (material) for (auto& value : pixel_constants) value = Word(input);
    gow3::renderer::SharedShaderConstants shared;
    shared.half_pixel = draw_state.half_pixel;
    shared.alpha_threshold = draw_state.alpha_reference;
    for (auto& value : shared.booleans) value = Word(input);
    for (auto& value : shared.loops) value = Word(input);
    auto interface = CreateVulkanInterface();
    if (!interface) throw std::runtime_error("Vulkan unavailable");
    auto device = interface->createDevice();
    if (!device) throw std::runtime_error("No compatible Vulkan device");
    auto queue = device->createCommandQueue(RenderCommandListType::DIRECT);
    auto commands = queue->createCommandList();
    auto fence = device->createCommandFence();
    std::vector<std::unique_ptr<RenderBuffer>> streams;
    size_t total_bytes = 0;
    for (uint32_t i = 0; i < stream_count; ++i) {
        const uint32_t slot = Word(input), size = Word(input), endian = Word(input);
        if (slot >= 96 || !size || size > 8 * 1024 * 1024 || endian > 3 || shared.vertex_streams[slot].address)
            throw std::runtime_error("Invalid vertex stream");
        total_bytes += size;
        if (total_bytes > 64 * 1024 * 1024) throw std::runtime_error("Replay exceeds geometry limit");
        auto buffer = device->createBuffer(RenderBufferDesc::UploadBuffer(size,
            RenderBufferFlag::CONSTANT | RenderBufferFlag::DEVICE_ADDRESSABLE));
        Read(input, buffer->map(), size);
        Flush(buffer.get()); buffer->unmap();
        shared.vertex_streams[slot] = {buffer->getDeviceAddress(), size, endian};
        streams.push_back(std::move(buffer));
    }
    const uint32_t index_bytes = index_width * index_count;
    auto indices = device->createBuffer(RenderBufferDesc::UploadBuffer(index_bytes, RenderBufferFlag::INDEX));
    Read(input, indices->map(), index_bytes);
    Flush(indices.get()); indices->unmap();
    const RenderDescriptorRange image_range(RenderDescriptorRangeType::TEXTURE, 0, 32);
    const RenderDescriptorRange sampler_range(RenderDescriptorRangeType::SAMPLER, 0, 32);
    const std::array<RenderDescriptorSetDesc, 4> descriptor_descs{{
        {&image_range, 1, true, 32}, {&image_range, 1, true, 32},
        {&image_range, 1, true, 32}, {&sampler_range, 1, true, 32}}};
    std::array<std::unique_ptr<RenderDescriptorSet>, 4> descriptors;
    std::vector<gow3::renderer::NativeTexture> textures;
    if (material) {
        for (size_t i = 0; i < 4; ++i) descriptors[i] = device->createDescriptorSet(descriptor_descs[i]);
        const uint32_t count = Word(input);
        if (count > 32) throw std::runtime_error("Too many texture bindings");
        std::array<bool, 32> bound{};
        commands->begin();
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t slot = Word(input);
            if (slot >= 32 || bound[slot]) throw std::runtime_error("Invalid texture slot");
            bound[slot] = true;
            std::array<uint32_t, 6> fetch;
            for (auto& word : fetch) word = Word(input);
            const uint32_t base_size = Word(input), mip_size = Word(input);
            total_bytes += uint64_t(base_size) + mip_size;
            if (base_size > 8 * 1024 * 1024 || mip_size > 8 * 1024 * 1024 || total_bytes > 256 * 1024 * 1024)
                throw std::runtime_error("Texture input exceeds replay limit");
            std::vector<uint8_t> base(base_size), mips(mip_size);
            Read(input, base.data(), base_size); Read(input, mips.data(), mip_size);
            try {
                auto data = gow3::renderer::DecodeTexture(fetch, base, mips);
                textures.push_back(gow3::renderer::UploadTexture(*device, *commands, data));
            } catch (const std::exception& error) {
                throw std::runtime_error("Texture slot " + std::to_string(slot) + ": " + error.what());
            }
            const auto& texture = textures.back();
            const uint32_t set = texture.binding.dimension == 2 ? 1 : texture.binding.dimension == 3 ? 2 : 0;
            descriptors[set]->setTexture(slot, texture.image.get(), RenderTextureLayout::SHADER_READ, texture.binding.view.get());
            descriptors[3]->setSampler(slot, texture.binding.sampler.get());
            shared.textures_2d[slot] = shared.textures_3d[slot] = shared.textures_cube[slot] = shared.samplers[slot] = slot;
            shared.texture_properties[slot] = texture.binding.properties;
        }
        commands->end(); queue->executeCommandLists(commands.get(), fence.get()); queue->waitForCommandFence(fence.get());
    }
    if (input.peek() != std::char_traits<char>::eof()) throw std::runtime_error("Trailing replay data");
    constexpr size_t constant_size = 8192 + sizeof(shared);
    auto constants = device->createBuffer(RenderBufferDesc::UploadBuffer(constant_size,
        RenderBufferFlag::CONSTANT | RenderBufferFlag::DEVICE_ADDRESSABLE));
    auto* mapped = static_cast<uint8_t*>(constants->map());
    std::memset(mapped, 0, constant_size);
    std::memcpy(mapped, vertex_constants.data(), 4096);
    constexpr std::array<float, 4> color{1.0f, 0.12f, 0.04f, 1.0f};
    if (material) std::memcpy(mapped + 4096, pixel_constants.data(), 4096);
    else std::memcpy(mapped + 4096 + 7 * 16, color.data(), 16);
    std::memcpy(mapped + 8192, &shared, sizeof(shared));
    Flush(constants.get()); constants->unmap();
    const auto vertex = Spirv(argv[2]), pixel = Spirv(argv[3]);
    auto vs = device->createShader(vertex.data(), vertex.size() * 4, "main", RenderShaderFormat::SPIRV);
    auto ps = device->createShader(pixel.data(), pixel.size() * 4, "main", RenderShaderFormat::SPIRV);
    const RenderPushConstantRange push(0, 0, 0, 24, RenderShaderStageFlag::VERTEX | RenderShaderStageFlag::PIXEL);
    auto layout = device->createPipelineLayout(RenderPipelineLayoutDesc(&push, 1,
        material ? descriptor_descs.data() : nullptr, material ? 4 : 0));
    constexpr auto format = RenderFormat::R8G8B8A8_UNORM;
    auto target = device->createTexture(RenderTextureDesc::ColorTarget(width, height, format));
    const RenderTexture* attachment = target.get();
    RenderFramebufferDesc framebuffer_desc;
    framebuffer_desc.colorAttachments = &attachment;
    framebuffer_desc.colorAttachmentsCount = 1;
    std::unique_ptr<RenderTexture> depth;
    if (raster) {
        depth = device->createTexture(RenderTextureDesc::DepthTarget(width, height, RenderFormat::D32_FLOAT_S8_UINT));
        framebuffer_desc.depthAttachment = depth.get();
    }
    auto framebuffer = device->createFramebuffer(framebuffer_desc);
    RenderGraphicsPipelineDesc pipeline_desc = draw_state.pipeline;
    pipeline_desc.pipelineLayout = layout.get();
    pipeline_desc.vertexShader = vs.get(); pipeline_desc.pixelShader = ps.get();
    pipeline_desc.renderTargetCount = 1; pipeline_desc.renderTargetFormat[0] = format;
    if (!raster) pipeline_desc.renderTargetBlend[0] = RenderBlendDesc::Copy();
    else pipeline_desc.depthTargetFormat = RenderFormat::D32_FLOAT_S8_UINT;
    const RenderSpecConstant specialization(0, draw_state.specialization);
    pipeline_desc.specConstants = &specialization; pipeline_desc.specConstantsCount = 1;
    pipeline_desc.primitiveTopology = primitive == 4 ? RenderPrimitiveTopology::TRIANGLE_LIST : RenderPrimitiveTopology::TRIANGLE_STRIP;
    auto pipeline = device->createGraphicsPipeline(pipeline_desc);
    if (!pipeline) throw std::runtime_error("Pipeline creation failed");
    auto readback = device->createBuffer(RenderBufferDesc::ReadbackBuffer(width * height * 4));
    const gow3::renderer::ShaderPushConstants addresses{constants->getDeviceAddress(), constants->getDeviceAddress() + 4096,
                                                       constants->getDeviceAddress() + 8192};
    commands->begin();
    commands->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(target.get(), RenderTextureLayout::COLOR_WRITE));
    if (depth) commands->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(depth.get(), RenderTextureLayout::DEPTH_WRITE));
    commands->setFramebuffer(framebuffer.get());
    commands->setViewports(raster ? draw_state.viewport : RenderViewport(0, 0, width, height));
    commands->setScissors(raster && !draw_state.discard ? draw_state.scissor : RenderRect(0, 0, width, height));
    commands->clearColor(0, RenderColor(0, 0, 0, 1));
    if (depth) commands->clearDepthStencil(true, true, draw_state.viewport.minDepth > draw_state.viewport.maxDepth ? 0.0f : 1.0f, 0);
    commands->setGraphicsPipelineLayout(layout.get()); commands->setGraphicsPushConstants(0, &addresses);
    if (material) for (uint32_t i = 0; i < 4; ++i) commands->setGraphicsDescriptorSet(descriptors[i].get(), i);
    commands->setPipeline(pipeline.get());
    if(draw_state.pipeline.dynamicDepthBiasEnabled)commands->setDepthBias(draw_state.depth_bias,0,draw_state.slope_depth_bias);
    const RenderIndexBufferView index_view(indices.get(), index_bytes, index_width == 2 ? RenderFormat::R16_UINT : RenderFormat::R32_UINT);
    commands->setIndexBuffer(&index_view);
    if (!draw_state.discard) commands->drawIndexedInstanced(index_count, 1, 0, draw_state.index_offset, 0);
    commands->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(target.get(), RenderTextureLayout::COPY_SOURCE));
    auto* vk_cmd = static_cast<VulkanCommandList*>(commands.get());
    auto* vk_readback = static_cast<VulkanBuffer*>(readback.get());
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {width, height, 1};
    vkCmdCopyImageToBuffer(vk_cmd->vk, static_cast<VulkanTexture*>(target.get())->vk,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, vk_readback->vk, 1, &copy);
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(vk_cmd->vk, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    commands->end(); queue->executeCommandLists(commands.get(), fence.get()); queue->waitForCommandFence(fence.get());
    const auto* pixels = static_cast<const uint8_t*>(readback->map());
    if (vmaInvalidateAllocation(vk_readback->device->allocator, vk_readback->allocation, 0, VK_WHOLE_SIZE) != VK_SUCCESS)
        throw std::runtime_error("Readback invalidate failed");
    std::ofstream output(argv[4], std::ios::binary);
    output << "P6\n" << width << ' ' << height << "\n255\n";
    size_t covered = 0;
    for (size_t i = 0; i < size_t(width) * height; ++i) {
        output.write(reinterpret_cast<const char*>(pixels + i * 4), 3);
        covered += pixels[i * 4] != 0;
    }
    readback->unmap(); output.close();
    if (!output) throw std::runtime_error("Failed writing preview");
    std::cout << "Native geometry preview: " << index_count << " indices, " << stream_count << " streams, "
              << covered << " covered pixels. " << (raster ? "Captured material and raster state; isolated draw, empty depth, RGBA8 target."
                                                : material ? "Captured shader pair, constants and CPU texture snapshots; substitute raster state."
                                                            : "Captured Gears vertex shader; substitute material/raster state.") << '\n';
    return covered ? 0 : 2;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
