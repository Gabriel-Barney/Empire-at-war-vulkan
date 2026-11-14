// alamo_render --particles (Phase 4 roadmap R4: particle systems).
//   alamo_render --particles [--synthetic] [--game DIR] [--effects A,B,..] [--count N] [--frames N] [--png PATH]
//       Showcase: a grid of particle systems simulated (one 0.03 s update per frame = one sim tick, B.4) and drawn
//       through Renderer::render's scene hook, offscreen; the last frame goes to --png (default
//       logs/particles_showcase.png, logs/particles_synthetic_showcase.png without data). Shipped effects come from
//       --game DIR or ALAMO_FOC_DATA (the corruption folder): --effects names them, otherwise --count (12) particle files
//       spread evenly over the sorted Data\Art\Models list. One-shot systems are restarted when they finish.
//   alamo_render --particles --map NAME [--game DIR] [--frames N] [--png PATH]
//       A shipped map with its placements: the particle proxies of every placement / background object attached
//       (ProxyAttachments::sync_static), lit frame graph with post; PNG logs/particles_map.png once everything settled.
//   alamo_render --particles --sim-check [--ticks N]   (needs alamo_render_sim)
//       The synthetic sim map twice: without particles, then with particle proxies on every object (sync_sim_frame +
//       one update per tick) and a GPU frame per tick. Exit 0 only when the per-tick CRC logs are identical and
//       validation reported nothing. PNG logs/particles_sim.png.
// Common: --width W --height H, --hidden (accepted; every mode is offscreen), --detail D (Particle Detail 0..1).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <chrono>
#include <functional>
#include <memory>
#include <thread>
#include <string>
#include <vector>

#include "alamo/assets/asset_cache.hpp"
#include "alamo/core/ascii.hpp"
#include "alamo/core/file_provider.hpp"
#include "alamo/core/host_fs.hpp"
#include "alamo/core/log.hpp"
#include "alamo/render/particles/particle_attach.hpp"
#include "alamo/render/particles/particle_data.hpp"
#include "alamo/render/particles/particle_gpu.hpp"
#include "alamo/render/particles/particle_sim.hpp"
#include "alamo/render/particles/particle_synthetic.hpp"
#include "alamo/data/game_database.hpp"
#include "alamo/jobs/job_system.hpp"
#include "alamo/render/fleet.hpp"
#include "alamo/render/map_scene.hpp"
#include "alamo/render/pipelines.hpp"
#include "alamo/render/png.hpp"
#include "alamo/render/renderer.hpp"
#include "alamo/rhi/rhi.hpp"
#include "alamo/vfs/vfs.hpp"
#include "app_common.hpp"  // engine/render/app (CU4 t03)
#if defined(ALAMO_PARTICLES_HAVE_SIM)
#include "alamo/core/sim_crc.hpp"
#include "alamo/jobs/crc_recorder.hpp"
#include "alamo/jobs/job_system.hpp"
#include "alamo/render/particles/particle_sim_attach.hpp"
#include "alamo/render/sim_session.hpp"
#include "alamo/render/sim_snapshot.hpp"
#include "alamo/render/sim_view.hpp"
#endif

int run_particles_main(int argc, char** argv);

namespace {

using namespace alamo;
using namespace alamo::render::particles;

struct Opt {
    bool synthetic = false, sim_check = false, dump = false, no_draw = false;
    std::string game, png, map;
    std::vector<std::string> effects;
    uint32_t count = 12;
    long frames = 20;
    uint64_t ticks = 300;
    uint32_t width = 1280, height = 720;
    float detail = 1.0f;
};

using render_app::env_or_empty;  // app_common.hpp

int usage() {
    std::fprintf(stderr,
                 "usage: alamo_render --particles [--synthetic] [--game DIR] [--effects A,B,..] [--count N] [--frames N] [--png PATH]\n"
                 "       alamo_render --particles --map NAME [--game DIR] [--frames N] [--png PATH]\n"
                 "       alamo_render --particles --sim-check [--ticks N]\n"
                 "       common: [--width W] [--height H] [--detail D] [--hidden]\n");
    return 2;
}

// Offscreen GPU harness: context with validation (required when the layers are installed), renderer, frame ring.
struct Gpu {
    rhi::Context ctx;
    rhi::Allocator alloc;
    rhi::Uploader up;
    rhi::FrameRing ring;
    render::Renderer renderer;
    render::Offscreen target;  // RGBA8 colour + readback (app_common.hpp / alamo/render/offscreen.hpp)
    uint32_t w = 0, h = 0;
    bool ok = false;
    float clear[3] = {0.01f, 0.01f, 0.03f};

