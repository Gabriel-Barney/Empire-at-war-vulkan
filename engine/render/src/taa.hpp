// alamo_render - TAA resolve on the GPU [port] (internal; the pure rules are in alamo/render/taa.hpp). Owned by the
// Renderer (created on the first TAA frame), used by render() at two points:
//   begin_frame(): before the builds - the jittered camera every scene draw of the frame uses, and the history test;
//   resolve():     at the end of the post chain - scene colour + scene depth + history[prev] + depth_history[prev] ->
//                  history[cur] (RGBA16F, so the 10 % blend does not stall on 8-bit steps); scene depth -> linear
//                  depth_history[cur] (R32F, the next frame's disocclusion test); then history[cur] -> scene colour
//                  through the sharpen pass (amount 0 = plain copy). The caller's StretchRect out follows unchanged.
#pragma once
#include <cstdint>

#include "alamo/render/taa.hpp"
#include "alamo/rhi/rhi.hpp"

namespace alamo::render {

class TaaPass {
public:
    TaaPass() = default;
    ~TaaPass() { shutdown(); }
    TaaPass(const TaaPass&) = delete;
    TaaPass& operator=(const TaaPass&) = delete;

    bool init(rhi::Context& ctx, rhi::Allocator& alloc);
    void shutdown();  // GPU idle
    void reset() { history_valid_ = false; }
    // Jittered camera of this frame (`view` = the caller's un-jittered camera); decides whether the history is used.
    ViewMatrices begin_frame(const ViewMatrices& view);
    // Resolve + sharpen into `scene` (colour in any tracked state, depth sampled). False (scene untouched, history
    // invalid) when a target / descriptor / pipeline is missing.
    bool resolve(rhi::Frame& frame, VkCommandBuffer cmd, rhi::RenderTarget& scene, float sharpen, float weight = kTaaCurrentWeight);
    bool history_reset() const { return reset_now_; }  // this frame took the current colour only
    uint64_t frame_index() const { return frame_; }

private:
    bool ensure_history(VkExtent2D ext);
    bool ensure_pipelines(VkFormat scene_format);
    void destroy_pipelines();

    rhi::Context* ctx_ = nullptr;
    rhi::Allocator* alloc_ = nullptr;
    rhi::PipelineCache cache_;
    rhi::SamplerCache samplers_;
    VkDescriptorSetLayout set_resolve_ = VK_NULL_HANDLE, set_sharpen_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_resolve_ = VK_NULL_HANDLE, layout_sharpen_ = VK_NULL_HANDLE;
    VkPipeline resolve_ = VK_NULL_HANDLE, sharpen_ = VK_NULL_HANDLE, depth_ = VK_NULL_HANDLE;
    VkFormat sharpen_format_ = VK_FORMAT_UNDEFINED;  // scene colour format the sharpen pipeline was made for
    rhi::RenderTarget history_[2];
    rhi::RenderTarget depth_history_[2];  // R32F linear view distance of the frame each history slot was resolved from
    uint32_t cur_ = 0;               // history written this frame
    uint64_t frame_ = 0;             // TAA frames begun (jitter index)
    bool history_valid_ = false;     // history_[cur_ ^ 1] holds the last resolve of prev_
    bool reset_now_ = true;
    TaaCamera prev_, cam_;           // un-jittered cameras of the last resolve / this frame
    Mat4 jittered_vp_;               // this frame's jittered view-projection
};

}  // namespace alamo::render
