// Execute an extracted Gears shader through plume on native Vulkan. This is
// an offscreen ABI test, NOT a replacement game renderer or a scene replay.
// Input: SPIR-V compiled from global shader 0a94610849f6cdeb...5e4cfa0b63,
// whose single ALU instruction exports max(c7, c7) to color 0.
#include "plume_render_interface.h"
#include "plume_vulkan.h"
#include "fullscreen_spv.h"
#include "renderer/native/shader_abi.h"
#include "renderer/native/texture_upload.h"
#include "renderer/native/draw_state.h"
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace plume { std::unique_ptr<RenderInterface> CreateVulkanInterface(); }
using namespace plume;

std::vector<uint32_t> ReadSpirv(const char* path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("Cannot read pixel SPIR-V");
    auto size = file.tellg();
    if (size < 20 || size > 16 * 1024 * 1024 || size % 4)
        throw std::runtime_error("Invalid SPIR-V length");
    std::vector<uint32_t> words(size_t(size) / 4);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(words.data()), size);
    if (!file || words[0] != 0x07230203) throw std::runtime_error("Invalid SPIR-V");
    return words;
}

int main(int argc, char** argv) try {
    if (argc < 2 || argc > 5) throw std::runtime_error("Usage: gow3-native-shader-test <pixel SPIR-V> [mode] [vertex SPIR-V] [vertex cases]");
    const std::string mode = argc >= 3 ? argv[2] : "constant";
    const bool texture_test = mode.starts_with("texture-") || mode == "vertex-texture-1d";
    const bool texture_upload = mode.starts_with("texture-upload-");
    const bool mixed_texture = mode.starts_with("texture-upload-mixed");
    const bool stacked_texture = mode == "texture-stacked";
    const bool vertex_texture = mode == "vertex-texture-1d";
    const bool vertex_fetch = mode == "vertex-fetch";
    const bool raster_test = mode == "raster-state";
    const bool vector_kill = mode.starts_with("vector-kill-");
    const bool vertex_pulling = vertex_texture || vertex_fetch;
    if (mode != "constant" && mode != "alpha-compare" && mode != "color-scale" && mode != "alu-paired" && mode != "gradients" && mode != "loops" && !texture_test && !vertex_fetch && !raster_test && !vector_kill)
        throw std::runtime_error("Unknown shader test mode");
    if ((vertex_pulling && argc != (vertex_fetch ? 5 : 4)) || (!vertex_pulling && argc > 3))
        throw std::runtime_error("Vertex modes require a vertex shader, and vertex-fetch requires a cases file");
    const auto code = ReadSpirv(argv[1]);
    auto interface = CreateVulkanInterface();
    if (!interface) throw std::runtime_error("Vulkan interface unavailable");
    auto device = interface->createDevice();
    if (!device) throw std::runtime_error("No compatible Vulkan device");
    auto queue = device->createCommandQueue(RenderCommandListType::DIRECT);
    auto commands = queue->createCommandList();
    auto fence = device->createCommandFence();
    auto vs = device->createShader(fullscreen_spv, sizeof(fullscreen_spv), "main", RenderShaderFormat::SPIRV);
    if (vertex_pulling) {
        const auto vertex_code = ReadSpirv(argv[3]);
        vs = device->createShader(vertex_code.data(), vertex_code.size() * 4, "main", RenderShaderFormat::SPIRV);
    }
    auto ps = device->createShader(code.data(), code.size() * 4, "main", RenderShaderFormat::SPIRV);
    const RenderPushConstantRange push(0, 0, 0, 24, RenderShaderStageFlag::VERTEX | RenderShaderStageFlag::PIXEL);
    const RenderDescriptorRange image_range(RenderDescriptorRangeType::TEXTURE, 0, 1);
    const RenderDescriptorRange sampler_range(RenderDescriptorRangeType::SAMPLER, 0, 1);
    const std::array<RenderDescriptorSetDesc, 4> descriptor_descs{{
        {&image_range, 1, true, 1}, {&image_range, 1, true, 1},
        {&image_range, 1, true, 1}, {&sampler_range, 1, true, 1}}};
    auto layout = device->createPipelineLayout(RenderPipelineLayoutDesc(&push, 1,
        texture_test ? descriptor_descs.data() : nullptr, texture_test ? 4 : 0));
    constexpr uint32_t width = 16, height = 16;
    constexpr auto format = RenderFormat::R32G32B32A32_FLOAT;
    auto target = device->createTexture(RenderTextureDesc::ColorTarget(width, height, format));
    const RenderTexture* attachment = target.get();
    RenderFramebufferDesc framebuffer_desc;
    framebuffer_desc.colorAttachments = &attachment;
    framebuffer_desc.colorAttachmentsCount = 1;
    std::unique_ptr<RenderTexture> depth;
    if (raster_test) {
        depth = device->createTexture(RenderTextureDesc::DepthTarget(width, height, RenderFormat::D32_FLOAT_S8_UINT));
        framebuffer_desc.depthAttachment = depth.get();
    }
    auto framebuffer = device->createFramebuffer(framebuffer_desc);
    constexpr size_t constant_bytes = 2 * 4096 + sizeof(gow3::renderer::SharedShaderConstants);
    auto constants = device->createBuffer(RenderBufferDesc::UploadBuffer(constant_bytes,
        RenderBufferFlag::CONSTANT | RenderBufferFlag::DEVICE_ADDRESSABLE));
    auto readback = device->createBuffer(RenderBufferDesc::ReadbackBuffer(width * height * 16));
    std::array<uint64_t, 3> addresses{constants->getDeviceAddress(), constants->getDeviceAddress() + 4096,
                                    constants->getDeviceAddress() + 8192};
    if (!addresses[0]) throw std::runtime_error("Buffer device addresses unavailable");
    constexpr std::array<std::array<float, 4>, 5> color_samples{{
        {0, 0, 0, 1}, {1, 0.25f, 0.75f, 1}, {-0.5f, 4, 0.125f, 1},
        {0.2f, 0.4f, 0.6f, 0.25f}, {0.2f, 0.4f, 0.6f, 0.75f}}};
    constexpr std::array<float, 4> clear{0.875f, 0.125f, 0.625f, 0.5f};
    constexpr std::array<float, 4> kill_survivor{0.125f, 0.375f, 0.625f, 1.f};
    struct Sample {
        std::array<float, 4> input{}, expected{};
        uint32_t outer = 0, inner = 0;
        float lod = 0;
        bool alpha = false;
        std::array<uint8_t, 32> vertex_bytes{};
        uint32_t vertex_endian = 0, vertex_size = 0;
        std::array<uint32_t, gow3::renderer::kDrawStateWords> raster_words{};
        float depth_clear = .5f;
        uint32_t stencil_clear = 0;
        bool scissor_check = false;
        float color_scale = 1;
        uint32_t alpha_function = 6;
        float alpha_reference = .5f;
        std::array<float,4> kill_reference{};
    };
    std::vector<Sample> samples;
    if (raster_test) {
        Sample base;
        base.input = {0.25f, .5f, .75f, .5f}; base.expected = base.input;
        auto put = [&](Sample& s, uint32_t address, uint32_t value) { s.raster_words[(address - 0x2880) / 4] = value; };
        auto fp = [&](uint32_t address, float value) { put(base, address, std::bit_cast<uint32_t>(value)); };
        put(base, 0x28bc, 0x20002000); put(base, 0x28c8, (height << 16) | width);
        put(base, 0x28dc, 15); put(base, 0x2944, 1u << 19); put(base, 0x294c, 0x43f);
        put(base, 0x2938, 0x10001); put(base, 0x2958, 0x10001); put(base, 0x295c, 0x10001); put(base, 0x2960, 0x10001);
        fp(0x2908, width / 2); fp(0x290c, width / 2); fp(0x2910, -float(height) / 2); fp(0x2914, height / 2);
        fp(0x2918, 0); fp(0x291c, .5f);
        // Eight depth comparisons at three clear depths. Shader depth is .5.
        for (uint32_t function = 0; function < 8; ++function) {
            for (float prior : {.25f, .5f, .75f}) {
                auto s = base; s.depth_clear = prior;
                const std::array<bool, 8> passes{false, .5f < prior, .5f == prior, .5f <= prior,
                    .5f > prior, .5f != prior, .5f >= prior, true};
                put(s, 0x2934, 2 | (function << 4));
                s.expected = passes[function] ? s.input : clear; samples.push_back(s);
            }
        }
        // Front-face and cull direction: the fixture has positive window-space area.
        for(float bias:{-0.00001f,0.f,0.00001f}) {
            auto s=base;put(s,0x2948,3u<<11);put(s,0x2934,2|(3<<4));put(s,0x2888,0x10000);
            put(s,0x2a54,std::bit_cast<uint32_t>(bias));put(s,0x2a5c,std::bit_cast<uint32_t>(bias));
            s.expected=bias>0?clear:s.input;samples.push_back(s);
        }
        for (uint32_t face = 0; face < 2; ++face) for (uint32_t cull = 0; cull < 4; ++cull) {
            auto s = base; put(s, 0x2948, (face << 2) | cull);
            s.expected = ((face ? (cull & 1) : (cull & 2)) != 0) ? clear : s.input;
            samples.push_back(s);
        }
        { auto s = base; put(s, 0x28c4, (3 << 16) | 2); put(s, 0x28c8, (11 << 16) | 10);
          s.scissor_check = true; samples.push_back(s); }
        // Source-alpha blend plus a restricted color write mask.
        { auto s = base; put(s, 0x2938, 0x00010706); put(s, 0x28dc, 5);
          s.expected = {s.input[0]*.5f + clear[0]*.5f, clear[1], s.input[2]*.5f + clear[2]*.5f, clear[3]};
          samples.push_back(s); }
        // Stencil equality with nontrivial reference/mask, match and mismatch.
        for (uint32_t stencil : {0x15u, 0x14u}) {
            auto s = base; put(s, 0x2934, 1 | (2 << 8)); put(s, 0x2900, 0xff0f05);
            s.stencil_clear = stencil; s.expected = stencil == 0x15 ? s.input : clear; samples.push_back(s);
        }
    } else if (mode == "constant") {
        for (size_t i = 0; i < color_samples.size(); ++i)
            samples.push_back({color_samples[i], i == 3 ? clear : color_samples[i], 0, 0, 0, i >= 3});
    } else if(mode=="alpha-compare") {
        const float nan=std::numeric_limits<float>::quiet_NaN(),inf=std::numeric_limits<float>::infinity();
        for(uint32_t function=0;function<8;++function)for(float reference:{0.f,.5f,1.f,nan})for(float alpha:{.25f,.5f,.75f,nan,-inf,inf}) {
            Sample s;s.input={.25f,.5f,.75f,alpha};s.alpha=true;s.alpha_function=function;
            s.alpha_reference=reference;
            const std::array<bool,8> pass{false,alpha<reference,alpha==reference,alpha<=reference,alpha>reference,alpha!=reference,alpha>=reference,true};
            s.expected=pass[function]?s.input:clear;samples.push_back(s);
        }
    } else if (vector_kill) {
        const auto operation=mode.substr(12,2);
        const auto mask=mode.substr(14);
        if((operation!="eq"&&operation!="gt"&&operation!="ge"&&operation!="ne")||
           (mask!="-mask0"&&mask!="-mask1"&&mask!="-mask5"&&mask!="-mask15"))
            throw std::runtime_error("Unknown vector kill test mode");
        Sample survives;survives.input.fill(operation=="eq"?1.f:operation=="ne"?0.f:-1.f);
        survives.expected=kill_survivor;samples.push_back(survives);
        for(unsigned channel=0;channel<4;++channel) {
            auto killed=survives;
            killed.input[channel]=(operation=="eq"||operation=="ge")?0.f:1.f;
            killed.expected=clear;samples.push_back(killed);
            // Equality/ordered comparisons survive an unordered component;
            // not-equal kills it, just like the reference FUnordNotEqual path.
            auto unordered=survives;
            unordered.input[channel]=std::numeric_limits<float>::quiet_NaN();
            unordered.expected=operation=="ne"?clear:kill_survivor;samples.push_back(unordered);
            auto unordered_reference=survives;
            unordered_reference.kill_reference[channel]=std::numeric_limits<float>::quiet_NaN();
            unordered_reference.expected=operation=="ne"?clear:kill_survivor;samples.push_back(unordered_reference);
        }
        for(float value:{0.f,-0.f,1.f}) {
            auto boundary=survives;boundary.input.fill(value);
            const bool kills=value==0.f?(operation=="eq"||operation=="ge"):operation!="eq";
            boundary.expected=kills?clear:kill_survivor;samples.push_back(boundary);
        }
    } else if (mode == "alu-paired") {
        for(const auto& input:color_samples) {
            const float a=input[0];
            samples.push_back({input,{a,3*a,4*a,3*a}});
        }
    } else if (mode == "color-scale") {
        for(float scale:{1.f/32,32.f})for(size_t i=0;i<color_samples.size();++i) {
            Sample s;s.input=color_samples[i];s.expected=s.input;s.alpha=i>=3;s.color_scale=scale;
            for(float& v:s.expected)v*=scale;
            if(i==3)s.expected=clear; // Compare alpha before exponent scaling.
            samples.push_back(s);
        }
    } else if (mode == "gradients") {
        samples.push_back({{}, {2.0f / width, 3.0f / height, 5.0f / width, 7.0f / height}});
    } else if (mode == "loops") {
        // Expected sums are computed independently from the generated program:
        // c[7+i] = (i+1) * (.25,.5,1,2); repeated inner loop inherits outer i.
        for (auto loops : std::array<std::array<int, 6>, 4>{{
            {3, 2, 1, 2, 0, 1}, {3, 5, -1, 2, 0, -1},
            {2, 8, -2, 3, 0, -1}, {0, 0, 1, 2, 0, 1}}}) {
            auto pack = [](int count, int start, int step) {
                return uint32_t(count) | uint32_t(start) << 8 | uint32_t(uint8_t(step)) << 16;
            };
            float sum = 0;
            for (int outer = 0; outer < loops[0]; ++outer)
                for (int inner = 0; inner < loops[3]; ++inner)
                    sum += loops[1] + outer * loops[2] + inner * loops[5] + 1;
            samples.push_back({{}, {sum * .25f, sum * .5f, sum, sum * 2},
                               pack(loops[0], loops[1], loops[2]), pack(loops[3], loops[4], loops[5])});
        }
    } else if (vertex_fetch) {
        std::ifstream cases(argv[4], std::ios::binary);
        std::array<char, 8> magic{};
        cases.read(magic.data(), magic.size());
        if (!cases || std::memcmp(magic.data(), "G3VF0001", 8)) throw std::runtime_error("Invalid vertex cases file");
        auto read_word = [&]() {
            std::array<uint8_t, 4> b{};
            cases.read(reinterpret_cast<char*>(b.data()), 4);
            if (!cases) throw std::runtime_error("Truncated vertex test case");
            return uint32_t(b[0]) | uint32_t(b[1]) << 8 | uint32_t(b[2]) << 16 | uint32_t(b[3]) << 24;
        };
        const uint32_t count = read_word();
        if (!count || count > 256) throw std::runtime_error("Invalid vertex case count");
        for (uint32_t i = 0; i < count; ++i) {
            Sample sample;
            cases.read(reinterpret_cast<char*>(sample.vertex_bytes.data()), sample.vertex_bytes.size());
            for (auto& value : sample.expected) value = std::bit_cast<float>(read_word());
            sample.vertex_endian = read_word(); sample.vertex_size = read_word();
            if (sample.vertex_endian > 3 || sample.vertex_size > sample.vertex_bytes.size())
                throw std::runtime_error("Invalid vertex stream descriptor");
            samples.push_back(sample);
        }
        if (cases.peek() != std::char_traits<char>::eof()) throw std::runtime_error("Trailing vertex case bytes");
    }
    auto flush = [](RenderBuffer* buffer) {
        auto* vk = static_cast<VulkanBuffer*>(buffer);
        if (vmaFlushAllocation(vk->device->allocator, vk->allocation, 0, VK_WHOLE_SIZE) != VK_SUCCESS)
            throw std::runtime_error("Upload flush failed");
    };
    std::unique_ptr<RenderTexture> texture;
    std::unique_ptr<RenderSampler> sampler;
    std::unique_ptr<RenderDescriptorSet> image_set, sampler_set;
    std::unique_ptr<RenderBuffer> vertices, vertex_data;
    std::unique_ptr<gow3::renderer::NativeTexture> uploaded_texture;
    if (vertex_pulling) {
        constexpr auto flags = RenderBufferFlag::CONSTANT | RenderBufferFlag::DEVICE_ADDRESSABLE;
        vertices = device->createBuffer(RenderBufferDesc::UploadBuffer(48, flags));
        vertex_data = device->createBuffer(RenderBufferDesc::UploadBuffer(32, flags));
        constexpr std::array<float, 12> positions{-1, 1, 0, 1, 3, 1, 0, 1, -1, -3, 0, 1};
        std::memcpy(vertices->map(), positions.data(), 48);
        flush(vertices.get()); vertices->unmap();
    }
    if (texture_test && !texture_upload && !stacked_texture) {
        const bool one_d = mode == "texture-1d" || vertex_texture;
        constexpr uint32_t tw = 8;
        const uint32_t th = one_d ? 1 : 4;
        texture = device->createTexture(RenderTextureDesc::Texture2D(tw, th, 3, format));
        RenderSamplerDesc sampler_desc;
        sampler_desc.minFilter = sampler_desc.magFilter = RenderFilter::NEAREST;
        sampler_desc.mipmapMode = RenderMipmapMode::NEAREST;
        sampler_desc.addressU = sampler_desc.addressV = RenderTextureAddressMode::CLAMP;
        sampler = device->createSampler(sampler_desc);
        image_set = device->createDescriptorSet(descriptor_descs[0]);
        sampler_set = device->createDescriptorSet(descriptor_descs[3]);
        image_set->setTexture(0, texture.get(), RenderTextureLayout::SHADER_READ);
        sampler_set->setSampler(0, sampler.get());
        // Unique values at every texel and mip reveal coordinate and LOD errors.
        auto texel = [](uint32_t x, uint32_t y, uint32_t mip) -> std::array<float, 4> {
            return {float(x) + .25f, float(y) + .5f, float(mip) + .75f, 1};
        };
        auto upload = device->createBuffer(RenderBufferDesc::UploadBuffer(3 * 512));
        auto* texels = static_cast<uint8_t*>(upload->map());
        for (uint32_t mip = 0; mip < 3; ++mip)
            for (uint32_t y = 0; y < std::max(1u, th >> mip); ++y)
                for (uint32_t x = 0; x < (tw >> mip); ++x) {
                    const auto value = texel(x, y, mip);
                    std::memcpy(texels + mip * 512 + (y * (tw >> mip) + x) * 16, value.data(), 16);
                }
        flush(upload.get()); upload->unmap();
        commands->begin();
        commands->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(texture.get(), RenderTextureLayout::COPY_DEST));
        for (uint32_t mip = 0; mip < 3; ++mip) {
            auto source = RenderTextureCopyLocation::PlacedFootprint(upload.get(), format,
                tw >> mip, std::max(1u, th >> mip), 1, tw >> mip, mip * 512);
            commands->copyTextureRegion(RenderTextureCopyLocation::Subresource(texture.get(), mip), source, 0, 0, 0, nullptr);
        }
        commands->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(texture.get(), RenderTextureLayout::SHADER_READ));
        commands->end();
        queue->executeCommandLists(commands.get(), fence.get());
        queue->waitForCommandFence(fence.get());
        for (uint32_t mip = 0; mip < (mode == "texture-explicit-lod" ? 3u : 1u); ++mip) {
            const uint32_t mw = tw >> mip, mh = std::max(1u, th >> mip);
            for (uint32_t x = 0; x < mw; ++x) {
                const uint32_t y = x % mh;
                const bool denorm = mode == "texture-2d-unnormalized";
                samples.push_back({{(x + .5f) / (denorm ? 1.0f : float(mw)),
                                    (y + .5f) / (denorm ? 1.0f : float(mh)), 0, 0},
                                   texel(x, y, mip), 0, 0, float(mip)});
            }
        }
    }
    if (stacked_texture) {
        // The game samples a tiled 16-layer LUT with 3D fetch instructions.
        // Verify interpolation across slices and clamping of out-of-range W.
        using namespace gow3::renderer;
        constexpr uint32_t swizzle=0|(1<<3)|(2<<6)|(3<<9);
        std::array<uint32_t,6> fetch{2u|(1u<<22)|(1u<<31),0x1000u|1024|6|(2u<<6),
            15|(15<<13)|(15u<<26),(swizzle<<1)|(1<<19)|(1<<21),3,1<<9};
        const auto description=DescribeTexture(fetch);
        std::vector<uint8_t> bytes(65536);
        for(unsigned z=0;z<16;++z)for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
            const auto offset=TextureBlockOffset(description,description.levels[z],x,y,0);
            const std::array<uint8_t,4> v{uint8_t(x*17),uint8_t(y*17),uint8_t(z*17),255};
            for(unsigned k=0;k<4;++k)bytes[(offset+k)^3]=v[k];
        }
        commands->begin();
        uploaded_texture=std::make_unique<NativeTexture>(UploadTexture(*device,*commands,DecodeTexture(fetch,bytes,{})));
        commands->end();queue->executeCommandLists(commands.get(),fence.get());queue->waitForCommandFence(fence.get());
        image_set=device->createDescriptorSet(descriptor_descs[1]);
        sampler_set=device->createDescriptorSet(descriptor_descs[3]);
        image_set->setTexture(0,uploaded_texture->image.get(),RenderTextureLayout::SHADER_READ,uploaded_texture->binding.view.get());
        sampler_set->setSampler(0,uploaded_texture->binding.sampler.get());
        for(float z:{-1.f,0.f,.5f,1.f,3.25f,7.5f,15.f,16.f})
            samples.push_back({{4.75f/16,8.f/16,(z+.5f)/16,0},{4.25f/15,7.5f/15,std::clamp(z,0.f,15.f)/15,1}});
    }
    if(mixed_texture) {
        const bool wide=mode.starts_with("texture-upload-mixed16-");
        const bool packed=mode.starts_with("texture-upload-mixed10-");
        const unsigned width=wide?16:packed?10:8,endian=unsigned(mode.back()-'0');
        if(endian>3)throw std::runtime_error("Invalid mixed-sign test endian");
        constexpr uint32_t swizzle=2|(1<<3)|(0<<6)|(3<<9);
        std::array<uint32_t,6> fetch{0x80400002|(0x11<<2),0x1000|(endian<<6)|(wide?26u:packed?7u:6u),
            1,(swizzle<<1)|(1<<19)|(1<<21)|(2<<23),0,1<<9};
        const auto layout=gow3::renderer::DescribeTexture(fetch);
        std::vector<uint8_t> source(32768);constexpr unsigned swaps[]{0,1,3,2};
        const uint32_t half=1u<<(width-1),maximum=(1u<<width)-1;
        for(unsigned x=0;x<2;++x) {
            const std::array<uint32_t,4> channels{x?half:half-1,x?half+half/2:half/2,
                x?1u:maximum,x?(packed?3u:maximum):0u};
            uint64_t bits=0;
            for(unsigned c=0;c<4;++c)bits|=uint64_t(channels[c])<<(c*width);
            const auto offset=gow3::renderer::TextureBlockOffset(layout,layout.levels[0],x,0,0);
            for(unsigned b=0;b<layout.block.bytes;++b)source[(offset+b)^swaps[endian]]=uint8_t(bits>>(8*b));
        }
        commands->begin();
        uploaded_texture=std::make_unique<gow3::renderer::NativeTexture>(gow3::renderer::UploadTexture(
            *device,*commands,gow3::renderer::DecodeTexture(fetch,source,{})));
        commands->end();queue->executeCommandLists(commands.get(),fence.get());queue->waitForCommandFence(fence.get());
        image_set=device->createDescriptorSet(descriptor_descs[0]);sampler_set=device->createDescriptorSet(descriptor_descs[3]);
        image_set->setTexture(0,uploaded_texture->image.get(),RenderTextureLayout::SHADER_READ,uploaded_texture->binding.view.get());
        sampler_set->setSampler(0,uploaded_texture->binding.sampler.get());
        const float epsilon=1.f/(half-1),low=float(half/2)/maximum,high=float(half+half/2)/maximum;
        samples.push_back({{.25f,.5f,0,0},{-epsilon,low,1,0}});
        samples.push_back({{.75f,.5f,0,0},{epsilon,high,-1,1}});
        samples.push_back({{.5f,.5f,0,0},{0,(low+high)*.5f,0,.5f}});
    }
    if (texture_upload&&!mixed_texture) {
        const bool bc1 = mode == "texture-upload-bc1";
        const bool packed10 = mode.starts_with("texture-upload-packed10-");
        const bool packed_alias = mode == "texture-upload-packed10-alias";
        const bool depth24 = mode == "texture-upload-depth24", depth_float = mode == "texture-upload-depth-float";
        const bool depth_format = depth24 || depth_float;
        const uint32_t endian = depth_format || packed_alias ? 2 : bc1 ? 1 : uint32_t(mode.back() - '0');
        if (endian > 3) throw std::runtime_error("Invalid upload test endian");
        // 8x4, tiled, packed mip tail. Distinct channels and levels expose
        // addressing, endian, view swizzle, gamma and exponent errors.
        const uint32_t swizzle = bc1 ? (0 | (1 << 3) | (2 << 6) | (3 << 9))
                                    : (2 | (1 << 3) | (0 << 6) | (3 << 9));
        std::array<uint32_t, 6> fetch{0x804000FE, 0x1000 | (endian << 6) | (bc1 ? 18u : 6u),
            7 | (3 << 13), (swizzle << 1) | (63 << 13), 2 << 6, 0x2000 | (1 << 9) | (1 << 11)};
        if (depth_format) { fetch[0] = 0x80400002; fetch[1] = 0x1000 | (endian << 6) | (depth_float ? 23u : 22u); fetch[3] = 1; }
        if (packed10) {fetch[0]=0x80400002;fetch[1]=0x1000|(endian<<6)|(packed_alias?54:7);fetch[3]=(swizzle<<1);}
        // Float24 literals include zero, the smallest subnormal, .5, 1 and the largest finite value.
        constexpr uint32_t depth_bits[]{0, 1, 0x80000, 0x100000, 0xd00000, 0xe00000, 0xf00000, 0xffffff};
        constexpr float depth_values[]{0, 0x1p-34f, 0x1p-15f, 0x1p-14f, .25f, .5f, 1, 2 - 0x1p-20f};
        const auto description = gow3::renderer::DescribeTexture(fetch);
        std::vector<uint8_t> base(32768), mips(32768);
        constexpr uint32_t swaps[]{0, 1, 3, 2};
        for (const auto& l : description.levels) {
            auto& bytes = l.mips ? mips : base;
            if (bc1) {
                // BC1 endpoint zero is pure red; index zero selects it.
                const std::array<uint8_t, 8> block{0, 248, 224, 7, 0, 0, 0, 0};
                for (uint32_t y = 0; y < (l.height + 3) / 4; ++y)
                    for (uint32_t x = 0; x < (l.width + 3) / 4; ++x) {
                        const auto offset = gow3::renderer::TextureBlockOffset(description, l, x, y, 0);
                        for (uint32_t b = 0; b < 8; ++b) bytes[(offset + b) ^ swaps[endian]] = block[b];
                    }
            } else {
                for (uint32_t y = 0; y < l.height; ++y) for (uint32_t x = 0; x < l.width; ++x) {
                    const auto offset = gow3::renderer::TextureBlockOffset(description, l, x, y, 0);
                    std::array<uint8_t, 4> pixel{uint8_t(x * 32), uint8_t(y * 64), uint8_t(l.mip * 96), 255};
                    if (depth_format) {
                        const uint32_t packed = depth_bits[(x + l.mip) % 8] << 8 | 0xa5;
                        for (uint32_t b = 0; b < 4; ++b) pixel[b] = uint8_t(packed >> (b * 8));
                    }
                    if (packed10) {
                        const uint32_t packed = (x*127) | ((y*341)<<10) | ((l.mip*511)<<20) | (((x+y)%4)<<30);
                        for(uint32_t b=0;b<4;++b)pixel[b]=uint8_t(packed>>(b*8));
                    }
                    for (uint32_t b = 0; b < 4; ++b) bytes[(offset + b) ^ swaps[endian]] = pixel[b];
                }
            }
        }
        const auto decoded = gow3::renderer::DecodeTexture(fetch, base, mips);
        commands->begin();
        uploaded_texture = std::make_unique<gow3::renderer::NativeTexture>(
            gow3::renderer::UploadTexture(*device, *commands, decoded));
        commands->end(); queue->executeCommandLists(commands.get(), fence.get()); queue->waitForCommandFence(fence.get());
        image_set = device->createDescriptorSet(descriptor_descs[0]);
        sampler_set = device->createDescriptorSet(descriptor_descs[3]);
        image_set->setTexture(0, uploaded_texture->image.get(), RenderTextureLayout::SHADER_READ, uploaded_texture->binding.view.get());
        sampler_set->setSampler(0, uploaded_texture->binding.sampler.get());
        // Exact PWL integer endpoints, independently tabulated from the
        // console's 10-bit output curve; not the implementation under test.
        constexpr float gamma32[]{0, 32, 64, 128, 257, 385, 516, 774};
        constexpr float gamma96[]{0, 128, 516};
        for (uint32_t mip = 0; mip < 3; ++mip) {
            const uint32_t w = 8 >> mip, h = std::max(1u, 4u >> mip);
            for (uint32_t x = 0; x < w; ++x) {
                const uint32_t y = x % h;
                std::array<float, 4> expected = bc1 ? std::array<float, 4>{.5f, 0, 0, .5f}
                    : std::array<float, 4>{gamma96[mip] / 2046, gamma32[y * 2] / 2046, gamma32[x] / 2046, .5f};
                if (depth_format) expected.fill(depth_float ? depth_values[(x + mip) % 8] : float(double(depth_bits[(x + mip) % 8]) / 0xffffff));
                if (packed10) expected={mip*511.f/1023,y*341.f/1023,x*127.f/1023,float((x+y)%4)/3};
                samples.push_back({{(x + .5f) / w, (y + .5f) / h, 0, 0}, expected, 0, 0, float(mip)});
            }
        }
    }
    size_t checked = 0;
    for (size_t sample = 0; sample < samples.size(); ++sample) {
        auto* mapped = static_cast<uint8_t*>(constants->map());
        if (!mapped) throw std::runtime_error("Constant upload mapping failed");
        std::memset(mapped, 0, constant_bytes);
        gow3::renderer::SharedShaderConstants shared;
        shared.alpha_threshold = samples[sample].alpha_reference;
        shared.color_scale[0] = samples[sample].color_scale;
        shared.loops[0] = samples[sample].outer;
        shared.loops[1] = samples[sample].inner;
        if (uploaded_texture) shared.texture_properties[0] = uploaded_texture->binding.properties;
        if (vertex_pulling) {
            shared.vertex_streams[95] = {vertices->getDeviceAddress(), 48, 0};
            std::memcpy(vertex_data->map(), samples[sample].vertex_bytes.data(), 32);
            flush(vertex_data.get()); vertex_data->unmap();
            shared.vertex_streams[94] = {vertex_data->getDeviceAddress(), samples[sample].vertex_size, samples[sample].vertex_endian};
        }
        std::memcpy(mapped + 8192, &shared, sizeof(shared));
        for (uint32_t offset : {0u, 4096u}) {
            std::memcpy(mapped + offset + 7 * 16, samples[sample].input.data(), 16);
            std::memcpy(mapped + offset + 8 * 16, &samples[sample].lod, 4);
            if(vector_kill) {
                std::memcpy(mapped + offset + 8 * 16, samples[sample].kill_reference.data(), 16);
                std::memcpy(mapped + offset + 9 * 16, kill_survivor.data(), 16);
            }
        }
        if (mode == "loops") {
            for (uint32_t i = 0; i < 16; ++i) {
                const std::array<float, 4> value{(i+1)*.25f, (i+1)*.5f, float(i+1), (i+1)*2.0f};
                std::memcpy(mapped + 4096 + (7+i)*16, value.data(), 16);
            }
        }
        auto* upload_vk = static_cast<VulkanBuffer*>(constants.get());
        if (vmaFlushAllocation(upload_vk->device->allocator, upload_vk->allocation, 0, VK_WHOLE_SIZE) != VK_SUCCESS)
            throw std::runtime_error("Constant flush failed");
        constants->unmap();
        const bool alpha_test = samples[sample].alpha;
        RenderSpecConstant specialization(0, alpha_test ? 2u|((samples[sample].alpha_function^6u)<<8) : 0u);
        gow3::renderer::DrawState draw_state;
        if (raster_test) draw_state = gow3::renderer::DecodeDrawState(samples[sample].raster_words, width, height, 4);
        RenderGraphicsPipelineDesc pipeline_desc = draw_state.pipeline;
        pipeline_desc.pipelineLayout = layout.get();
        pipeline_desc.vertexShader = vs.get();
        pipeline_desc.pixelShader = ps.get();
        pipeline_desc.renderTargetCount = 1;
        pipeline_desc.renderTargetFormat[0] = format;
        if (raster_test) pipeline_desc.depthTargetFormat = RenderFormat::D32_FLOAT_S8_UINT;
        else pipeline_desc.renderTargetBlend[0] = RenderBlendDesc::Copy();
        pipeline_desc.specConstants = &specialization;
        pipeline_desc.specConstantsCount = 1;
        auto pipeline = device->createGraphicsPipeline(pipeline_desc);
        if (!pipeline) throw std::runtime_error("Graphics pipeline creation failed");
        commands->begin();
        commands->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(target.get(), RenderTextureLayout::COLOR_WRITE));
        if (depth) commands->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(depth.get(), RenderTextureLayout::DEPTH_WRITE));
        commands->setFramebuffer(framebuffer.get());
        commands->setViewports(raster_test ? draw_state.viewport : RenderViewport(0, 0, width, height));
        commands->setScissors(raster_test && !draw_state.discard ? draw_state.scissor : RenderRect(0, 0, width, height));
        commands->clearColor(0, RenderColor(clear[0], clear[1], clear[2], clear[3]));
        if (depth) commands->clearDepthStencil(true, true, samples[sample].depth_clear, samples[sample].stencil_clear);
        commands->setGraphicsPipelineLayout(layout.get());
        commands->setGraphicsPushConstants(0, addresses.data());
        if (texture_test) {
            commands->setGraphicsDescriptorSet(image_set.get(), stacked_texture ? 1 : 0);
            commands->setGraphicsDescriptorSet(sampler_set.get(), 3);
        }
        commands->setPipeline(pipeline.get());
        if(draw_state.pipeline.dynamicDepthBiasEnabled)commands->setDepthBias(draw_state.depth_bias,0,draw_state.slope_depth_bias);
        if (!draw_state.discard) commands->drawInstanced(3, 1, 0, 0);
        commands->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(target.get(), RenderTextureLayout::COPY_SOURCE));
        // Plume's Vulkan copyTextureRegion supports uploads and image copies,
        // but not image-to-buffer readback. Use Vulkan explicitly for validation.
        auto* cmd_vk = static_cast<VulkanCommandList*>(commands.get());
        auto* target_vk = static_cast<VulkanTexture*>(target.get());
        auto* readback_vk = static_cast<VulkanBuffer*>(readback.get());
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {width, height, 1};
        vkCmdCopyImageToBuffer(cmd_vk->vk, target_vk->vk, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               readback_vk->vk, 1, &copy);
        VkMemoryBarrier host_barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        host_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        host_barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cmd_vk->vk, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                             0, 1, &host_barrier, 0, nullptr, 0, nullptr);
        commands->end();
        queue->executeCommandLists(commands.get(), fence.get());
        queue->waitForCommandFence(fence.get());
        const auto* pixels = static_cast<const float*>(readback->map());
        if (!pixels) throw std::runtime_error("Readback mapping failed");
        if (vmaInvalidateAllocation(readback_vk->device->allocator, readback_vk->allocation, 0, VK_WHOLE_SIZE) != VK_SUCCESS)
            throw std::runtime_error("Readback invalidate failed");
        const auto expected = samples[sample].expected;
        bool match = true;
        for (uint32_t p = 0; p < width * height; ++p) {
            const bool outside = samples[sample].scissor_check &&
                (p % width < 2 || p % width >= 10 || p / width < 3 || p / width >= 11);
            for (uint32_t c = 0; c < 4; ++c) {
                const float value = pixels[p * 4 + c];
                const float wanted = outside ? clear[c] : expected[c];
                const bool differs=std::isnan(wanted)?!std::isnan(value):
                    std::isinf(wanted)?value!=wanted:!std::isfinite(value)||std::abs(value-wanted)>0.00001f;
                if (differs) {
                    if (match) std::cerr << mode << " sample " << sample << " pixel " << p << " component " << c
                                         << ": got " << value << ", expected " << wanted << '\n';
                    match = false;
                }
                ++checked;
            }
        }
        readback->unmap();
        if (!match) throw std::runtime_error("Gears shader output mismatch in sample " + std::to_string(sample));
    }
    std::cout << "PASS: " << mode << ", " << samples.size() << " samples, " << checked
              << " component comparisons; native plume Vulkan, no Xenia GPU command processor.\n";
    return 0;
} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
