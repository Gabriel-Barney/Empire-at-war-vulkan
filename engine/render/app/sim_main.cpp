// alamo_render --sim / --sim-headless-check (Phase 4 roadmap R1: sim -> render snapshot).
//   alamo_render --sim [--map NAME] [--game DIR] [--ticks N] [--seed S] [--interp original|smooth] [--png PATH]
//       Runs a tactical map through alamo_sim (SimSession = sim::run_map's order: spawn, AI stand-ins, random UI
//       orders between ticks) with the real-time FrameSynchronizer pacing, and renders it live in a window: after
//       every tick an immutable SimSnapshot is captured and published (SnapshotBuffer); every frame interpolates
//       the two latest snapshots (original mode = latest as is, the default; smooth = blend by the tick alpha) and
//       turns them into render::Instance lists. Land maps draw the map's terrain / water / decorations and sky
//       (MapScene with its own placements off - the sim's objects replace them), space maps the background scene.
//       Without --map (or --map synthetic) a data-free synthetic space map with debug boxes. --png writes an
//       offscreen capture after the last tick (--ticks) once every model and pipeline is ready.
//   alamo_render --sim-headless-check [--map NAME] [--game DIR] [--ticks N] [--seed S] [--frames-per-tick K]
//       The same sim twice: rendering off, then a snapshot + K offscreen frames (both interpolation modes) after
//       every tick. Exit 0 only when the per-tick CRC logs are identical and validation reported nothing.
// Common: --workers N, --width W --height H, --hidden, --frames N (windowed: stop after N frames),
//         --camera-distance D, --mailbox / --immediate, --cache-dir DIR, MODPATH=.. FALLBACKPATH=..
//         --no-anim (R3: bind pose), --no-particles (R4: no particle systems), --particle-detail F (0..1),
//         --expect-particles (check: fail unless particles were spawned)
//         --no-sim-anim (R3: no SimAnimation on the host; poses from the snapshot-fed SimAnimDriver fallback),
//         --anim-crc (R3: the sim's opt-in "anim" CRC system)
// Animation (R3): by default the session's SimHost owns it (sim::SimAnimation, "T5.anim", synced RNG) in BOTH check
// runs, so rendering off / on compare the same sim; after every tick the render side reads the top mode's AnimWorld
// (const, same thread, between ticks) into an AnimPoseCache whose pure pose() feeds the instance build and the proxies.
// Particles (R4): every sim object's ALO proxies get their particle systems (sync_sim_frame after the instance build,
// with the R3 pose so effects follow animated bones and the object's damage _ALT index); ParticleWorld::update gets
// tick_delta(sim ticks since the previous frame), so particles advance with the sim and freeze while it does not tick.
// The synthetic world's debug box carries synthetic proxies (SYN_FIRE / SYN_SMOKE_ALT0 / SYN_SPARKS).
#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <process.h>  // Int-final: _getpid

#include "alamo/assets/asset_cache.hpp"
#include "alamo/core/file_provider.hpp"
#include "alamo/core/log.hpp"
#include "alamo/core/sim_crc.hpp"
#include "alamo/data/game_database.hpp"
#include "alamo/jobs/crc_recorder.hpp"
#include "alamo/jobs/job_system.hpp"
#include "alamo/render/fleet.hpp"
#include "alamo/render/map_scene.hpp"
#include "alamo/render/png.hpp"
#include "alamo/render/renderer.hpp"
#include "alamo/render/sim_session.hpp"
#include "alamo/render/sim_snapshot.hpp"
#include "alamo/render/sim_view.hpp"
#include "alamo/rhi/rhi.hpp"
#include "alamo/vfs/vfs.hpp"
#ifdef ALAMO_RENDER_HAVE_ANIM_DRIVER
#include "alamo/render/anim/anim_pose_cache.hpp"  // R3: poses of the sim-owned AnimWorld
#include "alamo/render/anim/sim_anim_driver.hpp"  // R3: fallback visual driver (--no-sim-anim)
#endif
#ifdef ALAMO_PARTICLES_HAVE_SIM
// R4: particle systems on the sim objects' proxies (render-side only, own free RNG; B.6).
#include "alamo/render/particles/particle_attach.hpp"
#include "alamo/render/particles/particle_data.hpp"
#include "alamo/render/particles/particle_gpu.hpp"
#include "alamo/render/particles/particle_sim.hpp"
#include "alamo/render/particles/particle_sim_attach.hpp"
#include "alamo/render/particles/particle_synthetic.hpp"
#endif

int run_sim_main(int argc, char** argv);

