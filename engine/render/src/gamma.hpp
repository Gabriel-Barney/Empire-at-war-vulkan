// alamo_render - output gamma on the GPU [port] (internal; Renderer::apply_gamma). The original sets the device gamma
// ramp from Options > Video > Gamma (TERRAIN_WATER_POST.md 4.6: no shader pass found); the port raises every colour
// channel of the finished frame (world + HUD) to 1 / gamma in one full-screen pass, so the slider also covers menus.
// The frame is copied into a scratch image first (the pass reads it while writing the same pixels).
#pragma once
#include <cstdint>

#include "alamo/rhi/rhi.hpp"

namespace alamo::render {

class GammaPass {
public:
    GammaPass() = default;
    ~GammaPass() { shutdown(); }
    GammaPass(const GammaPass&) = delete;
    GammaPass& operator=(const GammaPass&) = delete;

    bool init(rhi::Context& ctx, rhi::Allocator& alloc);
    void shutdown();  // GPU idle
    // `color` (view `view`, `format`, `extent`) is in `state` (TransferSrc or ColorAttachment / ShaderRead ...) and is left
    // in `after`. False (image untouched) when the scratch image, a descriptor or the pipeline is missing.
    bool record(rhi::Frame& frame, VkCommandBuffer cmd, VkImage color, VkImageView view, VkFormat format, VkExtent2D extent,
                rhi::ImageState state, rhi::ImageState after, float exponent);

private:
    bool ensure_scratch(VkFormat format, VkExtent2D ext);
    bool ensure_pipeline(VkFormat format);

    rhi::Context* ctx_ = nullptr;
    rhi::Allocator* alloc_ = nullptr;
    rhi::PipelineCache cache_;
    rhi::SamplerCache samplers_;
    VkDescriptorSetLayout set_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkFormat pipeline_format_ = VK_FORMAT_UNDEFINED;
    rhi::RenderTarget scratch_;
    VkFormat scratch_format_ = VK_FORMAT_UNDEFINED;
};

}  // namespace alamo::render
