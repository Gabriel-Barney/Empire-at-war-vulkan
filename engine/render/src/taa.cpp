// alamo_render temporal anti-aliasing [port] (alamo/render/taa.hpp, src/taa.hpp, shaders/taa.hlsl).
#include "taa.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <span>
#include <vector>

#include "alamo/core/log.hpp"
#include "alamo/render/upscale.hpp"
#include "render_internal.hpp"
#include "fsr.hpp"
#include "xess.hpp"
#include "taa_ps_depth.h"
#include "taa_ps_motion.h"
#include "taa_ps_resolve.h"
#include "taa_ps_sharpen.h"
#include "taa_vel_ps.h"
#include "taa_vel_vs.h"
#include "taa_vel_vs_skin.h"
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

struct VelocityPush {  // taa_velocity.hlsl VelPush
    float prev_clip[4][4];
    uint32_t draw_index;
    float pixel_x, pixel_y;
    uint32_t pad;
};
static_assert(sizeof(VelocityPush) == 80);
constexpr VkFormat kVelocityFormat = VK_FORMAT_R32G32B32A32_SFLOAT;  // previous uv, previous NDC depth, valid

constexpr VkFormat kHistoryFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kFsrMotionFormat = VK_FORMAT_R32G32_SFLOAT;  // [port] FSR motion vectors (the RHI has no RG16F)
constexpr VkFormat kFsrDepthFormat = VK_FORMAT_R32_SFLOAT;      // [port] FSR depth copy
constexpr VkFormat kDepthHistoryFormat = VK_FORMAT_R32_SFLOAT;  // linear view distance, written by the DEPTH pass
constexpr VkShaderStageFlags kPushStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;  // VS declares the block too

}  // namespace

// ------------------------------------------------------------------------------------------- GPU pass

