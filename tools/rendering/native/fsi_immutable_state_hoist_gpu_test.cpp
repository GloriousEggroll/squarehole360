#include "renderer/native/fsi_device.h"
#include "renderer/native/fsi_precision.h"
#include "renderer/native/fsi_immutable_state_hoist.h"
#include "renderer/native/fsi_state.h"
#include "renderer/native/query_tracker.h"
#include "plume_vulkan.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

using namespace plume;
using namespace gow3::renderer;
namespace {
using Words=std::vector<uint32_t>;
constexpr size_t kBytes=10u*1024*1024;
void Check(bool condition,const char* message) {if(!condition)throw std::runtime_error(message);}
Words ReadWords(const std::filesystem::path& path) {
    std::ifstream stream(path,std::ios::binary|std::ios::ate);
    Check(bool(stream)&&stream.tellg()>=20&&stream.tellg()%4==0,"Cannot read fixture SPIR-V");
    Words words(size_t(stream.tellg())/4);stream.seekg(0);
    Check(bool(stream.read(reinterpret_cast<char*>(words.data()),words.size()*4)),"Truncated fixture SPIR-V");
    return words;
}
void Upload(RenderBuffer& buffer,const void* data,size_t bytes) {
    auto* destination=buffer.map();Check(destination,"Cannot map fixture upload");
    std::memcpy(destination,data,bytes);
    auto& vulkan=static_cast<VulkanBuffer&>(buffer);
    Check(vmaFlushAllocation(vulkan.device->allocator,vulkan.allocation,0,VK_WHOLE_SIZE)==VK_SUCCESS,
        "Cannot flush fixture upload");
    buffer.unmap();
}
Words Seed() {
    Words words(kBytes/4);uint32_t random=0xfa71c9a3;
    for(auto& word:words) {random^=random<<13;random^=random>>17;random^=random<<5;word=random;}
    return words;
}
struct Draw {
    uint32_t width=80,height=16;
    NativeFsiConstants raster;
    std::vector<FsiFragment> fragments;
    bool inset=false;
    uint32_t export_mask=15;
    bool alpha_test=false;
    int32_t probe_x=-1;
};
Draw Basic() {
    Draw draw;draw.raster.pitch=80;draw.raster.depth_base=1024;
    draw.raster.depth_format=1;draw.raster.stencil_front=draw.raster.stencil_back=0x00ffff57;
    draw.raster.color[0]={31,3,15,0x10001};
    draw.fragments={{{.25f,.5f,31.875f,.5f},.75f}};
    return draw;
}
struct Outcome {Words words;uint64_t samples=0;};
struct HoistFragmentInput {FsiFragment fragment;uint32_t export_mask=0,alpha_test=0,alpha_reference=0,padding=0;};
static_assert(sizeof(HoistFragmentInput)==48);
class Fixture {
    RenderDevice& device_;
    RenderCommandQueue& queue_;
    std::unique_ptr<RenderBuffer> edram_;
    std::unique_ptr<RenderShader> vertex_,baseline_,candidate_;
    std::unique_ptr<RenderPipelineLayout> layout_;
    std::unique_ptr<RenderPipeline> baseline_pipeline_,candidate_pipeline_;
    std::unique_ptr<RenderDescriptorSet> edram_set_;
    NativeQueryTracker query_;
    const std::array<RenderDescriptorRange,2> ranges_{{
        {RenderDescriptorRangeType::READ_WRITE_BYTE_ADDRESS_BUFFER,0,1},
        {RenderDescriptorRangeType::READ_WRITE_BYTE_ADDRESS_BUFFER,1,1}}};
    Words Read() {
        auto readback=device_.createBuffer(RenderBufferDesc::ReadbackBuffer(kBytes));
        auto commands=queue_.createCommandList();auto fence=device_.createCommandFence();commands->begin();
        commands->barriers(RenderBarrierStage::COPY,RenderBufferBarrier(edram_.get(),RenderBufferAccess::READ));
        commands->copyBuffer(readback.get(),edram_.get());commands->end();
        queue_.executeCommandLists(commands.get(),fence.get());queue_.waitForCommandFence(fence.get());
        const auto* bytes=readback->map();Check(bytes,"Cannot map packed EDRAM readback");
        auto& vulkan=static_cast<VulkanBuffer&>(*readback);
        Check(vmaInvalidateAllocation(vulkan.device->allocator,vulkan.allocation,0,VK_WHOLE_SIZE)==VK_SUCCESS,
            "Cannot invalidate packed EDRAM readback");
        Words result(kBytes/4);std::memcpy(result.data(),bytes,kBytes);readback->unmap();return result;
    }
public:
    Fixture(RenderDevice& device,RenderCommandQueue& queue,std::span<const uint32_t> vertex,
        std::span<const uint32_t> baseline,std::span<const uint32_t> candidate)
        :device_(device),queue_(queue),query_(device) {
        edram_=device.createBuffer(RenderBufferDesc::DefaultBuffer(kBytes,
            RenderBufferFlag::STORAGE|RenderBufferFlag::UNORDERED_ACCESS));
        vertex_=device.createShader(vertex.data(),vertex.size_bytes(),"main",RenderShaderFormat::SPIRV);
        baseline_=device.createShader(baseline.data(),baseline.size_bytes(),"main",RenderShaderFormat::SPIRV);
        candidate_=device.createShader(candidate.data(),candidate.size_bytes(),"main",RenderShaderFormat::SPIRV);
        Check(vertex_&&baseline_&&candidate_,"Fixture shader module creation failed");
        const RenderDescriptorSetDesc input_set(ranges_.data(),ranges_.size());
        const std::array<RenderDescriptorSetDesc,5> sets{input_set,{}, {}, {},{ranges_.data(),1}};
        const RenderPushConstantRange push(0,0,0,sizeof(ShaderPushConstants),RenderShaderStageFlag::PIXEL);
        layout_=device.createPipelineLayout(RenderPipelineLayoutDesc(&push,1,sets.data(),sets.size()));
        edram_set_=device.createDescriptorSet(sets[4]);edram_set_->setBuffer(0,edram_.get());
        RenderGraphicsPipelineDesc pipeline;pipeline.pipelineLayout=layout_.get();pipeline.vertexShader=vertex_.get();
        pipeline.renderTargetCount=1;pipeline.renderTargetFormat[0]=RenderFormat::R8G8B8A8_UNORM;
        pipeline.renderTargetBlend[0]=RenderBlendDesc::Copy();pipeline.renderTargetBlend[0].renderTargetWriteMask=0;
        pipeline.cullMode=RenderCullMode::NONE;pipeline.depthClipEnabled=true;
        pipeline.pixelShader=baseline_.get();baseline_pipeline_=device.createGraphicsPipeline(pipeline);
        pipeline.pixelShader=candidate_.get();candidate_pipeline_=device.createGraphicsPipeline(pipeline);
        Check(baseline_pipeline_&&candidate_pipeline_&&static_cast<VulkanGraphicsPipeline&>(*baseline_pipeline_).vk&&
            static_cast<VulkanGraphicsPipeline&>(*candidate_pipeline_).vk,"Paired fixture pipeline creation failed");
    }
    void Initialize(const Words& seed) {
        auto upload=device_.createBuffer(RenderBufferDesc::UploadBuffer(kBytes));Upload(*upload,seed.data(),kBytes);
        auto commands=queue_.createCommandList();auto fence=device_.createCommandFence();commands->begin();
        commands->barriers(RenderBarrierStage::COPY,RenderBufferBarrier(edram_.get(),RenderBufferAccess::WRITE));
        commands->copyBuffer(edram_.get(),upload.get());commands->end();
        queue_.executeCommandLists(commands.get(),fence.get());queue_.waitForCommandFence(fence.get());
    }
    Outcome Run(const Draw& draw,bool enabled) {
        Check(!draw.fragments.empty()&&draw.fragments.size()<=4096,"Invalid fixture fragment count");
        NativeFsiShaderConstants constants;constants.raster=draw.raster;

        auto state=device_.createBuffer(RenderBufferDesc::UploadBuffer(sizeof(constants),
            RenderBufferFlag::CONSTANT|RenderBufferFlag::DEVICE_ADDRESSABLE));Upload(*state,&constants,sizeof(constants));
        std::vector<HoistFragmentInput> owned;
        for(const auto& fragment:draw.fragments)owned.push_back({fragment,draw.export_mask,uint32_t(draw.alpha_test),std::bit_cast<uint32_t>(.5f),0});
        auto inputs=device_.createBuffer(RenderBufferDesc::UploadBuffer(owned.size()*sizeof(HoistFragmentInput),RenderBufferFlag::STORAGE));
        Upload(*inputs,owned.data(),owned.size()*sizeof(HoistFragmentInput));
        auto descriptors=device_.createDescriptorSet(RenderDescriptorSetDesc(ranges_.data(),ranges_.size()));
        descriptors->setBuffer(0,edram_.get());descriptors->setBuffer(1,inputs.get());
        auto target=device_.createTexture(RenderTextureDesc::ColorTarget(draw.width,draw.height,RenderFormat::R8G8B8A8_UNORM));
        const RenderTexture* attachment=target.get();RenderFramebufferDesc description;
        description.colorAttachments=&attachment;description.colorAttachmentsCount=1;
        auto framebuffer=device_.createFramebuffer(description);
        auto commands=queue_.createCommandList();auto fence=device_.createCommandFence();commands->begin();
        commands->barriers(RenderBarrierStage::GRAPHICS,RenderBufferBarrier(edram_.get(),RenderBufferAccess::READ|RenderBufferAccess::WRITE));
        commands->barriers(RenderBarrierStage::GRAPHICS,RenderBufferBarrier(inputs.get(),RenderBufferAccess::READ));
        commands->barriers(RenderBarrierStage::GRAPHICS,RenderTextureBarrier(target.get(),RenderTextureLayout::COLOR_WRITE));
        commands->setFramebuffer(framebuffer.get());commands->setViewports(RenderViewport(0,0,draw.width,draw.height));
        commands->setScissors(draw.probe_x>=0?RenderRect(draw.probe_x,0,draw.probe_x+1,draw.height):
            draw.inset?RenderRect(3,2,draw.width-4,draw.height-3):RenderRect(0,0,draw.width,draw.height));
        commands->setGraphicsPipelineLayout(layout_.get());
        const ShaderPushConstants addresses{0,0,state->getDeviceAddress()};commands->setGraphicsPushConstants(0,&addresses);
        commands->setGraphicsDescriptorSet(descriptors.get(),0);commands->setGraphicsDescriptorSet(edram_set_.get(),4);
        commands->setPipeline(enabled?candidate_pipeline_.get():baseline_pipeline_.get());
        query_.Begin(1,32);query_.ResetDraw(*commands);query_.BeginDraw(*commands);
        commands->drawInstanced(3,draw.fragments.size(),0,0);query_.EndDraw(*commands);commands->end();
        queue_.executeCommandLists(commands.get(),fence.get());queue_.waitForCommandFence(fence.get());
        query_.CompleteDraw();const auto samples=query_.End(1,0).samples;
        return {Read(),samples};
    }
};
uint32_t Pack7e3(float value) {
    if(std::isnan(value)||!(value>0))return 0;
    value=std::min(value,31.875f);
    if(value<.25f) {
        const float scaled=value*512;const uint32_t lower=uint32_t(scaled);
        const float fraction=scaled-float(lower);
        return lower+(fraction>.5f||(fraction==.5f&&(lower&1u)));
    }
    const auto bits=std::bit_cast<uint32_t>(value)-0x3e000000u;
    return (bits+32767u+((bits>>16)&1u))>>16;
}
uint32_t PackFormat3(const std::array<float,4>& color) {
    const float alpha=std::isnan(color[3])?0:std::clamp(color[3],0.f,1.f);
    // Materialize binary32 multiplication before the half-up addition. The
    // production helper's precise intermediate excludes fused multiply-add.
    const volatile float scaled_alpha=alpha*3.f;
    const uint32_t a=uint32_t(scaled_alpha+.5f);
    return Pack7e3(color[0])|(Pack7e3(color[1])<<10)|(Pack7e3(color[2])<<20)|(a<<30);
}
uint32_t Format3Word(const Draw& draw,uint32_t x,uint32_t y) {
    // Independent 32-bit color layout oracle; the GPU still uses the original
    // helper's addressing. The whole-buffer comparison also detects corruption
    // outside this footprint.
    const uint32_t tile=(y/16)*((draw.raster.pitch+79)/80)+x/80;
    return ((draw.raster.color[0].base+tile)*1280+(y%16)*80+x%80)%(2048*1280);
}
}
int main(int argc,char** argv) try {
    Check(argc==4,"Usage: gpu-test ordinal-vertex.spv baseline-fragment.spv candidate-fragment.spv");
    auto vertex=ReadWords(argv[1]),baseline=ReadWords(argv[2]),candidate=ReadWords(argv[3]);
    Check(baseline!=candidate,"Paired fixture modules are identical");
    FsiDevice host;auto& device=host.device();auto queue=device.createCommandQueue(RenderCommandListType::DIRECT);
    Fixture fixture(device,*queue,vertex,baseline,candidate);const auto seed=Seed();
    size_t cases=0,word_checks=0,oracle_checks=0,alpha_draws=0,zero_export_draws=0,depth_guard_draws=0;
    auto verify=[&](const std::string& name,const std::vector<Draw>& draws,bool independent_oracle=false) {
        std::vector<Outcome> reference;fixture.Initialize(seed);
        for(const auto& draw:draws)reference.push_back(fixture.Run(draw,false));
        fixture.Initialize(seed);
        for(size_t i=0;i<draws.size();++i) {
            const auto& draw=draws[i];const auto actual=fixture.Run(draw,true);
            if(reference[i].words!=actual.words||reference[i].samples!=actual.samples)
                throw std::runtime_error("Packed EDRAM/query mismatch: "+name+" draw "+std::to_string(i));
            word_checks+=actual.words.size();
            const uint64_t pixels=uint64_t(draw.probe_x>=0?1u:draw.inset?draw.width-7:draw.width)*
                (draw.inset?draw.height-5:draw.height);
            const auto passing_alpha=std::count_if(draw.fragments.begin(),draw.fragments.end(),[&](const FsiFragment& f){return !draw.alpha_test||!(f.color[3]<.5f);});
            if(draw.raster.depth_control==0)Check(actual.samples==pixels*passing_alpha,"Unconditional alpha/occlusion count changed");
            alpha_draws+=draw.alpha_test;zero_export_draws+=draw.export_mask==0;depth_guard_draws+=(draw.raster.depth_control&3u)!=0;
            if(draw.raster.depth_control==2)Check(actual.samples==0,"Failed depth contributed occlusion samples");
            if(independent_oracle) {
                const auto& color=draw.fragments.back().color;
                const uint32_t expected=PackFormat3(color);
                for(uint32_t y=0;y<draw.height;++y)for(uint32_t x=0;x<draw.width;++x) {
                    Check(actual.words[Format3Word(draw,x,y)]==expected,"Independent format3 packing oracle mismatch");
                    ++oracle_checks;
                }
            }
        }
        ++cases;
    };
    // A different arbitrary packed destination for every addressed pixel.
    // Probe 7e3 tie-to-even and normalized alpha half-up boundaries, signed
    // zero, subnormals, infinities and multiple NaN payloads independently.
    std::vector<float> edges{-std::numeric_limits<float>::infinity(),-32,-.25f,-0.f,0.f,
        std::bit_cast<float>(1u),1.f/1024,3.f/1024,.25f,.5f,1,2,31.875f,32,
        std::numeric_limits<float>::infinity(),std::bit_cast<float>(0x7fc00001u),
        std::bit_cast<float>(0xffc00123u),1.f/6,.5f,5.f/6};
    for(unsigned value=1;value<8;++value)edges.push_back(std::bit_cast<float>(0x3e000000u+(value<<16)+0x8000u));
    const auto originals=edges;
    for(float value:originals)if(std::isfinite(value)) {
        edges.push_back(std::nextafter(value,-std::numeric_limits<float>::infinity()));
        edges.push_back(std::nextafter(value,std::numeric_limits<float>::infinity()));
    }
    for(size_t i=0;i<edges.size();++i) {
        auto draw=Basic();draw.fragments[0].color={edges[i],edges[(i+7)%edges.size()],edges[(i+13)%edges.size()],edges[(i+19)%edges.size()]};
        verify("quantization "+std::to_string(i),{draw},true);
    }
    // Every possible guest color mask, with blend disabled and nontrivial enabled
    // blending. Partial masks retain arbitrary noncanonical old word bits.
    for(uint32_t mask=0;mask<16;++mask)for(uint32_t blend=0;blend<2;++blend) {
        auto draw=Basic();draw.raster.color[0].mask=mask;draw.raster.blend_enabled=blend;
        draw.raster.color[0].blend=0x07060706;
        draw.fragments={{{1,.25f,.5f,.25f},.5f},{{0,2,.5f,.75f},.75f}};
        verify("mask/blend",{draw});
    }
    // All validated formats remain byte-identical, including
    // wide formats and float/half noncanonical masked destination words.
    for(uint32_t format:{0u,2u,4u,5u,6u,7u,14u,15u})for(uint32_t mask:{1u,3u,15u}) {
        auto draw=Basic();draw.raster.color[0].format=format;draw.raster.color[0].mask=mask;
        draw.fragments={{{-2,2,.5f,.25f},.25f},{{.5f,-.5f,1,.75f},.75f}};
        verify("format",{draw});
    }
    // Many overlapping primitives within a draw, then generic partial-color
    // accumulation, then complete overwrite. Query counts and the whole EDRAM
    // are checked after every transition, so a later draw cannot hide damage.
    auto crowded=Basic();crowded.fragments.clear();
    for(unsigned i=0;i<128;++i)crowded.fragments.push_back({{float(i%17)/8,float(i%13)/4,float(i%7)/2,float(i%4)/3},float(i%8)/8});
    auto blend_successor=Basic();blend_successor.raster.color[0].mask=7;
    blend_successor.raster.blend_enabled=1;blend_successor.raster.color[0].blend=0x01010101;
    verify("ordered overlapping and blended draws",{crowded,blend_successor,crowded});
    for(uint32_t control:{0x700766u,2u,1u|(7u<<8)|(2u<<14),
        1u|2u|(1u<<4)|(7u<<8)|(5u<<17)}) {
        auto draw=crowded;draw.raster.depth_control=control;
        draw.raster.front_constant_bias=.125f;draw.raster.back_constant_bias=-.125f;
        verify("depth stencil query",{draw,blend_successor});
    }
    // Read-only compare with arbitrary packed old FP24/D24+stencil
    // words, every compare mode, incoming special values, polygon bias and
    // repeated overlapping fragments. Both variants receive the same complete owned constants. Whole10MiB/query parity is
    // checked after each draw, so a later overwrite cannot hide a bad store.
    for(uint32_t format:{0u,1u})for(uint32_t mode=0;mode<8;++mode) {
        auto read_only=Basic();read_only.raster.depth_format=format;read_only.raster.depth_control=2u|(mode<<4);
        read_only.raster.color[0].mask=7;read_only.raster.blend_enabled=1;read_only.raster.color[0].blend=0x01010101;
        read_only.fragments.clear();
        for(float value:{-0.f,0.f,std::bit_cast<float>(1u),.25f,.5f,1.f,
            std::numeric_limits<float>::infinity(),std::bit_cast<float>(0x7fc00123u)})
            read_only.fragments.push_back({{.125f,.25f,.5f,.75f},value});
        read_only.raster.front_constant_bias=.125f;read_only.raster.back_constant_bias=-.125f;
        verify("readonly all compare/format",{read_only,read_only});
    }
    // The original source implements modular addressing for wrapping EDRAM.
    // Pure state/incoming hoists do not change any packed addressing. One-column
    // probes retain deterministic original alias behavior in this fixture.
    auto readonly_wrap=Basic();readonly_wrap.width=160;readonly_wrap.height=32;readonly_wrap.raster.pitch=240;
    readonly_wrap.raster.depth_control=0x700762;readonly_wrap.raster.depth_base=2047;readonly_wrap.probe_x=0;
    verify("readonly noninjective wrap",{readonly_wrap});
    auto wrap=Basic();wrap.width=160;wrap.height=32;wrap.raster.pitch=240;
    wrap.raster.color[0].base=2047;wrap.raster.depth_base=1001;wrap.inset=true;
    verify("wrapped pitched inset",{wrap});
    // Four exports alias the same packed word. Target1 reads destination for
    // blending, target2 fully overwrites, target3 partially overwrites. Each
    // draw must retain target0->1->2->3 order inside the same interlock.
    auto alias=Basic();
    alias.raster.color[1]={31,3,15,0x01010101};alias.raster.blend_enabled=2;
    alias.raster.color[2]={31,3,15,0x10001};alias.raster.color[3]={31,3,3,0x10001};
    alias.fragments={{{.125f,.25f,.5f,.75f},.5f},{{.25f,.5f,.75f,1},.75f}};
    verify("mixed MRT packed word alias",{alias,blend_successor,alias});
    auto wide_alias=alias;wide_alias.width=1;wide_alias.height=1;
    wide_alias.raster.color[1]={31,7,15,0x10001};wide_alias.raster.blend_enabled=0;
    wide_alias.raster.color[3]={31,0,15,0x10001};
    verify("mixed wide MRT alias",{wide_alias});
    // The depth tile swaps its two 40-pixel halves. Pixel0's color word is
    // pixel40's depth word, and vice versa. Draw only one column at a time,
    // with the original inter-draw barrier, to exercise actual depth/color
    // alias transitions without unsupported cross-pixel simultaneous races.
    auto depth_alias=alias;depth_alias.height=1;depth_alias.probe_x=0;
    depth_alias.raster.depth_base=31;depth_alias.raster.depth_control=0x700766;
    auto opposite=depth_alias;opposite.probe_x=40;
    verify("ordered depth color word aliases",{depth_alias,opposite,depth_alias});
    auto readonly_alias=depth_alias;readonly_alias.raster.depth_control=0x700762;
    auto readonly_opposite=readonly_alias;readonly_opposite.probe_x=40;
    verify("readonly depth/color alias",{readonly_alias,readonly_opposite,readonly_alias});
    // Prove the same killed material invocations never reach the epilogue,
    // including zero exports and failed depth. Every export mask is real input.
    for(uint32_t exports=0;exports<16;++exports)for(bool alpha:{false,true}) {
        auto draw=Basic();draw.export_mask=exports;draw.alpha_test=alpha;
        draw.fragments={{{.125f,.25f,.5f,.25f},.25f},{{.5f,.25f,.125f,.75f},.75f}};
        verify("material alpha and export masks",{draw});
    }
    auto killed=Basic();killed.alpha_test=true;killed.fragments={{{1,2,3,.25f},.75f}};
    verify("all alpha killed",{killed});
    // Keep exact original biased/clamped binary32 inputs and both depth formats.
    // Include the FP24 subnormal threshold and neighbours, signed zero, NaN,
    // infinity, front/back bias and independently selected shader-depth path.
    for(uint32_t format:{0u,1u})for(uint32_t control:{0u,1u|(7u<<8),2u|(6u<<4),6u|(6u<<4),7u|(6u<<4)|(7u<<8)})for(bool face:{false,true}) {
        auto draw=Basic();draw.raster.depth_format=format;draw.raster.depth_control=control;
        draw.raster.front_slope_bias=.125f;draw.raster.front_constant_bias=.0625f;
        draw.raster.back_slope_bias=-.125f;draw.raster.back_constant_bias=-.0625f;
        draw.raster.stencil_front=0x0055aa31;draw.raster.stencil_back=0x00aa5573;
        draw.fragments.clear();
        for(uint32_t bits:{0x80000000u,0u,1u,0x387fffffu,0x38800000u,0x38800001u,0x3e800000u,0x3f000000u,0x3f800000u,0x7f800000u,0x7fc00123u,0xff800000u}) {
            FsiFragment f{{.125f,.25f,.5f,.75f},std::bit_cast<float>(bits)};
            f.padding={float(bits&1u),face?0.f:1.f,(bits&1u)?.001f:0.f};draw.fragments.push_back(f);
        }
        verify("exact incoming depth guard domain",{draw});
    }
    Check(alpha_draws>0&&zero_export_draws>0&&depth_guard_draws>0&&oracle_checks>0,
        "Hoist fixture did not exercise alpha, exports, incoming guard and independent packing");
    std::cout<<"PASS: "<<cases<<" paired cases, "<<word_checks<<" whole-EDRAM word comparisons, "
        <<oracle_checks<<" independent packing comparisons, "<<alpha_draws<<" alpha inputs, "<<zero_export_draws
        <<" zero-export inputs, "<<depth_guard_draws<<" enabled depth/stencil inputs; exact query and ordered aliases\n";
} catch(const std::exception& exception) {std::cerr<<exception.what()<<'\n';return 1;}
