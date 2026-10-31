#include "temporal_inputs.h"
#include "plume_vulkan.h"
#include <rex/logging.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace gow3::renderer {
using namespace plume;
namespace {
constexpr size_t kVertexConstants=0x780;
constexpr size_t kPixelConstants=0x1780;
constexpr uint32_t kFlagLateDepth=1,kFlagHud=2,kFlagWorldDepth=4,kFlagVelocity=8,kFlagVelocitySwapped=16,
    kFlagPreviousDepth=32;
// Frames handed to the presenter are reused only after this many newer frames,
// in addition to the presenter having released them.
constexpr uint64_t kFrameReuseDistance=8;

float LoadFloat(std::span<const uint8_t> state,size_t offset) {
    uint32_t word;std::memcpy(&word,state.data()+offset,4);
    return std::bit_cast<float>(std::byteswap(word));
}
void StoreFloat(std::span<uint8_t> state,size_t offset,float value) {
    const uint32_t word=std::byteswap(std::bit_cast<uint32_t>(value));
    std::memcpy(state.data()+offset,&word,4);
}
float Constant(std::span<const uint8_t> state,unsigned reg,unsigned component) {
    return LoadFloat(state,kVertexConstants+reg*16+component*4);
}
uint32_t Register(std::span<const uint8_t> state,size_t offset) {
    uint32_t word;std::memcpy(&word,state.data()+offset,4);
    return std::byteswap(word);
}
float Halton(uint32_t index,uint32_t base) {
    float result=0,fraction=1;
    while(index) {fraction/=float(base);result+=fraction*float(index%base);index/=base;}
    return result;
}
using Matrix=std::array<std::array<double,4>,4>;
Matrix Multiply(const Matrix& a,const Matrix& b) {
    Matrix r{};
    for(int i=0;i<4;++i)for(int j=0;j<4;++j)for(int k=0;k<4;++k)r[i][j]+=a[i][k]*b[k][j];
    return r;
}
bool Invert(const Matrix& m,Matrix& out) {
    // Gauss-Jordan with partial pivoting.
    std::array<std::array<double,8>,4> a{};
    for(int i=0;i<4;++i)for(int j=0;j<4;++j) {a[i][j]=m[i][j];a[i][j+4]=i==j;}
    for(int c=0;c<4;++c) {
        int pivot=c;
        for(int r=c+1;r<4;++r)if(std::abs(a[r][c])>std::abs(a[pivot][c]))pivot=r;
        if(std::abs(a[pivot][c])<1e-12)return false;
        std::swap(a[c],a[pivot]);
        const double scale=1/a[c][c];
        for(auto& v:a[c])v*=scale;
        for(int r=0;r<4;++r)if(r!=c) {
            const double f=a[r][c];
            if(f!=0)for(int k=0;k<8;++k)a[r][k]-=f*a[c][k];
        }
    }
    for(int i=0;i<4;++i)for(int j=0;j<4;++j)out[i][j]=a[i][j+4];
    return true;
}
std::array<double,3> Column(const NativeTemporalCamera& camera,int column) {
    return {camera.view_projection[0][column],camera.view_projection[1][column],camera.view_projection[2][column]};
}
double Dot(const std::array<double,3>& a,const std::array<double,3>& b) {return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
const std::array<RenderDescriptorRange,9> kRanges{{
    {RenderDescriptorRangeType::TEXTURE,0,1},
    {RenderDescriptorRangeType::READ_WRITE_BYTE_ADDRESS_BUFFER,1,1},
    {RenderDescriptorRangeType::TEXTURE,2,1},
    {RenderDescriptorRangeType::TEXTURE,3,1},
    {RenderDescriptorRangeType::READ_WRITE_TEXTURE,4,1},
    {RenderDescriptorRangeType::READ_WRITE_TEXTURE,5,1},
    {RenderDescriptorRangeType::READ_WRITE_TEXTURE,6,1},
    {RenderDescriptorRangeType::TEXTURE,7,1},
    {RenderDescriptorRangeType::TEXTURE,8,1}}};
struct PushConstants {
    std::array<std::array<float,4>,4> reprojection;
    uint32_t width,height,flags;
    float min_depth;
    std::array<float,2> velocity_scale,jitter;
    float depth_scale,reactive_floor,padding[2];
};
static_assert(sizeof(PushConstants)==112);
VkFormat Format(const RenderTexture& texture) {return static_cast<const VulkanTexture&>(texture).imageFormat;}
rex::ui::vulkan::TemporalUpscaleImage Describe(RenderTexture& texture) {
    auto& vk=static_cast<VulkanTexture&>(texture);
    return {vk.vk,vk.imageView,vk.imageFormat};
}
// Restores a borrowed executor image to the layout the executor left it in.
struct BorrowedLayout {
    RenderTexture* texture;RenderTextureLayout layout;
    explicit BorrowedLayout(RenderTexture* t):texture(t),
        layout(t?static_cast<VulkanTexture&>(*t).textureLayout:RenderTextureLayout::UNKNOWN) {}
    void Restore(RenderCommandList& commands) const {
        if(!texture||layout==RenderTextureLayout::UNKNOWN)return;
        if(static_cast<VulkanTexture&>(*texture).textureLayout==layout)return;
        commands.barriers(RenderBarrierStage::GRAPHICS|RenderBarrierStage::COMPUTE,RenderTextureBarrier(texture,layout));
    }
};
}

struct NativeTemporalInputs::Frame {
    uint32_t width=0,height=0;
    uint64_t last_frame=0;
    std::unique_ptr<RenderTexture> depth,motion,reactive;
    std::unique_ptr<RenderTexture> dummy_texture;
    // Colour target before the HUD; created on first use in its format.
    std::unique_ptr<RenderTexture> hud;
    std::unique_ptr<RenderBuffer> late_depth;
    std::unique_ptr<RenderDescriptorSet> descriptors;
};

NativeTemporalJitter NativeTemporalJitterForPhase(uint32_t phase,uint32_t width,uint32_t height) {
    NativeTemporalJitter jitter;
    jitter.pixels_x=Halton(phase+1,2)-0.5f;
    jitter.pixels_y=Halton(phase+1,3)-0.5f;
    // FidelityFX: projection offset (2 * x / width, -2 * y / height).
    jitter.clip_x=2.0f*jitter.pixels_x/float(std::max(width,1u));
    jitter.clip_y=-2.0f*jitter.pixels_y/float(std::max(height,1u));
    return jitter;
}

std::optional<NativeTemporalCamera> NativeTemporalReadCamera(std::span<const uint8_t> state) {
    if(state.size()<kVertexConstants+6*16)return std::nullopt;
    NativeTemporalCamera camera;
    for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c) {
        const float value=Constant(state,r,c);
        if(!std::isfinite(value))return std::nullopt;
        camera.view_projection[r][c]=value;
    }
    for(unsigned c=0;c<3;++c) {
        const float value=Constant(state,5,c);
        if(!std::isfinite(value))return std::nullopt;
        camera.pre_view_translation[c]=value;
    }
    return camera;
}

