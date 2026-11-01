// alamo_render TAA tests [port] (no framework; non-zero exit on failure).
//   CPU (always): Halton values, the jitter cycle, jitter_view (exactly the view at zero jitter, an exact NDC shift
//     otherwise, carried into background_view), the history rules (cut / resize / FOV / small motion).
//   GPU (skips cleanly without a Vulkan 1.3 device): an empty scene through Renderer::render with TAA - the resolve
//     runs, the history restarts on the first frame, after a frame without TAA, on a resize, on a camera jump and on
//     reset_taa_history(), and the output keeps the clear colour. Validation is required when toolchain\vvl\bin (or
//     VK_LAYER_PATH) has the Khronos layer, and every message fails.
//   Real data (ALAMO_FOC_DATA): _MP_LAND_ENDOR at 1280x720 under a slowly orbiting tactical camera, TAA off vs on ->
//     logs/render_taa_off.png, logs/render_taa_on.png (+ 4x crops), edge-energy and frame-to-frame shimmer numbers,
//     GPU time of the resolve.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "alamo/assets/asset_cache.hpp"
#include "alamo/data/game_database.hpp"
#include "alamo/jobs/job_system.hpp"
#include "alamo/render/camera.hpp"
#include "alamo/render/map_scene.hpp"
#include "alamo/render/pipelines.hpp"
#include "alamo/render/png.hpp"
#include "alamo/render/renderer.hpp"
#include "alamo/render/taa.hpp"
#include "alamo/render/world.hpp"
#include "alamo/rhi/rhi.hpp"
#include "alamo/vfs/vfs.hpp"

using namespace alamo;

