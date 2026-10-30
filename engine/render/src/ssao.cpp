// [port] Screen-space ambient occlusion: pipelines, half-res targets and the pass recording (ssao.hpp,
// shaders/ssao.hlsl). Three fullscreen-triangle graphics passes, no vertex input; one descriptor set of two
// combined samplers (scene depth, AO input) and a 96-byte push block per pass.
#include "alamo/render/ssao.hpp"

#include <algorithm>
#include <initializer_list>
#include <span>

#include "alamo/core/log.hpp"
#include "alamo/rhi/context.hpp"
#include "alamo/rhi/descriptors.hpp"
#include "alamo/rhi/timer.hpp"
#include "render_internal.hpp"
#include "ssao_ps_apply.h"
#include "ssao_ps_ao.h"
#include "ssao_ps_blur.h"
#include "ssao_vs.h"

namespace alamo::render {

SsaoProjection ssao_projection(const Mat4& proj) {
    SsaoProjection k;
    k.p00 = proj.m[0][0];
    k.p11 = proj.m[1][1];
    k.p02 = proj.m[0][2];
    k.p12 = proj.m[1][2];
    k.p03 = proj.m[0][3];
    k.p13 = proj.m[1][3];
    k.p22 = proj.m[2][2];
    k.p23 = proj.m[2][3];
    k.p32 = proj.m[3][2];
    k.p33 = proj.m[3][3];
    return k;
}

Float3 ssao_view_position(const SsaoProjection& k, float u, float v, float d) {
    // d = (p22 z + p23) / (p32 z + p33)  ->  z = (p23 - d p33) / (d p32 - p22).
    const float z = (k.p23 - d * k.p33) / (d * k.p32 - k.p22);
    const float w = k.p32 * z + k.p33;
    const float nx = u * 2.0f - 1.0f, ny = 1.0f - v * 2.0f;
    return {(nx * w - k.p02 * z - k.p03) / k.p00, (ny * w - k.p12 * z - k.p13) / k.p11, z};
}

void ssao_project(const SsaoProjection& k, const Float3& p, float& u, float& v, float& d) {
    const float w = k.p32 * p.z + k.p33;
    const float nx = (k.p00 * p.x + k.p02 * p.z + k.p03) / w;
    const float ny = (k.p11 * p.y + k.p12 * p.z + k.p13) / w;
    d = (k.p22 * p.z + k.p23) / w;
    u = (nx + 1.0f) * 0.5f;
    v = (1.0f - ny) * 0.5f;
}

namespace {
// Fixed tuning (render/shaders/ssao.hlsl): cosine bias, AO intensity, blur depth sharpness, max disc radius.
constexpr float kBias = 0.15f;
constexpr float kIntensity = 1.6f;
constexpr float kSharpness = 12.0f;  // blur / upsample: weight 0 at a view-distance difference of 1/12 of the distance
constexpr float kMaxRadiusFraction = 0.25f;  // of the screen height
}  // namespace

struct SsaoPass::Push {
    float proj0[4], proj1[4], proj2[4], params[4], size[4], blur[4];
};

SsaoPass::~SsaoPass() { shutdown(); }

bool SsaoPass::init(rhi::Context& ctx, rhi::Allocator& alloc) {
    shutdown();
    ctx_ = &ctx;
    alloc_ = &alloc;
    samplers_.init(ctx);
    const rhi::DeviceFns& fn = ctx.vkd();
    VkDescriptorSetLayoutBinding b[2]{};
    for (uint32_t i = 0; i < 2; ++i) {
        b[i].binding = i;
        b[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    auto ci = rhi::vk_struct<VkDescriptorSetLayoutCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
    ci.bindingCount = 2;
    ci.pBindings = b;
    if (!cache_.init(ctx) || fn.vkCreateDescriptorSetLayout(ctx.device(), &ci, nullptr, &set_layout_) != VK_SUCCESS) {
        log::errorf("render", "ssao: layout creation failed");
        shutdown();
        return false;
    }
    const VkDescriptorSetLayout sets[1] = {set_layout_};
    layout_ = rhi::create_pipeline_layout(ctx, sets, sizeof(Push), VK_SHADER_STAGE_FRAGMENT_BIT);
    ao_ = pipeline(0, VK_FORMAT_R32G32_SFLOAT);
    blur_ = pipeline(1, VK_FORMAT_R32G32_SFLOAT);
    if (!layout_ || !ao_ || !blur_) {
        log::errorf("render", "ssao: pipeline creation failed");
        shutdown();
        return false;
    }
    return true;
}

void SsaoPass::shutdown() {
    if (!ctx_) return;
    const rhi::DeviceFns& fn = ctx_->vkd();
    const VkDevice dev = ctx_->device();
    for (VkPipeline p : {ao_, blur_})
        if (p) fn.vkDestroyPipeline(dev, p, nullptr);
    for (const Apply& a : apply_)
        if (a.pipe) fn.vkDestroyPipeline(dev, a.pipe, nullptr);
    apply_.clear();
    ao_ = blur_ = VK_NULL_HANDLE;
    if (layout_) fn.vkDestroyPipelineLayout(dev, layout_, nullptr);
    if (set_layout_) fn.vkDestroyDescriptorSetLayout(dev, set_layout_, nullptr);
    layout_ = VK_NULL_HANDLE;
    set_layout_ = VK_NULL_HANDLE;
    ao_a_.shutdown();
    ao_b_.shutdown();
    targets_ = false;
    cache_.shutdown(false);
    samplers_.shutdown();
    ctx_ = nullptr;
    alloc_ = nullptr;
}

VkPipeline SsaoPass::pipeline(int which, VkFormat color_format) {
    const std::vector<uint32_t> vs = rhi::spirv_from_bytes(std::span<const uint8_t>(g_ssao_vs, sizeof g_ssao_vs));
    std::vector<uint32_t> fs;
    if (which == 0) fs = rhi::spirv_from_bytes(std::span<const uint8_t>(g_ssao_ps_ao, sizeof g_ssao_ps_ao));
    else if (which == 1) fs = rhi::spirv_from_bytes(std::span<const uint8_t>(g_ssao_ps_blur, sizeof g_ssao_ps_blur));
    else fs = rhi::spirv_from_bytes(std::span<const uint8_t>(g_ssao_ps_apply, sizeof g_ssao_ps_apply));
    static const char* const kEntry[3] = {"PSAo", "PSBlur", "PSApply"};
    static const char* const kName[3] = {"ssao.ao", "ssao.blur", "ssao.apply"};
    rhi::GraphicsPipelineDesc d;
    d.vs_spirv = vs;
    d.fs_spirv = fs;
    d.vs_entry = "VSFull";
    d.fs_entry = kEntry[which];
    d.raster.cull = VK_CULL_MODE_NONE;
    if (which == 2) {
        // Scene colour x visibility (ZERO / SRC_COLOR), alpha untouched.
        d.blend.enable = true;
        d.blend.src_color = VK_BLEND_FACTOR_ZERO;
        d.blend.dst_color = VK_BLEND_FACTOR_SRC_COLOR;
        d.blend.src_alpha = VK_BLEND_FACTOR_ZERO;
        d.blend.dst_alpha = VK_BLEND_FACTOR_ONE;
        d.blend.write_mask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
    }
    d.color_formats = std::span<const VkFormat>(&color_format, 1);
    d.layout = layout_;
    d.name = kName[which];
    return cache_.create_graphics(d);
}

bool SsaoPass::record(rhi::Frame& frame, VkCommandBuffer cmd, rhi::RenderTarget& scene, VkFormat color_format,
                      const ViewMatrices& view, float strength, float radius, float depth_empty, rhi::GpuTimer* timer) {
    if (!ctx_ || !scene.has_depth() || !scene.has_color() || strength <= 0.0f || radius <= 0.0f) return false;
    const rhi::DeviceFns& fn = ctx_->vkd();
    // Apply pipeline for this colour format.
    VkPipeline apply = VK_NULL_HANDLE;
    for (const Apply& a : apply_)
        if (a.format == color_format) apply = a.pipe;
    if (!apply) {
        apply = pipeline(2, color_format);
        if (!apply) {
            log::errorf("render", "ssao: apply pipeline (format %d) failed", static_cast<int>(color_format));
            return false;
        }
        apply_.push_back({color_format, apply});
    }
    // Half-res targets following the scene size.
    const VkExtent2D ext = scene.extent();
    if (!targets_) {
        rhi::RenderTargetDesc td;
        td.color = rhi::Format::RG32F;
        td.scale = 0.5f;
        td.name = "ssao A";
        targets_ = ao_a_.init(*alloc_, td, ext);
        td.name = "ssao B";
        targets_ = targets_ && ao_b_.init(*alloc_, td, ext);
        if (!targets_) {
            ao_a_.shutdown();
            ao_b_.shutdown();
            log::errorf("render", "ssao: targets (%ux%u) could not be created", ext.width, ext.height);
            return false;
        }
    } else {
        const uint32_t hw = std::max(static_cast<uint32_t>(static_cast<float>(ext.width) * 0.5f), 1u);
        const uint32_t hh = std::max(static_cast<uint32_t>(static_cast<float>(ext.height) * 0.5f), 1u);
        if (ao_a_.extent().width != hw || ao_a_.extent().height != hh) {
            ctx_->wait_idle();  // rare (window resize): earlier frames may still sample the old images
            ao_a_.resize(ext);
            ao_b_.resize(ext);
            if (!ao_a_.has_color() || !ao_b_.has_color()) {
                ao_a_.shutdown();
                ao_b_.shutdown();
                targets_ = false;
                return false;
            }
        }
    }
    const VkExtent2D half = ao_a_.extent();
    rhi::DescriptorAllocator& da = frame.descriptors(0);
    VkDescriptorSet sets[3] = {da.allocate(set_layout_), da.allocate(set_layout_), da.allocate(set_layout_)};
    if (!sets[0] || !sets[1] || !sets[2]) return false;
    const VkSampler point = samplers_.get(rhi::SamplerKey::point_clamp());
    const VkImageView depth_view = scene.depth().view;  // depth aspect only
    // Set n: binding 0 = scene depth, binding 1 = the pass input (AO pass: the depth again, never read).
    const VkImageView inputs[3] = {depth_view, ao_a_.color().view, ao_b_.color().view};
    for (uint32_t s = 0; s < 3; ++s) {
        const VkDescriptorImageInfo ii[2] = {
            {point, depth_view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
            {point, inputs[s], s == 0 ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
        VkWriteDescriptorSet w[2];
        for (uint32_t i = 0; i < 2; ++i) {
            w[i] = rhi::vk_struct<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
            w[i].dstSet = sets[s];
            w[i].dstBinding = i;
            w[i].descriptorCount = 1;
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w[i].pImageInfo = &ii[i];
        }
        fn.vkUpdateDescriptorSets(ctx_->device(), 2, w, 0, nullptr);
    }

    const SsaoProjection k = ssao_projection(view.proj);
    Push pc{};
    pc.proj0[0] = k.p00;
    pc.proj0[1] = k.p11;
    pc.proj0[2] = k.p02;
    pc.proj0[3] = k.p12;
    pc.proj1[0] = k.p03;
    pc.proj1[1] = k.p13;
    pc.proj1[2] = k.p22;
    pc.proj1[3] = k.p23;
    pc.proj2[0] = k.p32;
    pc.proj2[1] = k.p33;
    pc.proj2[2] = depth_empty;
    pc.params[0] = radius;
    pc.params[1] = kIntensity;
    pc.params[2] = kBias;
    pc.params[3] = std::min(strength, 1.0f);
    pc.size[0] = static_cast<float>(ext.width);
    pc.size[1] = static_cast<float>(ext.height);
    pc.size[2] = static_cast<float>(half.width);
    pc.size[3] = static_cast<float>(half.height);
    pc.blur[2] = kSharpness;
    pc.blur[3] = kMaxRadiusFraction * static_cast<float>(ext.height);

    const uint32_t scope = timer ? timer->begin_scope(cmd, "ao") : 0u;
    scene.transition_depth(fn, cmd, rhi::ImageState::DepthRead);
    auto pass = [&](rhi::RenderTarget& dst, VkPipeline pipe, VkDescriptorSet set) {
        dst.begin_rendering(fn, cmd, nullptr);
        fn.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        fn.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &set, 0, nullptr);
        fn.vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
        fn.vkCmdDraw(cmd, 3, 1, 0, 0);
        fn.vkCmdEndRendering(cmd);
        dst.transition_color(fn, cmd, rhi::ImageState::ShaderRead);
    };
    pass(ao_a_, ao_, sets[0]);  // AO -> A
    pc.blur[0] = 1.0f;
    pass(ao_b_, blur_, sets[1]);  // A -> B (horizontal)
    pc.blur[0] = 0.0f;
    pc.blur[1] = 1.0f;
    pass(ao_a_, blur_, sets[2]);  // B -> A (vertical)
    // Multiply into the scene colour (colour attachment only; depth stays readable).
    detail::begin_target(fn, cmd, scene, false, nullptr, false, false);
    fn.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, apply);
    fn.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &sets[1], 0, nullptr);
    fn.vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    fn.vkCmdDraw(cmd, 3, 1, 0, 0);
    fn.vkCmdEndRendering(cmd);
    if (timer) timer->end_scope(cmd, scope);
    return true;
}

}  // namespace alamo::render