void NativeTemporalApplyJitter(std::span<uint8_t> state,const NativeTemporalJitter& jitter) {
    if(state.size()<kVertexConstants+4*16)throw std::runtime_error("Truncated temporal jitter constants");
    // Row-vector clip = x*c0 + y*c1 + z*c2 + w*c3, so adding j*c[i].w to
    // c[i].x adds j * clip.w to clip.x.
    for(unsigned r=0;r<4;++r) {
        const size_t row=kVertexConstants+r*16;
        const float w=LoadFloat(state,row+12);
        StoreFloat(state,row,LoadFloat(state,row)+jitter.clip_x*w);
        StoreFloat(state,row+4,LoadFloat(state,row+4)+jitter.clip_y*w);
    }
}

bool NativeTemporalIsHudDraw(std::span<const uint8_t> state,uint32_t width,uint32_t height) {
    if(state.size()<kVertexConstants+10*16||!width||!height)return false;
    auto close=[](float value,float expected) {
        return std::abs(value-expected)<=1e-4f*std::max(1.0f,std::abs(expected));
    };
    return close(Constant(state,6,0),2.0f/float(width))&&close(Constant(state,6,1),0)&&
           close(Constant(state,7,0),0)&&close(Constant(state,7,1),-2.0f/float(height))&&
           close(Constant(state,8,2),1)&&close(Constant(state,9,3),1)&&
           std::abs(Constant(state,9,0)+1.0f)<0.01f&&std::abs(Constant(state,9,1)-1.0f)<0.01f;
}

