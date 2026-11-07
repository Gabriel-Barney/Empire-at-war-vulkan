// alamo_render - TAA resolve on the GPU [port] (internal; the pure rules are in alamo/render/taa.hpp). Owned by the
// Renderer (created on the first TAA frame), used by render() at these points:
//   begin_frame():   before the builds - the jittered camera every scene draw of the frame uses, and the history test;
//   motion_delta():  after the builds - how far an instance (Instance::motion_id) moved since the last TAA frame;
//   resolve():       at the end of the post chain - the movers' velocity pass (see below), then scene colour + scene depth +
//                    history[prev] + depth_history[prev] + velocity -> history[cur] (RGBA16F, so the 10 % blend does not stall
//                    on 8-bit steps); scene depth -> linear depth_history[cur] (R32F, the next frame's disocclusion test);
//                    then history[cur] -> scene colour through the sharpen pass (amount 0 = plain copy). The caller's
//                    StretchRect out follows unchanged;
//   commit_motion(): remembers this frame's instance transforms for the next frame.
// [port] BM2b provisional frames (RecordOptions::taa_provisional): begin_frame(view, true) keeps the state the frame starts
// from; discard() puts it back, so the next frame jitters, reprojects and sees object motion as if this one had not been
// drawn. The GPU side needs nothing: the frame only read history[cur ^ 1] and wrote history[cur], which the next frame
// overwrites. keep() forgets the saved state (the frame stands); reset() and a non-provisional begin_frame() do too.
// Velocity pass: the opaque items of instances that moved are drawn again into an RGBA32F target (taa_velocity.hlsl),
// depth-tested against the finished scene depth, writing where each visible surface point stood in the previous image.
// The resolve uses it at the nearest-depth pixel instead of the camera-only reprojection.
#pragma once
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

#include "alamo/render/renderer.hpp"
#include "alamo/render/taa.hpp"
#include "alamo/rhi/rhi.hpp"

namespace alamo::render {

// One opaque draw of an instance that moved since the last TAA frame (render() builds the list from the scene build).
struct TaaMover {
    VkBuffer vertices = VK_NULL_HANDLE, indices = VK_NULL_HANDLE;
    VkIndexType index_type = VK_INDEX_TYPE_UINT16;
    uint32_t index_count = 0;
    bool skinned = false;     // rigid-skin vertex layout (palette index in NORMAL.w)
    bool translated = false;  // translated draw: carries the half-pixel clip offset (Renderer::translated_pixel_offset)
    uint32_t draw_index = 0;  // the build's item index: its DrawData and bone slice
    float prev_clip[4][4]{};  // register rows: world position of this frame -> previous frame's clip, instance motion applied
};

class TaaPass {
public:
    TaaPass() = default;
    ~TaaPass() { shutdown(); }
    TaaPass(const TaaPass&) = delete;
    TaaPass& operator=(const TaaPass&) = delete;

