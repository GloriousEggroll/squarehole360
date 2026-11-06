#pragma once
// Ray-traced ambient occlusion (gow3_rt_ao) and shadows (gow3_rt_shadows) for
// the native renderer.
//
// Each frame:
// - every scene-view draw that writes depth (no alpha test, not a rectangle)
//   is drawn a second time, right after itself, with a vertex-only pipeline
//   that discards rasterization: transform feedback stores its clip-space
//   triangles in a buffer. The game's own draw is unchanged;
// - at the first HUD draw, the triangles are turned into camera-relative world
//   positions with the frame's camera (VS c0..c3, c5), one bottom-level and one
//   top-level acceleration structure are built, and a compute pass traces
//   short rays from every pixel of the scene depth resolve;
// - the occlusion is accumulated over frames with the camera motion, blurred
//   by depth, and multiplied into a copy of the scene colour, which the caller
//   stores back into EDRAM before the HUD is drawn.
//
// Shadows: the game projects the dominant light's shadow maps into the red
// channel of a light attenuation buffer, which the base pass then samples.
// The light comes from the projection's ScreenToShadow matrix (PS c8..c11).
// Before that buffer is resolved, the acceleration structures are built from
// the geometry captured so far (the depth prepass) and every pixel is traced
// towards the light; the buffer keeps the darker of the two. The AO later in
// the frame reuses the same acceleration structures.
//
// All GPU work runs on the renderer's single queue in submission order. A
// full barrier opens each frame's ray-tracing stage, so one set of buffers and
// images serves every frame; only the capture buffer alternates, because the
// next frame's draws write it while this frame's stage may still read it.
#include "plume_vulkan.h"
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "temporal_inputs.h"

namespace gow3::renderer {

struct NativeRtAoSettings {
    bool enabled=false;
    float strength=0.6f;  // 0: off, 1: full occlusion
    float radius=150;     // ray length in Unreal units (cm)
    uint32_t rays=2;      // per pixel per frame
    bool debug_view=false;  // show only the occlusion
    bool shadows=false;
    float shadow_softness=0.25f;  // 0: hard; 1: a large light
    uint32_t shadow_debug=0;      // 1: traced visibility only; 2: everything shadowed
    bool timing=false;            // log GPU time per stage
    bool fast_build=true;         // build the geometry for speed rather than tracing speed
    // The scene is split into this many parts (by a per-mesh key); each frame
    // captures and builds one, and the others are reused from the frames
    // that built them, moved with the camera. Static geometry stays exact;
    // Meshes the game draws into its velocity buffer (characters, moving
    // objects) are taken out of the parts and built every frame on their own.
    // 1: everything every frame.
    uint32_t scene_parts=2;
};

// The game's velocity buffer of this frame (see NativeTemporalInputs), which
// lets the occlusion history follow objects that moved by themselves.
struct NativeRtAoObjectMotion {
    plume::RenderTexture* velocity=nullptr;  // half the frame size
    bool swapped=false;                      // velocity in blue/green
    std::array<float,2> scale{};             // full-size pixels per encoded unit
};

// Adds transform feedback to a vertex shader: its Position output is also
// written to buffer 0 (16 bytes per vertex). Empty if the module has no plain
// Position output variable.
std::vector<uint32_t> NativeAddPositionTransformFeedback(std::span<const uint32_t> spirv);

class NativeRayTracedAo {
public:
    // `draw_layout` is the pipeline layout of the scene draws; capture
    // pipelines share it, so a capture draw reuses the bound push constants
    // and descriptor sets.
    NativeRayTracedAo(plume::RenderDevice& device,const plume::RenderPipelineLayout& draw_layout);
    ~NativeRayTracedAo();
    NativeRayTracedAo(const NativeRayTracedAo&)=delete;
    NativeRayTracedAo& operator=(const NativeRayTracedAo&)=delete;
    bool supported() const noexcept {return supported_;}

    // A vertex shader module as created for scene draws (canonical variant).
    void RememberVertexShader(std::string_view digest,std::span<const uint8_t> spirv);

