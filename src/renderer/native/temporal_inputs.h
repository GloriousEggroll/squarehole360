#pragma once
// Scene inputs for temporal upscaling (FSR 3.1) and frame generation.
//
// While the presenter asks for them, every frame:
// - scene-view draws (reverse-Z viewport) get sub-pixel jitter added to the
//   ViewProjection in VS c0..c3, and the unjittered matrix and camera offset
//   (c5, PreViewTranslation) are recorded;
// - the last full-size depth resolve is taken as the world depth, and the
//   depth surface at the end of the frame supplies what was drawn after it;
// - the colour surface is copied when the first HUD draw arrives;
// - at the end of the frame one compute pass writes depth, camera motion
//   vectors and a HUD reactive mask for the presenter.
#include "plume_render_interface.h"
#include <rex/ui/vulkan/temporal_upscale.h>
#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace gow3::renderer {

struct NativeTemporalJitter {
    float pixels_x=0,pixels_y=0;  // FidelityFX sign convention
    float clip_x=0,clip_y=0;      // added to clip.xy per unit clip.w
};

// Halton(2,3) jitter for the given phase, in pixels in [-0.5, 0.5).
NativeTemporalJitter NativeTemporalJitterForPhase(uint32_t phase,uint32_t width,uint32_t height);

// Camera state read from a scene-view draw's VS constants.
struct NativeTemporalCamera {
    std::array<std::array<double,4>,4> view_projection{};  // rows c0..c3
    std::array<double,3> pre_view_translation{};           // c5.xyz
};
// Reads c0..c3 and c5 from a captured device state (big-endian floats at
// +0x780); nullopt if any value is not finite.
std::optional<NativeTemporalCamera> NativeTemporalReadCamera(std::span<const uint8_t> state);
// Adds jitter to c0..c3 of a mutable copy of a captured device state.
void NativeTemporalApplyJitter(std::span<uint8_t> state,const NativeTemporalJitter& jitter);
// True for a UE3 Canvas (HUD) draw: VS c6..c9 hold the pixel orthographic
// matrix for a width x height target.
bool NativeTemporalIsHudDraw(std::span<const uint8_t> state,uint32_t width,uint32_t height);
// UE3 velocity pass (per-object motion): a scene-view draw at half the frame
// size writing only red and green. It stores clampLength(ndc delta * c11.xy)
// * 0.5 + 0.5, current minus previous position, with PS c11 the scale; 0 is
// "no moving object". Returns screen pixels per encoded unit at width x
// height, or nullopt for a draw that is not one (or writes zero velocity).
bool NativeTemporalIsVelocityDraw(std::span<const uint8_t> state,uint32_t target_width,
    uint32_t target_height,uint32_t frame_width,uint32_t frame_height);
std::optional<std::array<float,2>> NativeTemporalVelocityScale(std::span<const uint8_t> state,
    uint32_t frame_width,uint32_t frame_height);
// Row-vector matrix mapping current NDC (x, y, z, 1) to previous clip space.
std::array<std::array<float,4>,4> NativeTemporalReprojection(
    const NativeTemporalCamera& current,const NativeTemporalCamera& previous);
// Projection parameters for the upscaler; false if the matrix is degenerate.
// View depth is depth_scale / (device depth - infinity_depth).
struct NativeTemporalProjection {float near_plane=1,fov_y=1,infinity_depth=0,depth_scale=1;};
bool NativeTemporalDescribeProjection(const NativeTemporalCamera&,NativeTemporalProjection&);

class NativeTemporalInputs {
public:
    NativeTemporalInputs(plume::RenderDevice& device,std::span<const uint32_t> compute_spirv);
    ~NativeTemporalInputs();
    bool supported() const noexcept {return supported_;}

    // Call at the first scene operation of a frame; reads the presenter's
    // request and picks this frame's jitter. Returns whether capture is on.
    bool BeginFrame(uint32_t width,uint32_t height);
    bool active() const noexcept {return active_;}
    const NativeTemporalJitter& jitter() const noexcept {return jitter_;}
    // Scene-view draw: record the camera (first draw) and return true when the
    // caller should upload a jittered copy of the constants.
    bool ObserveSceneDraw(std::span<const uint8_t> state);
    // A depth resolve of the scene's size. The caller owns the texture and
    // passes it (still alive) to Finish as `world_depth`.
    void ObserveDepthResolve(plume::RenderTexture* resolved);
    plume::RenderTexture* world_depth() const noexcept {return world_depth_;}
    // A velocity-pass draw (see NativeTemporalIsVelocityDraw).
    void ObserveVelocityDraw(std::span<const uint8_t> state);
    // The resolve of the velocity target; `red_blue_swapped` when the copy
    // swapped channels (velocity then in blue and green).
    void ObserveVelocityResolve(plume::RenderTexture* resolved,bool red_blue_swapped);
    plume::RenderTexture* velocity() const noexcept {return velocity_;}
    bool WantsHudSnapshot() const noexcept {return active_&&camera_&&!hud_source_;}
    // Records a copy of `surface` (the HUD's colour target) into `commands`.
    void RecordHudSnapshot(plume::RenderCommandList& commands,plume::RenderTexture& surface);
    plume::RenderTexture* hud_source() const noexcept {return hud_source_;}
    // Records the inputs pass. All three are optional and must be alive:
    // `world_depth` the observed depth resolve, `late_depth` the scene depth
    // surface at the end of the frame (D32_FLOAT_S8_UINT or D32_FLOAT) and
    // `hud_final` the HUD target. Returns nullptr when this frame has no inputs.
    std::shared_ptr<const rex::ui::vulkan::TemporalUpscaleInputs> Finish(
        plume::RenderCommandList& commands,plume::RenderTexture* world_depth,
        plume::RenderTexture* late_depth,plume::RenderTexture* hud_final,
        plume::RenderTexture* velocity=nullptr);
    // Abandons the frame without producing inputs (e.g. failed presentation).
    void Abort();

    struct Statistics {uint64_t frames=0,produced=0,jittered_draws=0,hud_snapshots=0,resets=0,velocity_frames=0;};
    const Statistics& statistics() const noexcept {return statistics_;}

private:
    struct Frame;
    std::shared_ptr<Frame> AcquireFrame();
    plume::RenderDevice& device_;
    std::unique_ptr<plume::RenderShader> shader_;
    std::unique_ptr<plume::RenderPipelineLayout> layout_;
    std::unique_ptr<plume::RenderPipeline> pipeline_;
    bool supported_=false;

    bool active_=false;
    uint32_t width_=0,height_=0;
    uint64_t frame_id_=0;
    uint32_t phase_=0;
    NativeTemporalJitter jitter_;
    float reactive_floor_=0;
    std::optional<NativeTemporalCamera> camera_,previous_camera_;
    plume::RenderTexture* world_depth_=nullptr;
    plume::RenderTexture* velocity_=nullptr;
    bool velocity_swapped_=false;
    std::array<float,2> velocity_scale_{};
    plume::RenderTexture* hud_source_=nullptr;
    // This frame's output set, acquired at the HUD snapshot or at Finish.
    std::shared_ptr<Frame> current_;
    // The last frame's outputs; its depth finds surfaces that moved on their own.
    std::shared_ptr<Frame> previous_;
    std::vector<std::shared_ptr<Frame>> frames_;
    std::chrono::steady_clock::time_point last_finish_{};
    bool history_valid_=false;
    Statistics statistics_;
};

}  // namespace gow3::renderer
