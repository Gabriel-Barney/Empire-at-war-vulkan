// alamo_render - [port] screen-space ambient occlusion (the original game renders none; render/shaders/ssao.hlsl).
// Off by default (RecordOptions::ambient_occlusion). Renderer::render() runs it after the opaque buckets and the
// stencil-shadow darken, before water / transparent / lasers / particles / post, so glass, glows and bloom keep
// their brightness. Depth only: half-res AO from the sampled scene depth, separable depth-aware blur, depth-aware
// upsample multiplied into the scene colour. Not run in galactic mode or space frames (no ground to occlude).
#pragma once
#include <cstdint>
#include <vector>

#include "alamo/render/camera.hpp"
#include "alamo/rhi/frame.hpp"
#include "alamo/rhi/memory.hpp"
#include "alamo/rhi/pipeline.hpp"
#include "alamo/rhi/rendertarget.hpp"
#include "alamo/rhi/sampler.hpp"

namespace alamo::rhi {
class Context;
class GpuTimer;
}  // namespace alamo::rhi

namespace alamo::render {

// Depth -> view-space reconstruction constants (the shader's push block rows) from a projection without x / y
// shear: perspective or orthographic, normal or reversed depth, sub-pixel offsets in m[0][2] / m[1][2] kept.
struct SsaoProjection {
    float p00 = 1, p11 = 1, p02 = 0, p12 = 0, p03 = 0, p13 = 0, p22 = 0, p23 = 0, p32 = 0, p33 = 1;
};
SsaoProjection ssao_projection(const Mat4& proj);
// CPU mirror of the shader's ViewPos: view-space position of screen uv (0,0 = top-left, y down) at depth d.
Float3 ssao_view_position(const SsaoProjection& k, float u, float v, float d);
// View-space point -> screen uv + depth (the inverse of ssao_view_position; tests).
void ssao_project(const SsaoProjection& k, const Float3& p, float& u, float& v, float& d);

class SsaoPass {
public:
    SsaoPass() = default;
    ~SsaoPass();
    SsaoPass(const SsaoPass&) = delete;
    SsaoPass& operator=(const SsaoPass&) = delete;

    bool init(rhi::Context& ctx, rhi::Allocator& alloc);
    void shutdown();  // caller makes sure the GPU is idle
    // Records outside rendering: `scene` depth -> DepthRead (left there; the next scene pass transitions it back),
    // AO + blur into the half-res targets (recreated on a size change), then the multiply into the scene colour
    // (color_format = its format). `depth_empty` = the depth clear value (sky / nothing drawn: no AO).
    // strength 0..1 (0 = off), radius in world units. False (nothing applied) on a failed pipeline / allocation.
    bool record(rhi::Frame& frame, VkCommandBuffer cmd, rhi::RenderTarget& scene, VkFormat color_format,
                const ViewMatrices& view, float strength, float radius, float depth_empty, rhi::GpuTimer* timer);

private:
    struct Push;
    VkPipeline pipeline(int which, VkFormat color_format);  // 0 AO, 1 blur, 2 apply
    rhi::Context* ctx_ = nullptr;
    rhi::Allocator* alloc_ = nullptr;
    rhi::PipelineCache cache_;
    rhi::SamplerCache samplers_;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline ao_ = VK_NULL_HANDLE, blur_ = VK_NULL_HANDLE;
    struct Apply {
        VkFormat format;
        VkPipeline pipe;
    };
    std::vector<Apply> apply_;  // per scene colour format (kept until shutdown: frames in flight may use them)
    rhi::RenderTarget ao_a_, ao_b_;  // half res, RG32F (visibility, view distance)
    bool targets_ = false;
};

}  // namespace alamo::render