namespace {

using namespace alamo;
using Clock = std::chrono::steady_clock;

struct SimOptions {
    bool sim = false, check = false, hidden = false, anim = true;  // anim: R3 unit animation (--no-anim)
    bool sim_anim = true;   // R3: the sim owns the animation (SimAnimation on the session's host); --no-sim-anim = fallback driver
    bool anim_crc = false;  // R3: register the opt-in "anim" CRC system (--anim-crc)
    bool particles = true, expect_particles = false;               // R4 (--no-particles, --expect-particles)
    float particle_detail = 1.0f;                                  // R4 --particle-detail
    std::string map, game, png, cache_dir;
    std::vector<std::string> game_args;
    uint64_t ticks = 0;  // 0 = until Esc / --frames (windowed); 300 for the check
    bool ticks_set = false;
    uint32_t seed = 1;
    int32_t workers = -1;
    render::InterpMode interp = render::InterpMode::Original;
    long frames = -1;
    uint32_t width = 1280, height = 720;
    uint32_t frames_per_tick = 2;
    float camera_distance = -1.0f;
    float anim_focus = 0.0f;  // R3 --anim-focus D: PNG camera on the first animated unit at distance D
    uint32_t aniso = 0;       // --aniso N: [port] Renderer::set_anisotropy (0/1 off, 2..16; default off)
    bool ao = false;          // --ao: [port] screen-space ambient occlusion (RecordOptions::ambient_occlusion)
    float ao_strength = -1.0f, ao_radius = -1.0f;  // --ao-strength S / --ao-radius R (< 0: RecordOptions default)
    rhi::PresentMode mode = rhi::PresentMode::Fifo;
};

std::string env_or_empty(const char* name) {
#ifdef _MSC_VER
    char* v = nullptr;
    size_t len = 0;
    std::string out;
    if (_dupenv_s(&v, &len, name) == 0 && v) {
        out = v;
        std::free(v);
    }
    return out;
#else
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
#endif
}

// Int-final: the process id (per-process temp folders).
long long process_id() {
    return static_cast<long long>(_getpid());
}

int sim_usage() {
    std::fprintf(stderr,
                 "usage: alamo_render --sim [--map NAME|synthetic] [--game DIR] [--ticks N] [--seed S] [--interp original|smooth]\n"
                 "                    [--png PATH] [--frames N] [--width W] [--height H] [--hidden] [--camera-distance D]\n"
                 "       alamo_render --sim-headless-check [--map NAME|synthetic] [--game DIR] [--ticks N] [--seed S]\n"
                 "                    [--frames-per-tick K] [--png PATH]\n"
                 "       common: [--workers N] [--cache-dir DIR] [--mailbox|--immediate] [--no-anim] [--anim-focus D] [MODPATH=..] [FALLBACKPATH=..]\n"
                 "               [--no-sim-anim] [--anim-crc] [--aniso N] [--ao] [--ao-strength S] [--ao-radius R]\n"
                 "               [--no-particles] [--particle-detail F] [--expect-particles]\n"
                 "       real maps need --game DIR or ALAMO_FOC_DATA (the corruption folder)\n");
    return 2;
}

double ms_since(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

// ------------------------------------------------------------------------------------------- world
struct World {
    std::unique_ptr<vfs::Vfs> files;
    std::unique_ptr<render::SyntheticSimWorld> synth;
    objects::ObjectTypeDatabase db;
    objects::GameObjectTypes types_real;
    std::unique_ptr<sim::TypeCrcIndex> index_real;
    terrain::MapData map_real;
    std::unique_ptr<data::GameDatabase> gdb;
    std::unique_ptr<render::GameTypeResolver> resolver;
    std::string name;
    std::filesystem::path synth_dir;  // Int-final: this process's synthetic data folder (removed at exit)

    World() = default;
    World(const World&) = delete;
    World& operator=(const World&) = delete;
    ~World() {
        synth.reset();
        if (!synth_dir.empty()) {
            std::error_code ec;
            remove_all_long(synth_dir, ec);
        }
    }

    bool synthetic() const { return synth != nullptr; }
    const FileProvider& provider() const { return synth ? static_cast<const FileProvider&>(*synth->files) : *files; }
    const objects::GameObjectTypes& types() const { return synth ? synth->types : types_real; }
    const sim::TypeCrcIndex& index() const { return synth ? *synth->index : *index_real; }
    const terrain::MapData& map() const { return synth ? synth->map : map_real; }
};

bool load_world(const SimOptions& opt, World& w) {
    if (opt.map.empty() || opt.map == "synthetic") {
        w.name = "synthetic";
        std::string err;
        // Int-final: one folder per process (parallel ctest runs three --sim-headless-check tests at once), removed
        // again by ~World.
        const std::filesystem::path dir =
            std::filesystem::temp_directory_path() / ("alamo_render_sim_synthetic_" + std::to_string(process_id()));
        std::error_code ec;
        remove_all_long(dir, ec);
        w.synth_dir = dir;
        w.synth = render::make_synthetic_sim_world(path_to_utf8(dir), &err);
        if (!w.synth) std::fprintf(stderr, "synthetic world: %s\n", err.c_str());
        return w.synth != nullptr;
    }
    w.name = opt.map;
    const std::string root = opt.game.empty() ? env_or_empty("ALAMO_FOC_DATA") : opt.game;
    if (root.empty()) {
        std::fprintf(stderr, "--map %s needs --game <corruption dir> or ALAMO_FOC_DATA\n", opt.map.c_str());
        return false;
    }
    std::string err;
    w.files = vfs::mount_game(root, opt.game_args, &err);  // mounted exactly as alamo_headless does
    if (!w.files) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return false;
    }
    const auto t0 = Clock::now();
    if (!w.db.load(*w.files) || !w.types_real.build(w.db, w.files.get())) {
        std::fprintf(stderr, "GameObjectFiles.xml not found / object types failed under %s\n", root.c_str());
        return false;
    }
    w.index_real = std::make_unique<sim::TypeCrcIndex>(w.types_real);
    w.gdb = std::make_unique<data::GameDatabase>();
    if (!w.gdb->load(*w.files)) std::fprintf(stderr, "game database: load reported errors (continuing)\n");
    w.resolver = std::make_unique<render::GameTypeResolver>(*w.gdb);
    if (!w.resolver->load_render_xml(*w.files)) std::fprintf(stderr, "hardpoint XML not found: damage decals use the name rule\n");
    // The sim's map: the same .ted MapScene loads for the world (one VFS, one file).
    if (!terrain::load_map(*w.files, opt.map, w.map_real, &err)) {
        std::fprintf(stderr, "map %s: %s\n", opt.map.c_str(), err.c_str());
        return false;
    }
    std::printf("data: %zu object types, map %s (%s, %zu placements) in %.0f ms\n", w.types_real.size(), opt.map.c_str(),
                w.map_real.is_space() ? "space" : "land", w.map_real.objects.size(), ms_since(t0));
    return true;
}

// The session spec of both check runs and the windowed run (R3: sim-owned animation unless --no-sim-anim).
render::SimSessionSpec session_spec(const SimOptions& opt, const World& w) {
    render::SimSessionSpec spec;
    spec.seed = opt.seed;
    if (opt.sim_anim) spec.anim_files = &w.provider();
    spec.anim_crc = opt.anim_crc;
    return spec;
}

// ---------------------------------------------------------------------------------- render state
struct BoxModels final : render::SimModelSource {  // synthetic world: every drawable type is the debug box
    const render::GpuModel* box = nullptr;
    const render::SnapshotTypeTable* table = nullptr;
    const render::GpuModel* type_model(objects::TypeIndex t) override {
        return table->valid(t) && !(*table)[t].model.empty() ? box : nullptr;
    }
    const render::GpuModel* named_model(const std::string&) override { return nullptr; }
};

struct Offscreen {
    rhi::Image color;
    rhi::Buffer readback;
    uint32_t w = 0, h = 0;
    bool bgra = false;
    bool create(rhi::Allocator& a, uint32_t width, uint32_t height, VkFormat fmt) {
        w = width;
        h = height;
        rhi::ImageDesc cd;
        if (fmt == VK_FORMAT_B8G8R8A8_UNORM) cd.format = rhi::Format::BGRA8;
        else if (fmt == VK_FORMAT_R8G8B8A8_UNORM) cd.format = rhi::Format::RGBA8;
        else return false;
        bgra = fmt == VK_FORMAT_B8G8R8A8_UNORM;
        cd.width = w;
        cd.height = h;
        cd.mips = 1;
        cd.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        cd.name = "sim.offscreen";
        return a.create_image(cd, color) &&
               a.create_buffer(static_cast<VkDeviceSize>(w) * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, rhi::MemoryUsage::Readback, readback);
    }
    void destroy(rhi::Allocator& a) {
        if (color) a.destroy(color);
        if (readback) a.destroy(readback);
    }
    bool write_png(const std::string& path) const {
        const auto* px = static_cast<const uint8_t*>(readback.mapped());
        std::vector<uint8_t> rgba(px, px + static_cast<size_t>(w) * h * 4);
        if (bgra)
            for (size_t i = 0; i < rgba.size(); i += 4) std::swap(rgba[i], rgba[i + 2]);
        for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
        return render::write_png_rgba(path, w, h, rgba);
    }
};

// Everything the render side owns: renderer, models, map world, the snapshot buffer and the instance builder.
// Never writes the sim: it sees SimSnapshots only (capture() reads the host through its const interface).
class SimRender {
public:
    SimRender(rhi::Context& ctx, World& world, jobs::JobSystem& js, const SimOptions& opt) : ctx_(ctx), world_(world), js_(js), opt_(opt) {}