    // `hot_set0`: the renderer's set-0 layout (FrameCB, DrawData, materials, bones, FF) the velocity pass binds.
    bool init(rhi::Context& ctx, rhi::Allocator& alloc, VkDescriptorSetLayout hot_set0);
    void shutdown();  // GPU idle
    void reset() {  // the next frame starts over: no history, no object motion, jitter cycle from its first point
        history_valid_ = false;
        motion_.clear();
        frame_ = 0;
        undo_.armed = false;  // nothing before a reset to go back to
    }
    // Jittered camera of this frame (`view` = the caller's un-jittered camera); decides whether the history is used.
    // `provisional`: keep the state this frame starts from for discard().
    ViewMatrices begin_frame(const ViewMatrices& view, bool provisional = false);
    // [port] BM2b: undo the last provisional frame's TAA step (false: none to undo). keep(): it stands.
    bool discard();
    void keep() { undo_.armed = false; }
    // `cur` is the instance's transform this frame: true (and `delta` = previous x inverse(cur), the world-space move of
    // everything attached to it) when the instance was seen in the previous TAA frame and has moved since.
    bool motion_delta(uint64_t motion_id, const Mat3x4& cur, Mat3x4& delta) const;
    // The last resolved frame's un-jittered view-projection (movers' previous clip).
    const Mat4& previous_view_proj() const { return prev_.view_proj; }
    // Remembers every keyed instance of this frame; forgets the ones that are gone.
    void commit_motion(std::span<const Instance> instances);
    std::vector<TaaMover>& movers() { return movers_; }  // scratch the caller fills each frame
    // Velocity pass + resolve + sharpen into `scene` (colour in any tracked state, depth sampled). `hot_set0` is the build's
    // set 0 (the movers' DrawData / bones). False (scene untouched, history invalid) when a target / descriptor / pipeline
    // is missing.
    bool resolve(rhi::Frame& frame, VkCommandBuffer cmd, rhi::RenderTarget& scene, float sharpen, float weight,
                 VkDescriptorSet hot_set0);
    bool history_reset() const { return reset_now_; }  // this frame took the current colour only
    uint32_t last_movers() const { return last_movers_; }  // movers the last resolve drew

private:
    bool ensure_history(VkExtent2D ext);
    bool ensure_pipelines(VkFormat scene_format);
    bool ensure_velocity_pipelines(VkFormat depth_format);
    void destroy_pipelines();
    void record_velocity(const rhi::DeviceFns& fn, VkCommandBuffer cmd, rhi::RenderTarget& scene, VkDescriptorSet hot_set0);

    rhi::Context* ctx_ = nullptr;
    rhi::Allocator* alloc_ = nullptr;
    rhi::PipelineCache cache_;
    rhi::SamplerCache samplers_;
    VkDescriptorSetLayout set_resolve_ = VK_NULL_HANDLE, set_sharpen_ = VK_NULL_HANDLE, hot_set0_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_resolve_ = VK_NULL_HANDLE, layout_sharpen_ = VK_NULL_HANDLE, layout_velocity_ = VK_NULL_HANDLE;
    VkPipeline resolve_ = VK_NULL_HANDLE, sharpen_ = VK_NULL_HANDLE, depth_ = VK_NULL_HANDLE;
    VkPipeline velocity_pipe_[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};  // static / rigid-skin vertex layout
    VkFormat sharpen_format_ = VK_FORMAT_UNDEFINED;   // scene colour format the sharpen pipeline was made for
    VkFormat velocity_depth_format_ = VK_FORMAT_UNDEFINED;  // scene depth format the velocity pipelines were made for
    rhi::RenderTarget history_[2];
    rhi::RenderTarget depth_history_[2];  // R32F linear view distance of the frame each history slot was resolved from
    rhi::RenderTarget velocity_;          // RGBA32F: previous uv, previous NDC depth, 1 = written (movers only)
    uint32_t cur_ = 0;               // history written this frame
    uint64_t frame_ = 0;             // TAA frames begun (jitter index)
    bool history_valid_ = false;     // history_[cur_ ^ 1] holds the last resolve of prev_
    bool reset_now_ = true;
    TaaCamera prev_, cam_;           // un-jittered cameras of the last resolve / this frame
    Mat4 jittered_vp_;               // this frame's jittered view-projection
    struct MotionRec {
        Mat3x4 world;
        uint64_t frame = 0;  // frame_ when it was committed
    };
    std::unordered_map<uint64_t, MotionRec> motion_;  // keyed lookups and an erase sweep only: never ordered
    std::vector<TaaMover> movers_;
    uint32_t last_movers_ = 0;
    uint64_t history_generation_ = 0;  // bumped whenever ensure_history (re)creates the history targets
    struct Undo {  // BM2b: the state a provisional frame started from
        bool armed = false;
        bool motion_saved = false;  // commit_motion ran: `motion` holds the previous frame's records
        uint32_t cur = 0;
        uint64_t frame = 0, generation = 0;
        bool history_valid = false, reset_now = true;
        TaaCamera prev, cam;
        Mat4 jittered_vp;
        uint32_t last_movers = 0;
        std::unordered_map<uint64_t, MotionRec> motion;
    } undo_;
};

}  // namespace alamo::render