bool NativeTemporalIsVelocityDraw(std::span<const uint8_t> state,uint32_t target_width,
        uint32_t target_height,uint32_t frame_width,uint32_t frame_height) {
    if(state.size()<0x2940||!frame_width||!frame_height)return false;
    if(target_width*2!=frame_width||target_height*2!=frame_height)return false;
    // Scene-view (reverse-Z) viewport, colour writes to red and green only.
    return LoadFloat(state,0x2918)==-1.f&&LoadFloat(state,0x291c)==1.f&&(Register(state,0x28dc)&15)==3;
}

std::optional<std::array<float,2>> NativeTemporalVelocityScale(std::span<const uint8_t> state,
        uint32_t frame_width,uint32_t frame_height) {
    if(state.size()<kPixelConstants+12*16)return std::nullopt;
    const float x=LoadFloat(state,kPixelConstants+11*16),y=LoadFloat(state,kPixelConstants+11*16+4);
    if(!std::isfinite(x)||!std::isfinite(y)||std::abs(x)<1e-6f||std::abs(y)<1e-6f)return std::nullopt;
    // ndc.x = encoded.x / c11.x; screen y points down while ndc y points up.
    const std::array<float,2> scale{0.5f*float(frame_width)/x,-0.5f*float(frame_height)/y};
    // The velocity scale is the same in both directions (square pixels). Some
    // draws of the pass in cutscenes hold something else in c11 (scales of
    // -150000 and 30000 px): taken as the frame's scale, every moving object
    // got motion vectors far off screen, which frame generation follows.
    if(scale[0]<=0||std::abs(scale[0]-scale[1])>0.02f*scale[0])return std::nullopt;
    return scale;
}

std::array<std::array<float,4>,4> NativeTemporalReprojection(
        const NativeTemporalCamera& current,const NativeTemporalCamera& previous) {
    // translated_prev = translated_cur + (c5_prev - c5_cur) * w.
    Matrix translate{};
    for(int i=0;i<4;++i)translate[i][i]=1;
    for(int c=0;c<3;++c)translate[3][c]=previous.pre_view_translation[c]-current.pre_view_translation[c];
    Matrix inverse;
    std::array<std::array<float,4>,4> result{};
    if(!Invert(current.view_projection,inverse)) {
        for(int i=0;i<4;++i)result[i][i]=1;
        return result;
    }
    const auto m=Multiply(Multiply(inverse,translate),previous.view_projection);
    for(int i=0;i<4;++i)for(int j=0;j<4;++j)result[i][j]=float(m[i][j]);
    return result;
}