namespace {

int g_failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

bool close_to(float a, float b, float eps = 1e-6f) { return std::fabs(a - b) <= eps; }

render::Float4 project(const render::ViewMatrices& v, const render::Float3& p) {
    const render::Float4 c = v.view_proj * render::Float4{p.x, p.y, p.z, 1.0f};
    return {c.x / c.w, c.y / c.w, c.z / c.w, c.w};
}

void test_cpu() {
    // Halton: radical inverse.
    CHECK(render::halton(0, 2) == 0.0f);
    CHECK(render::halton(1, 2) == 0.5f && render::halton(2, 2) == 0.25f && render::halton(3, 2) == 0.75f);
    CHECK(render::halton(4, 2) == 0.125f && render::halton(5, 2) == 0.625f);
    CHECK(close_to(render::halton(1, 3), 1.0f / 3.0f) && close_to(render::halton(2, 3), 2.0f / 3.0f) && close_to(render::halton(3, 3), 1.0f / 9.0f));
    CHECK(close_to(render::halton(4, 3), 4.0f / 9.0f) && close_to(render::halton(5, 3), 7.0f / 9.0f));
    // Jitter: point (frame % 8) + 1, minus 0.5; within (-0.5, 0.5); the cycle repeats; mean of a cycle near 0.
    float jx = 0, jy = 0;
    render::taa_jitter_pixels(0, jx, jy);
    CHECK(jx == 0.0f && close_to(jy, 1.0f / 3.0f - 0.5f));
    render::taa_jitter_pixels(1, jx, jy);
    CHECK(jx == -0.25f && close_to(jy, 2.0f / 3.0f - 0.5f));
    float sx = 0, sy = 0;
    for (uint64_t f = 0; f < render::kTaaSamples; ++f) {
        float ax = 0, ay = 0, bx = 0, by = 0;
        render::taa_jitter_pixels(f, ax, ay);
        render::taa_jitter_pixels(f + render::kTaaSamples, bx, by);
        CHECK(ax == bx && ay == by);
        CHECK(ax > -0.5f && ax < 0.5f && ay > -0.5f && ay < 0.5f);
        sx += ax;
        sy += ay;
    }
    CHECK(std::fabs(sx / 8.0f) < 0.07f && std::fabs(sy / 8.0f) < 0.07f);

    // jitter_view: zero jitter = the view bit for bit; otherwise every depth moves by exactly 2 j / size in NDC.
    render::OrbitCamera cam;
    cam.target = {10, -20, 5};
    cam.distance = 300.0f;
    cam.radius = 100.0f;
    const render::ViewMatrices v = render::make_view(cam, 1280, 720, 10.0f, 7000.0f);
    const render::ViewMatrices z = render::jitter_view(v, 0.0f, 0.0f);
    CHECK(std::memcmp(&z.proj, &v.proj, sizeof v.proj) == 0 && std::memcmp(&z.view_proj, &v.view_proj, sizeof v.view_proj) == 0);
    const render::ViewMatrices j = render::jitter_view(v, 0.25f, -0.375f);
    CHECK(std::memcmp(&j.view, &v.view, sizeof v.view) == 0);
    for (const render::Float3 p : {render::Float3{10, -20, 5}, render::Float3{60, 40, 0}, render::Float3{-90, 10, 30}}) {
        const render::Float4 a = project(v, p), b = project(j, p);
        CHECK(close_to(b.x - a.x, 2.0f * 0.25f / 1280.0f, 1e-5f) && close_to(b.y - a.y, 2.0f * -0.375f / 720.0f, 1e-5f));
        CHECK(close_to(b.z, a.z, 1e-6f) && b.w == a.w);
    }
    // The background camera follows the jitter (same NDC shift) and is unchanged without it.
    const render::ViewMatrices bg = render::background_view(v), bgj = render::background_view(j);
    CHECK(bg.proj.m[0][2] == 0.0f && bg.proj.m[1][2] == 0.0f);
    {
        const render::Float3 p{2000, 500, 300};
        const render::Float4 a = project(bg, p), b = project(bgj, p);
        CHECK(close_to(b.x - a.x, 2.0f * 0.25f / 1280.0f, 1e-5f) && close_to(b.y - a.y, 2.0f * -0.375f / 720.0f, 1e-5f));
    }

    // History rules.
    const render::TaaCamera c0 = render::taa_camera(v);
    CHECK(c0.valid && close_to(c0.far_plane, 7000.0f, 0.5f));
    CHECK(!render::taa_history_usable(render::TaaCamera{}, c0));  // no previous frame
    CHECK(render::taa_history_usable(c0, c0));
    render::OrbitCamera moved = cam;
    moved.orbit(0.01f, 0.005f);  // slow orbit
    moved.target.x += 3.0f;      // and a scroll
    CHECK(render::taa_history_usable(c0, render::taa_camera(render::make_view(moved, 1280, 720, 10.0f, 7000.0f))));
    render::OrbitCamera turned = cam;
    turned.orbit(0.5f, 0.0f);  // ~29 degrees in one frame: a cut
    CHECK(!render::taa_history_usable(c0, render::taa_camera(render::make_view(turned, 1280, 720, 10.0f, 7000.0f))));
    render::OrbitCamera jumped = cam;
    jumped.target.x += 1000.0f;  // teleport (over 5 % of the far plane)
    CHECK(!render::taa_history_usable(c0, render::taa_camera(render::make_view(jumped, 1280, 720, 10.0f, 7000.0f))));
    render::OrbitCamera zoomed = cam;
    zoomed.fov_x *= 1.1f;  // FOV change
    CHECK(!render::taa_history_usable(c0, render::taa_camera(render::make_view(zoomed, 1280, 720, 10.0f, 7000.0f))));
    CHECK(!render::taa_history_usable(c0, render::taa_camera(render::make_view(cam, 1920, 1080, 10.0f, 7000.0f))));  // resize
    // The jittered camera passes the test against its own un-jittered self (rules use the un-jittered camera only).
    CHECK(render::taa_history_usable(c0, render::taa_camera(j)));

    // Linear depth (the disocclusion test's unit): NDC 0 / 1 are the near / far planes, and a projected point comes back as
    // its distance along the view direction. The jittered camera linearises the same (jitter only touches rows 0 / 1).
    CHECK(c0.perspective);
    CHECK(close_to(render::taa_linear_depth(c0, 0.0f), 10.0f, 1e-3f) && close_to(render::taa_linear_depth(c0, 1.0f), 7000.0f, 1.0f));
    for (const render::Float3 p : {render::Float3{10, -20, 5}, render::Float3{60, 40, 0}, render::Float3{-90, 10, 30}}) {
        const float along = (p.x - v.eye.x) * c0.forward.x + (p.y - v.eye.y) * c0.forward.y + (p.z - v.eye.z) * c0.forward.z;
        const float d = render::taa_linear_depth(c0, project(v, p).z);
        CHECK(std::fabs(d - along) <= 2e-3f * along);
        CHECK(close_to(render::taa_linear_depth(render::taa_camera(j), project(j, p).z), d, 1e-3f * d));
    }
    CHECK(!render::taa_linear_depth(render::TaaCamera{}, 0.5f));  // no camera: 0
    {
        render::TaaCamera o;  // orthographic, near 1 / far 101: NDC 0 -> 1, NDC 1 -> 101
        o.valid = true;
        o.perspective = false;
        o.depth_a = 1.0f / (1.0f - 101.0f);
        o.depth_b = 1.0f / (1.0f - 101.0f);
        CHECK(close_to(render::taa_linear_depth(o, 0.0f), 1.0f, 1e-4f) && close_to(render::taa_linear_depth(o, 1.0f), 101.0f, 1e-3f));
    }
    std::printf("cpu: %s\n", g_failures ? "FAILED" : "ok");
}

// ------------------------------------------------------------------------------------------- GPU

struct Gpu {
    rhi::Context ctx;
    rhi::Allocator alloc;
    rhi::Uploader up;
    rhi::FrameRing ring;
    rhi::GpuTimer timer;
};

struct Offscreen {
    rhi::Image color;
    rhi::Buffer readback;
    uint32_t w = 0, h = 0;
    bool create(Gpu& g, uint32_t width, uint32_t height) {
        w = width;
        h = height;
        rhi::ImageDesc cd;
        cd.format = rhi::Format::RGBA8;
        cd.width = w;
        cd.height = h;
        cd.mips = 1;
        cd.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        return g.alloc.create_image(cd, color) &&
               g.alloc.create_buffer(static_cast<VkDeviceSize>(w) * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, rhi::MemoryUsage::Readback,
                                     readback);
    }
    void destroy(Gpu& g) {
        if (color) g.alloc.destroy(color);
        if (readback) g.alloc.destroy(readback);
    }
    const uint8_t* data() const { return static_cast<const uint8_t*>(readback.mapped()); }
};

struct GpuTimes {
    double taa = -1, post = -1, scene = -1;
    double cpu_ms = 0;  // DrawTiming build + record
};

// One frame through Renderer::render into `o`, read back; GPU scope times of this frame when the timer works.
bool frame(Gpu& g, render::Renderer& r, Offscreen& o, const render::ViewMatrices& vm, std::span<const render::Instance> inst,
           const render::RecordOptions& ro, render::DrawStats& st, const float (&clear)[4], std::span<const render::Instance> sky = {},
           GpuTimes* times = nullptr) {
    rhi::Frame* f = g.ring.begin_frame();
    if (!f) return false;
    VkCommandBuffer cmd = f->cmd();
    const rhi::DeviceFns& fn = g.ctx.vkd();
    const rhi::AcquireResult acq = g.up.record_acquires(cmd);
    const bool timed = times && g.timer.enabled();
    if (timed) g.timer.begin_frame(cmd, f->slot());
    render::PassTarget pt;
    pt.color = o.color.handle;
    pt.color_view = o.color.view;
    pt.extent = {o.w, o.h};
    std::memcpy(pt.clear, clear, sizeof pt.clear);
    pt.color_after = rhi::ImageState::TransferSrc;
    render::RecordOptions opts = ro;
    if (timed) opts.gpu_timer = &g.timer;
    render::DrawTiming tm;
    const bool drawn = r.render(*f, cmd, pt, vm, inst, render::DrawMode::Lit, opts, &st, &tm, sky);
    if (times) times->cpu_ms = tm.build_ms + tm.record_ms;
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {o.w, o.h, 1};
    fn.vkCmdCopyImageToBuffer(cmd, o.color.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, o.readback.handle, 1, &copy);
    const VkBufferMemoryBarrier2 hb = rhi::buffer_barrier(o.readback.handle, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                                          VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    rhi::cmd_barrier(fn, cmd, {}, std::span<const VkBufferMemoryBarrier2>(&hb, 1));
    VkSemaphoreSubmitInfo wait = rhi::semaphore_info(g.up.timeline(), acq.wait_value, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
    rhi::FrameSubmit fs;
    if (acq.wait_value) fs.waits = {&wait, 1};
    if (!g.ring.end_frame(*f, fs)) return false;
    g.ring.wait_idle();
    g.alloc.invalidate(o.readback.alloc);
    if (timed && g.timer.collect(f->slot())) {
        for (const rhi::GpuTimerResult& res : g.timer.results()) {
            if (res.name == "taa") times->taa = res.ms;
            if (res.name == "post") times->post = res.ms;
            if (res.name == "scene") times->scene = res.ms;
        }
    }
    return drawn;
}

bool rgb_close_to(const uint8_t* p, const float (&c)[4], int tol) {
    for (int k = 0; k < 3; ++k)
        if (std::abs(static_cast<int>(p[k]) - static_cast<int>(std::lround(c[k] * 255.0f))) > tol) return false;
    return true;
}

// Mean absolute luma gradient (edge energy: aliasing raises it) and the mean absolute per-pixel luma change between two
// frames (shimmer / instability) over the whole image.
double luma(const uint8_t* p) { return 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2]; }
double edge_energy(const std::vector<uint8_t>& img, uint32_t w, uint32_t h) {
    double s = 0;
    for (uint32_t y = 0; y + 1 < h; ++y)
        for (uint32_t x = 0; x + 1 < w; ++x) {
            const uint8_t* p = &img[(static_cast<size_t>(y) * w + x) * 4];
            s += std::fabs(luma(p) - luma(p + 4)) + std::fabs(luma(p) - luma(p + static_cast<size_t>(w) * 4));
        }
    return s / (static_cast<double>(w - 1) * (h - 1));
}
double frame_change(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    double s = 0;
    for (size_t i = 0; i + 3 < a.size(); i += 4) s += std::fabs(luma(&a[i]) - luma(&b[i]));
    return s / static_cast<double>(a.size() / 4);
}

bool save_png(const std::string& path, const std::vector<uint8_t>& img, uint32_t w, uint32_t h) {
    std::vector<uint8_t> rgba = img;
    for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
    return render::write_png_rgba(path, w, h, rgba);
}
// (x0, y0, cw, ch) of `img` scaled up `k` times (nearest), for looking at edges.
bool save_crop(const std::string& path, const std::vector<uint8_t>& img, uint32_t w, uint32_t x0, uint32_t y0, uint32_t cw,
               uint32_t ch, uint32_t k) {
    std::vector<uint8_t> out(static_cast<size_t>(cw) * k * ch * k * 4);
    for (uint32_t y = 0; y < ch * k; ++y)
        for (uint32_t x = 0; x < cw * k; ++x) {
            const uint8_t* s = &img[(static_cast<size_t>(y0 + y / k) * w + x0 + x / k) * 4];
            uint8_t* d = &out[(static_cast<size_t>(y) * cw * k + x) * 4];
            std::memcpy(d, s, 3);
            d[3] = 255;
        }
    return render::write_png_rgba(path, cw * k, ch * k, out);
}

void land_map(Gpu& g, const std::string& src_dir, const char* root) {
    vfs::VfsConfig cfg;
    cfg.foc_root = root;
    std::string err;
    auto files = vfs::Vfs::create(cfg, &err);
    CHECK(files != nullptr);
    if (!files) return;
    render::EffectCache effects(files.get());
    assets::AssetCache cache(*files, effects.model_load_options());
    jobs::JobSystem js;
    render::Renderer rr;
    render::RendererDesc rd;
    rd.color_format = VK_FORMAT_R8G8B8A8_UNORM;
    rd.effects = &effects;
    rd.js = &js;
    rd.pipeline_cache_dir = std::filesystem::temp_directory_path() / "alamo_render_test_taa_pso";
    CHECK(rr.init(g.ctx, g.alloc, g.up, rd));
    rr.set_frame_ring(&g.ring);
    data::GameDatabase db;
    CHECK(db.load(*files));
    render::GameTypeResolver resolver(db);
    CHECK(resolver.load_render_xml(*files));
    const uint32_t W = 1280, H = 720;
    Offscreen o;
    CHECK(o.create(g, W, H));
    {
        render::ModelStreamer streamer(rr, cache, js);
        const alamo::ParallelFor pf = jobs::make_parallel_for(js);
        render::MapScene scene;
        const bool loaded = scene.load(*files, "_MP_LAND_ENDOR", &resolver, pf, &err);
        CHECK(loaded);
        if (loaded && scene.start(rr, streamer, cache, {}, &err)) {
            rr.set_object_fade(render::MapScene::kLandObjectFadeBegin, render::MapScene::kLandObjectFadeEnd);
            const render::Float3 cc = scene.clear_color();
            const float clear[4] = {cc.x, cc.y, cc.z, 1.0f};
            render::OrbitCamera cam = scene.tactical_camera(static_cast<float>(W) / static_cast<float>(H));
            cam.distance *= 0.6f;  // closer: unit / tree silhouettes cover more pixels
            render::RecordOptions ro;
            ro.threads = js.thread_count();
            render::DrawStats st;
            auto view_at = [&](int k) {
                render::OrbitCamera c = cam;
                c.orbit(0.0015f * static_cast<float>(k), 0.0f);  // slow orbit: 0.086 degrees per frame
                return render::make_view(c, W, H, render::MapScene::kNear, render::MapScene::kFar);
            };
            for (int k = 0; k < 3000 && !scene.ready(); ++k) {
                streamer.update();
                scene.update(rr, streamer);
                CHECK(frame(g, rr, o, view_at(0), scene.instances(), ro, st, clear, scene.sky()));
            }
            bool done = false;
            for (int k = 0; k < 200 && !done; ++k) {
                streamer.update();
                scene.update(rr, streamer);
                CHECK(frame(g, rr, o, view_at(0), scene.instances(), ro, st, clear, scene.sky()));
                const render::WorldStats& ws = scene.world().stats();
                done = k > 2 && st.pending_pipeline == 0 && st.translation_pending == 0 && st.post_pending == 0 && ws.terrain_pending == 0 &&
                       ws.water_pending == 0;
                if (!done) {
                    rr.wait_translations();
                    rr.pipelines().wait_idle();
                }
            }
            CHECK(done);
            const size_t bytes = static_cast<size_t>(W) * H * 4;
            // The same 40-frame camera path with TAA off and on (scene clock fixed: only the camera moves).
            constexpr int kFrames = 40;
            std::vector<uint8_t> last[2], prev[2];
            double change[2] = {0, 0};
            GpuTimes t_on;
            double taa_ms = 0, post_on = 0, post_off = 0, cpu_ms[2] = {0, 0};
            int timed = 0;
            for (int pass = 0; pass < 2; ++pass) {
                ro.taa = pass == 1;
                for (int k = 0; k < kFrames; ++k) {
                    GpuTimes t;
                    CHECK(frame(g, rr, o, view_at(k), scene.instances(), ro, st, clear, scene.sky(), &t));
                    CHECK(st.taa == ro.taa);
                    if (ro.taa) CHECK(st.taa_reset == (k == 0));  // first TAA frame only: the slow orbit keeps the history
                    prev[pass] = last[pass];
                    last[pass].assign(o.data(), o.data() + bytes);
                    if (k >= kFrames - 10) change[pass] += frame_change(prev[pass], last[pass]) / 10.0;
                    if (k >= 10) {
                        cpu_ms[pass] += t.cpu_ms / (kFrames - 10);
                        if (pass == 1 && t.taa >= 0) {
                            taa_ms += t.taa;
                            post_on += t.post;
                            ++timed;
                        } else if (pass == 0 && t.post >= 0) {
                            post_off += t.post;
                        }
                    }
                    if (pass) t_on = t;
                }
            }
            CHECK(save_png(src_dir + "/logs/render_taa_off.png", last[0], W, H));
            CHECK(save_png(src_dir + "/logs/render_taa_on.png", last[1], W, H));
            // 4x crops of the start-camera view's silhouettes: the building at the top left, the landing pad right of centre.
            CHECK(save_crop(src_dir + "/logs/render_taa_off_crop.png", last[0], W, 0, 0, 240, 135, 4));
            CHECK(save_crop(src_dir + "/logs/render_taa_on_crop.png", last[1], W, 0, 0, 240, 135, 4));
            CHECK(save_crop(src_dir + "/logs/render_taa_off_crop2.png", last[0], W, 800, 380, 200, 140, 4));
            CHECK(save_crop(src_dir + "/logs/render_taa_on_crop2.png", last[1], W, 800, 380, 200, 140, 4));
            const double e_off = edge_energy(last[0], W, H), e_on = edge_energy(last[1], W, H);
            double mean_diff = 0;
            for (size_t i = 0; i + 3 < bytes; i += 4) mean_diff += std::fabs(luma(&last[0][i]) - luma(&last[1][i]));
            mean_diff /= static_cast<double>(bytes / 4);
            std::printf("land map _MP_LAND_ENDOR %ux%u, %d frames slow orbit: edge energy off %.3f / on %.3f, frame-to-frame luma change "
                        "(last 10) off %.3f / on %.3f, mean |off - on| luma %.2f; GPU taa %.3f ms (post chain %.3f ms with TAA, "
                        "%.3f ms without, scene %.3f ms); CPU build + record per moving-camera frame off %.2f / on %.2f ms\n",
                        W, H, kFrames, e_off, e_on, change[0], change[1], mean_diff, timed ? taa_ms / timed : -1.0,
                        timed ? post_on / timed : -1.0, post_off / std::max(1, kFrames - 10), t_on.scene, cpu_ms[0], cpu_ms[1]);
            CHECK(e_on < e_off);          // smoother edges
            CHECK(mean_diff < 6.0);       // same picture (no smear / shift / colour change)
            CHECK(change[1] < change[0] * 1.5 + 0.5);  // no extra shimmer from the jitter
            // A camera cut restarts the history.
            render::OrbitCamera far_cam = cam;
            far_cam.target.x += 1500.0f;
            CHECK(frame(g, rr, o, render::make_view(far_cam, W, H, render::MapScene::kNear, render::MapScene::kFar), scene.instances(), ro,
                        st, clear, scene.sky()));
            CHECK(st.taa && st.taa_reset);

            // Moving objects. The materials have no motion vectors, so the resolve has to tell a mover from the background
            // by depth. Fixed camera; the (up to 8) instances nearest the screen centre slide sideways ~3 px per frame for
            // kSlide frames. Ghost = how far TAA strays from the TAA-off picture over the slid path (mean |on - off| luma),
            // minus how far it strays there with nothing moving (plain AA differences).
            {
                const render::ViewMatrices v0 = view_at(0);
                const std::span<const render::Instance> all = scene.instances();
                struct Pick {
                    size_t index;
                    float dist2;
                    float step;  // world units per frame along the camera's right axis
                };
                std::vector<Pick> picks;
                for (size_t i = 0; i < all.size(); ++i) {
                    if (!all[i].model) continue;
                    const render::Float4 c = v0.view_proj * render::Float4{all[i].transform.m[0][3], all[i].transform.m[1][3],
                                                                           all[i].transform.m[2][3], 1.0f};
                    if (c.w <= 0.0f) continue;
                    const float nx = c.x / c.w, ny = c.y / c.w, nz = c.z / c.w;
                    if (std::fabs(nx) > 0.9f || std::fabs(ny) > 0.9f || nz < 0.0f || nz > 1.0f) continue;
                    picks.push_back({i, nx * nx + ny * ny, 3.0f * 2.0f * c.w / (v0.proj.m[0][0] * static_cast<float>(W))});
                }
                std::sort(picks.begin(), picks.end(), [](const Pick& a, const Pick& b) { return a.dist2 < b.dist2; });
                if (picks.size() > 8) picks.resize(8);
                if (picks.empty()) std::printf("moving objects: no instance near the screen centre, skipped\n");
                if (!picks.empty()) {
                    constexpr int kSlide = 24;
                    const render::Float3 right{v0.view.m[0][0], v0.view.m[0][1], v0.view.m[0][2]};
                    std::vector<render::Instance> moving(all.begin(), all.end());
                    // Screen box of the slid paths (start and end positions) plus a margin for the objects' own size.
                    float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
                    auto cover = [&](const render::Instance& in) {
                        const render::Float4 c = v0.view_proj * render::Float4{in.transform.m[0][3], in.transform.m[1][3],
                                                                               in.transform.m[2][3], 1.0f};
                        const float px = (c.x / c.w * 0.5f + 0.5f) * static_cast<float>(W), py = (0.5f - c.y / c.w * 0.5f) * static_cast<float>(H);
                        x0 = std::min(x0, px);
                        x1 = std::max(x1, px);
                        y0 = std::min(y0, py);
                        y1 = std::max(y1, py);
                    };
                    for (const Pick& p : picks) cover(all[p.index]);
                    auto shifted = [&](int k) {
                        for (const Pick& p : picks) {
                            render::Instance& in = moving[p.index];
                            const float s = p.step * static_cast<float>(k);
                            in.transform = all[p.index].transform;
                            in.transform.m[0][3] += right.x * s;
                            in.transform.m[1][3] += right.y * s;
                            in.transform.m[2][3] += right.z * s;
                        }
                    };
                    shifted(kSlide);
                    for (const Pick& p : picks) cover(moving[p.index]);
                    const uint32_t bx0 = static_cast<uint32_t>(std::clamp(x0 - 60.0f, 0.0f, static_cast<float>(W - 1)));
                    const uint32_t bx1 = static_cast<uint32_t>(std::clamp(x1 + 60.0f, 1.0f, static_cast<float>(W)));
                    const uint32_t by0 = static_cast<uint32_t>(std::clamp(y0 - 60.0f, 0.0f, static_cast<float>(H - 1)));
                    const uint32_t by1 = static_cast<uint32_t>(std::clamp(y1 + 60.0f, 1.0f, static_cast<float>(H)));
                    // The frame after `slide` slid steps (k frames run, the last one drawn at step k).
                    uint32_t mover_draws = 0;  // velocity-pass draws of the keyed run's last frame
                    uint32_t pending = 0;      // pipelines / translations still compiling in any run frame (a run then shows fallbacks)
                    auto run = [&](bool taa, bool slide, bool keyed, std::vector<uint8_t>& out) {
                        ro.taa = taa;
                        rr.reset_taa_history();
                        for (const Pick& p : picks) moving[p.index].motion_id = keyed ? render::motion_key(3, p.index + 1) : 0;
                        for (int k = 0; k <= kSlide; ++k) {
                            if (slide) shifted(k);
                            else shifted(0);
                            CHECK(frame(g, rr, o, v0, moving, ro, st, clear, scene.sky()));
                            if (keyed && taa && slide) mover_draws = st.taa_movers;
                            pending += st.pending_pipeline + st.translation_pending + st.post_pending;
                        }
                        out.assign(o.data(), o.data() + bytes);
                    };
                    auto region_diff = [&](const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
                        double s = 0;
                        for (uint32_t y = by0; y < by1; ++y)
                            for (uint32_t x = bx0; x < bx1; ++x) {
                                const size_t i = (static_cast<size_t>(y) * W + x) * 4;
                                s += std::fabs(luma(&a[i]) - luma(&b[i]));
                            }
                        return s / (static_cast<double>(bx1 - bx0) * (by1 - by0));
                    };
                    std::vector<uint8_t> off_move, on_move, key_move, off_still, on_still;
                    run(false, true, false, off_move);
                    run(true, true, false, on_move);   // no motion keys: camera motion + depth rejection only
                    run(true, true, true, key_move);   // keyed: the velocity pass gives the sliding objects real vectors
                    run(false, false, false, off_still);
                    run(true, false, false, on_still);
                    const double moved = region_diff(off_move, off_still);  // sanity: the objects really moved
                    const double d_move = region_diff(on_move, off_move), d_key = region_diff(key_move, off_move);
                    const double d_still = region_diff(on_still, off_still);
                    std::printf("moving objects: %zu instances slid %d frames, region %ux%u: |off moved - off still| %.2f; |on - off| still %.2f,"
                                " moving without keys %.2f (ghost %.2f), with keys %.2f (ghost %.2f, %u velocity draws)\n",
                                picks.size(), kSlide, bx1 - bx0, by1 - by0, moved, d_still, d_move, d_move - d_still, d_key, d_key - d_still,
                                mover_draws);
                    std::printf("moving objects: pending pipeline / translation frames-sum %u\n", pending);
                    CHECK(moved > 5.0);                   // the slid objects really changed the picture
                    CHECK(mover_draws > 0);               // the keyed run drew velocity
                    CHECK(d_move - d_still < 2.0);        // measured 0.85 with the depth rejection (RX 9070 XT); more than 2 = a trail
                    CHECK(d_key < d_move);                // measured 5.55 vs 5.80: motion vectors beat the depth rejection alone
                    CHECK(save_crop(src_dir + "/logs/render_taa_move_off.png", off_move, W, bx0, by0, std::min(bx1 - bx0, 300u),
                                    std::min(by1 - by0, 200u), 3));
                    CHECK(save_crop(src_dir + "/logs/render_taa_move_on.png", on_move, W, bx0, by0, std::min(bx1 - bx0, 300u),
                                    std::min(by1 - by0, 200u), 3));
                    CHECK(save_crop(src_dir + "/logs/render_taa_move_keyed.png", key_move, W, bx0, by0, std::min(bx1 - bx0, 300u),
                                    std::min(by1 - by0, 200u), 3));
                }
            }
        } else {
            std::printf("map: %s\n", err.c_str());
        }
        g.ring.wait_idle();
        scene.stop(rr);
        streamer.destroy_all();
    }
    o.destroy(g);
    rr.shutdown();
}

int run_gpu(const std::string& src_dir) {
    const bool strict = render::setup_validation_env(src_dir);
    {
        rhi::Loader probe;
        if (!probe.open()) {
            std::printf("SKIP gpu: no Vulkan loader\n");
            return 0;
        }
    }
    Gpu g;
    rhi::ContextDesc cd;
    cd.app_name = "alamo_render_taa_tests";
    cd.validation = strict ? rhi::Validation::Required : rhi::Validation::IfAvailable;
    if (!g.ctx.init(cd)) {
        std::printf(strict ? "FAIL gpu: no context with validation\n" : "SKIP gpu: no suitable Vulkan 1.3 device\n");
        return strict ? 1 : 0;
    }
    std::printf("gpu: %s, validation %s%s\n", g.ctx.caps().name.c_str(), g.ctx.validation_enabled() ? "on" : "off", strict ? " (required)" : "");
    CHECK(!strict || g.ctx.validation_enabled());
    rhi::FrameRingDesc fd;
    fd.frames_in_flight = 2;
    fd.uniform_bytes = 64ull << 20;
    fd.recording_threads = jobs::JobSystem::default_worker_count() + 1;
    CHECK(g.alloc.init(g.ctx) && g.up.init(g.ctx, g.alloc) && g.ring.init(g.ctx, g.alloc, fd));
    g.timer.init(g.ctx, fd.frames_in_flight);
    {
        // ---- empty scene: the resolve and its history rules.
        render::Renderer r;
        render::RendererDesc rd;
        rd.color_format = VK_FORMAT_R8G8B8A8_UNORM;
        rd.translated = false;
        const bool ok = r.init(g.ctx, g.alloc, g.up, rd);
        CHECK(ok);
        Offscreen o, o2;
        CHECK(o.create(g, 64, 48) && o2.create(g, 80, 40));
        if (ok) {
            r.set_frame_ring(&g.ring);
            render::OrbitCamera cam;
            const render::ViewMatrices v = render::make_view(cam, 64, 48);
            const float clear[4] = {0.2f, 0.4f, 0.6f, 1.0f};
            render::RecordOptions ro;
            ro.taa = true;
            render::DrawStats st;
            CHECK(frame(g, r, o, v, {}, ro, st, clear) && st.taa && st.taa_reset);  // first frame: no history
            CHECK(frame(g, r, o, v, {}, ro, st, clear) && st.taa && !st.taa_reset);
            CHECK(frame(g, r, o, v, {}, ro, st, clear) && st.taa && !st.taa_reset);
            bool same = true;
            for (size_t i = 0; i < static_cast<size_t>(o.w) * o.h; ++i) same = same && rgb_close_to(o.data() + i * 4, clear, 1);
            CHECK(same);  // resolve + sharpen of a flat image is the image
            r.reset_taa_history();
            CHECK(frame(g, r, o, v, {}, ro, st, clear) && st.taa_reset);
            CHECK(frame(g, r, o, v, {}, ro, st, clear) && !st.taa_reset);
            render::RecordOptions off;
            CHECK(frame(g, r, o, v, {}, off, st, clear) && !st.taa && !st.taa_reset);  // off: nothing runs
            CHECK(frame(g, r, o, v, {}, ro, st, clear) && st.taa && st.taa_reset);     // back on: restart
            CHECK(frame(g, r, o2, render::make_view(cam, 80, 40), {}, ro, st, clear) && st.taa && st.taa_reset);  // resize
            CHECK(frame(g, r, o2, render::make_view(cam, 80, 40), {}, ro, st, clear) && !st.taa_reset);
            render::OrbitCamera turned = cam;
            turned.orbit(1.0f, 0.0f);
            CHECK(frame(g, r, o2, render::make_view(turned, 80, 40), {}, ro, st, clear) && st.taa_reset);  // cut
            render::RecordOptions nosharp = ro;
            nosharp.taa_sharpen = 0.0f;
            CHECK(frame(g, r, o2, render::make_view(turned, 80, 40), {}, nosharp, st, clear) && st.taa && !st.taa_reset);
            same = true;
            for (size_t i = 0; i < static_cast<size_t>(o2.w) * o2.h; ++i) same = same && rgb_close_to(o2.data() + i * 4, clear, 1);
            CHECK(same);
            g.ring.wait_idle();
        }
        o.destroy(g);
        o2.destroy(g);
        r.shutdown();
    }
    const char* root = std::getenv("ALAMO_FOC_DATA");
    if (root && *root) {
        land_map(g, src_dir, root);
    } else {
        std::printf("real data: ALAMO_FOC_DATA not set, land map skipped\n");
    }
    g.ring.wait_idle();
    const uint32_t errors = g.ctx.validation_errors(), warnings = g.ctx.validation_warnings();
    std::printf("gpu: validation %s, %u errors, %u warnings\n", g.ctx.validation_enabled() ? "on" : "off", errors, warnings);
    CHECK(errors == 0 && warnings == 0);
    g.timer.shutdown();
    g.ring.shutdown();
    g.up.shutdown();
    g.alloc.shutdown();
    g.ctx.shutdown();
    return 0;
}

}  // namespace

int main() {
    test_cpu();
    run_gpu(ALAMO_RENDER_SOURCE_DIR);
    std::printf("%s (%d failures)\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures ? 1 : 0;
}
