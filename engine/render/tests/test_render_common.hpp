// Shared fixtures and helpers of test_render*.cpp (split from test_render.cpp, PAF t08). Test bodies live in the part files.
#pragma once

// alamo_render tests (no framework; non-zero exit on failure).
//   CPU (always): format / swizzle mapping tables, the 10 ALO vertex-format names -> materials
//     layouts (+ attribute tables vs assets), sampler-key conversion, camera matrices, LOD / ALT
//     selection, PNG encoder.
//   GPU (skips cleanly without a Vulkan 1.3 device): a synthetic textured model rendered offscreen
//     through the real pipeline (bindless set, uploads, LOD/hidden/cull/depth, attach bones,
//     skinned palette, a posed frame) with pixel checks. Validation is required when
//     toolchain\vvl\bin (or VK_LAYER_PATH) has the Khronos layer, and every message fails.
//   Real data (ALAMO_FOC_DATA = FoC "corruption" folder): EV_STARDESTOYER_THRAWN loaded on the job
//     system and rendered at 512x512 -> logs/render_thrawn_sd.png, coverage asserted.
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <array>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <vector>

#include "alamo/assets/asset_cache.hpp"
#include "alamo/core/file_provider.hpp"
#include "alamo/core/hash.hpp"
#include "alamo/core/log.hpp"
#include "alamo/jobs/job_system.hpp"
#include "alamo/render/camera.hpp"
#include "alamo/render/convert.hpp"
#include "alamo/render/fleet.hpp"
#include "alamo/render/lighting.hpp"
#include "alamo/render/objects.hpp"
#include "alamo/render/ted.hpp"
#include "alamo/render/world.hpp"
#include "alamo/render/map_scene.hpp"
#include "alamo/data/game_database.hpp"
#include "alamo/data/xml.hpp"
#include "alamo/render/hardpoints.hpp"
#include "alamo/terrain/placement.hpp"
#include "alamo/shaderxlat/interpreter.hpp"
#include "alamo/render/lod.hpp"
#include "alamo/render/pipelines.hpp"
#include "alamo/render/png.hpp"
#include "alamo/render/ssao.hpp"
#include "alamo/render/renderer.hpp"
#include "alamo/rhi/rhi.hpp"
#include "alamo/vfs/vfs.hpp"
#include "alamo/test/support.hpp"
#include "alamo/render/test/gpu.hpp"

// Counting global allocator (Q3: build() / render() allocate nothing per frame in steady state). Counts only while
// g_count_allocs is set; every thread (job-system workers included).
namespace test_render_t {
inline std::atomic<bool> g_count_allocs{false};
inline std::atomic<uint64_t> g_allocs{0};
inline size_t g_alloc_sizes[8] = {};  // sizes of the first counted allocations (diagnostics)
inline bool g_measure_allocs = false;  // render_frame(): count the allocations of Renderer::render()
inline uint64_t g_render_allocs = 0;   // ... of the last render_frame()
// libstdc++'s std::deque frees and allocates 512-byte node blocks as a queue advances, so the job system's queues
// (engine/jobs, std::deque<Job>) allocate during parallel_for on GCC builds; MSVC's deque reuses its blocks.
#if defined(__GLIBCXX__)
constexpr bool kStdDequeNodeChurn = true;
#else
constexpr bool kStdDequeNodeChurn = false;
#endif
}  // namespace test_render_t
using namespace alamo;
using render::Float3;
using render::Mat3x4;