float NativeTemporalScreenMotion(const NativeTemporalCamera& current,
        const NativeTemporalCamera& previous,const NativeTemporalProjection& projection) {
    const auto r=NativeTemporalReprojection(current,previous);
    std::array<float,18> distances{};
    size_t n=0;
    for(const double view_depth:{300.0,3000.0}) {  // Unreal units (cm)
        const double z=1-(projection.infinity_depth+projection.depth_scale/view_depth);
        for(const double y:{-0.6,0.0,0.6})for(const double x:{-0.6,0.0,0.6}) {
            double clip[4];
            for(int j=0;j<4;++j)clip[j]=x*r[0][j]+y*r[1][j]+z*r[2][j]+r[3][j];
            distances[n++]=clip[3]<=1e-6?4.0f:
                float(std::hypot(clip[0]/clip[3]-x,clip[1]/clip[3]-y));
        }
    }
    std::nth_element(distances.begin(),distances.begin()+n/2,distances.begin()+n);
    return distances[n/2];
}

bool NativeTemporalDescribeProjection(const NativeTemporalCamera& camera,NativeTemporalProjection& out) {
    const auto y=Column(camera,1),z=Column(camera,2),w=Column(camera,3);
    const double ww=Dot(w,w);
    if(ww<1e-12)return false;
    // Depth along the view ray: ndc.z = a + b / w_view, device depth = 1 - ndc.z.
    const double a=Dot(z,w)/ww,b=camera.view_projection[3][2];
    const double yy=std::sqrt(Dot(y,y)/ww);
    if(std::abs(a)<1e-9||yy<1e-9)return false;
    out.near_plane=float(std::abs(b/a));
    out.fov_y=float(2*std::atan(1/yy));
    out.infinity_depth=float(std::clamp(1-a,0.0,1.0));
    out.depth_scale=float(-b);
    return std::isfinite(out.near_plane)&&out.near_plane>0&&std::isfinite(out.fov_y);
}

NativeTemporalInputs::NativeTemporalInputs(RenderDevice& device,std::span<const uint32_t> compute):device_(device) {
    const RenderDescriptorSetDesc descriptors(kRanges.data(),uint32_t(kRanges.size()));
    const RenderPushConstantRange push(0,0,0,sizeof(PushConstants),RenderShaderStageFlag::COMPUTE);
    layout_=device.createPipelineLayout(RenderPipelineLayoutDesc(&push,1,&descriptors,1));
    shader_=device.createShader(compute.data(),compute.size_bytes(),"main",RenderShaderFormat::SPIRV);
    if(!layout_||!static_cast<VulkanPipelineLayout&>(*layout_).vk||!shader_||!static_cast<VulkanShader&>(*shader_).vk)return;
    pipeline_=device.createComputePipeline(RenderComputePipelineDesc(layout_.get(),shader_.get(),8,8,1));
    supported_=pipeline_&&static_cast<VulkanComputePipeline&>(*pipeline_).vk;
    if(!supported_)REXLOG_WARN("Temporal upscaling inputs unavailable: compute pipeline creation failed");
}
NativeTemporalInputs::~NativeTemporalInputs() {
    // The presenter may still hold frames through TemporalUpscaleInputs::
    // keep_alive, but Plume objects must go with the device that owns them.
    // The renderer is idle at this point and produces no further inputs.
    for(auto& frame:frames_) {
        frame->descriptors.reset();frame->late_depth.reset();frame->hud.reset();
        frame->depth.reset();frame->motion.reset();frame->reactive.reset();frame->dummy_texture.reset();
    }
}

bool NativeTemporalInputs::BeginFrame(uint32_t width,uint32_t height) {
    const auto request=rex::ui::vulkan::GetTemporalUpscaleRequest();
    active_=supported_&&request.active&&width&&height;
    if(!active_) {history_valid_=false;camera_.reset();previous_camera_.reset();return false;}
    if(width!=width_||height!=height_) {history_valid_=false;width_=width;height_=height;}
    ++frame_id_;++statistics_.frames;
    phase_=uint32_t(frame_id_%std::max(request.jitter_phase_count,1u));
    reactive_floor_=std::clamp(request.reactive_floor,0.0f,1.0f);
    jitter_=NativeTemporalJitterForPhase(phase_,width,height);
    camera_.reset();world_depth_=nullptr;hud_source_=nullptr;current_.reset();
    velocity_=nullptr;velocity_swapped_=false;velocity_scale_={};
    return true;
}

