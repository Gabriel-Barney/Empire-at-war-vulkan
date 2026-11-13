// alamo_render XeSS upscaling tests [port: Gabriel 2026-11-13] (no framework; non-zero exit on failure).
//   CPU (always): XeSS's preset ratios (1.0, 1.7, 2.0, 2.3, 3.0) next to FSR's, the estimated render sizes, the jitter
//     cycle length for XeSS's ratios (the jitter sequence, its direction and the camera numbers are FSR's and are checked
//     by alamo_render_fsr_tests).
//   GPU (skips cleanly without a Vulkan 1.3 device; the XeSS part skips when the runtime cannot run here - the reason is
//     printed - unless ALAMO_REQUIRE_XESS=1): an empty scene through Renderer::render with Upscaler::Xess - XeSS runs at
//     the size its runtime asks for (within a pixel of display / ratio), the output keeps the clear colour, the history
//     restarts on the first frame / resize / preset switch / reset_taa_history() / FSR <-> XeSS switch and not otherwise.
//     Validation is required when toolchain\vvl\bin (or VK_LAYER_PATH) has the Khronos layer, and every message outside
//     the runtime's own calls fails.
//   Real data (ALAMO_FOC_DATA): _MP_LAND_ENDOR at 1280x720, the same slow orbit drawn with TAA at full size and with XeSS
//     Quality / Performance -> logs/render_xess_*.png (+ 4x crops); the pictures match (mean luma difference), XeSS adds
//     no shimmer, a still camera settles (a wrong jitter sign or motion-vector direction would keep it moving), GPU times.
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
#include "alamo/render/upscale.hpp"
#include "alamo/render/world.hpp"
#include "alamo/rhi/rhi.hpp"
#include "alamo/vfs/vfs.hpp"
#include "alamo/test/support.hpp"
#include "alamo/render/test/gpu.hpp"

using namespace alamo;

namespace {

using ::alamo::test::g_fail;
using render::Upscaler;
using render::UpscaleQuality;

void test_cpu() {
    CHECK(render::upscale_ratio(UpscaleQuality::NativeAA, Upscaler::Xess) == 1.0f);
    CHECK(render::upscale_ratio(UpscaleQuality::Quality, Upscaler::Xess) == 1.7f);
    CHECK(render::upscale_ratio(UpscaleQuality::Balanced, Upscaler::Xess) == 2.0f);
    CHECK(render::upscale_ratio(UpscaleQuality::Performance, Upscaler::Xess) == 2.3f);
    CHECK(render::upscale_ratio(UpscaleQuality::UltraPerformance, Upscaler::Xess) == 3.0f);
    CHECK(render::upscale_ratio(static_cast<UpscaleQuality>(9), Upscaler::Xess) == 1.0f);
    // FSR's own ratios are unchanged by the upscaler argument's default.
    CHECK(render::upscale_ratio(UpscaleQuality::Quality) == 1.5f && render::upscale_ratio(UpscaleQuality::Quality, Upscaler::Fsr) == 1.5f);
    struct Size {
        UpscaleQuality q;
        uint32_t w, h;
    };
    for (const Size& s : {Size{UpscaleQuality::NativeAA, 1920, 1080}, Size{UpscaleQuality::Quality, 1129, 635},
                          Size{UpscaleQuality::Balanced, 960, 540}, Size{UpscaleQuality::Performance, 834, 469},
                          Size{UpscaleQuality::UltraPerformance, 640, 360}}) {
        uint32_t w = 0, h = 0;
        render::upscale_render_size(1920, 1080, s.q, w, h, Upscaler::Xess);
        CHECK(w == s.w && h == s.h);
    }
    // Jitter cycle for XeSS's ratios: at least 8 x ratio^2 (the developer guide's rule, FSR's formula).
    CHECK(render::upscale_jitter_phases(1129, 1920) == 23 && render::upscale_jitter_phases(834, 1920) == 42);
    std::printf("cpu: %s\n", g_fail ? "FAILED" : "ok");
}

// ------------------------------------------------------------------------------------------- GPU

struct Gpu : render::test::GpuCore {
    rhi::GpuTimer timer;
};

using Offscreen = render::test::OffscreenColorOnly;

struct GpuTimes {
    double up = -1, post = -1, scene = -1;  // "xess" or "taa" scope
};

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
    const bool drawn = r.render(*f, cmd, pt, vm, inst, render::DrawMode::Lit, opts, &st, nullptr, sky);
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
            if (res.name == "xess" || res.name == "taa") times->up = res.ms;
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

// Within a pixel of display / XeSS ratio (the runtime's xessGetOptimalInputResolution may round either way).
bool near_ratio(uint32_t render, uint32_t display, UpscaleQuality q) {
    const double want = static_cast<double>(display) / static_cast<double>(render::upscale_ratio(q, Upscaler::Xess));
    return std::fabs(static_cast<double>(render) - want) <= 1.0;
}

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
    if (a.size() != b.size() || a.empty()) return 0.0;
    double s = 0;
    for (size_t i = 0; i + 3 < a.size(); i += 4) s += std::fabs(luma(&a[i]) - luma(&b[i]));
    return s / static_cast<double>(a.size() / 4);
}

