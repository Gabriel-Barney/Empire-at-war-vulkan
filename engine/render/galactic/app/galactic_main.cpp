// alamo_render --galactic / --galactic-headless-check (Phase 4 roadmap R2: the galactic conquest map from the
// running galactic sim). Part of alamo_render.exe; dispatched from engine/render/app/main.cpp (R2 hook).
//   alamo_render --galactic [--campaign NAME|synthetic] [--game DIR] [--ticks N] [--seed S] [--interp original|smooth]
//                [--png PATH] [--frames N] [--width W] [--height H] [--hidden] [--overview] [--look-at X,Y]
//                [--camera-distance D] [--select]
//       Runs a galactic campaign through alamo_sim (SimHost::start_galactic + GalacticOrders between ticks) with the
//       real-time FrameSynchronizer pacing and renders the galaxy live: after every tick an immutable
//       GalacticSnapshot is published; every frame interpolates the two latest (original = latest as is, the
//       default; smooth = fleet positions blended by the tick alpha) and builds planets / routes / fleets / rings /
//       background instances. Camera = engine/ui GalacticCameraController (GameConstants GMC_* values) -> CameraParams
//       -> render::camera_view; strategic scroll / right-drag / wheel when the ui window adapter is built.
//       Without --campaign (or --campaign synthetic) a data-free 12-planet galaxy of procedural models. --png writes
//       an offscreen capture after the last tick (--ticks) once every model and pipeline is ready.
//   alamo_render --galactic-headless-check [--campaign NAME|synthetic] [--game DIR] [--ticks N] [--seed S]
//                [--frames-per-tick K] [--png PATH] [--overview] [--select]
//       The same campaign twice: rendering off, then a snapshot + K offscreen frames (both interpolation modes) after
//       every tick. Exit 0 only when the per-tick CRC logs (galactic.state included) and the final GalacticSim state
//       hash are identical and validation reported nothing.
// --select: the local player's first planet (the default camera start) is the selected planet: the planet-selection
// model (w_planet_select_neutral.alo, GALAXY_MAP_PRESENTATION §3) is drawn round it.
// Common: --workers N, --cache-dir DIR, --mailbox / --immediate, MODPATH=.. FALLBACKPATH=.. (real campaigns need
//         --game DIR or ALAMO_FOC_DATA, the corruption folder).
#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "alamo/assets/asset_cache.hpp"
#include "alamo/core/file_provider.hpp"
#include "alamo/core/log.hpp"
#include "alamo/core/sim_crc.hpp"
#include "alamo/data/xml.hpp"
#include "alamo/jobs/crc_recorder.hpp"
#include "alamo/jobs/job_system.hpp"
#include "alamo/render/fleet.hpp"
#include "alamo/render/galactic/galactic_session.hpp"
#include "alamo/render/galactic/galactic_snapshot.hpp"
#include "alamo/render/galactic/galactic_view.hpp"
#include "alamo/render/png.hpp"
#include "alamo/render/renderer.hpp"
#include "alamo/render/sim_view.hpp"
#include "alamo/rhi/rhi.hpp"
#include "alamo/ui/camera.hpp"
#include "alamo/ui/input.hpp"
#include "alamo/vfs/vfs.hpp"
#ifdef ALAMO_RENDER_GALACTIC_UI_INPUT
#include "alamo/ui/window_input.hpp"
#endif

int run_galactic_main(int argc, char** argv);

