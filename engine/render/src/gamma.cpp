// alamo_render output gamma [port] (src/gamma.hpp, shaders/gamma.hlsl).
#include "gamma.hpp"

#include <array>
#include <cmath>
#include <memory>
#include <vector>

#include "alamo/core/log.hpp"
#include "render_internal.hpp"
#include "gamma_ps.h"
#include "gamma_vs.h"

namespace alamo::render {

namespace {
struct Push {
    float texel[4];
    float params[4];
};
static_assert(sizeof(Push) == 32);
constexpr VkShaderStageFlags kPushStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;  // VS declares the block too
}  // namespace

bool GammaPass::init(rhi::Context& ctx, rhi::Allocator& alloc) {
    shutdown();
    ctx_ = &ctx;
    alloc_ = &alloc;
    samplers_.init(ctx);
    if (!cache_.init(ctx)) return false;
    VkDescriptorSetLayoutBinding b{};
    b.binding = 0;
    b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b.descriptorCount = 1;
    b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    auto ci = rhi::vk_struct<VkDescriptorSetLayoutCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
    ci.bindingCount = 1;
    ci.pBindings = &b;
    if (ctx.vkd().vkCreateDescriptorSetLayout(ctx.device(), &ci, nullptr, &set_) != VK_SUCCESS) return false;
    layout_ = rhi::create_pipeline_layout(ctx, {&set_, 1}, sizeof(Push), kPushStages);
    if (!layout_) {
        log::errorf("render", "gamma pipeline layout could not be created");
        return false;
    }
    return true;
}

void GammaPass::shutdown() {
    if (!ctx_) return;
    const rhi::DeviceFns& fn = ctx_->vkd();
    if (pipeline_) fn.vkDestroyPipeline(ctx_->device(), pipeline_, nullptr);
    pipeline_ = VK_NULL_HANDLE;
    pipeline_format_ = VK_FORMAT_UNDEFINED;
    if (layout_) fn.vkDestroyPipelineLayout(ctx_->device(), layout_, nullptr);
    layout_ = VK_NULL_HANDLE;
    if (set_) fn.vkDestroyDescriptorSetLayout(ctx_->device(), set_, nullptr);
    set_ = VK_NULL_HANDLE;
    scratch_.shutdown();
    scratch_format_ = VK_FORMAT_UNDEFINED;
    cache_.shutdown(false);
    samplers_.shutdown();
    ctx_ = nullptr;
    alloc_ = nullptr;
}

bool GammaPass::ensure_pipeline(VkFormat format) {
    if (pipeline_ && pipeline_format_ == format) return true;
    if (pipeline_) {  // a colour format change happens with the GPU idle (resize / recreated targets)
        ctx_->vkd().vkDestroyPipeline(ctx_->device(), pipeline_, nullptr);
        pipeline_ = VK_NULL_HANDLE;
    }
    const std::vector<uint32_t> vs = rhi::spirv_from_bytes({g_gamma_vs, sizeof(g_gamma_vs)});
    const std::vector<uint32_t> ps = rhi::spirv_from_bytes({g_gamma_ps, sizeof(g_gamma_ps)});
    rhi::GraphicsPipelineDesc d;
    d.vs_spirv = vs;
    d.fs_spirv = ps;
    d.vs_entry = "VSMain";
    d.fs_entry = "PSMain";
    d.raster.cull = VK_CULL_MODE_NONE;
    d.color_formats = {&format, 1};
    d.layout = layout_;
    d.name = "render.gamma";
    pipeline_ = cache_.create_graphics(d);
    pipeline_format_ = pipeline_ ? format : VK_FORMAT_UNDEFINED;
    if (!pipeline_) log::errorf("render", "gamma pipeline could not be created");
    return pipeline_ != VK_NULL_HANDLE;
}

bool GammaPass::ensure_scratch(VkFormat format, VkExtent2D ext) {
    if (scratch_.has_color() && scratch_format_ == format && scratch_.extent().width == ext.width && scratch_.extent().height == ext.height)
        return true;
    if (scratch_.has_color()) ctx_->wait_idle();  // resize on the frame thread; the old image may be in flight
    scratch_.shutdown();
    rhi::RenderTargetDesc d;
    d.color = rhi::from_vk(format);
    d.name = "render.gamma_scratch";
    if (d.color == rhi::Format::Undefined || !scratch_.init(*alloc_, d, ext)) {
        scratch_format_ = VK_FORMAT_UNDEFINED;
        return false;
    }
    scratch_format_ = format;
    return true;
}

bool GammaPass::record(rhi::Frame& frame, VkCommandBuffer cmd, VkImage color, VkImageView view, VkFormat format, VkExtent2D extent,
                       rhi::ImageState state, rhi::ImageState after, float exponent) {
    if (!ctx_ || !color || !view || extent.width == 0 || extent.height == 0) return false;
    if (!ensure_scratch(format, extent) || !ensure_pipeline(format)) return false;
    const VkDescriptorSet set = frame.descriptors(0).allocate(set_);
    const VkSampler point = samplers_.get(rhi::SamplerKey::point_clamp());
    if (!set || !point) return false;
    const rhi::DeviceFns& fn = ctx_->vkd();
    // 1. frame -> scratch (colour left in TransferSrc, scratch in ShaderRead).
    rhi::CopyImage src;
    src.image = color;
    src.extent = extent;
    src.format = format;
    src.before = state;
    src.after = rhi::ImageState::TransferSrc;
    scratch_.copy_from(fn, cmd, src, VK_FILTER_NEAREST, rhi::ImageState::ShaderRead);
    const VkDescriptorImageInfo ii{point, scratch_.color().view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wr = rhi::vk_struct<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
    wr.dstSet = set;
    wr.dstBinding = 0;
    wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr.pImageInfo = &ii;
    fn.vkUpdateDescriptorSets(ctx_->device(), 1, &wr, 0, nullptr);
    // 2. scratch -> frame through pow().
    rhi::cmd_barrier(fn, cmd, rhi::transition(color, rhi::ImageState::TransferSrc, rhi::ImageState::ColorAttachment));
    auto ca = rhi::vk_struct<VkRenderingAttachmentInfo>(VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO);
    ca.imageView = view;
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;  // every pixel is written
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    auto ri = rhi::vk_struct<VkRenderingInfo>(VK_STRUCTURE_TYPE_RENDERING_INFO);
    ri.renderArea = {{0, 0}, extent};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    fn.vkCmdBeginRendering(cmd, &ri);
    VkViewport vp{};
    vp.width = static_cast<float>(extent.width);
    vp.height = static_cast<float>(extent.height);
    vp.maxDepth = 1.0f;
    fn.vkCmdSetViewport(cmd, 0, 1, &vp);
    const VkRect2D sc{{0, 0}, extent};
    fn.vkCmdSetScissor(cmd, 0, 1, &sc);
    Push pc{};
    pc.texel[0] = 1.0f / static_cast<float>(extent.width);
    pc.texel[1] = 1.0f / static_cast<float>(extent.height);
    pc.texel[2] = static_cast<float>(extent.width);
    pc.texel[3] = static_cast<float>(extent.height);
    pc.params[0] = exponent;
    fn.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    fn.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &set, 0, nullptr);
    fn.vkCmdPushConstants(cmd, layout_, kPushStages, 0, sizeof pc, &pc);
    fn.vkCmdDraw(cmd, 3, 1, 0, 0);
    fn.vkCmdEndRendering(cmd);
    if (after != rhi::ImageState::ColorAttachment) rhi::cmd_barrier(fn, cmd, rhi::transition(color, rhi::ImageState::ColorAttachment, after));
    return true;
}

// ------------------------------------------------------------------------------------------- Renderer hook

bool Renderer::apply_gamma(rhi::Frame& frame, VkCommandBuffer cmd, VkImage color, VkImageView view, VkFormat format, VkExtent2D extent,
                           rhi::ImageState state, rhi::ImageState after, float gamma) {
    if (!(gamma >= 0.25f && gamma <= 4.0f) || std::fabs(gamma - 1.0f) < 1e-3f) return true;  // identity: nothing to do
    if (!ctx_) return false;
    if (!gamma_) {
        gamma_ = std::make_unique<GammaPass>();
        if (!gamma_->init(*ctx_, *alloc_)) {
            log::errorf("render", "gamma could not be initialised; frames are presented without it");
            gamma_.reset();
            return false;
        }
    }
    return gamma_->record(frame, cmd, color, view, format, extent, state, after, 1.0f / gamma);
}

}  // namespace alamo::render