    bool init(VkFormat color_format, uint32_t width, uint32_t height, bool presentation_ring) {
        (void)presentation_ring;
        rhi::FrameRingDesc fd;
        fd.frames_in_flight = 2;
        fd.uniform_bytes = world_.synthetic() ? (16ull << 20) : (128ull << 20);
        fd.recording_threads = render::ring_recording_threads(1, js_.thread_count());
        if (!alloc.init(ctx_) || !up.init(ctx_, alloc) || !ring.init(ctx_, alloc, fd)) {
            std::fprintf(stderr, "rhi init failed\n");
            return false;
        }
        render::RendererDesc rd;
        rd.color_format = color_format;
        rd.depth_format = VK_FORMAT_D32_SFLOAT;
        rd.js = &js_;
        if (!world_.synthetic()) {
            effects_ = std::make_unique<render::EffectCache>(world_.files.get());
            rd.effects = effects_.get();
            rd.pipeline_cache_dir = opt_.cache_dir.empty() ? render::default_pipeline_cache_dir() : path_from_utf8(opt_.cache_dir);
            if (!opt_.cache_dir.empty()) rd.shader_cache_dir = path_from_utf8(opt_.cache_dir) / "shaders";
        } else {
            rd.translated = false;
        }
        renderer.set_anisotropy(opt_.aniso);
        if (!renderer.init(ctx_, alloc, up, rd)) {
            std::fprintf(stderr, "renderer init failed\n");
            return false;
        }
        renderer.set_frame_ring(&ring);
        const bool space = world_.map().is_space();
        table.build(world_.types(), space ? objects::ModeKind::Space : objects::ModeKind::Land, world_.resolver.get());
        builder_ = std::make_unique<render::SimInstanceBuilder>(table, world_.resolver.get());
#ifdef ALAMO_RENDER_HAVE_ANIM_DRIVER
        // R3: unit poses -> Instance::bone_world. Default: the sim-owned AnimWorld (SimAnimation of the session,
        // AnimPoseCache refreshed after every tick in capture()). --no-sim-anim: the snapshot-fed fallback driver (own
        // RNG, never touches the sim). --no-anim keeps the pose source's state (for --anim-focus) but draws the bind pose.
        if (!opt_.sim_anim && !world_.synthetic())
            anim_ = std::make_unique<render::SimAnimDriver>(*world_.files, world_.types(), space ? objects::ModeKind::Space : objects::ModeKind::Land);
        if (opt_.anim && (opt_.sim_anim || anim_)) {
            // Pure getter (poses of the last capture): shared by the instance build and the particle proxies (R4).
            pose_fn_ = [this](objects::ObjectId id, const render::GpuModel& m) {
                if (!m.asset) return std::span<const render::Mat3x4>{};
                return anim_ ? anim_->pose(id, m.asset->skeleton.size()) : poses_.pose(id, m.asset->skeleton.size());
            };
            builder_->set_pose_source(pose_fn_);
        }
#endif
        ropt.threads = js_.worker_count();
        ropt.ambient_occlusion = opt_.ao;  // [port] --ao
        if (opt_.ao_strength >= 0.0f) ropt.ao_strength = opt_.ao_strength;
        if (opt_.ao_radius > 0.0f) ropt.ao_radius = opt_.ao_radius;
        if (world_.synthetic()) {
            std::string err;
            auto box_asset = render::debug_box_model(render::kSyntheticModel);
#ifdef ALAMO_PARTICLES_HAVE_SIM
            // R4: synthetic proxies (the R4 sim check's set) so the data-free world exercises the particle path.
            if (opt_.particles)
                box_asset->proxies = {{"SYN_FIRE", 0, false}, {"SYN_SMOKE_ALT0", 0, false}, {"SYN_SPARKS", 0, false}, {"P_UNRESOLVED", 0, false}};
#endif
            if (!renderer.upload_model(box_asset,
                                       [](std::string_view, assets::TextureRole) { return std::shared_ptr<const assets::TextureAsset>(); },
                                       box_, nullptr, &err)) {
                std::fprintf(stderr, "debug box upload: %s\n", err.c_str());
                return false;
            }
            box_ok_ = true;
            box_models_.box = &box_;
            box_models_.table = &table;
            models_ = &box_models_;
            ropt.post = false;
            ropt.shadows = false;
            ropt.space = true;
            cam_.target = {0, 0, 0};
            cam_.yaw = -1.5707963f;
            cam_.pitch = 1.0f;
            cam_.distance = opt_.camera_distance > 0 ? opt_.camera_distance : 9000.0f;
            cam_.fov_x = render::tactical_fov_x(55.0f, static_cast<float>(width) / static_cast<float>(height));
            znear_ = 10.0f;
            zfar_ = 50000.0f;
            return init_particles();
        }
        cache_ = std::make_unique<assets::AssetCache>(*world_.files, effects_->model_load_options());
        streamer_ = std::make_unique<render::ModelStreamer>(renderer, *cache_, js_);
        scene_ = std::make_unique<render::MapScene>();
        scene_->set_objects_enabled(false);  // the sim's objects replace the placements
        std::string err;
        if (!scene_->load(*world_.files, opt_.map, world_.resolver.get(), jobs::make_parallel_for(js_), &err) ||
            !scene_->start(renderer, *streamer_, *cache_, {}, &err)) {
            std::fprintf(stderr, "--map %s: %s\n", opt_.map.c_str(), err.c_str());
            return false;
        }
        if (scene_->map().objects.size() != world_.map().objects.size())
            std::fprintf(stderr, "warning: MapScene and the sim parsed different object lists (%zu vs %zu)\n",
                         scene_->map().objects.size(), world_.map().objects.size());
        scene_->apply(ropt);
        // Laser projectiles (Projectile_Custom_Render 1 / 2): procedural beams / kites, GameConstants Z scale factors.
        {
            std::vector<uint8_t> gc;
            if (world_.files->read("Data\\XML\\GameConstants.xml", gc)) render::parse_laser_constants(gc, laser_k_);
            if (!renderer.init_lasers([this](std::string_view n, assets::TextureRole r) { return cache_->texture(n, r); }))
                std::fprintf(stderr, "warning: laser projectiles not drawn (PrimAdditive / textures unavailable)\n");
        }
        streamed_ = std::make_unique<render::StreamedSimModels>(table, *streamer_);
        models_ = streamed_.get();
        cam_ = scene_->tactical_camera(static_cast<float>(width) / static_cast<float>(height), opt_.camera_distance);
        znear_ = render::MapScene::kNear;
        zfar_ = render::MapScene::kFar;
        return init_particles();
    }