namespace {

using namespace alamo;
using Clock = std::chrono::steady_clock;

struct GalOptions {
    bool run = false, check = false, hidden = false, overview = false, select = false;
    std::string campaign, game, png, cache_dir;
    std::vector<std::string> game_args;
    uint64_t ticks = 0;
    bool ticks_set = false;
    uint32_t seed = 1;
    int32_t workers = -1;
    render::InterpMode interp = render::InterpMode::Original;
    long frames = -1;
    uint32_t width = 1280, height = 720;
    uint32_t frames_per_tick = 2;
    float camera_distance = -1.0f;
    bool look_at_set = false;
    float look_x = 0, look_y = 0;
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

int usage() {
    std::fprintf(stderr,
                 "usage: alamo_render --galactic [--campaign NAME|synthetic] [--game DIR] [--ticks N] [--seed S]\n"
                 "                    [--interp original|smooth] [--png PATH] [--frames N] [--width W] [--height H] [--hidden]\n"
                 "                    [--overview] [--look-at X,Y] [--camera-distance D] [--select]\n"
                 "       alamo_render --galactic-headless-check [--campaign NAME|synthetic] [--game DIR] [--ticks N] [--seed S]\n"
                 "                    [--frames-per-tick K] [--png PATH] [--overview] [--select]\n"
                 "       common: [--workers N] [--cache-dir DIR] [--mailbox|--immediate] [MODPATH=..] [FALLBACKPATH=..]\n"
                 "       real campaigns need --game DIR or ALAMO_FOC_DATA (the corruption folder)\n");
    return 2;
}

double ms_since(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }
uint64_t final_crc(const CrcLog& log) { return log.records.empty() ? 0 : log.records.back().combined; }

std::unique_ptr<render::GalacticWorld> load_world(const GalOptions& opt) {
    std::string err;
    if (opt.campaign.empty() || opt.campaign == "synthetic") {
        const std::filesystem::path dir = std::filesystem::temp_directory_path() / "alamo_render_galactic_synthetic";
        std::error_code ec;
        remove_all_long(dir, ec);
        auto w = render::make_synthetic_galactic_world(path_to_utf8(dir), &err);
        if (!w) std::fprintf(stderr, "synthetic galaxy: %s\n", err.c_str());
        return w;
    }
    const std::string root = opt.game.empty() ? env_or_empty("ALAMO_FOC_DATA") : opt.game;
    if (root.empty()) {
        std::fprintf(stderr, "--campaign %s needs --game <corruption dir> or ALAMO_FOC_DATA\n", opt.campaign.c_str());
        return nullptr;
    }
    auto files = vfs::mount_game(root, opt.game_args, &err);  // mounted exactly as alamo_headless does
    if (!files) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return nullptr;
    }
    const auto t0 = Clock::now();
    auto w = render::load_galactic_world(std::move(files), opt.campaign == "default" ? std::string() : opt.campaign, &err);
    if (!w) {
        std::fprintf(stderr, "campaign %s: %s\n", opt.campaign.c_str(), err.c_str());
        return nullptr;
    }
    std::printf("data: %zu object types, campaign %s: %zu planet rows, %zu routes, %zu players, %zu starting forces in %.0f ms\n",
                w->types.size(), w->name.c_str(), w->setup.map.planets.size(), w->setup.map.routes.size(), w->setup.players.size(),
                w->setup.forces.size(), ms_since(t0));
    return w;
}

// GameConstants (GMC_*, scroll speeds) for the galactic camera; vanilla defaults without data.
ui::UiConstants ui_constants(const render::GalacticWorld& w) {
    if (w.synthetic) return {};
    std::vector<uint8_t> bytes;
    if (!w.files->read("Data\\XML\\GameConstants.xml", bytes)) return {};
    const data::XmlDocument doc = data::XmlDocument::parse(bytes, "GameConstants.xml");
    return doc.ok() ? ui::parse_ui_constants(doc.root()) : ui::UiConstants{};
}

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
        cd.name = "galactic.offscreen";
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

// Everything the render side owns. Never writes the sim: capture() reads it through its const interface.
class GalacticRender {
public:
    GalacticRender(rhi::Context& ctx, render::GalacticWorld& world, jobs::JobSystem& js, const GalOptions& opt)
        : ctx_(ctx), world_(world), js_(js), opt_(opt) {}