bool NativeTemporalInputs::ObserveSceneDraw(std::span<const uint8_t> state) {
    if(!active_)return false;
    if(!camera_) {
        camera_=NativeTemporalReadCamera(state);
        if(!camera_)return false;
    }
    ++statistics_.jittered_draws;
    return true;
}

void NativeTemporalInputs::ObserveDepthResolve(RenderTexture* resolved) {
    if(!active_||!resolved)return;
    const auto& desc=static_cast<const VulkanTexture&>(*resolved).desc;
    if(desc.width!=width_||desc.height!=height_)return;
    if(Format(*resolved)!=VK_FORMAT_R32_SFLOAT)return;
    world_depth_=resolved;
}

void NativeTemporalInputs::ObserveVelocityDraw(std::span<const uint8_t> state) {
    if(!active_||velocity_scale_[0]!=0)return;
    // The player's draws write zero velocity with c11 = 0; any other draw of
    // the pass carries the frame's scale.
    if(const auto scale=NativeTemporalVelocityScale(state,width_,height_))velocity_scale_=*scale;
}

void NativeTemporalInputs::ObserveVelocityResolve(RenderTexture* resolved,bool red_blue_swapped) {
    if(!active_||!resolved)return;
    const auto& desc=static_cast<const VulkanTexture&>(*resolved).desc;
    if(desc.width*2!=width_||desc.height*2!=height_)return;
    velocity_=resolved;velocity_swapped_=red_blue_swapped;
}

void NativeTemporalInputs::RecordHudSnapshot(RenderCommandList& commands,RenderTexture& surface) {
    if(!WantsHudSnapshot())return;
    const auto& desc=static_cast<const VulkanTexture&>(surface).desc;
    if(desc.width!=width_||desc.height!=height_)return;
    if(!current_)current_=AcquireFrame();
    if(!current_)return;
    auto& hud=current_->hud;
    if(!hud||static_cast<const VulkanTexture&>(*hud).desc.format!=desc.format) {
        hud=device_.createTexture(RenderTextureDesc::Texture2D(width_,height_,1,desc.format));
        if(!hud||!static_cast<VulkanTexture&>(*hud).vk) {hud.reset();return;}
    }
    const BorrowedLayout borrowed(&surface);
    commands.barriers(RenderBarrierStage::COPY,RenderTextureBarrier(&surface,RenderTextureLayout::COPY_SOURCE));
    commands.barriers(RenderBarrierStage::COPY,RenderTextureBarrier(hud.get(),RenderTextureLayout::COPY_DEST));
    commands.copyTexture(hud.get(),&surface);
    borrowed.Restore(commands);
    hud_source_=&surface;
    ++statistics_.hud_snapshots;
}