bool TaaPass::init(rhi::Context& ctx, rhi::Allocator& alloc, VkDescriptorSetLayout hot_set0) {
    shutdown();
    ctx_ = &ctx;
    alloc_ = &alloc;
    hot_set0_ = hot_set0;
    samplers_.init(ctx);
    if (!cache_.init(ctx)) return false;
    const rhi::DeviceFns& fn = ctx.vkd();
    std::array<VkDescriptorSetLayoutBinding, 5> b{};  // resolve: scene colour, scene depth, colour history, depth history, velocity
    for (uint32_t i = 0; i < 5; ++i) {
        b[i].binding = i;
        b[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    auto ci = rhi::vk_struct<VkDescriptorSetLayoutCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
    ci.bindingCount = 5;
    ci.pBindings = b.data();
    if (fn.vkCreateDescriptorSetLayout(ctx.device(), &ci, nullptr, &set_resolve_) != VK_SUCCESS) return false;
    ci.bindingCount = 1;
    if (fn.vkCreateDescriptorSetLayout(ctx.device(), &ci, nullptr, &set_sharpen_) != VK_SUCCESS) return false;
    ci.bindingCount = 2;  // [port] FSR motion pass: scene depth, velocity
    if (fn.vkCreateDescriptorSetLayout(ctx.device(), &ci, nullptr, &set_motion_) != VK_SUCCESS) return false;
    layout_resolve_ = rhi::create_pipeline_layout(ctx, {&set_resolve_, 1}, sizeof(Push), kPushStages);
    layout_sharpen_ = rhi::create_pipeline_layout(ctx, {&set_sharpen_, 1}, sizeof(Push), kPushStages);
    layout_motion_ = rhi::create_pipeline_layout(ctx, {&set_motion_, 1}, sizeof(Push), kPushStages);
    layout_velocity_ = hot_set0_ ? rhi::create_pipeline_layout(ctx, {&hot_set0_, 1}, sizeof(VelocityPush), VK_SHADER_STAGE_VERTEX_BIT)
                                 : VK_NULL_HANDLE;
    if (!layout_resolve_ || !layout_sharpen_ || !layout_velocity_ || !layout_motion_) {
        log::errorf("render", "TAA pipeline layouts could not be created");
        return false;
    }
    history_valid_ = false;
    return true;
}

void TaaPass::destroy_pipelines() {
    if (!ctx_) return;
    for (VkPipeline* p : {&resolve_, &sharpen_, &depth_, &motion_pipe_, &velocity_pipe_[0], &velocity_pipe_[1]}) {
        if (*p) ctx_->vkd().vkDestroyPipeline(ctx_->device(), *p, nullptr);
        *p = VK_NULL_HANDLE;
    }
    sharpen_format_ = VK_FORMAT_UNDEFINED;
    velocity_depth_format_ = VK_FORMAT_UNDEFINED;
}

void TaaPass::shutdown() {
    if (!ctx_) return;
    destroy_pipelines();
    const rhi::DeviceFns& fn = ctx_->vkd();
    for (VkPipelineLayout* l : {&layout_resolve_, &layout_sharpen_, &layout_velocity_, &layout_motion_}) {
        if (*l) fn.vkDestroyPipelineLayout(ctx_->device(), *l, nullptr);
        *l = VK_NULL_HANDLE;
    }
    for (VkDescriptorSetLayout* s : {&set_resolve_, &set_sharpen_, &set_motion_}) {
        if (*s) fn.vkDestroyDescriptorSetLayout(ctx_->device(), *s, nullptr);
        *s = VK_NULL_HANDLE;
    }
    for (rhi::RenderTarget& t : history_) t.shutdown();
    for (rhi::RenderTarget& t : depth_history_) t.shutdown();
    velocity_.shutdown();
    fsr_motion_.shutdown();
    fsr_depth_.shutdown();
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

bool TaaPass::ensure_velocity_pipelines(VkFormat depth_format) {
    if (velocity_pipe_[0] && velocity_pipe_[1] && velocity_depth_format_ == depth_format) return true;
    for (VkPipeline& p : velocity_pipe_) {
        if (p) ctx_->vkd().vkDestroyPipeline(ctx_->device(), p, nullptr);  // the scene depth format changed with the GPU idle
        p = VK_NULL_HANDLE;
    }
    const std::vector<uint32_t> vs[2] = {rhi::spirv_from_bytes({g_taa_vel_vs, sizeof(g_taa_vel_vs)}),
                                         rhi::spirv_from_bytes({g_taa_vel_vs_skin, sizeof(g_taa_vel_vs_skin)})};
    const std::vector<uint32_t> ps = rhi::spirv_from_bytes({g_taa_vel_ps, sizeof(g_taa_vel_ps)});
    // Static layout: position at 0 (stride 64); rigid-skin layout: + NORMAL (float4, .w = palette index) at 12.
    const rhi::VertexBinding binding{0, 64, false};
    const rhi::VertexAttribute attrs[2] = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0}, {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 12}};
    const VkFormat color = kVelocityFormat;
    for (int k = 0; k < 2; ++k) {
        rhi::GraphicsPipelineDesc d;
        d.vs_spirv = vs[k];
        d.fs_spirv = ps;
        d.vs_entry = "VSMain";
        d.fs_entry = "PSMain";
        d.bindings = {&binding, 1};
        d.attributes = {attrs, k == 0 ? 1u : 2u};
        d.raster.cull = VK_CULL_MODE_NONE;
        d.depth.depth_test = true;  // against the finished scene depth, never written
        d.depth.depth_write = false;
        d.depth.depth_op = VK_COMPARE_OP_LESS_OR_EQUAL;
        d.color_formats = {&color, 1};
        d.depth_format = depth_format;
        d.layout = layout_velocity_;
        d.name = k == 0 ? "render.taa_velocity" : "render.taa_velocity_skin";
        velocity_pipe_[k] = cache_.create_graphics(d);
    }
    velocity_depth_format_ = depth_format;
    if (!velocity_pipe_[0] || !velocity_pipe_[1]) {
        log::errorf("render", "TAA velocity pipelines could not be created; movers fall back to camera motion");
        for (VkPipeline& p : velocity_pipe_) {
            if (p) ctx_->vkd().vkDestroyPipeline(ctx_->device(), p, nullptr);
            p = VK_NULL_HANDLE;
        }
        velocity_depth_format_ = VK_FORMAT_UNDEFINED;
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
    velocity_.shutdown();
    rhi::RenderTargetDesc vd;
    vd.color = rhi::from_vk(kVelocityFormat);
    vd.name = "render.taa_velocity";
    if (!velocity_.init(*alloc_, vd, ext)) {
        log::errorf("render", "TAA velocity target (%ux%u) could not be created", ext.width, ext.height);
        for (int k = 0; k < 2; ++k) {
            history_[k].shutdown();
            depth_history_[k].shutdown();
        }
        return false;
    }
    history_valid_ = false;  // resize: start over
    ++history_generation_;
    return true;
}

ViewMatrices TaaPass::begin_frame(const ViewMatrices& view, bool provisional) {
    undo_.armed = provisional;
    undo_.motion_saved = false;
    if (provisional) {
        undo_.cur = cur_;
        undo_.frame = frame_;
        undo_.generation = history_generation_;
        undo_.history_valid = history_valid_;
        undo_.reset_now = reset_now_;
        undo_.prev = prev_;
        undo_.cam = cam_;
        undo_.jittered_vp = jittered_vp_;
        undo_.last_movers = last_movers_;
    }
    float jx = 0, jy = 0;
    ViewMatrices j;
    if (fsr_phases_ > 0) {  // [port] FSR: its sequence; pixel space y down, render()'s NDC y up
        upscale_jitter_pixels(frame_++, fsr_phases_, jx, jy);
        fsr_jx_ = jx;
        fsr_jy_ = jy;
        j = jitter_view(view, jx, -jy);
    } else {
        taa_jitter_pixels(frame_++, jx, jy);
        fsr_jx_ = fsr_jy_ = 0.0f;
        j = jitter_view(view, jx, jy);
    }
    cam_ = taa_camera(view);
    jittered_vp_ = j.view_proj;
    reset_now_ = !history_valid_ || !taa_history_usable(prev_, cam_);
    return j;
}

bool TaaPass::motion_delta(uint64_t motion_id, const Mat3x4& cur, Mat3x4& delta) const {
    if (!motion_id) return false;
    const auto it = motion_.find(motion_id);
    if (it == motion_.end() || it->second.frame + 1 != frame_) return false;  // not seen in the previous TAA frame
    const Mat3x4& prev = it->second.world;
    bool moved = false;
    for (int r = 0; r < 3 && !moved; ++r)
        for (int c = 0; c < 4; ++c) moved = moved || prev.m[r][c] != cur.m[r][c];
    if (!moved) return false;
    delta = prev * cur.inverse();
    return true;
}

bool TaaPass::discard() {
    if (!undo_.armed) return false;
    undo_.armed = false;
    cur_ = undo_.cur;
    frame_ = undo_.frame;
    // Targets recreated inside the frame (resize) hold nothing of the old history.
    history_valid_ = undo_.history_valid && undo_.generation == history_generation_;
    reset_now_ = undo_.reset_now;
    prev_ = undo_.prev;
    cam_ = undo_.cam;
    jittered_vp_ = undo_.jittered_vp;
    last_movers_ = undo_.last_movers;
    if (undo_.motion_saved) {
        motion_ = std::move(undo_.motion);
        undo_.motion.clear();
        undo_.motion_saved = false;
    }
    return true;
}

void TaaPass::commit_motion(std::span<const Instance> instances) {
    if (undo_.armed && !undo_.motion_saved) {  // provisional: set the previous records aside (the sweep below would rebuild
        undo_.motion = std::move(motion_);     // exactly this frame's keyed instances from an empty map anyway)
        motion_.clear();
        undo_.motion_saved = true;
    }
    for (const Instance& in : instances)
        if (in.motion_id) motion_[in.motion_id] = MotionRec{in.transform, frame_};
    for (auto it = motion_.begin(); it != motion_.end();) it = it->second.frame != frame_ ? motion_.erase(it) : std::next(it);
}

// The movers' velocity pass: depth-tested against the finished scene depth (sampled state, read-only attachment),
// RGBA32F target cleared to "no vector".
void TaaPass::record_velocity(const rhi::DeviceFns& fn, VkCommandBuffer cmd, rhi::RenderTarget& scene, VkDescriptorSet hot_set0) {
    velocity_.transition_color(fn, cmd, rhi::ImageState::ColorAttachment);
    auto ca = rhi::vk_struct<VkRenderingAttachmentInfo>(VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO);
    ca.imageView = velocity_.color().attachment_view ? velocity_.color().attachment_view : velocity_.color().view;
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ca.clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
    auto da = rhi::vk_struct<VkRenderingAttachmentInfo>(VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO);
    da.imageView = scene.depth().attachment_view ? scene.depth().attachment_view : scene.depth().view;
    da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    da.storeOp = VK_ATTACHMENT_STORE_OP_NONE;
    auto ri = rhi::vk_struct<VkRenderingInfo>(VK_STRUCTURE_TYPE_RENDERING_INFO);
    ri.renderArea = {{0, 0}, scene.extent()};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    ri.pDepthAttachment = &da;
    fn.vkCmdBeginRendering(cmd, &ri);
    VkViewport vp{};  // the scene's Y-flipped viewport
    vp.y = static_cast<float>(scene.extent().height);
    vp.width = static_cast<float>(scene.extent().width);
    vp.height = -static_cast<float>(scene.extent().height);
    vp.maxDepth = 1.0f;
    fn.vkCmdSetViewport(cmd, 0, 1, &vp);
    const VkRect2D sc{{0, 0}, scene.extent()};
    fn.vkCmdSetScissor(cmd, 0, 1, &sc);
    fn.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_velocity_, 0, 1, &hot_set0, 0, nullptr);
    const float px = 1.0f / static_cast<float>(scene.extent().width), py = -1.0f / static_cast<float>(scene.extent().height);
    VkPipeline bound = VK_NULL_HANDLE;
    for (const TaaMover& m : movers_) {
        const VkPipeline pipe = velocity_pipe_[m.skinned ? 1 : 0];
        if (pipe != bound) {
            fn.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
            bound = pipe;
        }
        VelocityPush vpp{};
        std::memcpy(vpp.prev_clip, m.prev_clip, sizeof vpp.prev_clip);
        vpp.draw_index = m.draw_index;
        vpp.pixel_x = m.translated ? px : 0.0f;
        vpp.pixel_y = m.translated ? py : 0.0f;
        fn.vkCmdPushConstants(cmd, layout_velocity_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof vpp, &vpp);
        const VkDeviceSize zero = 0;
        fn.vkCmdBindVertexBuffers(cmd, 0, 1, &m.vertices, &zero);
        fn.vkCmdBindIndexBuffer(cmd, m.indices, 0, m.index_type);
        fn.vkCmdDrawIndexed(cmd, m.index_count, 1, 0, 0, 0);
    }
    fn.vkCmdEndRendering(cmd);
    velocity_.transition_color(fn, cmd, rhi::ImageState::ShaderRead);
}

bool TaaPass::resolve(rhi::Frame& frame, VkCommandBuffer cmd, rhi::RenderTarget& scene, float sharpen, float weight,
                      VkDescriptorSet hot_set0) {
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
    const bool velocity = !reset_now_ && !movers_.empty() && hot_set0 && ensure_velocity_pipelines(scene.depth().vk_format());
    pc.params[3] = velocity ? 1.0f : 0.0f;
    last_movers_ = velocity ? static_cast<uint32_t>(movers_.size()) : 0u;

    const rhi::DeviceFns& fn = ctx_->vkd();
    rhi::RenderTarget& dst = history_[cur_];
    rhi::RenderTarget& src = history_[cur_ ^ 1u];
    rhi::RenderTarget& depth_dst = depth_history_[cur_];
    rhi::RenderTarget& depth_src = depth_history_[cur_ ^ 1u];
    scene.transition_color(fn, cmd, rhi::ImageState::ShaderRead);
    scene.transition_depth(fn, cmd, rhi::ImageState::DepthRead);
    if (velocity) record_velocity(fn, cmd, scene, hot_set0);
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
        const VkDescriptorImageInfo ii[5] = {
            {point, scene.color().view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {point, scene.depth().view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
            {linear, prev_c, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {point, prev_d, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {point, velocity ? velocity_.color().view : scene.color().view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
        const VkDescriptorImageInfo si[2] = {{point, dst.color().view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                             {point, scene.depth().view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL}};
        VkWriteDescriptorSet wr[7];
        for (uint32_t i = 0; i < 7; ++i) {
            wr[i] = rhi::vk_struct<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
            wr[i].descriptorCount = 1;
            wr[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            if (i < 5) {
                wr[i].dstSet = rs;
                wr[i].dstBinding = i;
                wr[i].pImageInfo = &ii[i];
            } else {
                wr[i].dstSet = i == 5 ? ss : ds;
                wr[i].dstBinding = 0;
                wr[i].pImageInfo = &si[i - 5];
            }
        }
        fn.vkUpdateDescriptorSets(ctx_->device(), 7, wr, 0, nullptr);
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

// ------------------------------------------------------------------------------------------- [port] FSR inputs

bool TaaPass::ensure_fsr_targets(VkExtent2D ext) {
    auto same = [&](const rhi::RenderTarget& t) {
        return t.has_color() && t.extent().width == ext.width && t.extent().height == ext.height;
    };
    if (same(fsr_motion_) && same(fsr_depth_) && same(velocity_)) return true;
    if (fsr_motion_.has_color() || velocity_.has_color()) ctx_->wait_idle();  // resize: old images may be in flight
    struct Spec {
        rhi::RenderTarget* t;
        VkFormat f;
        const char* name;
    };
    const Spec specs[3] = {{&fsr_motion_, kFsrMotionFormat, "render.fsr_motion"},
                           {&fsr_depth_, kFsrDepthFormat, "render.fsr_depth"},
                           {&velocity_, kVelocityFormat, "render.taa_velocity"}};
    for (const Spec& sp : specs) {
        sp.t->shutdown();
        rhi::RenderTargetDesc d;
        d.color = rhi::from_vk(sp.f);
        d.name = sp.name;
        if (!sp.t->init(*alloc_, d, ext)) {
            log::errorf("render", "FSR input target %s (%ux%u) could not be created", sp.name, ext.width, ext.height);
            fsr_motion_.shutdown();
            fsr_depth_.shutdown();
            velocity_.shutdown();
            return false;
        }
    }
    // The TAA history (other mode) no longer pairs with velocity_: a later TAA frame recreates it (ensure_history).
    for (rhi::RenderTarget& t : history_) t.shutdown();
    for (rhi::RenderTarget& t : depth_history_) t.shutdown();
    history_valid_ = false;  // resize: the previous camera no longer matches
    ++history_generation_;
    return true;
}

bool TaaPass::ensure_motion_pipeline() {
    if (motion_pipe_) return true;
    const std::vector<uint32_t> vs = rhi::spirv_from_bytes({g_taa_vs, sizeof(g_taa_vs)});
    const std::vector<uint32_t> ps = rhi::spirv_from_bytes({g_taa_ps_motion, sizeof(g_taa_ps_motion)});
    const VkFormat formats[2] = {kFsrMotionFormat, kFsrDepthFormat};
    rhi::GraphicsPipelineDesc d;
    d.vs_spirv = vs;
    d.fs_spirv = ps;
    d.vs_entry = "VSMain";
    d.fs_entry = "PSMotion";
    d.raster.cull = VK_CULL_MODE_NONE;
    d.color_formats = {formats, 2};
    d.layout = layout_motion_;
    d.name = "render.fsr_motion";
    motion_pipe_ = cache_.create_graphics(d);
    if (!motion_pipe_) log::errorf("render", "FSR motion-vector pipeline could not be created");
    return motion_pipe_ != VK_NULL_HANDLE;
}

bool TaaPass::fsr_inputs(rhi::Frame& frame, VkCommandBuffer cmd, rhi::RenderTarget& scene, VkDescriptorSet hot_set0) {
    history_valid_ = false;  // set again below once this frame's camera is the previous one
    if (!scene.has_color() || !scene.has_depth()) return false;
    const VkExtent2D ext = scene.extent();
    if (!ensure_fsr_targets(ext) || !ensure_motion_pipeline()) return false;
    Push pc{};
    Mat4 inv;
    if (!reset_now_ && inverse(jittered_vp_, inv)) {
        const Mat4 r = prev_.view_proj * inv;
        for (int i = 0; i < 4; ++i)
            for (int k = 0; k < 4; ++k) pc.reproj[i][k] = r.m[i][k];
    } else {
        reset_now_ = true;
    }
    const float w = static_cast<float>(ext.width), h = static_cast<float>(ext.height);
    pc.texel[0] = 1.0f / w;
    pc.texel[1] = 1.0f / h;
    pc.texel[2] = w;
    pc.texel[3] = h;
    // Jittered uv - un-jittered uv: the projection moved by (jx, -jy) pixels in y-up NDC = (jx, jy) pixels in y-down uv.
    pc.params[0] = fsr_jx_ / w;
    pc.params[1] = fsr_jy_ / h;
    pc.params[2] = reset_now_ ? 1.0f : 0.0f;
    const bool velocity = !reset_now_ && !movers_.empty() && hot_set0 && ensure_velocity_pipelines(scene.depth().vk_format());
    pc.params[3] = velocity ? 1.0f : 0.0f;
    last_movers_ = velocity ? static_cast<uint32_t>(movers_.size()) : 0u;

    const rhi::DeviceFns& fn = ctx_->vkd();
    scene.transition_depth(fn, cmd, rhi::ImageState::DepthRead);
    if (velocity) record_velocity(fn, cmd, scene, hot_set0);
    const VkSampler point = samplers_.get(rhi::SamplerKey::point_clamp());
    const VkDescriptorSet ms = frame.descriptors(0).allocate(set_motion_);
    if (!ms || !point) return false;
    {
        // Without movers the velocity slot is never read (Params.w 0); the scene depth stands in for it.
        const VkDescriptorImageInfo ii[2] = {
            {point, scene.depth().view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
            {point, velocity ? velocity_.color().view : scene.depth().view,
             velocity ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL}};
        VkWriteDescriptorSet wr[2];
        for (uint32_t i = 0; i < 2; ++i) {
            wr[i] = rhi::vk_struct<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
            wr[i].dstSet = ms;
            wr[i].dstBinding = i;
            wr[i].descriptorCount = 1;
            wr[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            wr[i].pImageInfo = &ii[i];
        }
        fn.vkUpdateDescriptorSets(ctx_->device(), 2, wr, 0, nullptr);
    }
    fsr_motion_.transition_color(fn, cmd, rhi::ImageState::ColorAttachment);
    fsr_depth_.transition_color(fn, cmd, rhi::ImageState::ColorAttachment);
    VkRenderingAttachmentInfo ca[2];
    rhi::RenderTarget* outs[2] = {&fsr_motion_, &fsr_depth_};
    for (int i = 0; i < 2; ++i) {
        ca[i] = rhi::vk_struct<VkRenderingAttachmentInfo>(VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO);
        const rhi::Image& im = outs[i]->color();
        ca[i].imageView = im.attachment_view ? im.attachment_view : im.view;
        ca[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;  // every pixel is written
        ca[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    auto ri = rhi::vk_struct<VkRenderingInfo>(VK_STRUCTURE_TYPE_RENDERING_INFO);
    ri.renderArea = {{0, 0}, ext};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 2;
    ri.pColorAttachments = ca;
    fn.vkCmdBeginRendering(cmd, &ri);
    VkViewport vp{};
    vp.width = w;
    vp.height = h;
    vp.maxDepth = 1.0f;
    fn.vkCmdSetViewport(cmd, 0, 1, &vp);
    const VkRect2D sc{{0, 0}, ext};
    fn.vkCmdSetScissor(cmd, 0, 1, &sc);
    fn.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, motion_pipe_);
    fn.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_motion_, 0, 1, &ms, 0, nullptr);
    fn.vkCmdPushConstants(cmd, layout_motion_, kPushStages, 0, sizeof pc, &pc);
    fn.vkCmdDraw(cmd, 3, 1, 0, 0);
    fn.vkCmdEndRendering(cmd);
    fsr_motion_.transition_color(fn, cmd, rhi::ImageState::ShaderRead);
    fsr_depth_.transition_color(fn, cmd, rhi::ImageState::ShaderRead);
    prev_ = cam_;
    history_valid_ = true;
    return true;
}

// ------------------------------------------------------------------------------------------- Renderer hooks

ViewMatrices Renderer::taa_begin(const ViewMatrices& view, const RecordOptions& opts, uint32_t fsr_phases) {
    if (!opts.taa && fsr_phases == 0) {
        if (taa_) taa_->reset();  // the history is stale once a frame went without it (mode switch)
        return view;
    }
    if (!taa_) {
        taa_ = std::make_unique<TaaPass>();
        if (!taa_->init(*ctx_, *alloc_, hot_.buffers())) {
            log::errorf("render", "TAA could not be initialised; frames render without it");
            taa_.reset();
            return view;
        }
    }
    taa_->set_fsr_jitter(fsr_phases);  // a change of mode or of the render / display ratio restarts the history
    return taa_->begin_frame(view, opts.taa_provisional);
}

// [port] FSR (fsr.hpp) / XeSS (xess.hpp): true when this frame is drawn at `render` and upscaled by opts.upscaler;
// false leaves `render` = `display`.
bool Renderer::fsr_begin(VkExtent2D display, const RecordOptions& opts, VkExtent2D& render) {
    render = display;
    if (display.width == 0 || display.height == 0) return false;
    if (opts.upscaler == Upscaler::Xess) {  // XeSS's own input size for the preset (xessGetOptimalInputResolution)
        if (!xess_supported() || !xess_->prepare(display, opts.upscale_quality)) return false;
        render = xess_->render_size();
        return true;
    }
    if (opts.upscaler != Upscaler::Fsr) return false;
    if (!fsr_supported() || !fsr_->prepare(display)) return false;
    upscale_render_size(display.width, display.height, opts.upscale_quality, render.width, render.height);
    return true;
}

bool Renderer::xess_supported() {
    if (!xess_) {
        xess_ = std::make_unique<XessPass>();
        xess_->init(*ctx_, *alloc_);  // logs why when it cannot
    }
    return xess_->available();
}

bool Renderer::fsr_supported() {
    if (!fsr_) {
        fsr_ = std::make_unique<FsrPass>();
        fsr_->init(*ctx_, *alloc_);  // logs why when it cannot
    }
    return fsr_->available();
}

std::string Renderer::upscaler_status(Upscaler u) const {
    if (u == Upscaler::Xess) return xess_ ? xess_->status() : std::string("XeSS not loaded yet");
    return fsr_ ? fsr_->status() : std::string("FSR not loaded yet");
}

void Renderer::reset_taa_history() {
    if (taa_) taa_->reset();
}

bool Renderer::discard_taa_frame() { return taa_ && taa_->discard(); }

void Renderer::keep_taa_frame() {
    if (taa_) taa_->keep();
}

}  // namespace alamo::render