    bool init(VkFormat color_format) {
        rhi::FrameRingDesc fd;
        fd.frames_in_flight = 2;
        fd.uniform_bytes = world_.synthetic ? (16ull << 20) : (64ull << 20);
        fd.recording_threads = render::ring_recording_threads(1, js_.thread_count());
        if (!alloc.init(ctx_) || !up.init(ctx_, alloc) || !ring.init(ctx_, alloc, fd)) {
            std::fprintf(stderr, "rhi init failed\n");
            return false;
        }
        render::RendererDesc rd;
        rd.color_format = color_format;
        rd.depth_format = VK_FORMAT_D32_SFLOAT;
        rd.js = &js_;
        if (!world_.synthetic) {
            effects_ = std::make_unique<render::EffectCache>(world_.files.get());
            rd.effects = effects_.get();
            rd.pipeline_cache_dir = opt_.cache_dir.empty() ? render::default_pipeline_cache_dir() : path_from_utf8(opt_.cache_dir);
            if (!opt_.cache_dir.empty()) rd.shader_cache_dir = path_from_utf8(opt_.cache_dir) / "shaders";
        } else {
            rd.translated = false;
        }
        if (!renderer.init(ctx_, alloc, up, rd)) {
            std::fprintf(stderr, "renderer init failed\n");
            return false;
        }
        renderer.set_frame_ring(&ring);
        // R2: the galactic light record (GameConstants Galactic_Zoom_*_Light_*; FoC defaults for the synthetic world).
        if (world_.files) light_ = render::load_galactic_light_settings(*world_.files);
        renderer.set_lighting(render::galactic_lighting(light_));  // per frame: prepare() follows the camera's zoom blend
        renderer.set_object_fade(-1.0f, -1.0f);  // galactic: no distance fade (SPACE_AND_OPEN.md B.8)
        types.build(world_.types);
        if (!world_.synthetic) {
            cache_ = std::make_unique<assets::AssetCache>(*world_.files, effects_->model_load_options());
            streamer_ = std::make_unique<render::ModelStreamer>(renderer, *cache_, js_);
        }
        models_ = std::make_unique<render::GalacticModels>(renderer, streamer_.get(), cache_.get(), world_.files.get());
        ropt.threads = js_.worker_count();
        ropt.space = true;     // background list = the galactic background scene (world transforms, near 10 / far 100000)
        ropt.galactic = true;  // no bloom / heat
        ropt.shadows = false;
        ropt.post = !world_.synthetic;
        camera_ = ui::GalacticCameraController(ui_constants(world_).galactic, ui_constants(world_).strategic);
        return true;
    }

    // After the session started (palette and view options come from the sim's players).
    void attach(const galactic::GalacticSim& sim) {
        palette = render::make_galactic_palette(sim, world_.faction_colors);
        render::GalacticViewOptions vo = render::galactic_view_options(sim);
        vo.corruption_path = world_.corruption_path;
        builder_ = std::make_unique<render::GalacticSceneBuilder>(types, world_.route_style, vo);
        // Camera: the galaxy box bounds the look-at; start at --look-at, the overview centre, or the local player's
        // first planet (default).
        float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
        for (size_t i = 0; i < sim.map().planets.size(); ++i) {
            if (!(sim.map().planets.flags[i] & kPlanetHasPlanetBehavior)) continue;
            const Vec3& p = sim.map().planets.position[i];
            x0 = std::min(x0, p.x);
            y0 = std::min(y0, p.y);
            x1 = std::max(x1, p.x);
            y1 = std::max(y1, p.y);
        }
        if (x0 > x1) x0 = y0 = x1 = y1 = 0;
        camera_.set_bounds({x0, y0, x1, y1});
        extent_ = std::max(x1 - x0, y1 - y0);
        centre_ = {0.5f * (x0 + x1), 0.5f * (y0 + y1), 0};
        if (opt_.look_at_set) camera_.set_look_at({opt_.look_x, opt_.look_y, 0});
        else if (opt_.overview) camera_.set_look_at({centre_.x, centre_.y, 0});
        else {
            // [port] the local player's first planet (row order); else GameConstants Starting_Galactic_Camera_Position.
            // The campaign's Camera_Shift_X / Camera_Shift_Y / Camera_Distance (CampaignDef +0xcc/+0xd0/+0xd4) are not
            // documented (open item).
            const int32_t me = render::galactic_view_options(sim).viewpoint;
            bool found = false;
            for (const galactic::PlanetState& p : sim.planets())
                if (p.is_planet && p.owner == me) {
                    camera_.set_look_at({p.position.x, p.position.y, 0});
                    found = true;
                    break;
                }
            if (!found) camera_.set_look_at(ui_constants(world_).galactic.start_position);
        }
        // --select: the local player's first planet (row order).
        selected_ = objects::kNoObjectId;
        if (opt_.select) {
            const int32_t me = render::galactic_view_options(sim).viewpoint;
            for (const galactic::PlanetState& p : sim.planets())
                if (p.is_planet && p.owner == me && sim.store().exists(p.object)) {
                    selected_ = sim.store().id(p.object);
                    break;
                }
        }
    }

