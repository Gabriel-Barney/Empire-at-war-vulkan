// alamo_render - SMAA 1x on the GPU [port] (internal; RecordOptions::smaa). The original has no post-process
// anti-aliasing. Subpixel Morphological Anti-Aliasing (Jimenez et al., third_party/smaa, MIT licence) at one fixed
// quality (the reference HIGH preset), run by Renderer::render() at the end of the post chain - after the TAA resolve
// when both are on - and before the copy out:
//   1. the scene colour is copied into a scratch target of the same size and format (SceneTexture);
//   2. luma edge detection of the copy -> edges (RG8);
//   3. blending weights from the edges and the two lookup textures (area 160 x 560 RG8, search 64 x 16 R8; uploaded with
//      the first frame) -> weights (RGBA8);
//   4. neighbourhood blending of the copy with the weights -> the scene colour.
// Shaders: render/shaders/smaa_pass.hlsl. "smaa" GPU timer scope.
#pragma once
#include <cstdint>
#include <vector>

#include "alamo/rhi/rhi.hpp"

namespace alamo::render {

class SmaaPass {
public:
    SmaaPass() = default;
    ~SmaaPass() { shutdown(); }
    SmaaPass(const SmaaPass&) = delete;
    SmaaPass& operator=(const SmaaPass&) = delete;

    bool init(rhi::Context& ctx, rhi::Allocator& alloc);
    void shutdown();  // GPU idle
    // Records outside rendering. `scene` colour (any tracked state) is anti-aliased in place and left as a colour
    // attachment; `scratch` (same extent and format as the scene colour) receives a copy of it first. False (scene
    // untouched) when the sizes differ or a target, descriptor set, lookup texture or pipeline is missing.
    bool record(VkCommandBuffer cmd, rhi::RenderTarget& scene, rhi::RenderTarget& scratch);

private:
    struct Push;
    VkPipeline pipeline(int which, VkFormat color_format);  // 0 edges, 1 weights, 2 blend
    bool ensure_targets(VkExtent2D ext);
    bool ensure_lookup(const rhi::DeviceFns& fn, VkCommandBuffer cmd);
    void write_sets(VkImageView scratch_view);

    rhi::Context* ctx_ = nullptr;
    rhi::Allocator* alloc_ = nullptr;
    rhi::PipelineCache cache_;
    rhi::SamplerCache samplers_;
    rhi::DescriptorAllocator pool_;  // three long-lived sets (rewritten only when a view changes)
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline edge_pipe_ = VK_NULL_HANDLE, weights_pipe_ = VK_NULL_HANDLE;
    struct Blend {
        VkFormat format;
        VkPipeline pipe;
    };
    std::vector<Blend> blend_;  // per scene colour format (kept until shutdown: frames in flight may use them)
    VkDescriptorSet sets_[3] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};  // edges, weights, blend
    VkImageView set_views_[3] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};  // scratch, edges, weights they hold
    rhi::RenderTarget edges_, weights_;
    rhi::Image area_, search_;
    rhi::Buffer staging_;  // lookup texture bytes; kept until shutdown (the upload is recorded into a frame)
    bool lookup_ready_ = false, lookup_failed_ = false;
};

}  // namespace alamo::render