bool save_png(const std::string& path, const std::vector<uint8_t>& img, uint32_t w, uint32_t h) {
    std::vector<uint8_t> rgba = img;
    for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
    return render::write_png_rgba(path, w, h, rgba);
}
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
    rd.pipeline_cache_dir = std::filesystem::temp_directory_path() / "alamo_render_test_xess_pso";
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
            cam.distance *= 0.6f;
            render::RecordOptions ro;
            ro.threads = js.thread_count();
            render::DrawStats st;
            auto view_at = [&](int k) {
                render::OrbitCamera c = cam;
                c.orbit(0.0015f * static_cast<float>(k), 0.0f);
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
            // The same 40-frame slow orbit: 0 TAA at full size, 1 XeSS Quality, 2 XeSS Performance. Then 24 frames with the
            // camera still: what is left of the frame-to-frame change once the history has settled.
            constexpr int kFrames = 40, kStill = 24;
            const char* names[3] = {"taa", "xess_quality", "xess_performance"};
            const UpscaleQuality modes[3] = {UpscaleQuality::Quality, UpscaleQuality::Quality, UpscaleQuality::Performance};
            std::vector<uint8_t> last[3], prev[3];
            double change[3] = {0, 0, 0}, still[3] = {0, 0, 0}, up_ms[3] = {0, 0, 0}, scene_ms[3] = {0, 0, 0};
            int timed[3] = {0, 0, 0};
            uint32_t rw[3] = {0, 0, 0}, rh[3] = {0, 0, 0};
            for (int pass = 0; pass < 3; ++pass) {
                ro.taa = pass == 0;
                ro.upscaler = pass == 0 ? Upscaler::None : Upscaler::Xess;
                ro.upscale_quality = modes[pass];
                for (int k = 0; k < kFrames + kStill; ++k) {
                    GpuTimes t;
                    CHECK(frame(g, rr, o, view_at(std::min(k, kFrames - 1)), scene.instances(), ro, st, clear, scene.sky(), &t));
                    CHECK(st.xess == (pass > 0) && !st.fsr);
                    CHECK(st.taa_reset == (k == 0));  // first frame of the mode only: the slow orbit keeps the history
                    rw[pass] = st.render_width;
                    rh[pass] = st.render_height;
                    prev[pass] = last[pass];
                    last[pass].assign(o.data(), o.data() + bytes);
                    if (k >= kFrames - 10 && k < kFrames) change[pass] += frame_change(prev[pass], last[pass]) / 10.0;
                    if (k >= kFrames + kStill - 8) still[pass] += frame_change(prev[pass], last[pass]) / 8.0;
                    if (k >= 10 && t.up >= 0) {
                        up_ms[pass] += t.up;
                        scene_ms[pass] += t.scene;
                        ++timed[pass];
                    }
                }
                CHECK(save_png(src_dir + "/logs/render_" + names[pass] + ".png", last[pass], W, H));
                CHECK(save_crop(src_dir + "/logs/render_" + names[pass] + "_crop.png", last[pass], W, 0, 0, 240, 135, 4));
                CHECK(save_crop(src_dir + "/logs/render_" + names[pass] + "_crop2.png", last[pass], W, 800, 380, 200, 140, 4));
            }
            CHECK(rw[0] == W && rh[0] == H);
            CHECK(near_ratio(rw[1], W, modes[1]) && near_ratio(rh[1], H, modes[1]));
            CHECK(near_ratio(rw[2], W, modes[2]) && near_ratio(rh[2], H, modes[2]));
            for (int pass = 0; pass < 3; ++pass) {
                double diff = 0;
                for (size_t i = 0; i + 3 < bytes; i += 4) diff += std::fabs(luma(&last[0][i]) - luma(&last[pass][i]));
                diff /= static_cast<double>(bytes / 4);
                std::printf("land map _MP_LAND_ENDOR %ux%u %-16s render %4ux%-4u edge energy %.3f, orbit change (last 10) %.3f, "
                            "still change (last 8) %.3f, mean |taa - this| luma %.2f; GPU %s %.3f ms, scene %.3f ms\n",
                            W, H, names[pass], rw[pass], rh[pass], edge_energy(last[pass], W, H), change[pass], still[pass], diff,
                            pass ? "xess" : "taa", timed[pass] ? up_ms[pass] / timed[pass] : -1.0,
                            timed[pass] ? scene_ms[pass] / timed[pass] : -1.0);
                if (pass) {
                    CHECK(diff < 8.0);                              // the same picture (no shift / smear / colour change)
                    CHECK(change[pass] < change[0] * 1.5 + 0.5);    // no extra shimmer while moving
                    CHECK(still[pass] < 0.5);                       // a still camera settles (jitter / vector direction)
                }
            }
            // A camera cut restarts XeSS's history.
            render::OrbitCamera far_cam = cam;
            far_cam.target.x += 1500.0f;
            CHECK(frame(g, rr, o, render::make_view(far_cam, W, H, render::MapScene::kNear, render::MapScene::kFar), scene.instances(), ro,
                        st, clear, scene.sky()));
            CHECK(st.xess && st.taa_reset);
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
    cd.app_name = "alamo_render_xess_tests";
    cd.validation = strict ? rhi::Validation::Required : rhi::Validation::IfAvailable;
    cd.upscaler_features = true;
    if (!g.ctx.init(cd)) {
        std::printf(strict ? "FAIL gpu: no context with validation\n" : "SKIP gpu: no suitable Vulkan 1.3 device\n");
        return strict ? 1 : 0;
    }
    std::printf("gpu: %s, validation %s%s, VK_EXT_mutable_descriptor_type %s\n", g.ctx.caps().name.c_str(),
                g.ctx.validation_enabled() ? "on" : "off", strict ? " (required)" : "",
                g.ctx.device_extension_enabled("VK_EXT_mutable_descriptor_type") ? "enabled" : "not enabled");
    CHECK(!strict || g.ctx.validation_enabled());
    CHECK(g.ctx.caps().upscaler_features);
    rhi::FrameRingDesc fd;
    fd.frames_in_flight = 2;
    fd.uniform_bytes = 64ull << 20;
    fd.recording_threads = jobs::JobSystem::default_worker_count() + 1;
    CHECK(g.alloc.init(g.ctx) && g.up.init(g.ctx, g.alloc) && g.ring.init(g.ctx, g.alloc, fd));
    g.timer.init(g.ctx, fd.frames_in_flight);
    const char* req = std::getenv("ALAMO_REQUIRE_XESS");
    const bool require = req && *req == '1';
    bool xess = false;
    {
        render::Renderer r;
        render::RendererDesc rd;
        rd.color_format = VK_FORMAT_R8G8B8A8_UNORM;
        rd.translated = false;
        const bool ok = r.init(g.ctx, g.alloc, g.up, rd);
        CHECK(ok);
        Offscreen o, o2;
        CHECK(o.create(g, 640, 360) && o2.create(g, 320, 200));
        if (ok) {
            r.set_frame_ring(&g.ring);
            xess = r.xess_supported();
            render::OrbitCamera cam;
            const render::ViewMatrices v = render::make_view(cam, 640, 360);
            const float clear[4] = {0.2f, 0.4f, 0.6f, 1.0f};
            render::RecordOptions ro;
            ro.upscaler = Upscaler::Xess;
            ro.upscale_quality = UpscaleQuality::Quality;
            render::DrawStats st;
            if (!xess) {
                std::printf("%s XeSS: %s\n", require ? "FAIL" : "SKIP", r.upscaler_status(Upscaler::Xess).c_str());
                CHECK(!require);
                // Without the runtime an XeSS frame is a full-size TAA frame.
                ro.taa = true;
                CHECK(frame(g, r, o, v, {}, ro, st, clear) && !st.xess && st.taa && st.render_width == 640);
            } else {
                CHECK(frame(g, r, o, v, {}, ro, st, clear));
                std::printf("XeSS: %s, Quality at 640x360 renders %ux%u\n", r.upscaler_status(Upscaler::Xess).c_str(), st.render_width,
                            st.render_height);
                CHECK(st.xess && !st.fsr && st.taa_reset && !st.taa);
                CHECK(near_ratio(st.render_width, 640, UpscaleQuality::Quality) && near_ratio(st.render_height, 360, UpscaleQuality::Quality));
                CHECK(frame(g, r, o, v, {}, ro, st, clear) && st.xess && !st.taa_reset);
                for (int k = 0; k < 6; ++k) CHECK(frame(g, r, o, v, {}, ro, st, clear) && st.xess && !st.taa_reset);
                bool same = true;
                for (size_t i = 0; i < static_cast<size_t>(o.w) * o.h; ++i) same = same && rgb_close_to(o.data() + i * 4, clear, 3);
                CHECK(same);  // a flat image upscales to itself
                r.reset_taa_history();
                CHECK(frame(g, r, o, v, {}, ro, st, clear) && st.xess && st.taa_reset);
                // Another preset: another render size (xessVKInit again); the history restarts (jitter cycle changed).
                render::RecordOptions perf = ro;
                perf.upscale_quality = UpscaleQuality::Performance;
                CHECK(frame(g, r, o, v, {}, perf, st, clear) && st.xess && st.taa_reset);
                CHECK(near_ratio(st.render_width, 640, UpscaleQuality::Performance) &&
                      near_ratio(st.render_height, 360, UpscaleQuality::Performance));
                CHECK(frame(g, r, o, v, {}, perf, st, clear) && st.xess && !st.taa_reset);
                render::RecordOptions native = ro;
                native.upscale_quality = UpscaleQuality::NativeAA;
                CHECK(frame(g, r, o, v, {}, native, st, clear) && st.xess && st.taa_reset && st.render_width == 640);
                CHECK(frame(g, r, o, v, {}, native, st, clear) && st.xess && !st.taa_reset);
                // Display resize: XeSS initialised again for the new output size.
                CHECK(frame(g, r, o2, render::make_view(cam, 320, 200), {}, ro, st, clear) && st.xess && st.taa_reset);
                CHECK(near_ratio(st.render_width, 320, UpscaleQuality::Quality) && near_ratio(st.render_height, 200, UpscaleQuality::Quality));
                CHECK(frame(g, r, o2, render::make_view(cam, 320, 200), {}, ro, st, clear) && st.xess && !st.taa_reset);
                same = true;
                for (size_t i = 0; i < static_cast<size_t>(o2.w) * o2.h; ++i) same = same && rgb_close_to(o2.data() + i * 4, clear, 3);
                CHECK(same);
                // Mode switches: TAA, nothing, FSR (where it runs), XeSS again.
                render::RecordOptions taa;
                taa.taa = true;
                CHECK(frame(g, r, o2, render::make_view(cam, 320, 200), {}, taa, st, clear) && !st.xess && st.taa && st.taa_reset &&
                      st.render_width == 320);
                render::RecordOptions off;
                CHECK(frame(g, r, o2, render::make_view(cam, 320, 200), {}, off, st, clear) && !st.xess && !st.taa);
                if (r.fsr_supported()) {
                    render::RecordOptions fsr = ro;
                    fsr.upscaler = Upscaler::Fsr;
                    CHECK(frame(g, r, o2, render::make_view(cam, 320, 200), {}, fsr, st, clear) && st.fsr && !st.xess && st.taa_reset);
                    CHECK(frame(g, r, o2, render::make_view(cam, 320, 200), {}, fsr, st, clear) && st.fsr && !st.taa_reset);
                    // FSR Quality is 1.5x, XeSS Quality 1.7x: another jitter cycle, so the history restarts on the switch.
                }
                CHECK(frame(g, r, o2, render::make_view(cam, 320, 200), {}, ro, st, clear) && st.xess && st.taa_reset);
                CHECK(frame(g, r, o2, render::make_view(cam, 320, 200), {}, ro, st, clear) && st.xess && !st.taa_reset);
            }
            g.ring.wait_idle();
        }
        o.destroy(g);
        o2.destroy(g);
        r.shutdown();
    }
    const char* root = std::getenv("ALAMO_FOC_DATA");
    if (!xess) {
        std::printf("real data: XeSS unavailable, land map skipped\n");
    } else if (root && *root) {
        land_map(g, src_dir, root);
    } else {
        std::printf("real data: ALAMO_FOC_DATA not set, land map skipped\n");
    }
    g.ring.wait_idle();
    const uint32_t errors = g.ctx.validation_errors(), warnings = g.ctx.validation_warnings();
    std::printf("gpu: validation %s, %u errors, %u warnings (%u more inside the upscaler runtimes' own calls, not counted)\n",
                g.ctx.validation_enabled() ? "on" : "off", errors, warnings, g.ctx.validation_third_party());
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
    std::printf("%s (%d failures)\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail ? 1 : 0;
}