    // First draw of a frame. `width` x `height` is the scene size.
    void BeginFrame(uint32_t width,uint32_t height,const NativeRtAoSettings& settings);
    bool capturing() const noexcept {return capturing_;}
    // Constants of a scene-view draw; the first one of a frame sets the camera.
    void ObserveCamera(std::span<const uint8_t> state);
    // Records the capture draw right after a scene draw, inside its render
    // pass, then rebinds `restore` (the draw's pipeline). Same index buffer,
    // push constants and descriptor sets as that draw. False if not captured.
    // `depth_only`: the draw writes no colour (a depth prepass). Once a frame
    // has one, colour draws are not captured: the prepass already holds the
    // opaque geometry, and the base pass would add every mesh a second time.
    bool CaptureDraw(plume::RenderCommandList& commands,std::string_view vertex_digest,
                     plume::RenderPrimitiveTopology topology,uint32_t index_count,int32_t index_offset,
                     const plume::RenderPipeline& restore,bool depth_only=false,uint64_t mesh=0);
    // `mesh` identifies a mesh across draws and frames (its vertex and index
    // buffers and range; 0: unknown). A mesh drawn into the game's velocity
    // buffer moves: from the next frame its captures go to a structure built
    // every frame instead of a reused part, which would hold it where it was.
    void ObserveMovingMesh(uint64_t mesh);
    // A full-size depth resolve (R32_FLOAT device depth) of this frame.
    void ObserveDepthResolve(plume::RenderTexture* depth);
    plume::RenderTexture* depth() const noexcept {return depth_;}
    bool WantsApply() const noexcept {
        return capturing_&&settings_.enabled&&!applied_&&(used_vertices_||moving_vertices_||reused_parts_)&&camera_&&depth_;
    }
    // A draw that writes only the red channel of a frame-sized target; true
    // if it is a shadow projection (its ScreenToShadow matrix is kept).
    bool ObserveShadowProjection(std::span<const uint8_t> state);
    bool WantsShadows() const noexcept {
        return capturing_&&settings_.shadows&&!shadows_applied_&&shadow_projection_&&
            (used_vertices_||moving_vertices_||reused_parts_)&&camera_&&depth_;
    }
    // Records the shadow pass into `commands` (outside a render pass) and
    // returns the updated copy of the attenuation buffer `attenuation`, or null.
    std::shared_ptr<plume::RenderTexture> ApplyShadows(plume::RenderCommandList& commands,plume::RenderTexture& attenuation,
        plume::RenderTexture& depth,const std::function<void(std::shared_ptr<void>)>& keep);
    // Records the occlusion passes into `commands` (outside a render pass)
    // and returns the darkened copy of `scene` in its format, or null. `keep`
    // must retain the given lease until the recorded work has completed.
    std::shared_ptr<plume::RenderTexture> Apply(plume::RenderCommandList& commands,plume::RenderTexture& scene,
        plume::RenderTexture& depth,const std::function<void(std::shared_ptr<void>)>& keep,
        const NativeRtAoObjectMotion& motion={});
    // End of frame: a frame whose captures were not used clears them here
    // (outside a render pass) so the buffer can be reused.
    void EndFrame(plume::RenderCommandList& commands);

    struct Statistics {
        uint64_t frames=0,applied=0,captured_draws=0,pending_pipelines=0,missing_shaders=0,
            overflows=0,unpatchable_shaders=0,pipelines=0,pipeline_failures=0,max_vertices=0,shadow_frames=0,
            prepass_frames=0,skipped_after_prepass=0,reused_scenes=0,moving_draws=0,max_moving_vertices=0,
            moving_overflows=0;
    };
    const Statistics& statistics() const noexcept {return statistics_;}

private:
    struct CapturePipeline;
    struct Job;
    struct FrameLease;
    void Worker();
    void FillCapture(plume::RenderCommandList& commands,uint32_t index);
    VkRenderPass CompatibleRenderPass(const plume::RenderCommandList& commands,std::string& signature);
    bool CreatePasses();
    bool EnsureResources();
    // Converts this frame's captures and builds the acceleration structures,
    // once per frame; later captures are skipped.
    bool BuildScene(plume::RenderCommandList& commands,const std::array<std::array<double,4>,4>& inverse);
    plume::RenderPipeline* Composite(VkFormat format);
    plume::RenderPipeline* Shadow(VkFormat format);
    // GPU timestamps around the stages (settings.timing): scene build,
    // shadows and occlusion, each a start/end pair, for kTimingFrames frames
    // in flight; read back kTimingFrames frames later and logged as averages.
    enum TimingStage : uint32_t {kTimingBuild,kTimingShadows,kTimingAo,kTimingStages};
    static constexpr uint32_t kTimingFrames=4;
    void Stamp(VkCommandBuffer cb,TimingStage stage,bool end);
    void CollectTiming();

    plume::RenderDevice& device_;
    VkDevice vk_=VK_NULL_HANDLE;
    VkPipelineLayout draw_layout_=VK_NULL_HANDLE;
    bool supported_=false,resources_ready_=false,resources_failed_=false;
    PFN_vkCmdBindTransformFeedbackBuffersEXT bind_feedback_=nullptr;
    PFN_vkCmdBeginTransformFeedbackEXT begin_feedback_=nullptr;
    PFN_vkCmdEndTransformFeedbackEXT end_feedback_=nullptr;

    // Capture: two alternating transform feedback buffers of clip positions.
    // Static draws fill it from the start; moving meshes from kMovingBase.
    static constexpr uint32_t kMaxVertices=4u*1024*1024,kMaxMovingVertices=1024u*1024,
        kMovingBase=kMaxVertices-kMaxMovingVertices;
    struct CaptureBuffer {VkBuffer buffer=VK_NULL_HANDLE;VmaAllocation allocation=VK_NULL_HANDLE;uint64_t address=0;
        uint32_t dirty_vertices=0,dirty_moving=0;};
    std::array<CaptureBuffer,2> capture_{};
    uint32_t capture_index_=0;
    bool capture_cleared_=false;