    void capture(const sim::SimHost& host) {
        auto snap = std::make_shared<render::GalacticSnapshot>();
        render::capture_galactic_snapshot(host, types, palette, *snap, jobs::make_parallel_for(js_));
        last_fleets_ = static_cast<uint32_t>(snap->fleets.size());
        buffer.publish(std::move(snap));
    }

    void prepare(float alpha, render::InterpMode mode) {
        models_->update();
        const render::GalacticSnapshotBuffer::Pair p = buffer.latest();
        main_.clear();
        bg_.clear();
        if (p.curr && builder_) {
            render::interpolate_galactic(p.prev.get(), *p.curr, alpha, mode, frame_);
            // Selection model input: the selected planet and the frame's camera (it faces the screen).
            render::GalacticSelection sel;
            sel.planet = selected_;
            const render::CameraParams cp = camera_params(opt_.width, std::max(1u, opt_.height));
            const auto eye = render::orbit_camera(cp).eye();
            sel.eye = {eye.x, eye.y, eye.z};
            sel.target = {cp.target.x, cp.target.y, cp.target.z};
            sel.up = {0, 0, 1};
            builder_->set_selection(sel);
            // FIXPASS_GALACTIC 3.1 route zoom factor and R2 t03 light zoom blend: 0 zoomed out, p zooming in, 1 - p out.
            builder_->set_route_zoom(camera_.zoom_blend());
            renderer.set_lighting(render::galactic_lighting(light_, camera_.zoom_blend()));
            builder_->build(frame_, *models_, main_, bg_, &stats_);
        }
    }

    render::CameraParams camera_params(uint32_t w, uint32_t h) const {
        const ui::CameraParams cp = camera_.params(static_cast<float>(w) / static_cast<float>(h));
        render::CameraParams p;
        p.target = {cp.target.x, cp.target.y, cp.target.z};
        p.yaw = cp.orbit_yaw;
        p.pitch = cp.orbit_pitch;
        p.distance = cp.distance;
        p.fov_x = cp.fov_x;
        p.znear = cp.znear;
        p.zfar = cp.zfar;
        if (opt_.camera_distance > 0) p.distance = opt_.camera_distance;
        else if (opt_.overview && !opt_.look_at_set) {
            // [port] overview: fit the galaxy's planet box (plus a margin) in the horizontal FOV.
            p.distance = std::max(p.distance, 0.62f * extent_ / std::tan(0.5f * p.fov_x));
        }
        p.zfar = std::max(p.zfar, p.distance * 2.5f);
        return p;
    }
    ui::GalacticCameraController& camera() { return camera_; }

    bool draw(rhi::Frame& f, VkCommandBuffer cmd, render::PassTarget& pt) {
        pt.clear[0] = pt.clear[1] = pt.clear[2] = 0.0f;
        pt.clear[3] = 1.0f;
        const render::ViewMatrices v = render::camera_view(camera_params(pt.extent.width, pt.extent.height), pt.extent.width, pt.extent.height);
        return renderer.render(f, cmd, pt, v, main_, render::DrawMode::Lit, ropt, &draw_stats, nullptr, bg_);
    }

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

    bool settled() const {
        return models_ && models_->all_finished() && draw_stats.pending_pipeline == 0 && draw_stats.translation_pending == 0 &&
               draw_stats.post_pending == 0;
    }