    // R4: particle world + GPU renderer as the Renderer's scene hook (no-op without the particle libs / --no-particles).
    bool init_particles() {
#ifdef ALAMO_PARTICLES_HAVE_SIM
        if (!opt_.particles) return true;
        pworld_ = std::make_unique<render::particles::ParticleWorld>(1u << 21);
        pworld_->set_detail(opt_.particle_detail);
        render::particles::ParticleTextureSource tex;
        if (world_.synthetic()) {
            plib_ = std::make_unique<render::particles::ParticleLibrary>();
            for (const render::particles::SystemPtr& p : render::particles::synthetic_systems()) {  // x 8: readable at 9000 units
                auto big = std::make_shared<render::particles::SystemDef>(*p);
                for (render::particles::EmitterDef& e : big->emitters)
                    for (float& v : e.tracks[render::particles::kScale].v) v *= 8.0f;
                render::particles::finalize_system(*big);
                plib_->add(big);
            }
        } else {
            plib_ = std::make_unique<render::particles::ParticleLibrary>(world_.files.get());
            tex = [this](std::string_view n) { return cache_->texture(n, assets::TextureRole::Color); };
        }
        if (!prender_.init(ctx_, alloc, up, *pworld_, std::move(tex))) {
            std::fprintf(stderr, "particle renderer init failed\n");
            return false;
        }
        prender_.set_sort_range(zfar_);
        renderer.set_scene_hook(&prender_);
        pattach_ = std::make_unique<render::particles::ProxyAttachments>(*pworld_, *plib_);
        popt_.detail = opt_.particle_detail;
#endif
        return true;
    }

    void shutdown() {
        ring.wait_idle();
        ctx_.wait_idle();
#ifdef ALAMO_PARTICLES_HAVE_SIM
        renderer.set_scene_hook(nullptr);
        if (pattach_) pattach_->clear();
        pattach_.reset();
        if (prender_.initialized()) prender_.shutdown();
#endif
        if (scene_) scene_->stop(renderer);
        if (streamer_) streamer_->destroy_all();
        streamer_.reset();
        if (box_ok_) renderer.destroy_model(box_);
        box_ok_ = false;
        renderer.shutdown();
        cache_.reset();
        effects_.reset();
        ring.shutdown();
        up.shutdown();
        alloc.shutdown();
    }

    // Producer side, once per finished tick (same thread as the tick): capture (const reads of the host and its
    // sim-owned AnimWorld) + ordered publish.
    void capture(const render::SimSession& session) {
        const sim::SimHost& host = session.host();
        auto snap = std::make_shared<render::SimSnapshot>();
        render::capture_snapshot(host, table, *snap, jobs::make_parallel_for(js_));
        // Int-B: a battle host would add its attached effects / combat hardpoints here (render::apply_battle_state);
        // SimSession runs plain tactical maps (no TacticalBattle), so snap->effects stays empty. The battle-host render
        // session is alamo_game's BattleView (engine/apps/game game_render.cpp: battle state, fog, reinforcement overlay).
        last_objects_ = static_cast<uint32_t>(snap->objects.size());
        if (captured_any_ && snap->tick > last_tick_) pending_ticks_ += static_cast<uint32_t>(snap->tick - last_tick_);  // R4 clock
        captured_any_ = true;
        last_tick_ = snap->tick;
#ifdef ALAMO_RENDER_HAVE_ANIM_DRIVER
        if (anim_) {  // R3 fallback: advance the snapshot-fed driver once per finished tick
            anim_objects_.clear();
            for (const render::SnapshotObject& o : snap->objects) anim_objects_.push_back({o.id, o.type, o.position});
            anim_->update(snap->tick, anim_objects_, jobs::make_parallel_for(js_));
        } else if (opt_.sim_anim) {  // R3: poses of the sim-owned world as of this tick (the world is only read)
            sim_world_ = session.anim_world();
            poses_.update(sim_world_, jobs::make_parallel_for(js_));
        }
#endif
#ifdef ALAMO_SIM_HAVE_ANIM
        if (const sim::SimAnimation* a = session.animation()) sim_anim_stats_ = a->stats();
        sim_anim_attached_ = session.has_animation();
#endif
        buffer.publish(std::move(snap));
    }

    // Consumer side, inside a frame after Uploader::record_acquires: residency, interpolation, instances.
    void prepare(float alpha, render::InterpMode mode) {
        if (box_ok_) renderer.make_resident(box_);
        if (streamer_) streamer_->update();
        const render::SnapshotBuffer::Pair p = buffer.latest();
        main_.clear();
        sky_.clear();
        if (p.curr) {
            render::interpolate(p.prev.get(), *p.curr, alpha, mode, frame_);
            if (streamed_) streamed_->request(frame_);
            builder_->build(frame_, *models_, main_, bg_, &stats_);
#ifdef ALAMO_PARTICLES_HAVE_SIM
            // R4: proxies after the build (hardpoint masks, R3 pose, damage alt), then one particle update covering the
            // sim ticks since the previous frame (0 = frozen: frames between ticks only redraw).
            if (pattach_) {
                psync_ = render::particles::sync_sim_frame(*pattach_, frame_, table, *models_, *builder_, pose_fn_ ? &pose_fn_ : nullptr,
                                                           &popt_);
                pworld_->update(render::particles::ParticleWorld::tick_delta(pending_ticks_));
                pending_ticks_ = 0;
            }
#endif
        }
        if (scene_) {
            scene_->update(renderer, *streamer_);
            if (p.curr) scene_->set_sim_tick(p.curr->tick);  // background idle clips advance on sim ticks
            sky_.assign(scene_->sky().begin(), scene_->sky().end());
        }
        sky_.insert(sky_.end(), bg_.begin(), bg_.end());  // In_Background sim objects (space)
    }

#ifdef ALAMO_RENDER_HAVE_ANIM_DRIVER
    // R3 --anim-focus D: frame the first unit that plays a bound clip (close-up PNGs of animated units).
    void focus_animated(float distance) {
        const render::SnapshotBuffer::Pair p = buffer.latest();
        const anim::AnimWorld* w = anim_ ? &anim_->world() : sim_world_;
        if (!w || !(distance > 0.0f) || !p.curr) return;
        for (const anim::ObjectId id : w->ids()) {
            const render::SnapshotObject* o = p.curr->find(id);
            const anim::AnimModel* m = w->model(id);
            const bool posed = m && !(anim_ ? anim_->pose(id, m->skeleton.size()) : poses_.pose(id, m->skeleton.size())).empty();
            if (!o || !posed) continue;
            cam_.target = {o->position.x, o->position.y, o->position.z + 10.0f};
            cam_.distance = distance;
            std::printf("anim focus: object %d (%s) at distance %.0f\n", id, m->base.c_str(), static_cast<double>(distance));
            return;
        }
    }
#endif