std::shared_ptr<NativeTemporalInputs::Frame> NativeTemporalInputs::AcquireFrame() {
    for(auto& frame:frames_)
        if(frame.use_count()==1&&frame->width==width_&&frame->height==height_&&
           frame->last_frame+kFrameReuseDistance<=frame_id_) {frame->last_frame=frame_id_;return frame;}
    if(frames_.size()>=kFrameReuseDistance+4)return nullptr;
    auto frame=std::make_shared<Frame>();
    frame->width=width_;frame->height=height_;
    // STORAGE gives the images VK_IMAGE_USAGE_STORAGE_BIT; without it the
    // shader's writes are undefined (NVIDIA scrambled all three outputs).
    const auto storage=RenderTextureFlag::STORAGE|RenderTextureFlag::UNORDERED_ACCESS;
    frame->depth=device_.createTexture(RenderTextureDesc::Texture2D(width_,height_,1,RenderFormat::R32_FLOAT,storage));
    frame->motion=device_.createTexture(RenderTextureDesc::Texture2D(width_,height_,1,RenderFormat::R16G16_FLOAT,storage));
    // Red: reactive (current frame over history). Green: HUD coverage.
    frame->reactive=device_.createTexture(RenderTextureDesc::Texture2D(width_,height_,1,RenderFormat::R8G8_UNORM,storage));
    frame->dummy_texture=device_.createTexture(RenderTextureDesc::Texture2D(1,1,1,RenderFormat::R32_FLOAT));
    frame->late_depth=device_.createBuffer(RenderBufferDesc::DefaultBuffer(uint64_t(width_)*height_*4,
        RenderBufferFlag::STORAGE|RenderBufferFlag::UNORDERED_ACCESS));
    frame->descriptors=device_.createDescriptorSet(RenderDescriptorSetDesc(kRanges.data(),uint32_t(kRanges.size())));
    if(!frame->depth||!frame->motion||!frame->reactive||!frame->dummy_texture||!frame->late_depth||!frame->descriptors)
        return nullptr;
    frame->last_frame=frame_id_;
    frames_.push_back(frame);
    return frame;
}