namespace test_render_t {

using ::alamo::test::g_fail;

inline bool near_eq(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

inline render::Float4 project(const render::Mat4& vp, const Float3& p) {
    const render::Float4 c = vp * render::Float4{p.x, p.y, p.z, 1.0f};
    return {c.x / c.w, c.y / c.w, c.z / c.w, c.w};
}

// Angle (degrees, -180..180] of a bone's world X axis about +Z.
inline float z_angle_deg(const Mat3x4& m) { return std::atan2(m.m[1][0], m.m[0][0]) * 57.29578f; }

inline assets::MeshInfo mesh(const char* name, int lod, bool hidden = false) {
    assets::MeshInfo m;
    m.name = name;
    m.lod = lod;
    m.hidden = hidden;
    return m;
}

// ------------------------------------------------------------------------------------------- GPU

struct Gpu : render::test::GpuCore {
    render::Renderer renderer;
};

using Offscreen = render::test::OffscreenWithDepth;  // alamo/render/offscreen.hpp

const float kClear[4] = {0.0f, 0.0f, 0.25f, 1.0f};  // (0, 0, 64, 255)
// Most MeshBumpColorize/MeshGloss* triangles of all ships in the install (alamo_render --sweep).
constexpr const char* kSecondShip = "EV_EXECUTORSTARDESTROYER.ALO";
constexpr const char* kSecondShipPng = "render_ev_executorstardestroyer_lit.png";

// One frame: acquires, residency, pass, draw, copy to readback, submit, wait.
inline bool render_frame(Gpu& g, render::Renderer& r, Offscreen& o, const render::ViewMatrices& vm,
                  std::span<const render::Instance> inst, render::GpuModel* resident, render::DrawStats* stats = nullptr,
                  render::DrawMode mode = render::DrawMode::Lit, const render::RecordOptions& ro = {},
                  render::DrawTiming* timing = nullptr, std::span<const render::Instance> sky = {}, const float* clear = nullptr) {
    rhi::Frame* f = g.ring.begin_frame();
    if (!f) return false;
    VkCommandBuffer cmd = f->cmd();
    const rhi::DeviceFns& fn = g.ctx.vkd();
    const rhi::AcquireResult acq = g.up.record_acquires(cmd);
    if (resident) r.make_resident(*resident);
    render::PassTarget pt;
    pt.color = o.color.handle;
    pt.color_view = o.color.view;
    pt.depth = o.depth.handle;
    pt.depth_view = o.depth.view;
    pt.extent = {o.w, o.h};
    std::memcpy(pt.clear, kClear, sizeof kClear);
    if (clear) std::memcpy(pt.clear, clear, sizeof pt.clear);
    pt.color_after = rhi::ImageState::TransferSrc;
    const bool counting = g_measure_allocs;
    if (counting) {
        g_allocs = 0;
        g_count_allocs = true;
    }
    const bool drawn = r.render(*f, cmd, pt, vm, inst, mode, ro, stats, timing, sky);
    if (counting) {
        g_count_allocs = false;
        g_render_allocs = g_allocs.load();
    }
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {o.w, o.h, 1};
    fn.vkCmdCopyImageToBuffer(cmd, o.color.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, o.readback.handle, 1, &copy);
    const VkBufferMemoryBarrier2 hb = rhi::buffer_barrier(o.readback.handle, VK_PIPELINE_STAGE_2_COPY_BIT,
                                                          VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
                                                          VK_ACCESS_2_HOST_READ_BIT);
    rhi::cmd_barrier(fn, cmd, {}, std::span<const VkBufferMemoryBarrier2>(&hb, 1));
    VkSemaphoreSubmitInfo wait = rhi::semaphore_info(g.up.timeline(), acq.wait_value, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
    rhi::FrameSubmit fs;
    if (acq.wait_value) fs.waits = {&wait, 1};
    if (!g.ring.end_frame(*f, fs)) return false;
    g.ring.wait_idle();
    g.alloc.invalidate(o.readback.alloc);
    return drawn;
}

// PNG captures: render_frame() repeated (bounded) until no pass waits on a translation or a pipeline. Post passes
// (SceneBloom, SceneHeat, StencilDarken) are skipped while compiling (DrawStats::post_pending), so a single frame
// would differ run to run. Callers CHECK(st.post_pending == 0) on the returned stats.
inline bool render_settled(Gpu& g, render::Renderer& r, Offscreen& o, const render::ViewMatrices& vm,
                    std::span<const render::Instance> inst, render::GpuModel* resident, render::DrawStats& st,
                    render::DrawMode mode = render::DrawMode::Lit, const render::RecordOptions& ro = {},
                    render::DrawTiming* timing = nullptr, std::span<const render::Instance> sky = {}, const float* clear = nullptr) {
    bool drawn = false;
    for (int k = 0; k < 64; ++k) {
        drawn = render_frame(g, r, o, vm, inst, resident, &st, mode, ro, timing, sky, clear);
        if (!drawn || (st.post_pending == 0 && st.translation_pending == 0 && st.pending_pipeline == 0)) break;
        r.wait_translations();
        r.pipelines().wait_idle();
    }
    return drawn;
}

// Same through the inline Renderer::draw() path (caller's attachments, no frame graph).
inline bool draw_frame(Gpu& g, render::Renderer& r, Offscreen& o, const render::ViewMatrices& vm, std::span<const render::Instance> inst,
                render::DrawStats* stats = nullptr) {
    rhi::Frame* f = g.ring.begin_frame();
    if (!f) return false;
    VkCommandBuffer cmd = f->cmd();
    const rhi::DeviceFns& fn = g.ctx.vkd();
    const rhi::AcquireResult acq = g.up.record_acquires(cmd);
    render::PassTarget pt;
    pt.color = o.color.handle;
    pt.color_view = o.color.view;
    pt.depth = o.depth.handle;
    pt.depth_view = o.depth.view;
    pt.extent = {o.w, o.h};
    std::memcpy(pt.clear, kClear, sizeof kClear);
    pt.color_after = rhi::ImageState::TransferSrc;
    render::begin_pass(fn, cmd, pt);
    const bool drawn = r.draw(*f, cmd, vm, inst, render::DrawMode::Lit, stats);
    render::end_pass(fn, cmd, pt);
    VkSemaphoreSubmitInfo wait = rhi::semaphore_info(g.up.timeline(), acq.wait_value, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
    rhi::FrameSubmit fs;
    if (acq.wait_value) fs.waits = {&wait, 1};
    if (!g.ring.end_frame(*f, fs)) return false;
    g.ring.wait_idle();
    return drawn;
}

inline bool rgb_close(const uint8_t* p, int r, int gr, int b, int tol = 10) {
    return std::abs(p[0] - r) <= tol && std::abs(p[1] - gr) <= tol && std::abs(p[2] - b) <= tol;
}
inline void print_px(const char* what, const uint8_t* p) { std::printf("  %s = (%u,%u,%u,%u)\n", what, p[0], p[1], p[2], p[3]); }

inline void add_quad(assets::ModelAsset& m, const char* name, uint32_t bone, float x0, float x1, float z0, float z1, float y,
              const uint8_t col[4], const char* texture, bool hidden = false, int lod = -1, bool clockwise = false,
              bool skinned = false) {
    assets::MeshInfo mi;
    mi.name = name;
    mi.lod = lod;
    mi.hidden = hidden;
    mi.attach_bone = skinned ? 0 : bone;
    mi.first_submesh = static_cast<uint32_t>(m.submeshes.size());
    mi.submesh_count = 1;
    m.meshes.push_back(mi);
    assets::Submesh sm;
    sm.mesh = static_cast<uint32_t>(m.meshes.size() - 1);
    sm.flags = (hidden ? assets::kSubmeshHidden : 0u) | (skinned ? assets::kSubmeshSkinned : 0u);
    sm.vertex_format = skinned ? "alD3dVertRSkinNU2" : "alD3dVertNU2C";
    sm.layout = skinned ? assets::VertexLayout::Skinned : assets::VertexLayout::Static;
    sm.attach_bone = skinned ? 0 : bone;
    if (skinned) sm.bone_palette = {static_cast<uint16_t>(bone)};
    // Viewed from -Y (camera), +X right, +Z up: BL, BR, TR, TL is counter-clockwise; uv origin top-left.
    const float px[4] = {x0, x1, x1, x0}, pz[4] = {z0, z0, z1, z1};
    const float u[4] = {0, 1, 1, 0}, v[4] = {1, 1, 0, 0};
    sm.vertex_count = 4;
    sm.vertices.resize(4 * 64);
    for (int i = 0; i < 4; ++i) {
        if (skinned) {
            assets::SkinnedVertex sv{};
            sv.position[0] = px[i];
            sv.position[1] = y;
            sv.position[2] = pz[i];
            sv.normal[1] = -1.0f;
            sv.normal[3] = 0.0f;  // palette index 0
            sv.uv0[0] = u[i];
            sv.uv0[1] = v[i];
            std::memcpy(sv.color, col, 4);
            std::memcpy(sm.vertices.data() + i * 64, &sv, 64);
        } else {
            assets::StaticVertex st{};
            st.position[0] = px[i];
            st.position[1] = y;
            st.position[2] = pz[i];
            st.normal[1] = -1.0f;
            st.tangent[0] = 1.0f;   // +X along u
            st.binormal[2] = 1.0f;  // +Z (up the screen; v grows downwards, as authored binormals vary)
            st.uv0[0] = u[i];
            st.uv0[1] = v[i];
            std::memcpy(st.color, col, 4);
            std::memcpy(sm.vertices.data() + i * 64, &st, 64);
        }
    }
    const uint16_t ccw[6] = {0, 1, 2, 0, 2, 3}, cw[6] = {0, 2, 1, 0, 3, 2};
    sm.index_type = assets::IndexType::U16;
    sm.index_count = 6;
    sm.indices.resize(12);
    std::memcpy(sm.indices.data(), clockwise ? cw : ccw, 12);
    sm.bounds = {{x0, y, z0}, {x1, y, z1}};
    sm.material.effect = "MeshGloss.fx";
    if (texture) {
        assets::MaterialParam p;
        p.name = "BaseTexture";
        p.type = assets::ParamType::Texture;
        p.texture = texture;
        sm.material.params.push_back(p);
    }
    m.submeshes.push_back(std::move(sm));
}

inline std::shared_ptr<assets::ModelAsset> synthetic_model() {
    auto m = std::make_shared<assets::ModelAsset>();
    m->name = "SYNTHETIC";
    assets::Skeleton& sk = m->skeleton;
    sk.names = {"ROOT", "ARM"};
    sk.parents = {-1, 0};
    sk.visible = {1, 1};
    sk.billboard_mode = {0, 0};
    Mat3x4 arm;
    arm.m[1][3] = -1.0f;  // bone 1 sits 1 unit towards the camera
    sk.bind_local = {Mat3x4{}, arm};
    sk.finalize();
    const uint8_t white[4] = {255, 255, 255, 255}, magenta[4] = {255, 0, 255, 255}, green[4] = {0, 255, 0, 255},
                  yellow[4] = {255, 255, 0, 255};
    // Drawn first and nearer (bone 1): the far quad drawn later must fail the depth test there.
    add_quad(*m, "NEAR", 1, -0.1f, 0.1f, -0.1f, 0.1f, 0.0f, magenta, "white.dds");
    add_quad(*m, "QUAD", 0, -0.5f, 0.5f, -0.5f, 0.5f, 0.0f, white, "quad.dds");
    add_quad(*m, "BACK", 0, 0.6f, 0.9f, 0.6f, 0.9f, 0.0f, white, nullptr, false, -1, true);   // back-facing: culled
    add_quad(*m, "FLASH", 0, -0.9f, -0.6f, 0.6f, 0.9f, 0.0f, white, nullptr, true);          // hidden
    add_quad(*m, "DETAIL_LOD0", 0, -0.9f, -0.6f, -0.9f, -0.6f, 0.0f, white, nullptr, false, 0);  // not the chosen LOD
    add_quad(*m, "DETAIL_LOD1", 0, 0.6f, 0.9f, -0.9f, -0.6f, 0.0f, green, "missing.dds", false, 1);
    // Skinned (model space, palette = bone 1): bind pose leaves it in place.
    add_quad(*m, "SKIN", 1, -0.9f, -0.6f, -0.15f, 0.15f, 0.0f, yellow, nullptr, false, -1, false, true);
    m->bounds = {{-1, -1, -1}, {1, 1, 1}};
    m->sphere = {{0, 0, 0}, 1.8f};
    return m;
}

inline std::shared_ptr<const assets::TextureAsset> synthetic_texture(std::string_view name, assets::TextureRole) {
    auto t = std::make_shared<assets::TextureAsset>();
    t->name = std::string(name);
    t->format = assets::TextureFormat::RGBA8;
    t->mip_levels = 1;
    if (name == "quad.dds") {
        t->width = t->height = 2;
        t->data = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};  // R G / B W
    } else if (name == "white.dds") {
        t->width = t->height = 1;
        t->data = {255, 255, 255, 255};
    } else {
        return nullptr;  // missing -> renderer fallback (checker) ... still multiplied by vertex colour
    }
    t->levels.push_back({0, 0, t->width, t->height, 0, t->data.size()});
    return t;
}

// Tests defined in the part files.
int run_gpu(const std::string& src_dir);

}  // namespace test_render_t