    bool capture_png(const std::string& path, VkFormat fmt, uint32_t w, uint32_t h, render::InterpMode mode) {
        Offscreen o;
        if (!o.create(alloc, w, h, fmt)) {
            std::fprintf(stderr, "--png: offscreen target for format %d failed\n", static_cast<int>(fmt));
            return false;
        }
        bool ok = false;
        for (int i = 0; i < 900; ++i) {  // models stream and pipelines compile: wait until a frame is complete
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
        std::printf("%s: %u planets, %u background, %u starbases, %u rings, %u routes (%u sparks), %u corruption path lines, %u fleets in transit (%u stealthed, %u in orbit "
                    "not drawn, %u in snapshot), %u fleet lines, %u selection, %u animated, %u without model yet; models %u requested, "
                    "%u ready, %u failed; last frame %u draws, %u instances (%u culled)\n",
                    what, stats_.planets, stats_.background, stats_.starbases, stats_.rings, stats_.routes, stats_.route_sparks,
                    stats_.corruption_paths, stats_.fleets,
                    stats_.stealth_fleets, stats_.orbit_fleets + stats_.fogged_fleets, last_fleets_, stats_.fleet_lines, stats_.selection,
                    stats_.animated, stats_.no_model, models_ ? models_->requested() : 0u,
                    models_ ? models_->ready() : 0u, models_ ? models_->failed() : 0u, draw_stats.draws, draw_stats.instances,
                    draw_stats.culled);
    }

    void shutdown() {
        ring.wait_idle();
        ctx_.wait_idle();
        if (models_) models_->destroy();
        if (streamer_) streamer_->destroy_all();
        streamer_.reset();
        models_.reset();
        renderer.shutdown();
        cache_.reset();
        effects_.reset();
        ring.shutdown();
        up.shutdown();
        alloc.shutdown();
    }

    rhi::Allocator alloc;
    rhi::Uploader up;
    rhi::FrameRing ring;
    render::Renderer renderer;
    render::GalacticLightSettings light_;  // GameConstants Galactic_Zoom_*_Light_* (FoC defaults for the synthetic world)
    render::GalacticTypeTable types;
    render::GalacticPalette palette;
    render::GalacticSnapshotBuffer buffer;
    render::RecordOptions ropt;
    render::DrawStats draw_stats;

private:
    rhi::Context& ctx_;
    render::GalacticWorld& world_;
    jobs::JobSystem& js_;
    const GalOptions& opt_;
    std::unique_ptr<render::EffectCache> effects_;
    std::unique_ptr<assets::AssetCache> cache_;
    std::unique_ptr<render::ModelStreamer> streamer_;
    std::unique_ptr<render::GalacticModels> models_;
    std::unique_ptr<render::GalacticSceneBuilder> builder_;
    render::GalacticFrame frame_;
    std::vector<render::Instance> main_, bg_;
    render::GalacticViewStats stats_;
    uint32_t last_fleets_ = 0;
    ui::GalacticCameraController camera_;
    float extent_ = 600.0f;
    Vec3 centre_{};
    objects::ObjectId selected_ = objects::kNoObjectId;  // --select
};

// Ownership changes since the start (diagnostic).
uint32_t owner_changes(const galactic::GalacticSim& gs, const std::vector<int32_t>& start) {
    uint32_t n = 0;
    for (size_t i = 0; i < gs.planets().size() && i < start.size(); ++i) n += gs.planets()[i].owner != start[i] ? 1u : 0u;
    return n;
}
std::vector<int32_t> owners(const galactic::GalacticSim& gs) {
    std::vector<int32_t> o;
    for (const galactic::PlanetState& p : gs.planets()) o.push_back(p.owner);
    return o;
}

// ----------------------------------------------------------------------------------- headless check
int run_check(const GalOptions& opt, render::GalacticWorld& world) {
    const uint64_t ticks = opt.ticks_set ? opt.ticks : 600;
    render::GalacticSessionSpec spec;
    spec.seed = opt.seed;
    // Run A: rendering off.
    CrcLog off;
    uint64_t hash_off = 0;
    {
        jobs::JobSystem js(jobs::JobSystemConfig{opt.workers});
        std::vector<std::string> diag;
        render::GalacticSession s(world.types, js, world.rules, world.setup, spec, &diag);
        if (!s.ok()) {
            for (const auto& d : diag) std::fprintf(stderr, "%s\n", d.c_str());
            std::fprintf(stderr, "galactic campaign did not start\n");
            return 1;
        }
        const std::vector<int32_t> start = owners(*s.sim());
        const auto t0 = Clock::now();
        for (uint64_t i = 0; i < ticks && s.tick(); ++i) {
        }
        off = s.host().crc_log();
        hash_off = s.state_hash();
        uint32_t moving = 0;
        for (const galactic::FleetState& f : s.sim()->fleets()) moving += (f.alive && f.moving) ? 1u : 0u;
        std::printf("rendering off: %" PRIu64 " ticks in %.0f ms, galactic frame %d, %" PRIu64 " orders, %" PRIu64
                    " ground units boarded, %zu fleets (%u in flight), %u planets changed hands, state %016" PRIx64 ", final CRC %016" PRIx64
                    "\n",
                    s.ticks(), ms_since(t0), s.sim()->frame(), s.orders(), s.boarded(), s.sim()->fleets().size(), moving,
                    owner_changes(*s.sim(), start), hash_off, final_crc(off));
    }
    // Run B: snapshot + offscreen frames after every tick.
    const bool strict = render::setup_validation_env(ALAMO_RENDER_SOURCE_DIR);
    rhi::Context ctx;
    rhi::ContextDesc cd;
    cd.app_name = "alamo_render_galactic_check";
    cd.validation = strict ? rhi::Validation::Required : rhi::Validation::IfAvailable;
    if (!ctx.init(cd)) {
        std::fprintf(stderr, "no suitable Vulkan 1.3 device%s\n", strict ? " (or the validation layer failed to load)" : "");
        return 1;
    }
    std::printf("device %s, validation %s%s\n", ctx.caps().name.c_str(), ctx.validation_enabled() ? "on" : "off", strict ? " (required)" : "");
    int rc = 0;
    CrcLog on;
    uint64_t hash_on = 0;
    {
        jobs::JobSystem js(jobs::JobSystemConfig{opt.workers});
        GalacticRender r(ctx, world, js, opt);
        Offscreen o;
        bool ok = r.init(VK_FORMAT_R8G8B8A8_UNORM) && o.create(r.alloc, opt.width, opt.height, VK_FORMAT_R8G8B8A8_UNORM);
        if (ok) {
            render::GalacticSession s(world.types, js, world.rules, world.setup, spec);
            ok = s.ok();
            if (ok) {
                r.attach(*s.sim());
                r.capture(s.host());
                const auto t0 = Clock::now();
                uint64_t frames = 0;
                const uint32_t k = std::max(1u, opt.frames_per_tick);
                for (uint64_t i = 0; i < ticks && ok && s.tick(); ++i) {
                    r.capture(s.host());
                    for (uint32_t f = 0; f < k && ok; ++f) {
                        const render::InterpMode m = (f % 2 == 0) ? render::InterpMode::Original : render::InterpMode::Smooth;
                        ok = r.offscreen_frame(o, static_cast<float>(f) / static_cast<float>(k), m, false);
                        ++frames;
                    }
                }
                on = s.host().crc_log();
                hash_on = s.state_hash();
                std::printf("rendering on: %" PRIu64 " ticks, %" PRIu64 " frames in %.0f ms, state %016" PRIx64 ", final CRC %016" PRIx64 "\n",
                            s.ticks(), frames, ms_since(t0), hash_on, final_crc(on));
                r.print_stats("rendering on");
                if (ok && !opt.png.empty()) ok = r.capture_png(opt.png, VK_FORMAT_R8G8B8A8_UNORM, opt.width, opt.height, opt.interp);
            }
        }
        if (!ok) rc = 1;
        ctx.wait_idle();
        o.destroy(r.alloc);
        r.shutdown();
    }
    const CrcDivergence d = first_divergence(off, on);
    std::printf("per-tick CRC, rendering off vs on: %zu vs %zu records: %s\n", off.records.size(), on.records.size(), d.message.c_str());
    std::printf("galactic state hash, rendering off vs on: %016" PRIx64 " vs %016" PRIx64 ": %s\n", hash_off, hash_on,
                hash_off == hash_on ? "identical" : "DIFFERENT");
    if (d.diverged || off.records.size() != on.records.size() || off.records.empty() || hash_off != hash_on) rc = 1;
    const uint32_t ve = ctx.validation_errors(), vw = ctx.validation_warnings();
    std::printf("validation: %s, %u errors, %u warnings\n", ctx.validation_enabled() ? "on" : "off", ve, vw);
    if (ve || vw) rc = 1;
    ctx.shutdown();
    std::printf(rc == 0 ? "galactic headless check: OK\n" : "galactic headless check: FAILED\n");
    return rc;
}

// ---------------------------------------------------------------------------------------- windowed
int run_windowed(const GalOptions& opt, render::GalacticWorld& world) {
    const bool strict = render::setup_validation_env(ALAMO_RENDER_SOURCE_DIR);
    rhi::Window win;
    if (!win.create({"alamo_render --galactic (Esc quits)", opt.width, opt.height, true, !opt.hidden})) return 1;
    rhi::Context ctx;
    rhi::ContextDesc cd;
    cd.app_name = "alamo_render_galactic";
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
        GalacticRender r(ctx, world, js, opt);
        rhi::Swapchain sc;
        bool ok = true;
        {
            rhi::SwapchainDesc sd;
            sd.hinstance = win.native_instance();
            sd.hwnd = win.native_handle();
            sd.width = win.width();
            sd.height = win.height();
            sd.mode = opt.mode;
            ok = sc.init(ctx, sd);
        }
        ok = ok && r.init(sc.format());
        if (!ok) rc = 1;
        std::unique_ptr<render::GalacticSession> session;
        if (ok) {
            render::GalacticSessionSpec spec;
            spec.seed = opt.seed;
            std::vector<std::string> diag;
            session = std::make_unique<render::GalacticSession>(world.types, js, world.rules, world.setup, spec, &diag);
            if (!session->ok()) {
                for (const auto& d : diag) std::fprintf(stderr, "%s\n", d.c_str());
                ok = false;
                rc = 1;
            } else {
                r.attach(*session->sim());
                std::printf("galactic %s: %zu planet rows, %zu fleets, %" PRIu64 " ground units boarded; interpolation %s\n", world.name.c_str(),
                            session->sim()->planets().size(), session->sim()->fleets().size(), session->boarded(),
                            render::interp_mode_name(opt.interp));
                r.capture(session->host());  // tick 0: the campaign start
            }
        }
        const auto t0 = Clock::now();
        auto now_ms = [&] { return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count()); };
        uint32_t last_tick_ms = 0;
        long frames = 0, frames_after_ticks = 0;
        bool need_recreate = false, ticks_done = false;
        double last_s = 0.0;
        ui::InputState input;
        const ui::Bindings keys;
        while (ok && win.pump()) {
            const double now_s = ms_since(t0) / 1000.0;
            const float dt = static_cast<float>(std::clamp(now_s - last_s, 0.0, 0.1));
            last_s = now_s;
#ifdef ALAMO_RENDER_GALACTIC_UI_INPUT
            input.update(ui::capture_input(win, now_s, dt));  // right after pump, before the tick (MAIN_LOOP step 3)
            if (!opt.hidden) r.camera().update(input, keys, dt);  // hidden: the cursor sits at (0, 0) = edge scroll
#endif
            if (win.key_down(0x1B)) break;  // VK_ESCAPE
            if (opt.frames >= 0 && frames >= opt.frames) break;
            if (win.take_resized()) need_recreate = true;
            if (need_recreate) {
                r.ring.wait_idle();
                sc.recreate(win.width(), win.height());
                if (sc.valid() && !r.renderer.set_color_format(sc.format())) break;
                need_recreate = false;
            }
            const uint32_t now = now_ms();
            if (!ticks_done) {
                if (session->loop_iteration(now)) {
                    last_tick_ms = now;
                    r.capture(session->host());
                }
                if (opt.ticks && session->ticks() >= opt.ticks) {
                    ticks_done = true;
                    std::printf("galactic: %" PRIu64 " ticks in %.0f ms, %" PRIu64 " orders, state %016" PRIx64 ", final CRC %016" PRIx64 "\n",
                                session->ticks(), ms_since(t0), session->orders(), session->state_hash(),
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
            r.renderer.set_time(static_cast<float>(now_s));
            r.prepare(alpha, opt.interp);
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
                std::snprintf(title, sizeof title, "alamo_render --galactic %s - galactic frame %d, day %d, %u draws - %.1f fps (Esc quits)",
                              world.name.c_str(), session->sim()->frame(), session->sim()->galactic_day(), r.draw_stats.draws,
                              static_cast<double>(frames) * 1000.0 / std::max(1.0, ms_since(t0)));
                win.set_title(title);
            }
            if (ticks_done) {
                ++frames_after_ticks;
                if (r.settled() || frames_after_ticks > 1800) {
                    if (!opt.png.empty() && !r.capture_png(opt.png, sc.format(), opt.width, opt.height, opt.interp)) rc = 1;
                    break;
                }
            }
        }
        (void)keys;
        (void)input;
        r.print_stats("galactic view");
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

int run_galactic_main(int argc, char** argv) {
    GalOptions opt;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (a == "--galactic") opt.run = true;
        else if (a == "--galactic-headless-check") opt.check = true;
        else if (a == "--campaign") opt.campaign = next();
        else if (a == "--game") opt.game = next();
        else if (a == "--ticks") {
            opt.ticks = std::strtoull(next().c_str(), nullptr, 10);
            opt.ticks_set = true;
        } else if (a == "--seed") opt.seed = static_cast<uint32_t>(std::strtoul(next().c_str(), nullptr, 10));
        else if (a == "--workers") opt.workers = static_cast<int32_t>(std::strtol(next().c_str(), nullptr, 10));
        else if (a == "--interp") {
            const std::string m = next();
            if (m == "smooth") opt.interp = render::InterpMode::Smooth;
            else if (m == "original") opt.interp = render::InterpMode::Original;
            else return usage();
        } else if (a == "--png") opt.png = next();
        else if (a == "--frames") opt.frames = std::atol(next().c_str());
        else if (a == "--width") opt.width = static_cast<uint32_t>(std::strtoul(next().c_str(), nullptr, 10));
        else if (a == "--height") opt.height = static_cast<uint32_t>(std::strtoul(next().c_str(), nullptr, 10));
        else if (a == "--hidden") opt.hidden = true;
        else if (a == "--overview") opt.overview = true;
        else if (a == "--select") opt.select = true;
        else if (a == "--frames-per-tick") opt.frames_per_tick = static_cast<uint32_t>(std::strtoul(next().c_str(), nullptr, 10));
        else if (a == "--camera-distance") opt.camera_distance = std::strtof(next().c_str(), nullptr);
        else if (a == "--look-at") {
            const std::string v = next();
            char* end = nullptr;
            opt.look_x = std::strtof(v.c_str(), &end);
            opt.look_y = (end && *end == ',') ? std::strtof(end + 1, nullptr) : 0.0f;
            opt.look_at_set = true;
        } else if (a == "--cache-dir") opt.cache_dir = next();
        else if (a == "--mailbox") opt.mode = rhi::PresentMode::Mailbox;
        else if (a == "--immediate") opt.mode = rhi::PresentMode::Immediate;
        else if (a.find('=') != std::string::npos) opt.game_args.push_back(a);
        else return usage();
    }
    if (opt.run == opt.check || opt.width == 0 || opt.height == 0) return usage();
    log::set_min_level(log::Level::Warn);
    std::unique_ptr<render::GalacticWorld> world = load_world(opt);
    if (!world) return 1;
    return opt.check ? run_check(opt, *world) : run_windowed(opt, *world);
}