    // Geometry and acceleration structures.
    std::unique_ptr<plume::RenderBuffer> positions_,tlas_buffer_,scratch_;
    // Top-level instance data, one per frame in a ring: a reused scene moves
    // its instance with the camera, and the CPU writes it while earlier
    // frames' builds may still read theirs.
    static constexpr uint32_t kInstanceRing=8;
    std::array<std::unique_ptr<plume::RenderBuffer>,kInstanceRing> instances_;
    // Scene parts (settings.scene_parts), each a bottom-level structure with
    // the camera offset it was built with (VS c5: camera-relative world =
    // world + c5) and the frame that built it.
    static constexpr uint32_t kMaxParts=2;
    struct Part {
        std::unique_ptr<plume::RenderBuffer> buffer;
        std::unique_ptr<plume::RenderAccelerationStructure> blas;
        std::array<uint8_t,64> instance{};  // VkAccelerationStructureInstanceKHR
        std::array<double,3> translation{};
        uint64_t frame=0;bool valid=false;
    };
    std::array<Part,kMaxParts> scene_parts_;
    Part moving_part_;  // this frame's moving meshes only
    uint64_t moving_blas_capacity_=0;
    uint32_t parts_=1,part_=0;  // part count and the part this frame builds
    uint32_t reused_parts_=0;    // other parts usable this frame
    bool capture_all_=true;      // this frame's part takes every draw
    // Top-level build infos for 1..kMaxInstances instances.
    static constexpr uint32_t kMaxInstances=kMaxParts+1;
    std::array<plume::RenderTopLevelASBuildInfo,kMaxInstances> tlas_infos_;
    std::unique_ptr<plume::RenderAccelerationStructure> tlas_;
    uint64_t blas_capacity_=0,scratch_capacity_=0;

    // Passes.
    std::unique_ptr<plume::RenderPipelineLayout> convert_layout_,trace_layout_,temporal_layout_,composite_layout_;
    std::vector<std::unique_ptr<plume::RenderShader>> shaders_;
    std::unique_ptr<plume::RenderPipeline> convert_,trace_,temporal_;
    std::map<VkFormat,std::unique_ptr<plume::RenderPipeline>> composites_,shadow_passes_;
    std::unique_ptr<plume::RenderPipelineLayout> shadow_layout_,shadow_merge_layout_;
    std::unique_ptr<plume::RenderPipeline> shadow_trace_;

    // Images, recreated when the size or scene format changes.
    uint32_t image_width_=0,image_height_=0;
    std::shared_ptr<plume::RenderTexture> ao_raw_,output_,shadow_output_,shadow_visibility_;
    std::array<std::shared_ptr<plume::RenderTexture>,2> history_,history_depth_;
    uint32_t history_index_=0;
    bool history_valid_=false;

    // Frame state.
    uint32_t width_=0,height_=0;
    NativeRtAoSettings settings_;
    bool capturing_=false,applied_=false,scene_built_=false,scene_ready_=false,shadows_applied_=false;
    std::optional<std::array<std::array<double,4>,4>> shadow_projection_;  // ScreenToShadow
    uint32_t used_vertices_=0,moving_vertices_=0;
    // Meshes seen in the velocity pass, this frame and the one before.
    std::array<std::unordered_set<uint64_t>,2> moving_meshes_;
    uint32_t moving_index_=0;
    bool prepass_seen_=false;
    std::optional<NativeTemporalCamera> camera_,previous_camera_;
    plume::RenderTexture* depth_=nullptr;
    uint64_t frame_=0;

    // Vertex shaders (transform feedback added) and capture pipelines, built
    // on a worker thread against private render passes compatible with the
    // scene's.
    std::mutex shaders_mutex_;
    std::unordered_map<std::string,std::shared_ptr<const std::vector<uint32_t>>> vertex_shaders_;
    std::map<std::string,VkRenderPass> render_passes_;
    std::unordered_map<std::string,std::shared_ptr<CapturePipeline>> pipelines_;
    std::mutex jobs_mutex_;
    std::condition_variable jobs_changed_;
    std::deque<std::unique_ptr<Job>> jobs_;
    bool stopping_=false;
    std::atomic<uint64_t> worker_pipelines_{0},worker_failures_{0};
    VkQueryPool timing_pool_=VK_NULL_HANDLE;
    double timestamp_period_ns_=1;
    std::array<uint32_t,kTimingFrames> timing_written_{};  // bits: stamps written per slot
    bool timing_reset_=false;                              // this frame's slot was reset
    std::array<double,kTimingStages> timing_ms_{};
    std::array<uint32_t,kTimingStages> timing_count_{};
    uint64_t timing_frames_=0,timing_vertices_=0;
    std::thread worker_;
    Statistics statistics_;
};

}  // namespace gow3::renderer