std::shared_ptr<const rex::ui::vulkan::TemporalUpscaleInputs> NativeTemporalInputs::Finish(
        RenderCommandList& commands,RenderTexture* world_depth,RenderTexture* late_depth,RenderTexture* hud_final,
        RenderTexture* velocity) {
    if(!active_)return nullptr;
    active_=false;
    if(world_depth!=world_depth_)world_depth=nullptr;
    // Per-object motion needs both the buffer and the frame's scale; a frame
    // with only zero-velocity draws (the player alone) has no scale but its
    // buffer still marks the player as not moving on screen.
    if(velocity!=velocity_)velocity=nullptr;
    const auto now=std::chrono::steady_clock::now();
    const float frame_ms=last_finish_.time_since_epoch().count()?
        std::clamp(std::chrono::duration<float,std::milli>(now-last_finish_).count(),1.0f,100.0f):16.667f;
    last_finish_=now;
    NativeTemporalProjection projection;
    if(!camera_||!NativeTemporalDescribeProjection(*camera_,projection)) {
        history_valid_=false;previous_camera_.reset();return nullptr;
    }
    auto frame=current_?std::move(current_):AcquireFrame();
    if(!frame) {history_valid_=false;previous_camera_=camera_;return nullptr;}

    // A camera cut (teleport, large turn, or a new shot in a cutscene that the
    // scene jumps across on screen) invalidates history; frame generation
    // would otherwise blend two unrelated shots.
    bool reset=!history_valid_||!previous_camera_;
    if(!reset) {
        double moved=0;
        for(int c=0;c<3;++c) {
            const double d=camera_->pre_view_translation[c]-previous_camera_->pre_view_translation[c];
            moved+=d*d;
        }
        const auto w=Column(*camera_,3),pw=Column(*previous_camera_,3);
        const double facing=Dot(w,pw)/std::sqrt(std::max(Dot(w,w)*Dot(pw,pw),1e-24));
        const float screen=NativeTemporalScreenMotion(*camera_,*previous_camera_,projection);
        // Sensitive on purpose: a missed cut ghosts the generated frame, while a
        // false one only costs one frame of history. Cutscene cuts that moved
        // 0.35-0.6 of the screen used to slip through.
        reset=moved>500.0*500.0||facing<0.85||screen>0.35f;
        if(reset&&logged_cuts_<24) {
            ++logged_cuts_;
            REXLOG_INFO("Temporal inputs: camera cut at frame {} (moved {:.0f}, facing {:.3f}, screen {:.2f})",
                        frame_id_,std::sqrt(moved),facing,screen);
        }
    }
    statistics_.resets+=reset;
    PushConstants push{};
    push.reprojection=NativeTemporalReprojection(*camera_,reset?*camera_:*previous_camera_);
    push.width=width_;push.height=height_;
    push.min_depth=std::max(projection.infinity_depth,1e-6f);
    push.velocity_scale=velocity_scale_;
    push.depth_scale=projection.depth_scale;
    push.reactive_floor=reactive_floor_;
    // History is valid only across consecutive frames of the same size.
    RenderTexture* previous_depth=!reset&&previous_&&previous_!=frame&&previous_->width==width_&&
        previous_->height==height_?previous_->depth.get():nullptr;
    push.jitter={jitter_.pixels_x,jitter_.pixels_y};

    const bool hud=frame->hud&&hud_source_&&hud_final==hud_source_;
    const bool late=late_depth&&static_cast<const VulkanTexture&>(*late_depth).desc.width==width_&&
        static_cast<const VulkanTexture&>(*late_depth).desc.height==height_&&
        (Format(*late_depth)==VK_FORMAT_D32_SFLOAT_S8_UINT||Format(*late_depth)==VK_FORMAT_D32_SFLOAT);
    push.flags=(late?kFlagLateDepth:0)|(hud?kFlagHud:0)|(world_depth?kFlagWorldDepth:0)|
        (velocity?kFlagVelocity:0)|(velocity&&velocity_swapped_?kFlagVelocitySwapped:0)|
        (previous_depth?kFlagPreviousDepth:0);
    statistics_.velocity_frames+=velocity!=nullptr;

    const BorrowedLayout late_layout(late?late_depth:nullptr),hud_layout(hud?hud_final:nullptr),
        world_layout(world_depth),velocity_layout(velocity);
    if(late) {
        commands.barriers(RenderBarrierStage::COPY,RenderTextureBarrier(late_depth,RenderTextureLayout::COPY_SOURCE));
        commands.barriers(RenderBarrierStage::COPY,RenderBufferBarrier(frame->late_depth.get(),RenderBufferAccess::WRITE));
        VkBufferImageCopy copy{};
        copy.imageSubresource={VK_IMAGE_ASPECT_DEPTH_BIT,0,0,1};
        copy.imageExtent={width_,height_,1};
        vkCmdCopyImageToBuffer(static_cast<VulkanCommandList&>(commands).vk,static_cast<VulkanTexture&>(*late_depth).vk,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,static_cast<VulkanBuffer&>(*frame->late_depth).vk,1,&copy);
    }
    commands.barriers(RenderBarrierStage::COMPUTE,RenderBufferBarrier(frame->late_depth.get(),RenderBufferAccess::READ));
    RenderTexture* world=world_depth?world_depth:frame->dummy_texture.get();
    RenderTexture* before=hud?frame->hud.get():frame->dummy_texture.get();
    RenderTexture* after=hud?hud_final:frame->dummy_texture.get();
    RenderTexture* motion=velocity?velocity:frame->dummy_texture.get();
    RenderTexture* earlier=previous_depth?previous_depth:frame->dummy_texture.get();
    std::array<RenderTextureBarrier,8> barriers{
        RenderTextureBarrier(earlier,RenderTextureLayout::SHADER_READ),
        RenderTextureBarrier(motion,RenderTextureLayout::SHADER_READ),
        RenderTextureBarrier(world,RenderTextureLayout::SHADER_READ),
        RenderTextureBarrier(before,RenderTextureLayout::SHADER_READ),
        RenderTextureBarrier(after,RenderTextureLayout::SHADER_READ),
        RenderTextureBarrier(frame->depth.get(),RenderTextureLayout::GENERAL),
        RenderTextureBarrier(frame->motion.get(),RenderTextureLayout::GENERAL),
        RenderTextureBarrier(frame->reactive.get(),RenderTextureLayout::GENERAL)};
    // The dummy may appear more than once; one barrier per distinct image.
    std::vector<RenderTextureBarrier> unique;
    for(const auto& barrier:barriers)
        if(std::none_of(unique.begin(),unique.end(),[&](const auto& u){return u.texture==barrier.texture;}))
            unique.push_back(barrier);
    commands.barriers(RenderBarrierStage::COMPUTE,unique.data(),uint32_t(unique.size()));
    auto* sets=frame->descriptors.get();
    sets->setTexture(0,world,RenderTextureLayout::SHADER_READ);
    sets->setBuffer(1,frame->late_depth.get(),uint64_t(width_)*height_*4);
    sets->setTexture(2,before,RenderTextureLayout::SHADER_READ);
    sets->setTexture(3,after,RenderTextureLayout::SHADER_READ);
    sets->setTexture(4,frame->depth.get(),RenderTextureLayout::GENERAL);
    sets->setTexture(5,frame->motion.get(),RenderTextureLayout::GENERAL);
    sets->setTexture(6,frame->reactive.get(),RenderTextureLayout::GENERAL);
    sets->setTexture(7,motion,RenderTextureLayout::SHADER_READ);
    sets->setTexture(8,earlier,RenderTextureLayout::SHADER_READ);
    commands.setComputePipelineLayout(layout_.get());
    commands.setComputePushConstants(0,&push);
    commands.setComputeDescriptorSet(sets,0);
    commands.setPipeline(pipeline_.get());
    commands.dispatch((width_+7)/8,(height_+7)/8,1);
    const std::array<RenderTextureBarrier,3> done{
        RenderTextureBarrier(frame->depth.get(),RenderTextureLayout::SHADER_READ),
        RenderTextureBarrier(frame->motion.get(),RenderTextureLayout::SHADER_READ),
        RenderTextureBarrier(frame->reactive.get(),RenderTextureLayout::SHADER_READ)};
    commands.barriers(RenderBarrierStage::COMPUTE|RenderBarrierStage::GRAPHICS,done.data(),uint32_t(done.size()));
    late_layout.Restore(commands);hud_layout.Restore(commands);world_layout.Restore(commands);
    velocity_layout.Restore(commands);

    auto inputs=std::make_shared<rex::ui::vulkan::TemporalUpscaleInputs>();
    inputs->width=width_;inputs->height=height_;
    inputs->depth=Describe(*frame->depth);
    inputs->motion_vectors=Describe(*frame->motion);
    inputs->reactive=Describe(*frame->reactive);
    if(hud)inputs->hudless_color=Describe(*frame->hud);
    inputs->jitter_x=jitter_.pixels_x;inputs->jitter_y=jitter_.pixels_y;
    inputs->frame_time_ms=frame_ms;
    inputs->camera_near=projection.near_plane;
    inputs->fov_y_radians=projection.fov_y;
    // Unreal units are centimetres.
    inputs->view_space_to_meters=0.01f;
    inputs->inverted_depth=true;inputs->infinite_depth=true;
    inputs->reset=reset;
    inputs->frame_id=frame_id_;
    inputs->keep_alive=frame;
    previous_camera_=camera_;history_valid_=true;previous_=frame;
    if(++statistics_.produced==1||statistics_.produced%3600==0)
        REXLOG_INFO("Temporal inputs {}x{}: frames {} produced {} jittered draws {} HUD snapshots {} resets {}; "
                    "near {:.2f} fov {:.1f} deg, world depth {}, late depth {}, HUD {}; object motion frames {} "
                    "(scale {:.1f} px)",
                    width_,height_,statistics_.frames,statistics_.produced,statistics_.jittered_draws,
                    statistics_.hud_snapshots,statistics_.resets,projection.near_plane,
                    projection.fov_y*57.29578f,world_depth!=nullptr,late,hud,statistics_.velocity_frames,
                    velocity_scale_[0]);
    return inputs;
}

void NativeTemporalInputs::Abort() {
    active_=false;history_valid_=false;camera_.reset();world_depth_=nullptr;hud_source_=nullptr;current_.reset();
    velocity_=nullptr;previous_.reset();
}

}  // namespace gow3::renderer