    void clear_color(float (&c)[4]) const {
        const render::Float3 cc = scene_ ? scene_->clear_color() : render::Float3{0.0f, 0.0f, 0.02f};
        c[0] = cc.x;
        c[1] = cc.y;
        c[2] = cc.z;
        c[3] = 1.0f;
    }
    render::ViewMatrices view(uint32_t w, uint32_t h) const { return render::camera_view(render::camera_params(cam_, znear_, zfar_), w, h); }
    render::OrbitCamera& camera() { return cam_; }

    bool draw(rhi::Frame& f, VkCommandBuffer cmd, render::PassTarget& pt) {
        clear_color(pt.clear);
        renderer.set_lasers(builder_ ? builder_->lasers() : std::span<const render::Laser>(), laser_k_);
        return renderer.render(f, cmd, pt, view(pt.extent.width, pt.extent.height), main_, render::DrawMode::Lit, ropt, &draw_stats,
                               nullptr, sky_);
    }

    // One offscreen frame (readback when `o` is given); waits for the GPU.
    bool offscreen_frame(Offscreen& o, float alpha, render::InterpMode mode, bool readback) {
        rhi::Frame* f = ring.begin_frame();
        if (!f) return false;
        VkCommandBuffer cmd = f->cmd();
        const rhi::DeviceFns& fn = ctx_.vkd();
        const rhi::AcquireResult acq = up.record_acquires(cmd);
        prepare(alpha, mode);
        render::PassTarget pt;
        pt.color = o.color.handle;
        pt.color_view = o.color.view;
        pt.extent = {o.w, o.h};
        pt.color_after = rhi::ImageState::TransferSrc;
        const bool drawn = draw(*f, cmd, pt);
        if (readback) {
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {o.w, o.h, 1};
            fn.vkCmdCopyImageToBuffer(cmd, o.color.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, o.readback.handle, 1, &copy);
            const VkBufferMemoryBarrier2 hb = rhi::buffer_barrier(o.readback.handle, VK_PIPELINE_STAGE_2_COPY_BIT,
                                                                  VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
                                                                  VK_ACCESS_2_HOST_READ_BIT);
            rhi::cmd_barrier(fn, cmd, {}, std::span<const VkBufferMemoryBarrier2>(&hb, 1));
        }
        VkSemaphoreSubmitInfo wait = rhi::semaphore_info(up.timeline(), acq.wait_value, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
        rhi::FrameSubmit fs;
        if (acq.wait_value) fs.waits = {&wait, 1};
        if (!ring.end_frame(*f, fs)) return false;
        ring.wait_idle();
        if (readback) alloc.invalidate(o.readback.alloc);
        return drawn;
    }

    // Every model finished, the world resident and nothing compiling in the last frame.
    bool settled() const {
        const bool models = streamed_ ? streamed_->all_finished() : true;
        const bool world = !scene_ || (scene_->ready() && scene_->world().stats().terrain_pending == 0 &&
                                       scene_->world().stats().water_pending == 0 && scene_->world().stats().decorations.pending == 0);
        bool fx = true;
#ifdef ALAMO_PARTICLES_HAVE_SIM
        fx = !prender_.initialized() || prender_.last_stats().textures_pending == 0;
#endif
        return models && world && fx && draw_stats.pending_pipeline == 0 && draw_stats.translation_pending == 0 && draw_stats.post_pending == 0;
    }

    // R4: particles spawned so far (0 without particles).
    uint64_t particles_spawned() const {
#ifdef ALAMO_PARTICLES_HAVE_SIM
        if (pworld_) return pworld_->stats().spawned;
#endif
        return 0;
    }

    // Settled offscreen capture of the current state into `path`.
    bool capture_png(const std::string& path, VkFormat fmt, uint32_t w, uint32_t h, render::InterpMode mode) {
        Offscreen o;
        if (!o.create(alloc, w, h, fmt)) {
            std::fprintf(stderr, "--png: offscreen target for format %d failed\n", static_cast<int>(fmt));
            return false;
        }
        bool ok = false;
        for (int i = 0; i < 600; ++i) {  // models stream and pipelines compile: wait until a frame is complete
            ok = offscreen_frame(o, 1.0f, mode, true);
            if (ok && settled() && i >= 2) break;
            if (!settled()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!settled()) std::fprintf(stderr, "--png: capture taken before everything settled\n");
        ok = ok && o.write_png(path);
        std::printf("%s %s: %u instances, %u draws (%u lit, %u FF, %u unlit), %llu triangles, %u sky draws\n", ok ? "wrote" : "FAILED",
                    path.c_str(), draw_stats.instances, draw_stats.draws, draw_stats.lit_draws, draw_stats.ff_draws, draw_stats.unlit_draws,
                    static_cast<unsigned long long>(draw_stats.triangles), draw_stats.sky_draws);
        ring.wait_idle();
        o.destroy(alloc);
        return ok;
    }

    void print_stats(const char* what) const {
        std::printf("%s: snapshot %u objects -> %u drawn (%u background, %u attachments, %u without model yet, %u faded out); "
                    "models %u requested, %u ready, %u failed; last frame %u draws, %u instances (%u culled)\n",
                    what, last_objects_, stats_.drawn, stats_.background, stats_.attachments, stats_.no_model, stats_.faded_out,
                    streamed_ ? streamed_->requested() : 1u, streamed_ ? streamed_->ready() : 1u, streamed_ ? streamed_->failed() : 0u,
                    draw_stats.draws, draw_stats.instances, draw_stats.culled);
#ifdef ALAMO_RENDER_HAVE_ANIM_DRIVER
        if (anim_) {
            const render::AnimDriverStats& a = anim_->stats();
            std::printf("%s: anim tick %" PRIu64 ": %u objects, %u animated, %u posed; %u models (%u with clips); %u idle / %u move plays; digest %016" PRIx64 "\n",
                        what, anim_->tick(), a.objects, a.animated, a.posed, a.models, a.models_with_clips, a.idle_plays, a.move_plays, anim_->digest());
        } else if (opt_.sim_anim) {
            std::printf("%s: sim-owned anim (SimAnimation%s): %u rows, %u posed", what, opt_.anim_crc ? ", \"anim\" CRC on" : "",
                        poses_.stats().rows, poses_.stats().posed);
#ifdef ALAMO_SIM_HAVE_ANIM
            std::printf("; %u models (%u with clips), %u created, %u removed%s", sim_anim_stats_.models, sim_anim_stats_.models_with_clips,
                        sim_anim_stats_.created, sim_anim_stats_.removed, sim_anim_attached_ ? "" : " (session has NO SimAnimation)");
#endif
            std::printf("\n");
        }
#endif
#ifdef ALAMO_PARTICLES_HAVE_SIM
        if (pworld_) {
            const render::particles::WorldStats ws = pworld_->stats();
            const render::particles::AttachStats& as = pattach_->stats();
            const render::particles::ParticleFrameStats& ps = prender_.last_stats();
            std::printf("%s: particles: %u objects synced (%u effects, %u attached, %u below detail, %u culled); proxies %u (%u with a particle file, "
                        "%u via _ALT/_LOD, %u unresolved, %u hidden); %u systems, %u live particles, %llu spawned; last frame %u drawn / "
                        "%u culled systems, %u draw calls (%u transparent, %u water, %u heat)\n",
                        what, psync_.synced, psync_.effects, psync_.attached, psync_.below_detail, psync_.culled, as.proxies, as.particle_proxies, as.stripped,
                        as.unresolved, as.hidden, ws.systems, ws.live_particles, static_cast<unsigned long long>(ws.spawned), ps.systems_drawn,
                        ps.systems_culled, ps.draw_calls, ps.transparent_draws, ps.water_draws, ps.heat_draws);
        }
#endif
    }

    rhi::Allocator alloc;
    rhi::Uploader up;
    rhi::FrameRing ring;
    render::Renderer renderer;
    render::SnapshotTypeTable table;
    render::SnapshotBuffer buffer;
    render::RecordOptions ropt;
    render::DrawStats draw_stats;

private:
    rhi::Context& ctx_;
    World& world_;
    jobs::JobSystem& js_;
    const SimOptions& opt_;
    std::unique_ptr<render::EffectCache> effects_;
    std::unique_ptr<assets::AssetCache> cache_;
    std::unique_ptr<render::ModelStreamer> streamer_;
    std::unique_ptr<render::MapScene> scene_;
    std::unique_ptr<render::StreamedSimModels> streamed_;
    std::unique_ptr<render::SimInstanceBuilder> builder_;
#ifdef ALAMO_RENDER_HAVE_ANIM_DRIVER
    std::unique_ptr<render::SimAnimDriver> anim_;  // R3 fallback (--no-sim-anim)
    std::vector<render::AnimDriverObject> anim_objects_;
    render::AnimPoseCache poses_;                 // R3: poses of the sim-owned world (default)
    const anim::AnimWorld* sim_world_ = nullptr;  // the session's world at the last capture (read between ticks only)
#endif
#ifdef ALAMO_SIM_HAVE_ANIM
    sim::SimAnimStats sim_anim_stats_;
    bool sim_anim_attached_ = false;
#endif
    render::SimInstanceBuilder::PoseSource pose_fn_;  // R3 pose (empty = bind pose), shared with the particle proxies
    uint64_t last_tick_ = 0;
    uint32_t pending_ticks_ = 0;  // sim ticks captured since the last particle update
    bool captured_any_ = false;
#ifdef ALAMO_PARTICLES_HAVE_SIM
    std::unique_ptr<render::particles::ParticleWorld> pworld_;  // R4
    std::unique_ptr<render::particles::ParticleLibrary> plib_;
    render::particles::ParticleRenderer prender_;
    std::unique_ptr<render::particles::ProxyAttachments> pattach_;
    render::particles::SimAttachOptions popt_;
    render::particles::SimAttachStats psync_;
#endif
    render::GpuModel box_;
    bool box_ok_ = false;
    BoxModels box_models_;
    render::SimModelSource* models_ = nullptr;
    render::InterpolatedFrame frame_;
    std::vector<render::Instance> main_, bg_, sky_;
    render::SimInstanceStats stats_;
    uint32_t last_objects_ = 0;
    render::OrbitCamera cam_;
    float znear_ = 10.0f, zfar_ = 7000.0f;
    render::LaserConstants laser_k_;  // GameConstants Laser_Beam / Laser_Kite_Z_Scale_Factor
};

uint64_t final_crc(const CrcLog& log) { return log.records.empty() ? 0 : log.records.back().combined; }

// ----------------------------------------------------------------------------------- headless check
int run_check(const SimOptions& opt, World& world) {
    const uint64_t ticks = opt.ticks_set ? opt.ticks : 300;
    const render::SimSessionSpec spec = session_spec(opt, world);
    // Run A: rendering off.
    CrcLog off;
    {
        jobs::JobSystem js(jobs::JobSystemConfig{opt.workers});
        render::SimSession s(world.types(), js, world.map(), world.index(), spec);
        const auto t0 = Clock::now();
        for (uint64_t i = 0; i < ticks && s.tick(); ++i) {
        }
        off = s.host().crc_log();
        std::printf("rendering off: %" PRIu64 " ticks in %.0f ms, %u objects spawned, final CRC %016" PRIx64 "\n", s.ticks(), ms_since(t0),
                    s.spawn().spawned, final_crc(off));
    }
    // Run B: snapshot + offscreen frames after every tick.
    const bool strict = render::setup_validation_env(ALAMO_RENDER_SOURCE_DIR);
    rhi::Context ctx;
    rhi::ContextDesc cd;
    cd.app_name = "alamo_render_sim_check";
    cd.validation = strict ? rhi::Validation::Required : rhi::Validation::IfAvailable;
    if (!ctx.init(cd)) {
        std::fprintf(stderr, "no suitable Vulkan 1.3 device%s\n", strict ? " (or the validation layer failed to load)" : "");
        return 1;
    }
    std::printf("device %s, validation %s%s\n", ctx.caps().name.c_str(), ctx.validation_enabled() ? "on" : "off", strict ? " (required)" : "");
    int rc = 0;
    CrcLog on;
    {
        jobs::JobSystem js(jobs::JobSystemConfig{opt.workers});
        SimRender r(ctx, world, js, opt);
        Offscreen o;
        bool ok = r.init(VK_FORMAT_R8G8B8A8_UNORM, opt.width, opt.height, false) &&
                  o.create(r.alloc, opt.width, opt.height, VK_FORMAT_R8G8B8A8_UNORM);
        if (ok) {
            render::SimSession s(world.types(), js, world.map(), world.index(), spec);
            r.capture(s);
            const auto t0 = Clock::now();
            uint64_t frames = 0;
            const uint32_t k = std::max(1u, opt.frames_per_tick);
            for (uint64_t i = 0; i < ticks && ok && s.tick(); ++i) {
                r.capture(s);
                for (uint32_t f = 0; f < k && ok; ++f) {
                    const render::InterpMode m = (f % 2 == 0) ? render::InterpMode::Original : render::InterpMode::Smooth;
                    ok = r.offscreen_frame(o, static_cast<float>(f) / static_cast<float>(k), m, false);
                    ++frames;
                }
            }
            on = s.host().crc_log();
            std::printf("rendering on: %" PRIu64 " ticks, %" PRIu64 " frames in %.0f ms, final CRC %016" PRIx64 "\n", s.ticks(), frames,
                        ms_since(t0), final_crc(on));
            r.print_stats("rendering on");
            if (opt.expect_particles && r.particles_spawned() == 0) {  // R4: the particle path must have run
                std::fprintf(stderr, "--expect-particles: no particle was spawned%s\n", opt.particles ? "" : " (--no-particles)");
                ok = false;
            }
#ifdef ALAMO_RENDER_HAVE_ANIM_DRIVER
            r.focus_animated(opt.anim_focus);  // R3
#endif
            if (ok && !opt.png.empty()) ok = r.capture_png(opt.png, VK_FORMAT_R8G8B8A8_UNORM, opt.width, opt.height, opt.interp);
        }
        if (!ok) rc = 1;
        r.ring.wait_idle();
        ctx.wait_idle();
        o.destroy(r.alloc);
        r.shutdown();
    }
    const CrcDivergence d = first_divergence(off, on);
    std::printf("per-tick CRC, rendering off vs on: %zu vs %zu records: %s\n", off.records.size(), on.records.size(), d.message.c_str());
    if (d.diverged || off.records.size() != on.records.size() || off.records.empty()) rc = 1;
    // R3: the "anim" CRC system is in the log exactly when the sim owns the animation and it was opted in.
    const bool has_anim_crc = std::find(on.systems.begin(), on.systems.end(), "anim") != on.systems.end();
    bool want_anim_crc = false;
#ifdef ALAMO_SIM_HAVE_ANIM
    want_anim_crc = opt.sim_anim && opt.anim_crc;
#endif
    std::printf("CRC systems: %zu (\"anim\" %s, expected %s)\n", on.systems.size(), has_anim_crc ? "registered" : "not registered",
                want_anim_crc ? "registered" : "not registered");
    if (has_anim_crc != want_anim_crc || on.systems != off.systems) rc = 1;
    const uint32_t ve = ctx.validation_errors(), vw = ctx.validation_warnings();
    std::printf("validation: %s, %u errors, %u warnings\n", ctx.validation_enabled() ? "on" : "off", ve, vw);
    if (ve || vw) rc = 1;
    ctx.shutdown();
    std::printf(rc == 0 ? "sim headless check: OK\n" : "sim headless check: FAILED\n");
    return rc;
}

// ---------------------------------------------------------------------------------------- windowed
int run_windowed(const SimOptions& opt, World& world) {
    const bool strict = render::setup_validation_env(ALAMO_RENDER_SOURCE_DIR);
    rhi::Window win;
    if (!win.create({"alamo_render --sim (Esc quits)", opt.width, opt.height, true, !opt.hidden})) return 1;
    rhi::Context ctx;
    rhi::ContextDesc cd;
    cd.app_name = "alamo_render_sim";
    cd.presentation = true;
    cd.validation = strict ? rhi::Validation::Required : rhi::Validation::IfAvailable;
    if (!ctx.init(cd)) {
        std::fprintf(stderr, "no suitable Vulkan 1.3 device%s\n", strict ? " (or the validation layer failed to load)" : "");
        return 1;
    }
    std::printf("device %s, validation %s%s\n", ctx.caps().name.c_str(), ctx.validation_enabled() ? "on" : "off", strict ? " (required)" : "");
    int rc = 0;
    {
        jobs::JobSystem js(jobs::JobSystemConfig{opt.workers});
        SimRender r(ctx, world, js, opt);
        rhi::Swapchain sc;
        bool ok = true;
        {
            // Swapchain first (its format is the renderer's colour format); the allocator is created by r.init.
            rhi::SwapchainDesc sd;
            sd.hinstance = win.native_instance();
            sd.hwnd = win.native_handle();
            sd.width = win.width();
            sd.height = win.height();
            sd.mode = opt.mode;
            ok = sc.init(ctx, sd);
        }
        ok = ok && r.init(sc.format(), sc.extent().width, sc.extent().height, true);
        if (!ok) rc = 1;
        std::unique_ptr<render::SimSession> session;
        if (ok) {
            const render::SimSessionSpec spec = session_spec(opt, world);
            session = std::make_unique<render::SimSession>(world.types(), js, world.map(), world.index(), spec);
            const sim::SpawnReport& sp = session->spawn();
            std::printf("sim %s: %u placements, %u spawned (%u units, %u structures, %u props), %u unknown types; interpolation %s\n",
                        world.name.c_str(), sp.placements, sp.spawned, sp.units, sp.structures, sp.props, sp.unknown_type,
                        render::interp_mode_name(opt.interp));
            r.capture(*session);  // tick 0: the spawned map
        }
        const auto t0 = Clock::now();
        auto now_ms = [&] { return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count()); };
        uint32_t last_tick_ms = 0;
        long frames = 0, frames_after_ticks = 0;
        bool need_recreate = false, ticks_done = false;
        int last_mx = 0, last_my = 0;
        while (ok && win.pump()) {
            if (win.key_down(0x1B)) break;  // VK_ESCAPE
            if (opt.frames >= 0 && frames >= opt.frames) break;
            if (win.take_resized()) need_recreate = true;
            if (need_recreate) {
                r.ring.wait_idle();
                sc.recreate(win.width(), win.height());
                if (sc.valid() && !r.renderer.set_color_format(sc.format())) break;
                need_recreate = false;
            }
            // Sim: at most one paced tick per loop iteration, then the snapshot of the finished tick.
            const uint32_t now = now_ms();
            if (!ticks_done) {
                if (session->loop_iteration(now)) {
                    last_tick_ms = now;
                    r.capture(*session);
                }
                if (opt.ticks && session->ticks() >= opt.ticks) {
                    ticks_done = true;
                    std::printf("sim: %" PRIu64 " ticks in %.0f ms, final CRC %016" PRIx64 "\n", session->ticks(), ms_since(t0),
                                final_crc(session->host().crc_log()));
                }
            }
            if (!sc.valid() || win.minimized()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
                continue;
            }
            const float alpha = ticks_done ? 1.0f : render::tick_alpha(now, last_tick_ms, session->host().pacing().period_ms());
            rhi::Frame* f = r.ring.begin_frame();
            if (!f) break;
            uint32_t image = 0;
            const rhi::SwapchainStatus as = sc.acquire(f->acquire_semaphore(), image);
            if (as == rhi::SwapchainStatus::OutOfDate || as == rhi::SwapchainStatus::Minimized || as == rhi::SwapchainStatus::Error) {
                r.ring.end_frame(*f);
                need_recreate = true;
                if (as == rhi::SwapchainStatus::Error) break;
                continue;
            }
            VkCommandBuffer cmd = f->cmd();
            const rhi::AcquireResult acq = r.up.record_acquires(cmd);
            r.renderer.set_time(static_cast<float>(ms_since(t0) / 1000.0));
            r.prepare(alpha, opt.interp);
            // Camera input (engine/ui will own this): left-drag orbits, wheel zooms; parameters in, view out.
            const int mx = win.mouse_x(), my = win.mouse_y();
            if (win.mouse_down(rhi::MouseButton::Left))
                r.camera().orbit(-0.008f * static_cast<float>(mx - last_mx), 0.008f * static_cast<float>(my - last_my));
            last_mx = mx;
            last_my = my;
            const float wheel = win.take_wheel();
            if (wheel != 0) r.camera().zoom(wheel);
            render::PassTarget pt;
            pt.color = sc.image(image);
            pt.color_view = sc.view(image);
            pt.extent = sc.extent();
            pt.color_after = rhi::ImageState::Present;
            if (!r.draw(*f, cmd, pt)) {
                rc = 1;
                ok = false;
            }
            VkSemaphoreSubmitInfo waits[2];
            uint32_t nw = 0;
            waits[nw++] = rhi::semaphore_info(f->acquire_semaphore(), 0,
                                              VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
            if (acq.wait_value) waits[nw++] = rhi::semaphore_info(r.up.timeline(), acq.wait_value, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
            const VkSemaphoreSubmitInfo signal = rhi::semaphore_info(sc.render_finished(image), 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
            rhi::FrameSubmit fs;
            fs.waits = {waits, nw};
            fs.signals = {&signal, 1};
            if (!r.ring.end_frame(*f, fs)) break;
            const rhi::SwapchainStatus ps = sc.present(image);
            if (ps == rhi::SwapchainStatus::OutOfDate || ps == rhi::SwapchainStatus::Suboptimal || as == rhi::SwapchainStatus::Suboptimal)
                need_recreate = true;
            else if (ps == rhi::SwapchainStatus::Error)
                break;
            ++frames;
            if (frames % 60 == 0) {
                char title[256];
                std::snprintf(title, sizeof title, "alamo_render --sim %s - tick %" PRIu64 ", %u draws - %.1f fps (Esc quits)", world.name.c_str(),
                              session->host().frame(), r.draw_stats.draws, static_cast<double>(frames) * 1000.0 / std::max(1.0, ms_since(t0)));
                win.set_title(title);
            }
            // --ticks with --png: after the last tick, the settled offscreen capture, then exit; --ticks alone exits
            // once everything is drawn (bounded).
            if (ticks_done) {
                ++frames_after_ticks;
                if (r.settled() || frames_after_ticks > 1800) {
                    if (!opt.png.empty() && !r.capture_png(opt.png, sc.format(), opt.width, opt.height, opt.interp)) rc = 1;
                    break;
                }
            }
        }
        r.print_stats("sim view");
        std::printf("%ld frames, %" PRIu64 " ticks, final CRC %016" PRIx64 "\n", frames, session ? session->ticks() : 0,
                    session ? final_crc(session->host().crc_log()) : 0);
        r.ring.wait_idle();
        session.reset();
        r.shutdown();
        sc.shutdown();
    }
    const uint32_t ve = ctx.validation_errors(), vw = ctx.validation_warnings();
    std::printf("validation: %s, %u errors, %u warnings\n", ctx.validation_enabled() ? "on" : "off", ve, vw);
    if (ve || vw) rc = 1;
    ctx.shutdown();
    win.destroy();
    return rc;
}

}  // namespace

int run_sim_main(int argc, char** argv) {
    SimOptions opt;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (a == "--sim") opt.sim = true;
        else if (a == "--sim-headless-check") opt.check = true;
        else if (a == "--map") opt.map = next();
        else if (a == "--game") opt.game = next();
        else if (a == "--ticks") {
            opt.ticks = std::strtoull(next().c_str(), nullptr, 10);
            opt.ticks_set = true;
        } else if (a == "--seed") opt.seed = static_cast<uint32_t>(std::strtoul(next().c_str(), nullptr, 10));
        else if (a == "--workers") opt.workers = static_cast<int32_t>(std::strtol(next().c_str(), nullptr, 10));
        else if (a == "--interp") {
            const std::string m = next();
            if (m == "smooth") opt.interp = alamo::render::InterpMode::Smooth;
            else if (m == "original") opt.interp = alamo::render::InterpMode::Original;
            else return sim_usage();
        } else if (a == "--png") opt.png = next();
        else if (a == "--aniso") opt.aniso = static_cast<uint32_t>(std::strtoul(next().c_str(), nullptr, 10));
        else if (a == "--ao") opt.ao = true;
        else if (a == "--ao-strength") opt.ao_strength = std::strtof(next().c_str(), nullptr);
        else if (a == "--ao-radius") opt.ao_radius = std::strtof(next().c_str(), nullptr);
        else if (a == "--frames") opt.frames = std::atol(next().c_str());
        else if (a == "--width") opt.width = static_cast<uint32_t>(std::strtoul(next().c_str(), nullptr, 10));
        else if (a == "--height") opt.height = static_cast<uint32_t>(std::strtoul(next().c_str(), nullptr, 10));
        else if (a == "--hidden") opt.hidden = true;
        else if (a == "--no-anim") opt.anim = false;  // R3: bind pose (no unit animation)
        else if (a == "--no-sim-anim") opt.sim_anim = false;  // R3: no SimAnimation on the host; fallback SimAnimDriver
        else if (a == "--anim-crc") opt.anim_crc = true;      // R3: opt-in "anim" CRC system
        else if (a == "--anim-focus") opt.anim_focus = std::strtof(next().c_str(), nullptr);  // R3
        else if (a == "--no-particles") opt.particles = false;                                 // R4
        else if (a == "--expect-particles") opt.expect_particles = true;
        else if (a == "--particle-detail") opt.particle_detail = std::strtof(next().c_str(), nullptr);
        else if (a == "--frames-per-tick") opt.frames_per_tick = static_cast<uint32_t>(std::strtoul(next().c_str(), nullptr, 10));
        else if (a == "--camera-distance") opt.camera_distance = std::strtof(next().c_str(), nullptr);
        else if (a == "--cache-dir") opt.cache_dir = next();
        else if (a == "--mailbox") opt.mode = alamo::rhi::PresentMode::Mailbox;
        else if (a == "--immediate") opt.mode = alamo::rhi::PresentMode::Immediate;
        else if (a.find('=') != std::string::npos) opt.game_args.push_back(a);
        else return sim_usage();
    }
    if (opt.sim == opt.check || opt.width == 0 || opt.height == 0) return sim_usage();
    alamo::log::set_min_level(alamo::log::Level::Warn);
    World world;
    if (!load_world(opt, world)) return 1;
    return opt.check ? run_check(opt, world) : run_windowed(opt, world);
}
