// alamo_render temporal anti-aliasing [port] (alamo/render/taa.hpp, src/taa.hpp, shaders/taa.hlsl).
#include "taa.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <span>
#include <vector>

#include "alamo/core/log.hpp"
#include "render_internal.hpp"
#include "taa_ps_depth.h"
#include "taa_ps_resolve.h"
#include "taa_ps_sharpen.h"
#include "taa_vs.h"

namespace alamo::render {

// ------------------------------------------------------------------------------------------- pure rules

float halton(uint32_t index, uint32_t base) {
    float f = 1.0f, r = 0.0f;
    const float inv = 1.0f / static_cast<float>(base);
    while (index > 0) {
        f *= inv;
        r += f * static_cast<float>(index % base);
        index /= base;
    }
    return r;
}

void taa_jitter_pixels(uint64_t frame, float& jx, float& jy) {
    const uint32_t i = static_cast<uint32_t>(frame % kTaaSamples) + 1u;
    jx = halton(i, 2) - 0.5f;
    jy = halton(i, 3) - 0.5f;
}

ViewMatrices jitter_view(const ViewMatrices& v, float jx, float jy) {
    if (jx == 0.0f && jy == 0.0f) return v;
    ViewMatrices j = v;
    const float ox = 2.0f * jx / static_cast<float>(v.width ? v.width : 1u);
    const float oy = 2.0f * jy / static_cast<float>(v.height ? v.height : 1u);
    for (int c = 0; c < 4; ++c) {  // clip.xy += o * clip.w: NDC moves by o for every depth
        j.proj.m[0][c] += ox * v.proj.m[3][c];
        j.proj.m[1][c] += oy * v.proj.m[3][c];
    }
    j.view_proj = j.proj * j.view;
    return j;
}

TaaCamera taa_camera(const ViewMatrices& v) {
    TaaCamera c;
    c.valid = true;
    c.view_proj = v.view_proj;
    c.eye = v.eye;
    c.forward = {-v.view.m[2][0], -v.view.m[2][1], -v.view.m[2][2]};
    c.proj_x = v.proj.m[0][0];
    c.proj_y = v.proj.m[1][1];
    const bool perspective = v.proj.m[3][3] == 0.0f && v.proj.m[3][2] != 0.0f;
    c.far_plane = perspective && v.proj.m[2][2] != -1.0f ? std::fabs(v.proj.m[2][3] / (v.proj.m[2][2] + 1.0f)) : 0.0f;
    c.depth_a = v.proj.m[2][2];
    c.depth_b = v.proj.m[2][3];
    c.perspective = perspective;
    c.width = v.width;
    c.height = v.height;
    return c;
}

float taa_linear_depth(const TaaCamera& c, float ndc) {
    if (!c.valid) return 0.0f;
    if (c.perspective) {
        const float den = ndc + c.depth_a;
        return den != 0.0f ? c.depth_b / den : 0.0f;
    }
    return c.depth_a != 0.0f ? (c.depth_b - ndc) / c.depth_a : 0.0f;
}

bool taa_history_usable(const TaaCamera& a, const TaaCamera& b) {
    if (!a.valid || !b.valid || a.width != b.width || a.height != b.height) return false;
    auto rel = [](float x, float y) { return std::fabs(x - y) > 0.02f * std::max(std::fabs(x), std::fabs(y)); };
    if (rel(a.proj_x, b.proj_x) || rel(a.proj_y, b.proj_y)) return false;
    const float d = a.forward.x * b.forward.x + a.forward.y * b.forward.y + a.forward.z * b.forward.z;
    if (d < 0.98f) return false;
    const float dx = a.eye.x - b.eye.x, dy = a.eye.y - b.eye.y, dz = a.eye.z - b.eye.z;
    const float move = std::sqrt(dx * dx + dy * dy + dz * dz);
    float limit = 0.0f;
    if (a.far_plane > 0.0f && b.far_plane > 0.0f) {
        limit = 0.05f * std::max(a.far_plane, b.far_plane);
    } else {  // orthographic: half extents 1 / scale
        const float ext = std::max(1.0f / std::max(std::fabs(b.proj_x), 1e-12f), 1.0f / std::max(std::fabs(b.proj_y), 1e-12f));
        limit = 0.05f * 2.0f * ext;
    }
    return move <= limit;
}

namespace {

// General 4x4 inverse (cofactors, double precision); false when singular.
bool inverse(const Mat4& a, Mat4& out) {
    double m[16], inv[16];
    for (int i = 0; i < 16; ++i) m[i] = static_cast<double>(a.m[i / 4][i % 4]);
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] -
             m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] +
             m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] -
             m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] +
              m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] +
             m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] -
             m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] +
             m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] -
              m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] -
             m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] +
             m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] -
              m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] +
              m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] +
             m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] -
             m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] +
              m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] -
              m[8] * m[2] * m[5];
    const double det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (det == 0.0 || !std::isfinite(det)) return false;
    for (int i = 0; i < 16; ++i) out.m[i / 4][i % 4] = static_cast<float>(inv[i] / det);
    return true;
}

