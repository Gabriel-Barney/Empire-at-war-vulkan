// alamo_render soft shadow tests [port] (no framework; non-zero exit on failure). Playtest 3: with Soft Shadows on, the whole
// land frame was darkened. Cause: StencilDarkenFinalBlur multiplies the frame by the blurred ALPHA of the back buffer
// (alpha 0 = shadowed), and the port's passes write alpha (texture alpha of terrain, units, decorations, water, particles),
// so every pixel whose alpha ended below 1 took the shadow colour. The original keeps unshadowed alpha at 1 (RGB-only writes,
// TERRAIN_WATER_POST.md 4.3).
//   GPU (skips without a Vulkan 1.3 device): a plane under a shadow volume whose pass leaves alpha < 1 outside the shadow
//     (an alpha-writing overlay quad): the soft frame must equal the hard frame outside the shadow and darken the shadow.
//   Detail options [port] (t11): output gamma over a flat frame (identity at 1, c^(1/g) otherwise, alpha kept), Shader detail
//     0 (no shadow volumes, hard darken) and Water detail (reflection / refraction targets recorded: 2 both, 1 reflection, 0 none).
//   Real data (ALAMO_FOC_DATA): _MP_LAND_ENDOR, shadows off / hard / soft at the same camera -> logs/render_shadow_*.png; the
//     soft frame must not be darker than the hard one anywhere beyond the blur (fraction of pixels darker than hard by > 8 %
//     of the luma stays under 1.5 %) and its mean luma stays within 3 % of hard.
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
#include "alamo/render/world.hpp"
#include "alamo/rhi/rhi.hpp"
#include "alamo/vfs/vfs.hpp"
#include "alamo/test/support.hpp"
#include "alamo/render/test/gpu.hpp"

using namespace alamo;

namespace {

using ::alamo::test::g_fail;

using Gpu = render::test::GpuCore;

using Offscreen = render::test::OffscreenColorOnly;  // alamo/render/offscreen.hpp

bool frame(Gpu& g, render::Renderer& r, Offscreen& o, const render::ViewMatrices& vm, std::span<const render::Instance> inst,
           const render::RecordOptions& ro, render::DrawStats& st, const float (&clear)[4], std::span<const render::Instance> sky = {},
           float gamma = 1.0f, bool* gamma_ok = nullptr) {
    rhi::Frame* f = g.ring.begin_frame();
    if (!f) return false;
    VkCommandBuffer cmd = f->cmd();
    const rhi::DeviceFns& fn = g.ctx.vkd();
    const rhi::AcquireResult acq = g.up.record_acquires(cmd);
    render::PassTarget pt;
    pt.color = o.color.handle;
    pt.color_view = o.color.view;
    pt.extent = {o.w, o.h};
    std::memcpy(pt.clear, clear, sizeof pt.clear);
    pt.color_after = rhi::ImageState::TransferSrc;
    const bool drawn = r.render(*f, cmd, pt, vm, inst, render::DrawMode::Lit, ro, &st, nullptr, sky);
    // The game's order: world, HUD, gamma over the finished frame (the HUD target stays in TransferSrc).
    const bool gok = r.apply_gamma(*f, cmd, o.color.handle, o.color.view, VK_FORMAT_R8G8B8A8_UNORM, {o.w, o.h},
                                   rhi::ImageState::TransferSrc, rhi::ImageState::TransferSrc, gamma);
    if (gamma_ok) *gamma_ok = gok;
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
    return drawn;
}

double luma(const uint8_t* p) { return 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2]; }