    render::DrawMode mode = render::DrawMode::Unlit;
    render::RecordOptions ro;
    render::DrawStats stats;

    bool init(uint32_t width, uint32_t height, render::EffectCache* effects = nullptr, jobs::JobSystem* js = nullptr) {
        ro.post = false;
        ro.shadows = false;
        const bool strict = render::setup_validation_env(ALAMO_RENDER_SOURCE_DIR);
        rhi::ContextDesc cd;
        cd.app_name = "alamo_render --particles";
        cd.validation = strict ? rhi::Validation::Required : rhi::Validation::IfAvailable;
        if (!ctx.init(cd)) {
            std::fprintf(stderr, "no Vulkan 1.3 device%s\n", strict ? " (validation required)" : "");
            return false;
        }
        rhi::FrameRingDesc fd;
        fd.frames_in_flight = 2;
        fd.uniform_bytes = effects ? (128ull << 20) : (32ull << 20);  // real maps: as alamo_render --sim
        if (js) fd.recording_threads = render::ring_recording_threads(1, js->thread_count());
        if (!alloc.init(ctx) || !up.init(ctx, alloc) || !ring.init(ctx, alloc, fd)) return false;
        render::RendererDesc rd;
        rd.color_format = VK_FORMAT_R8G8B8A8_UNORM;
        rd.translated = effects != nullptr;
        rd.effects = effects;
        rd.js = js;
        if (effects) rd.pipeline_cache_dir = render::default_pipeline_cache_dir();
        if (!renderer.init(ctx, alloc, up, rd)) return false;
        renderer.set_frame_ring(&ring);
        w = width;
        h = height;
        render::OffscreenDesc od;
        od.name = "particles.offscreen";
        if (!target.create(alloc, w, h, od)) return false;
        ok = true;
        return true;
    }
    // inst / sky are read after `before` ran (it may refill them).
    bool frame(const render::ViewMatrices& vm, const std::vector<render::Instance>& inst, const std::vector<render::Instance>& sky = {},
               const std::function<void()>& before = {}) {
        rhi::Frame* f = ring.begin_frame();
        if (!f) return false;
        VkCommandBuffer cmd = f->cmd();
        const rhi::DeviceFns& fn = ctx.vkd();
        const rhi::AcquireResult acq = up.record_acquires(cmd);
        if (before) before();  // residency / streaming after the acquires
        render::PassTarget pt;
        pt.color = target.color.handle;
        pt.color_view = target.color.view;
        pt.extent = {w, h};
        pt.clear[0] = clear[0];
        pt.clear[1] = clear[1];
        pt.clear[2] = clear[2];
        pt.color_after = rhi::ImageState::TransferSrc;
        const bool drawn = renderer.render(*f, cmd, pt, vm, std::span<const render::Instance>(inst), mode, ro, &stats, nullptr,
                                           std::span<const render::Instance>(sky));
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {w, h, 1};
        fn.vkCmdCopyImageToBuffer(cmd, target.color.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target.readback.handle, 1, &copy);
        const VkBufferMemoryBarrier2 hb = rhi::buffer_barrier(target.readback.handle, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                                              VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
        rhi::cmd_barrier(fn, cmd, {}, std::span<const VkBufferMemoryBarrier2>(&hb, 1));
        VkSemaphoreSubmitInfo wait = rhi::semaphore_info(up.timeline(), acq.wait_value, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
        rhi::FrameSubmit fs;
        if (acq.wait_value) fs.waits = {&wait, 1};
        if (!ring.end_frame(*f, fs)) return false;
        ring.wait_idle();
        alloc.invalidate(target.readback.alloc);
        return drawn;
    }
    bool png(const std::string& path) const {
        return target.write_png(path);
    }
    uint32_t lit_pixels() const {
        const uint8_t* px = target.data();
        uint8_t base[3];
        for (int c = 0; c < 3; ++c) base[c] = static_cast<uint8_t>(std::lround(clear[c] * 255.0f));
        uint32_t n = 0;
        for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i)
            if (std::abs(static_cast<int>(px[i * 4]) - static_cast<int>(base[0])) > 12 || std::abs(static_cast<int>(px[i * 4 + 1]) - static_cast<int>(base[1])) > 12 ||
                std::abs(static_cast<int>(px[i * 4 + 2]) - static_cast<int>(base[2])) > 12)
                ++n;
        return n;
    }
    // Returns the validation message count; tears everything down.
    uint32_t shutdown(ParticleRenderer& pr) {
        ring.wait_idle();
        ctx.wait_idle();
        renderer.set_scene_hook(nullptr);
        pr.shutdown();
        target.destroy(alloc);
        renderer.shutdown();
        ring.shutdown();
        up.shutdown();
        alloc.shutdown();
        const uint32_t v = ctx.validation_errors() + ctx.validation_warnings();
        ctx.shutdown();
        return v;
    }
};

// ------------------------------------------------------------------------------------------- dump
void dump_system(const SystemDef& s) {
    std::printf("%s: leave_particles %d, %zu emitters\n", s.name.c_str(), s.leave_particles ? 1 : 0, s.emitters.size());
    for (const EmitterDef& e : s.emitters) {
        std::printf("  [%u] %s tex %s bump %s blend %u cells %u sort %d heat %d tail %d(%.2f) bursts %d pps %u ppb %u delay %.3f count %u "
                    "init %.2f stop %.2f skip %.2f life %.2f (r %.2f) obj %d acc (%.1f %.1f %.1f) grav %.2f out %.2f/%.2f inherit %d(%.2f) "
                    "src %u pool %u parent %d reach %.0f\n",
                    e.index, e.name.c_str(), e.color_texture.c_str(), e.bump_texture.c_str(), e.blend_mode, e.texture_cells, e.depth_sort ? 1 : 0,
                    e.heat ? 1 : 0, e.has_tail ? 1 : 0, static_cast<double>(e.tail_size), e.use_bursts ? 1 : 0, e.particles_per_second,
                    e.particles_per_burst, static_cast<double>(e.burst_delay), e.burst_count, static_cast<double>(e.initial_delay),
                    static_cast<double>(e.stop_time), static_cast<double>(e.skip_time), static_cast<double>(e.lifetime),
                    static_cast<double>(e.random_lifetime), e.object_space ? 1 : 0, static_cast<double>(e.acceleration[0]),
                    static_cast<double>(e.acceleration[1]), static_cast<double>(e.acceleration[2]), static_cast<double>(e.gravity),
                    static_cast<double>(e.outward_speed), static_cast<double>(e.outward_acceleration), e.inherit_velocity ? 1 : 0,
                    static_cast<double>(e.inherit_velocity_factor), e.spawn_source, e.pool, e.parent, static_cast<double>(e.reach));
        std::printf("      vel kind %u (r %.2f box %.1f..%.1f pt %.1f %.1f %.1f) pos kind %u (r %.2f cyl %.2f/%.2f box %.1f..%.1f) rcolor %.2f %.2f %.2f %.2f rot %.2f spin %.2f\n",
                    e.velocity.kind, static_cast<double>(e.velocity.sphere_radius), static_cast<double>(e.velocity.box_min[2]),
                    static_cast<double>(e.velocity.box_max[2]), static_cast<double>(e.velocity.point[0]), static_cast<double>(e.velocity.point[1]),
                    static_cast<double>(e.velocity.point[2]), e.position.kind, static_cast<double>(e.position.sphere_radius),
                    static_cast<double>(e.position.cylinder_radius), static_cast<double>(e.position.cylinder_height),
                    static_cast<double>(e.position.box_min[0]), static_cast<double>(e.position.box_max[0]), static_cast<double>(e.random_color[0]),
                    static_cast<double>(e.random_color[1]), static_cast<double>(e.random_color[2]), static_cast<double>(e.random_color[3]),
                    static_cast<double>(e.random_initial_rotation), static_cast<double>(e.random_rotation_speed));
        static const char* names[kTrackCount] = {"R", "G", "B", "A", "S", "T", "W"};
        for (uint32_t t = 0; t < kTrackCount; ++t) {
            std::printf("      %s(m%u):", names[t], e.tracks[t].interpolation);
            for (uint32_t i = 0; i < kTrackSamples; i += 4) std::printf(" %.3g", static_cast<double>(e.tracks[t].v[i]));
            std::printf(" | %.3g\n", static_cast<double>(e.tracks[t].v[kTrackSamples - 1]));
        }
    }
}

// --------------------------------------------------------------------------------------- showcase
int run_showcase(const Opt& opt) {
    std::unique_ptr<vfs::Vfs> files;
    const std::string root = opt.synthetic ? std::string() : (opt.game.empty() ? env_or_empty("ALAMO_FOC_DATA") : opt.game);
    if (!root.empty()) {
        vfs::VfsConfig cfg;
        cfg.foc_root = root;
        std::string err;
        files = vfs::Vfs::create(cfg, &err);
        if (!files) {
            std::fprintf(stderr, "VFS: %s\n", err.c_str());
            return 1;
        }
    }
    ParticleLibrary lib(files.get());
    std::vector<SystemPtr> systems;
    if (!files) {
        systems = synthetic_systems();
    } else {
        std::vector<std::string> names = opt.effects;
        if (names.empty()) {  // curated FoC set (explosions, fire / smoke, engines, weapons, abilities)
            for (const char* n : {"P_EXPLOSION_MED01", "P_IMPERIAL_EXPLOSION_BIG00", "P_MAGNA_EXPLOSION", "P_CARBONITE_EXPLOSION",
                                  "P_FIRE_BILLOW7", "P_SSD_FIRESMOKEBAR_MED", "P_SMOGSMOKE00", "P_EWOKONFIRE",
                                  "PE_STARDESTROYERENGINES_THRAWN", "P_PROTONSHOT", "P_URAI_BLADE_SPARKS", "P_ELECTRICALSTATIC",
                                  "P_GREEN_RINGS3", "P_KRAYT_CHARGEUP", "P_WEAPON_BUILDUP_BLUE", "P_SPRAY1"})
                if (lib.get(n)) names.push_back(n);
            if (names.size() < 4) names.clear();
        }
        if (names.empty()) {
            std::vector<std::string> all;
            for (const std::string& p : files->list("Data\\Art\\Models\\*.alo")) {
                std::vector<uint8_t> b;
                if (!files->read(p, b) || !is_particle_file(b)) continue;
                std::string n = ascii_upper(p.substr(p.find_last_of("\\/") + 1));
                n.resize(n.size() - 4);
                all.push_back(n);
            }
            std::sort(all.begin(), all.end());
            const size_t k = std::min<size_t>(opt.count, all.size());
            for (size_t i = 0; i < k; ++i) names.push_back(all[i * all.size() / k]);
        }
        for (const std::string& n : names) {
            SystemPtr s = lib.get(n);
            if (s) systems.push_back(s);
            else std::fprintf(stderr, "effect %s: not a particle file, skipped\n", n.c_str());
        }
    }
    if (systems.empty()) {
        std::fprintf(stderr, "no particle systems to show\n");
        return 1;
    }
    if (opt.dump) {
        for (const SystemPtr& s : systems) dump_system(*s);
        return 0;
    }
    Gpu g;
    if (!g.init(opt.width, opt.height)) return 1;
    g.clear[0] = g.clear[1] = g.clear[2] = 0.18f;  // neutral grey: dark alpha smoke stays visible
    std::unique_ptr<assets::AssetCache> cache = files ? std::make_unique<assets::AssetCache>(*files) : nullptr;
    ParticleWorld world(1u << 20);
    world.set_detail(opt.detail);
    ParticleRenderer pr;
    ParticleTextureSource tex;
    if (cache) tex = [&](std::string_view n) { return cache->texture(n, assets::TextureRole::Color); };
    if (!pr.init(g.ctx, g.alloc, g.up, world, tex)) {
        g.shutdown(pr);
        return 1;
    }
    g.renderer.set_scene_hook(&pr);
    // Grid: columns x rows, spacing from the largest reach (clamped), camera framing the grid.
    const auto n = static_cast<uint32_t>(systems.size());
    const uint32_t cols = std::max<uint32_t>(1, static_cast<uint32_t>(std::ceil(std::sqrt(static_cast<double>(n) * 16.0 / 9.0))));
    const uint32_t rows = (n + cols - 1) / cols;
    // Spacing: the median system's visual radius (spawn shape + largest particle + half its travel), clamped.
    std::vector<float> radius;
    for (const SystemPtr& s : systems) {
        float r = 0.0f;
        for (const EmitterDef& e : s->emitters) {
            float size = 0.0f;
            for (float v : e.tracks[kScale].v) size = std::max(size, std::fabs(v));
            const float travel = (e.velocity.extent() + std::fabs(e.outward_speed)) * e.lifetime;
            r = std::max(r, e.position.extent() + size + 0.5f * travel);
        }
        radius.push_back(r);
    }
    std::sort(radius.begin(), radius.end());
    const float spacing = std::clamp(1.6f * radius[radius.size() / 2], 60.0f, 600.0f);
    std::vector<assets::Mat3x4> where(n);
    std::vector<ParticleWorld::Handle> hs(n);
    for (uint32_t i = 0; i < n; ++i) {
        where[i].m[0][3] = (static_cast<float>(i % cols) - 0.5f * static_cast<float>(cols - 1)) * spacing;
        where[i].m[2][3] = (0.5f * static_cast<float>(rows - 1) - static_cast<float>(i / cols)) * spacing;
        hs[i] = world.create(systems[i], where[i]);
    }
    render::OrbitCamera cam;
    cam.target = {0, 0, 0};
    cam.yaw = -1.5707963f;  // camera on -Y looking +Y at the XZ grid
    cam.pitch = 0.0f;
    cam.frame({0, 0, 0}, 0.5f * spacing * std::sqrt(static_cast<float>(cols * cols + rows * rows)), static_cast<float>(g.w) / static_cast<float>(g.h));
    float zn = 1.0f, zf = 10000.0f;
    cam.clip_planes(zn, zf);
    const render::ViewMatrices vm = render::make_view(cam, g.w, g.h, std::max(zn, 0.5f), zf * 4.0f);
    pr.set_sort_range(zf * 4.0f);
    bool ok = true;
    uint32_t restarts = 0, max_draws = 0;
    for (long f = 0; f < opt.frames; ++f) {
        for (uint32_t i = 0; i < n; ++i)
            if (!world.valid(hs[i])) {  // one-shot finished: restart
                hs[i] = world.create(systems[i], where[i]);
                ++restarts;
            }
        world.update(ParticleWorld::tick_delta(1));
        ok = g.frame(vm, {}, {}) && ok;
        max_draws = std::max(max_draws, pr.last_stats().draw_calls);
    }
    const ParticleFrameStats st = pr.last_stats();
    const WorldStats ws = world.stats();
    std::printf("particles: %u systems (%s), %u frames: %u drawn, %u culled, %u draw calls (max %u), %u sorted emitters, "
                "%u live particles, %llu spawned, %llu dropped, %u restarts, %u textures (%u pending)\n",
                n, files ? "shipped" : "synthetic", static_cast<unsigned>(opt.frames), st.systems_drawn, st.systems_culled,
                st.draw_calls, max_draws, st.sorted_emitters, ws.live_particles, static_cast<unsigned long long>(ws.spawned),
                static_cast<unsigned long long>(ws.dropped), restarts, st.textures, st.textures_pending);
    for (const SystemPtr& s : systems) std::printf("  %s (%zu emitters)\n", s->name.c_str(), s->emitters.size());
    const std::string png = !opt.png.empty() ? opt.png
                                             : std::string(ALAMO_RENDER_SOURCE_DIR) +
                                                   (files ? "/logs/particles_showcase.png" : "/logs/particles_synthetic_showcase.png");
    const uint32_t lit = g.lit_pixels();
    ok = ok && g.png(png) && lit > 1000 && max_draws > 0;
    std::printf("%s %s (%u lit pixels)\n", ok ? "wrote" : "FAILED", png.c_str(), lit);
    const uint32_t vmsg = g.shutdown(pr);
    std::printf("validation: %u messages\n", vmsg);
    return ok && vmsg == 0 ? 0 : 1;
}

// ------------------------------------------------------------------------------------- map placements
// A shipped map with its object placements (MapScene): every placement's (and background object's) ALO proxies get
// their particle systems (ProxyAttachments::sync_static), lit frame graph with post (heat bucket), one particle update
// per frame; PNG after the models settled and --frames more frames.
int run_map(const Opt& opt) {
    const std::string root = opt.game.empty() ? env_or_empty("ALAMO_FOC_DATA") : opt.game;
    if (root.empty()) {
        std::fprintf(stderr, "--map needs --game DIR or ALAMO_FOC_DATA\n");
        return 2;
    }
    vfs::VfsConfig cfg;
    cfg.foc_root = root;
    std::string err;
    auto files = vfs::Vfs::create(cfg, &err);
    if (!files) {
        std::fprintf(stderr, "VFS: %s\n", err.c_str());
        return 1;
    }
    data::GameDatabase gdb;
    if (!gdb.load(*files)) std::fprintf(stderr, "game database: load reported errors (continuing)\n");
    render::GameTypeResolver resolver(gdb);
    resolver.load_render_xml(*files);
    jobs::JobSystem js(jobs::JobSystemConfig{-1});
    render::EffectCache effects(files.get());
    Gpu g;
    if (!g.init(opt.width, opt.height, &effects, &js)) return 1;
    g.mode = render::DrawMode::Lit;
    g.ro.post = true;
    g.ro.shadows = true;
    g.ro.threads = js.worker_count();
    assets::AssetCache cache(*files, effects.model_load_options());
    auto streamer = std::make_unique<render::ModelStreamer>(g.renderer, cache, js);
    render::MapScene scene;
    ParticleWorld world(1u << 21);
    world.set_detail(opt.detail);
    ParticleRenderer pr;
    ParticleLibrary lib(files.get());
    if (!scene.load(*files, opt.map, &resolver, jobs::make_parallel_for(js), &err) || !scene.start(g.renderer, *streamer, cache, {}, &err) ||
        !pr.init(g.ctx, g.alloc, g.up, world, [&](std::string_view n) { return cache.texture(n, assets::TextureRole::Color); })) {
        std::fprintf(stderr, "--map %s: %s\n", opt.map.c_str(), err.c_str());
        g.ring.wait_idle();
        scene.stop(g.renderer);
        streamer->destroy_all();
        g.shutdown(pr);
        return 1;
    }
    scene.apply(g.ro);
    g.renderer.set_scene_hook(opt.no_draw ? nullptr : &pr);
    const Float3 cc = scene.clear_color();
    g.clear[0] = cc.x;
    g.clear[1] = cc.y;
    g.clear[2] = cc.z;
    const render::OrbitCamera cam = scene.tactical_camera(static_cast<float>(g.w) / static_cast<float>(g.h));
    const render::ViewMatrices vm = render::make_view(cam, g.w, g.h, render::MapScene::kNear, render::MapScene::kFar);
    ProxyAttachments attach(world, lib);
    std::vector<render::Instance> main, sky;
    bool ok = true;
    long after_settle = -1;
    uint64_t tick = 0;
    for (int i = 0; i < 3000 && after_settle < opt.frames; ++i) {
        ok = g.frame(vm, main, sky, [&] {
            streamer->update();
            scene.update(g.renderer, *streamer);
            scene.set_sim_tick(++tick);
            main.assign(scene.instances().begin(), scene.instances().end());
            sky.assign(scene.sky().begin(), scene.sky().end());
            attach.begin();
            attach.sync_static(main, 1);
            attach.sync_static(sky, 2);
            attach.end();
            world.update(ParticleWorld::tick_delta(1));
        }) && ok;
        const bool settled = scene.ready() && g.stats.pending_pipeline == 0 && g.stats.translation_pending == 0;
        if (settled) ++after_settle;
        else std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const AttachStats as = attach.stats();
    const WorldStats ws = world.stats();
    const ParticleFrameStats ps = pr.last_stats();
    std::printf("particles --map %s: %zu instances + %zu sky; proxies %u (%u with a particle file, %u resolved only after stripping "
                "_ALT/_LOD, %u unresolved, %u hidden); %u systems, %u live particles, %llu spawned; last frame %u drawn / %u culled, "
                "%u draw calls (%u transparent, %u water, %u heat), %u sorted; %u mesh draws\n",
                opt.map.c_str(), main.size(), sky.size(), as.proxies, as.particle_proxies, as.stripped, as.unresolved, as.hidden, ws.systems,
                ws.live_particles, static_cast<unsigned long long>(ws.spawned), ps.systems_drawn, ps.systems_culled, ps.draw_calls,
                ps.transparent_draws, ps.water_draws, ps.heat_draws, ps.sorted_emitters, g.stats.draws);
    const std::string png = !opt.png.empty() ? opt.png : std::string(ALAMO_RENDER_SOURCE_DIR) + "/logs/particles_map.png";
    ok = ok && after_settle >= 0 && g.png(png);
    std::printf("%s %s\n", ok ? "wrote" : "FAILED", png.c_str());
    attach.clear();
    g.ring.wait_idle();
    scene.stop(g.renderer);
    streamer->destroy_all();
    streamer.reset();
    const uint32_t vmsg = g.shutdown(pr);
    std::printf("validation: %u messages\n", vmsg);
    return ok && vmsg == 0 ? 0 : 1;
}

// -------------------------------------------------------------------------------------- sim check
#if defined(ALAMO_PARTICLES_HAVE_SIM)
struct ProxyBoxModels final : render::SimModelSource {
    const render::GpuModel* model = nullptr;
    const render::SnapshotTypeTable* table = nullptr;
    const render::GpuModel* type_model(objects::TypeIndex t) override {
        return table && table->valid(t) && !(*table)[t].model.empty() ? model : nullptr;
    }
    const render::GpuModel* named_model(const std::string&) override { return nullptr; }
};

int run_sim_check(const Opt& opt) {
    std::string err;
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "alamo_render_particles_sim";
    std::error_code ec;
    remove_all_long(dir, ec);
    const auto w = render::make_synthetic_sim_world(path_to_utf8(dir), &err);
    if (!w) {
        std::fprintf(stderr, "synthetic world: %s\n", err.c_str());
        return 1;
    }
    render::SnapshotTypeTable table;
    table.build(w->types, objects::ModeKind::Space);
    const uint32_t seed = 3;
    CrcLog ref;
    {
        jobs::JobSystem js(jobs::JobSystemConfig{-1});
        render::SimSessionSpec ss;
        ss.seed = seed;
        render::SimSession s(w->types, js, w->map, *w->index, ss);
        for (uint64_t t = 0; t < opt.ticks && s.tick(); ++t) {
        }
        ref = s.host().crc_log();
    }
    Gpu g;
    if (!g.init(opt.width, opt.height)) return 1;
    auto box = render::debug_box_model(render::kSyntheticModel);
    box->proxies = {{"SYN_FIRE", 0, false}, {"SYN_SMOKE_ALT0", 0, false}, {"SYN_SPARKS", 0, false}, {"P_UNRESOLVED", 0, false}};
    render::GpuModel model;
    if (!g.renderer.upload_model(box, [](std::string_view, assets::TextureRole) { return std::shared_ptr<const assets::TextureAsset>(); },
                                 model, nullptr, &err)) {
        std::fprintf(stderr, "box upload: %s\n", err.c_str());
        return 1;
    }
    ProxyBoxModels models;
    models.model = &model;
    models.table = &table;
    ParticleLibrary lib;
    for (const SystemPtr& p : synthetic_systems()) {  // particles x 8 so they read at the sim camera's distance
        auto big = std::make_shared<SystemDef>(*p);
        for (EmitterDef& e : big->emitters)
            for (float& v : e.tracks[kScale].v) v *= 8.0f;
        finalize_system(*big);
        lib.add(big);
    }
    ParticleWorld world(1u << 20);
    world.set_detail(opt.detail);
    ParticleRenderer pr;
    if (!pr.init(g.ctx, g.alloc, g.up, world, nullptr)) {
        g.renderer.destroy_model(model);
        g.shutdown(pr);
        return 1;
    }
    g.renderer.set_scene_hook(&pr);
    pr.set_enabled(!opt.no_draw);
    ProxyAttachments attach(world, lib);
    jobs::JobSystem js(jobs::JobSystemConfig{-1});
    render::SimSessionSpec ss;
    ss.seed = seed;
    render::SimSession s(w->types, js, w->map, *w->index, ss);
    render::SnapshotBuffer buf;
    render::InterpolatedFrame frame;
    render::SimInstanceBuilder builder(table);
    std::vector<render::Instance> fg, bg;
    render::OrbitCamera cam;
    cam.target = {0, 0, 0};
    cam.yaw = -1.5707963f;
    cam.pitch = 1.0f;
    cam.distance = 4500.0f;
    cam.fov_x = render::tactical_fov_x(55.0f, static_cast<float>(g.w) / static_cast<float>(g.h));
    const render::ViewMatrices vm = render::make_view(cam, g.w, g.h, 10.0f, 50000.0f);
    pr.set_sort_range(50000.0f);
    bool ok = true;
    SimAttachStats as;
    uint32_t max_draws = 0;
    for (uint64_t t = 0; t < opt.ticks && s.tick(); ++t) {
        auto snap = std::make_shared<render::SimSnapshot>();
        render::capture_snapshot(s.host(), table, *snap, jobs::make_parallel_for(js));
        buf.publish(std::move(snap));
        const render::SnapshotBuffer::Pair p = buf.latest();
        render::interpolate(p.prev.get(), *p.curr, 1.0f, render::InterpMode::Smooth, frame);
        builder.build(frame, models, fg, bg);
        as = sync_sim_frame(attach, frame, table, models, builder);
        world.update(ParticleWorld::tick_delta(1));  // one sim tick in this loop iteration
        g.renderer.make_resident(model);
        ok = g.frame(vm, fg) && ok;
        max_draws = std::max(max_draws, pr.last_stats().draw_calls);
    }
    const CrcDivergence dv = first_divergence(ref, s.host().crc_log());
    const bool same = !dv.diverged && ref.records.size() == s.host().crc_log().records.size() && !ref.records.empty();
    const WorldStats ws = world.stats();
    std::printf("particles sim check (%llu ticks): CRC logs %s (%zu records; %s); %u objects synced, %u systems, %u live particles, "
                "%llu spawned, max %u particle draw calls; attach: %u unresolved proxies\n",
                static_cast<unsigned long long>(opt.ticks), same ? "IDENTICAL" : "DIFFER", ref.records.size(), dv.message.c_str(), as.synced,
                ws.systems, ws.live_particles, static_cast<unsigned long long>(ws.spawned), max_draws, attach.stats().unresolved);
    const std::string png = std::string(ALAMO_RENDER_SOURCE_DIR) + "/logs/particles_sim.png";
    ok = ok && g.png(png);
    std::printf("%s %s\n", ok ? "wrote" : "FAILED", png.c_str());
    attach.clear();
    g.ring.wait_idle();
    g.renderer.destroy_model(model);
    const uint32_t vmsg = g.shutdown(pr);
    std::printf("validation: %u messages\n", vmsg);
    return ok && same && vmsg == 0 && max_draws > 0 && ws.spawned > 0 ? 0 : 1;
}
#endif

}  // namespace

int run_particles_main(int argc, char** argv) {
    Opt opt;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--particles" || a == "--hidden") continue;
        if (a == "--synthetic") opt.synthetic = true;
        else if (a == "--sim-check") opt.sim_check = true;
        else if (a == "--dump") opt.dump = true;
        else if (a == "--no-draw") opt.no_draw = true;  // simulate, draw nothing (B.5 particles-enabled flag off)
        else if (a == "--game") opt.game = next();
        else if (a == "--map") opt.map = next();
        else if (a == "--png") opt.png = next();
        else if (a == "--count") opt.count = static_cast<uint32_t>(std::strtoul(next().c_str(), nullptr, 10));
        else if (a == "--frames") opt.frames = std::atol(next().c_str());
        else if (a == "--ticks") opt.ticks = std::strtoull(next().c_str(), nullptr, 10);
        else if (a == "--width") opt.width = static_cast<uint32_t>(std::strtoul(next().c_str(), nullptr, 10));
        else if (a == "--height") opt.height = static_cast<uint32_t>(std::strtoul(next().c_str(), nullptr, 10));
        else if (a == "--detail") opt.detail = std::strtof(next().c_str(), nullptr);
        else if (a == "--effects") {
            const std::string list = next();
            size_t s = 0;
            while (s <= list.size()) {
                const size_t e = std::min(list.find(',', s), list.size());
                if (e > s) opt.effects.push_back(list.substr(s, e - s));
                s = e + 1;
            }
        } else {
            std::fprintf(stderr, "unknown argument %s\n", a.c_str());
            return usage();
        }
    }
    log::set_min_level(log::Level::Warn);
    if (opt.width < 16 || opt.height < 16 || opt.frames < 1) return usage();
    if (opt.sim_check) {
#if defined(ALAMO_PARTICLES_HAVE_SIM)
        return run_sim_check(opt);
#else
        std::fprintf(stderr, "--sim-check needs alamo_render_sim (apps / objects / tactical modules)\n");
        return 2;
#endif
    }
    if (!opt.map.empty()) return run_map(opt);
    return run_showcase(opt);
}