struct Push {
    float reproj[4][4];
    float texel[4];
    float params[4];
    float depth_cur[4];   // x depth_a, y depth_b, z 1 = perspective (this frame's camera)
    float depth_prev[4];  // the same for the previous frame's camera
};
static_assert(sizeof(Push) == 128);  // the guaranteed push-constant minimum

constexpr VkFormat kHistoryFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kDepthHistoryFormat = VK_FORMAT_R32_SFLOAT;  // linear view distance, written by the DEPTH pass
constexpr VkShaderStageFlags kPushStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;  // VS declares the block too

}  // namespace

// ------------------------------------------------------------------------------------------- GPU pass

bool TaaPass::init(rhi::Context& ctx, rhi::Allocator& alloc) {
    shutdown();
    ctx_ = &ctx;
    alloc_ = &alloc;
    samplers_.init(ctx);
    if (!cache_.init(ctx)) return false;
    const rhi::DeviceFns& fn = ctx.vkd();
    std::array<VkDescriptorSetLayoutBinding, 4> b{};  // resolve: scene colour, scene depth, colour history, depth history
    for (uint32_t i = 0; i < 4; ++i) {
        b[i].binding = i;
        b[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    auto ci = rhi::vk_struct<VkDescriptorSetLayoutCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
    ci.bindingCount = 4;
    ci.pBindings = b.data();
    if (fn.vkCreateDescriptorSetLayout(ctx.device(), &ci, nullptr, &set_resolve_) != VK_SUCCESS) return false;
    ci.bindingCount = 1;
    if (fn.vkCreateDescriptorSetLayout(ctx.device(), &ci, nullptr, &set_sharpen_) != VK_SUCCESS) return false;
    layout_resolve_ = rhi::create_pipeline_layout(ctx, {&set_resolve_, 1}, sizeof(Push), kPushStages);
    layout_sharpen_ = rhi::create_pipeline_layout(ctx, {&set_sharpen_, 1}, sizeof(Push), kPushStages);
    if (!layout_resolve_ || !layout_sharpen_) {
        log::errorf("render", "TAA pipeline layouts could not be created");
        return false;
    }
    history_valid_ = false;
    return true;
}

void TaaPass::destroy_pipelines() {
    if (!ctx_) return;
    for (VkPipeline* p : {&resolve_, &sharpen_, &depth_}) {
        if (*p) ctx_->vkd().vkDestroyPipeline(ctx_->device(), *p, nullptr);
        *p = VK_NULL_HANDLE;
    }
    sharpen_format_ = VK_FORMAT_UNDEFINED;
}

void TaaPass::shutdown() {
    if (!ctx_) return;
    destroy_pipelines();
    const rhi::DeviceFns& fn = ctx_->vkd();
    for (VkPipelineLayout* l : {&layout_resolve_, &layout_sharpen_}) {
        if (*l) fn.vkDestroyPipelineLayout(ctx_->device(), *l, nullptr);
        *l = VK_NULL_HANDLE;
    }
    for (VkDescriptorSetLayout* s : {&set_resolve_, &set_sharpen_}) {
        if (*s) fn.vkDestroyDescriptorSetLayout(ctx_->device(), *s, nullptr);
        *s = VK_NULL_HANDLE;
    }
    for (rhi::RenderTarget& t : history_) t.shutdown();
    for (rhi::RenderTarget& t : depth_history_) t.shutdown();
    cache_.shutdown(false);
    samplers_.shutdown();
    ctx_ = nullptr;
    alloc_ = nullptr;
    history_valid_ = false;
}

bool TaaPass::ensure_pipelines(VkFormat scene_format) {
    if (resolve_ && sharpen_ && depth_ && sharpen_format_ == scene_format) return true;
    // A colour format change (Renderer::set_color_format) happens with the GPU idle, so the old pipelines are unused.
    destroy_pipelines();
    const std::vector<uint32_t> vs = rhi::spirv_from_bytes({g_taa_vs, sizeof(g_taa_vs)});
    const std::vector<uint32_t> ps_r = rhi::spirv_from_bytes({g_taa_ps_resolve, sizeof(g_taa_ps_resolve)});
    const std::vector<uint32_t> ps_s = rhi::spirv_from_bytes({g_taa_ps_sharpen, sizeof(g_taa_ps_sharpen)});
    const std::vector<uint32_t> ps_d = rhi::spirv_from_bytes({g_taa_ps_depth, sizeof(g_taa_ps_depth)});
    auto make = [&](const std::vector<uint32_t>& ps, const char* entry, VkFormat format, VkPipelineLayout layout, const char* name) {
        rhi::GraphicsPipelineDesc d;
        d.vs_spirv = vs;
        d.fs_spirv = ps;
        d.vs_entry = "VSMain";
        d.fs_entry = entry;
        d.raster.cull = VK_CULL_MODE_NONE;
        d.color_formats = {&format, 1};
        d.layout = layout;
        d.name = name;
        return cache_.create_graphics(d);
    };
    resolve_ = make(ps_r, "PSResolve", kHistoryFormat, layout_resolve_, "render.taa_resolve");
    sharpen_ = make(ps_s, "PSSharpen", scene_format, layout_sharpen_, "render.taa_sharpen");
    depth_ = make(ps_d, "PSDepth", kDepthHistoryFormat, layout_sharpen_, "render.taa_depth");
    sharpen_format_ = scene_format;
    if (!resolve_ || !sharpen_ || !depth_) {
        log::errorf("render", "TAA pipelines could not be created");
        destroy_pipelines();
        return false;
    }
    return true;
}

bool TaaPass::ensure_history(VkExtent2D ext) {
    if (history_[0].has_color() && history_[0].extent().width == ext.width && history_[0].extent().height == ext.height)
        return true;
    if (history_[0].has_color()) ctx_->wait_idle();  // render() resizes on the frame thread; old images may be in flight
    for (int i = 0; i < 2; ++i) {
        history_[i].shutdown();
        depth_history_[i].shutdown();
        rhi::RenderTargetDesc d;
        d.color = rhi::from_vk(kHistoryFormat);
        d.name = i == 0 ? "render.taa_history0" : "render.taa_history1";
        rhi::RenderTargetDesc dd;
        dd.color = rhi::from_vk(kDepthHistoryFormat);
        dd.name = i == 0 ? "render.taa_depth0" : "render.taa_depth1";
        if (!history_[i].init(*alloc_, d, ext) || !depth_history_[i].init(*alloc_, dd, ext)) {
            log::errorf("render", "TAA history (%ux%u) could not be created", ext.width, ext.height);
            for (int k = 0; k < 2; ++k) {
                history_[k].shutdown();
                depth_history_[k].shutdown();
            }
            return false;
        }
    }
    history_valid_ = false;  // resize: start over
    return true;
}

ViewMatrices TaaPass::begin_frame(const ViewMatrices& view) {
    float jx = 0, jy = 0;
    taa_jitter_pixels(frame_++, jx, jy);
    const ViewMatrices j = jitter_view(view, jx, jy);
    cam_ = taa_camera(view);
    jittered_vp_ = j.view_proj;
    reset_now_ = !history_valid_ || !taa_history_usable(prev_, cam_);
    return j;
}

bool TaaPass::resolve(rhi::Frame& frame, VkCommandBuffer cmd, rhi::RenderTarget& scene, float sharpen, float weight) {
    history_valid_ = false;  // set again below once the new history is written
    if (!scene.has_color() || !scene.has_depth()) return false;
    const VkExtent2D ext = scene.extent();
    if (!ensure_history(ext)) return false;
    if (history_[0].extent().width != ext.width || history_[0].extent().height != ext.height) return false;
    if (!ensure_pipelines(scene.color().vk_format())) return false;
    // ensure_history may just have invalidated the history (resize).
    const rhi::RenderTarget& prev_colour = history_[cur_ ^ 1u];
    const rhi::RenderTarget& prev_depth = depth_history_[cur_ ^ 1u];
    const bool reset = reset_now_ || !prev_colour.has_color() || prev_colour.color_state() != rhi::ImageState::ShaderRead ||
                       !prev_depth.has_color() || prev_depth.color_state() != rhi::ImageState::ShaderRead;
    reset_now_ = reset;
    Push pc{};
    Mat4 inv;
    if (!reset && inverse(jittered_vp_, inv)) {
        const Mat4 r = prev_.view_proj * inv;
        for (int i = 0; i < 4; ++i)
            for (int k = 0; k < 4; ++k) pc.reproj[i][k] = r.m[i][k];
    } else {
        reset_now_ = true;
    }
    pc.depth_cur[0] = cam_.depth_a;
    pc.depth_cur[1] = cam_.depth_b;
    pc.depth_cur[2] = cam_.perspective ? 1.0f : 0.0f;
    pc.depth_prev[0] = prev_.depth_a;
    pc.depth_prev[1] = prev_.depth_b;
    pc.depth_prev[2] = prev_.perspective ? 1.0f : 0.0f;
    const float w = static_cast<float>(ext.width), h = static_cast<float>(ext.height);
    pc.texel[0] = 1.0f / w;
    pc.texel[1] = 1.0f / h;
    pc.texel[2] = w;
    pc.texel[3] = h;
    pc.params[0] = std::clamp(weight, 0.02f, 1.0f);
    pc.params[1] = reset_now_ ? 1.0f : 0.0f;
    pc.params[2] = std::clamp(sharpen, 0.0f, 1.0f);

    const rhi::DeviceFns& fn = ctx_->vkd();
    rhi::RenderTarget& dst = history_[cur_];
    rhi::RenderTarget& src = history_[cur_ ^ 1u];
    rhi::RenderTarget& depth_dst = depth_history_[cur_];
    rhi::RenderTarget& depth_src = depth_history_[cur_ ^ 1u];
    scene.transition_color(fn, cmd, rhi::ImageState::ShaderRead);
    scene.transition_depth(fn, cmd, rhi::ImageState::DepthRead);
    const VkSampler point = samplers_.get(rhi::SamplerKey::point_clamp());
    const VkSampler linear = samplers_.get(rhi::SamplerKey::linear_clamp());
    const VkDescriptorSet rs = frame.descriptors(0).allocate(set_resolve_);
    const VkDescriptorSet ss = frame.descriptors(0).allocate(set_sharpen_);
    const VkDescriptorSet ds = frame.descriptors(0).allocate(set_sharpen_);  // same layout: one sampled image
    if (!rs || !ss || !ds || !point || !linear) return false;
    {
        // On a reset the history slots show the scene colour (unread: the shader takes the current colour only).
        const VkImageView prev_c = reset_now_ ? scene.color().view : src.color().view;
        const VkImageView prev_d = reset_now_ ? scene.color().view : depth_src.color().view;
        const VkDescriptorImageInfo ii[4] = {
            {point, scene.color().view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {point, scene.depth().view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
            {linear, prev_c, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {point, prev_d, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
        const VkDescriptorImageInfo si[2] = {{point, dst.color().view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                             {point, scene.depth().view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL}};
        VkWriteDescriptorSet wr[6];
        for (uint32_t i = 0; i < 6; ++i) {
            wr[i] = rhi::vk_struct<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
            wr[i].descriptorCount = 1;
            wr[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            if (i < 4) {
                wr[i].dstSet = rs;
                wr[i].dstBinding = i;
                wr[i].pImageInfo = &ii[i];
            } else {
                wr[i].dstSet = i == 4 ? ss : ds;
                wr[i].dstBinding = 0;
                wr[i].pImageInfo = &si[i - 4];
            }
        }
        fn.vkUpdateDescriptorSets(ctx_->device(), 6, wr, 0, nullptr);
    }
    // 1. Resolve -> history[cur].
    const rhi::ClearValues clear{};
    dst.begin_rendering(fn, cmd, &clear);
    fn.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, resolve_);
    fn.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_resolve_, 0, 1, &rs, 0, nullptr);
    fn.vkCmdPushConstants(cmd, layout_resolve_, kPushStages, 0, sizeof pc, &pc);
    fn.vkCmdDraw(cmd, 3, 1, 0, 0);
    dst.end_rendering(fn, cmd);
    dst.transition_color(fn, cmd, rhi::ImageState::ShaderRead);
    // 2. This frame's linear scene depth -> depth_history[cur] (the next frame's disocclusion test).
    depth_dst.begin_rendering(fn, cmd, &clear);
    fn.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, depth_);
    fn.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_sharpen_, 0, 1, &ds, 0, nullptr);
    fn.vkCmdPushConstants(cmd, layout_sharpen_, kPushStages, 0, sizeof pc, &pc);
    fn.vkCmdDraw(cmd, 3, 1, 0, 0);
    depth_dst.end_rendering(fn, cmd);
    depth_dst.transition_color(fn, cmd, rhi::ImageState::ShaderRead);
    // 3. Sharpen history[cur] -> scene colour (depth back to an attachment for the next frame's first pass).
    scene.transition_depth(fn, cmd, rhi::ImageState::DepthAttachment);
    const float black[4] = {0, 0, 0, 0};
    detail::begin_target(fn, cmd, scene, false, black, false, false);
    fn.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, sharpen_);
    fn.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_sharpen_, 0, 1, &ss, 0, nullptr);
    fn.vkCmdPushConstants(cmd, layout_sharpen_, kPushStages, 0, sizeof pc, &pc);
    fn.vkCmdDraw(cmd, 3, 1, 0, 0);
    fn.vkCmdEndRendering(cmd);
    prev_ = cam_;
    cur_ ^= 1u;
    history_valid_ = true;
    return true;
}

// ------------------------------------------------------------------------------------------- Renderer hooks

ViewMatrices Renderer::taa_begin(const ViewMatrices& view, const RecordOptions& opts) {
    if (!opts.taa) {
        if (taa_) taa_->reset();  // the history is stale once a frame went without it (mode switch)
        return view;
    }
    if (!taa_) {
        taa_ = std::make_unique<TaaPass>();
        if (!taa_->init(*ctx_, *alloc_)) {
            log::errorf("render", "TAA could not be initialised; frames render without it");
            taa_.reset();
            return view;
        }
    }
    return taa_->begin_frame(view);
}

void Renderer::reset_taa_history() {
    if (taa_) taa_->reset();
}

}  // namespace alamo::render