bool save_png(const std::string& path, const std::vector<uint8_t>& img, uint32_t w, uint32_t h) {
    std::vector<uint8_t> rgba = img;
    for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
    return render::write_png_rgba(path, w, h, rgba);
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
    rd.pipeline_cache_dir = std::filesystem::temp_directory_path() / "alamo_render_test_shadow_pso";
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
        const char* map = std::getenv("ALAMO_SHADOW_TEST_MAP");
        if (!map || !*map) map = "_MP_LAND_ENDOR";
        const bool loaded = scene.load(*files, map, &resolver, pf, &err);
        CHECK(loaded);
        if (loaded && scene.start(rr, streamer, cache, {}, &err)) {
            rr.set_object_fade(render::MapScene::kLandObjectFadeBegin, render::MapScene::kLandObjectFadeEnd);
            const render::Float3 cc = scene.clear_color();
            const float clear[4] = {cc.x, cc.y, cc.z, 1.0f};
            render::OrbitCamera cam = scene.tactical_camera(static_cast<float>(W) / static_cast<float>(H));
            cam.distance *= 0.6f;
            render::RecordOptions ro;
            ro.threads = js.thread_count();
            ro.bloom_strength = 0.0f;  // isolate the darken
            render::DrawStats st;
            const render::ViewMatrices vm = render::make_view(cam, W, H, render::MapScene::kNear, render::MapScene::kFar);
            for (int k = 0; k < 3000 && !scene.ready(); ++k) {
                streamer.update();
                scene.update(rr, streamer);
                CHECK(frame(g, rr, o, vm, scene.instances(), ro, st, clear, scene.sky()));
            }
            bool done = false;
            for (int k = 0; k < 200 && !done; ++k) {
                streamer.update();
                scene.update(rr, streamer);
                render::RecordOptions soft = ro;
                soft.soft_shadows = true;
                CHECK(frame(g, rr, o, vm, scene.instances(), soft, st, clear, scene.sky()));
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
            std::vector<uint8_t> img[3];  // off, hard, soft
            uint32_t volumes[3] = {};
            for (int mode = 0; mode < 3; ++mode) {
                render::RecordOptions r2 = ro;
                r2.shadows = mode != 0;
                r2.soft_shadows = mode == 2;
                for (int k = 0; k < 64; ++k) {  // until the mode's darken / blur pipelines exist
                    CHECK(frame(g, rr, o, vm, scene.instances(), r2, st, clear, scene.sky()));
                    if (st.post_pending == 0 && st.pending_pipeline == 0 && st.translation_pending == 0) break;
                    rr.wait_translations();
                    rr.pipelines().wait_idle();
                }
                CHECK(st.post_pending == 0);
                volumes[mode] = st.shadow_volumes;
                CHECK(mode == 0 || st.darkened == (st.shadow_volumes > 0));
                CHECK(mode != 2 || st.soft_shadows == (st.shadow_volumes > 0));
                img[mode].assign(o.data(), o.data() + bytes);
            }
            CHECK(save_png(src_dir + "/logs/render_shadow_off.png", img[0], W, H));
            CHECK(save_png(src_dir + "/logs/render_shadow_hard.png", img[1], W, H));
            CHECK(save_png(src_dir + "/logs/render_shadow_soft.png", img[2], W, H));
            double mean[3] = {0, 0, 0};
            size_t darker = 0, soft_darker = 0;
            for (size_t i = 0; i + 3 < bytes; i += 4) {
                for (int m = 0; m < 3; ++m) mean[m] += luma(&img[m][i]);
                const double off = luma(&img[0][i]), hard = luma(&img[1][i]), soft = luma(&img[2][i]);
                if (hard < off * 0.92) ++darker;
                if (soft < hard * 0.92 - 2.0) ++soft_darker;
            }
            const double n = static_cast<double>(bytes / 4);
            for (double& m : mean) m /= n;
            std::printf("land map shadows: volumes off/hard/soft %u/%u/%u; mean luma off %.2f / hard %.2f / soft %.2f; pixels darkened by hard %.2f %%, "
                        "pixels darker in soft than hard %.2f %%\n",
                        volumes[0], volumes[1], volumes[2], mean[0], mean[1], mean[2], 100.0 * static_cast<double>(darker) / n,
                        100.0 * static_cast<double>(soft_darker) / n);
            // Shader detail 0 = the fixed-function path: no shadow volumes, hard darken only.
            {
                render::RecordOptions fixed = ro;
                fixed.soft_shadows = true;
                fixed.shader_detail = 0;
                for (int k = 0; k < 8; ++k) CHECK(frame(g, rr, o, vm, scene.instances(), fixed, st, clear, scene.sky()));
                CHECK(st.shadow_volumes == 0 && !st.darkened && !st.soft_shadows);
                std::vector<uint8_t> f(o.data(), o.data() + bytes);
                size_t differ = 0;
                for (size_t i = 0; i + 3 < bytes; i += 4) differ += std::abs(luma(&f[i]) - luma(&img[0][i])) > 2.0 ? 1u : 0u;
                std::printf("shader detail 0: %.2f %% of the pixels differ from shadows off\n", 100.0 * static_cast<double>(differ) / (static_cast<double>(bytes) / 4.0));
                fixed.shader_detail = 1;  // dx8: shadows are back (soft needs the blur effect, any level >= 1 keeps it)
                for (int k = 0; k < 8; ++k) CHECK(frame(g, rr, o, vm, scene.instances(), fixed, st, clear, scene.sky()));
                CHECK(st.shadow_volumes > 0 && st.darkened);
            }
            // Water detail: the reflection / refraction targets the frame records (shader detail < 3 caps it).
            const render::WorldStats& wst = scene.world().stats();
            if (wst.water_patches > 0) {
                struct Case { int water, shader; bool refl, refr; };
                const Case cases[] = {{2, 3, true, true}, {1, 3, true, false}, {0, 3, false, false}, {2, 2, true, false}, {2, 0, false, false}};
                for (const Case& c : cases) {
                    render::RecordOptions wo = ro;
                    wo.water_detail = c.water;
                    wo.shader_detail = c.shader;
                    for (int k = 0; k < 4; ++k) CHECK(frame(g, rr, o, vm, scene.instances(), wo, st, clear, scene.sky()));
                    CHECK(wst.reflection == c.refl && wst.refraction == c.refr);
                }
            } else {
                std::printf("water detail: %s has no water, skipped\n", map);
            }
            CHECK(volumes[1] > 0);  // the camera sees shadow casters
            CHECK(mean[2] > mean[1] * 0.97);
            CHECK(static_cast<double>(soft_darker) / n < 0.015);
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
    cd.app_name = "alamo_render_shadow_tests";
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
    {   // ---- output gamma over a flat frame
        render::Renderer r;
        render::RendererDesc rd;
        rd.color_format = VK_FORMAT_R8G8B8A8_UNORM;
        rd.translated = false;
        const bool ok = r.init(g.ctx, g.alloc, g.up, rd);
        CHECK(ok);
        Offscreen o;
        CHECK(o.create(g, 64, 48));
        if (ok) {
            r.set_frame_ring(&g.ring);
            render::OrbitCamera cam;
            const render::ViewMatrices v = render::make_view(cam, 64, 48);
            const float clear[4] = {0.25f, 0.5f, 1.0f, 1.0f};
            render::RecordOptions ro;
            render::DrawStats st;
            auto mid = [&](float gamma, bool* gok) {
                CHECK(frame(g, r, o, v, {}, ro, st, clear, {}, gamma, gok));
                return o.data() + (static_cast<size_t>(24) * 64 + 32) * 4;
            };
            bool gok = false;
            const uint8_t* p = mid(1.0f, &gok);
            CHECK(gok && std::abs(p[0] - 64) <= 1 && std::abs(p[1] - 128) <= 1 && p[2] == 255);  // identity: untouched
            p = mid(2.0f, &gok);  // brighter: c^(1/2)
            std::printf("gamma 2: (%d,%d,%d) alpha %d\n", p[0], p[1], p[2], p[3]);
            CHECK(gok && std::abs(p[0] - 128) <= 2 && std::abs(p[1] - 180) <= 2 && p[2] == 255);
            p = mid(0.5f, &gok);  // darker: c^2
            CHECK(gok && std::abs(p[0] - 16) <= 2 && std::abs(p[1] - 64) <= 2 && p[2] == 255);
            const uint8_t alpha = p[3];
            CHECK(alpha == 255);
            p = mid(1.0f, &gok);  // back to identity: the scratch copy of the last pass leaves no trace
            CHECK(std::abs(p[0] - 64) <= 1);
            g.ring.wait_idle();
        }
        o.destroy(g);
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
    g.ring.shutdown();
    g.up.shutdown();
    g.alloc.shutdown();
    g.ctx.shutdown();
    return 0;
}

}  // namespace

int main() {
    run_gpu(ALAMO_RENDER_SOURCE_DIR);
    std::printf("%s (%d failures)\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail ? 1 : 0;
}
