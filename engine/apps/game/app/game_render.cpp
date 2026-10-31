#include "game_render.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "alamo/data/xml.hpp"
#include "alamo/render/galactic/galactic_session.hpp"
#include "alamo/render/pipelines.hpp"
#include "alamo/sim/tactical_battle.hpp"
#include "alamo/ui/render_bridge.hpp"
#ifdef ALAMO_SIM_HAVE_ANIM
#include "alamo/sim/sim_anim.hpp"
#endif
// --- OI render: Lua Attach_Particle_Effect requests (story VMs) ---
#ifdef ALAMO_SIM_HAVE_STORY
#include "alamo/lua/presentation.hpp"
#include "alamo/sim/story_session.hpp"
#endif
// --- end OI render ---

namespace alamo::game {

namespace {

data::XmlDocument read_xml(const FileProvider& files, const char* path) {
    std::vector<uint8_t> bytes;
    if (!files.read(path, bytes)) return data::XmlDocument::parse(std::string_view(), path);
    return data::XmlDocument::parse(bytes, path);
}

ui::UiConstants ui_constants(const FileProvider& files) {
    const data::XmlDocument doc = read_xml(files, "Data\\XML\\GameConstants.xml");
    return doc.ok() ? ui::parse_ui_constants(doc.root()) : ui::UiConstants{};
}

// --- OI render: R2 galactic light record (STRATEGIC_GAPS §6.1: GameConstants Galactic_Zoom_*_Light_*), read once per
// file provider (frame thread only) ---
const render::GalacticLightSettings& galactic_light_settings(const FileProvider* files) {
    static const FileProvider* loaded_for = nullptr;
    static render::GalacticLightSettings settings;
    if (files && files != loaded_for) {
        settings = render::load_galactic_light_settings(*files);
        loaded_for = files;
    }
    return settings;
}
// --- end OI render ---

}  // namespace

// ------------------------------------------------------------------------------------------------ targets
bool FrameTargets::create(rhi::Allocator& a, uint32_t n, uint32_t w, uint32_t h) {
    width = w;
    height = h;
    slots.resize(n);
    for (Slot& s : slots) {
        rhi::ImageDesc cd;
        cd.format = rhi::Format::RGBA8;
        cd.width = w;
        cd.height = h;
        cd.mips = 1;
        cd.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        cd.name = "game.color";
        rhi::ImageDesc dd = cd;
        dd.format = rhi::Format::D32F;
        dd.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        dd.name = "game.hud_depth";
        if (!a.create_image(cd, s.color) || !a.create_image(dd, s.depth)) return false;
    }
    return a.create_buffer(static_cast<VkDeviceSize>(w) * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, rhi::MemoryUsage::Readback, readback);
}

void FrameTargets::destroy(rhi::Allocator& a) {
    for (Slot& s : slots) {
        if (s.color) a.destroy(s.color);
        if (s.depth) a.destroy(s.depth);
    }
    slots.clear();
    if (readback) a.destroy(readback);
}

// ------------------------------------------------------------------------------------------------ GPU core
bool GpuCore::init(rhi::Context& c, const GameData& d, jobs::JobSystem& j, const std::string& cache_dir, std::string* err) {
    ctx = &c;
    data = &d;
    js = &j;
    rhi::FrameRingDesc fd;
    fd.frames_in_flight = 2;
    fd.uniform_bytes = 128ull << 20;
    fd.recording_threads = render::ring_recording_threads(1, j.thread_count());
    if (!alloc.init(c) || !up.init(c, alloc) || !ring.init(c, alloc, fd)) {
        if (err) *err = "rhi init failed";
        return false;
    }
    effects = std::make_unique<render::EffectCache>(d.files.get());
    render::RendererDesc rd;
    rd.color_format = kColorFormat;
    rd.depth_format = kDepthFormat;
    rd.js = &j;
    rd.effects = effects.get();
    rd.pipeline_cache_dir = cache_dir.empty() ? render::default_pipeline_cache_dir() : path_from_utf8(cache_dir);
    if (!cache_dir.empty()) rd.shader_cache_dir = path_from_utf8(cache_dir) / "shaders";
    if (!renderer.init(c, alloc, up, rd)) {
        if (err) *err = "renderer init failed";
        return false;
    }
    renderer.set_frame_ring(&ring);
    ui::HudRendererDesc hd;
    hd.color_format = kColorFormat;
    hd.depth_format = kDepthFormat;
    if (!hud.init(c, alloc, up, hd)) {
        if (err) *err = "HUD renderer init failed";
        return false;
    }
    cache = std::make_unique<assets::AssetCache>(*d.files, effects->model_load_options());
    streamer = std::make_unique<render::ModelStreamer>(renderer, *cache, j);
    resolver = std::make_unique<render::GameTypeResolver>(d.db);
    if (!resolver->load_render_xml(*d.files)) std::fprintf(stderr, "hardpoint XML not found: damage decals use the name rule\n");
    return true;
}

void GpuCore::shutdown() {
    if (!ctx) return;
    ring.wait_idle();
    ctx->wait_idle();
    renderer.set_scene_hook(nullptr);
    if (streamer) streamer->destroy_all();
    streamer.reset();
    hud.shutdown();
    renderer.collect_released();
    renderer.shutdown();
    cache.reset();
    effects.reset();
    ring.shutdown();
    up.shutdown();
    alloc.shutdown();
    ctx = nullptr;
}

// ------------------------------------------------------------------------------------------------ galactic
void GalacticView::init(GpuCore& gpu, const GameData& data, std::vector<render::FactionColor> colors, render::RouteLineStyle style) {
    gpu_ = &gpu;
    colors_ = std::move(colors);
    style_ = style;
    // FIXPASS_GALACTIC §3: the corruption path settings and the evil side name (GameConstants).
    corruption_path_ = render::load_corruption_path_settings(*data.files);
    {
        const data::XmlDocument gc = read_xml(*data.files, "Data\\XML\\GameConstants.xml");
        if (gc.ok())
            if (const std::string_view evil = gc.root().child_text("Evil_Side_Name"); !evil.empty()) evil_side_name_ = std::string(evil);
    }
    types_.build(data.types);
    models_ = std::make_unique<render::GalacticModels>(gpu.renderer, gpu.streamer.get(), gpu.cache.get(), data.files.get());
    portrait_models_ = std::make_unique<render::GalacticModels>(gpu.renderer, gpu.streamer.get(), gpu.cache.get(), data.files.get());
    portraits_ = std::make_unique<render::PlanetPortraits>(gpu.renderer, gpu.alloc, gpu.ring, *portrait_models_, types_);
    portraits_->set_lighting(render::galactic_lighting(galactic_light_settings(data.files.get()), 1.0f));
    ropt.threads = gpu.js->worker_count();
    ropt.space = true;     // background list = the galactic background scene
    ropt.galactic = true;  // no bloom / heat
    ropt.shadows = false;
    const ui::UiConstants uc = ui_constants(*data.files);
    camera_ = ui::GalacticCameraController(uc.galactic, uc.strategic);
    corruption_ = render::load_galactic_corruption_settings(*data.files);
#ifdef ALAMO_PARTICLES_HAVE_SIM
    pworld_ = std::make_unique<render::particles::ParticleWorld>(1u << 18);  // [port] capacity: corruption smoke only
    plib_ = std::make_unique<render::particles::ParticleLibrary>(data.files.get());
    render::particles::ParticleTextureSource tex = [&gpu](std::string_view n) { return gpu.cache->texture(n, assets::TextureRole::Color); };
    if (prender_.init(*gpu.ctx, gpu.alloc, gpu.up, *pworld_, std::move(tex))) {
        prender_.set_sort_range(render::MapScene::kFar);
        pattach_ = std::make_unique<render::particles::ProxyAttachments>(*pworld_, *plib_);
    } else {
        std::fprintf(stderr, "galactic: particle renderer init failed - no corruption smoke\n");
    }
#endif
}

void GalacticView::attach(const galactic::GalacticSim& sim, int32_t local_player) {
    palette_ = render::make_galactic_palette(sim, colors_);
    render::GalacticViewOptions vo = render::galactic_view_options(sim, evil_side_name_);
    vo.viewpoint = local_player;
    vo.corruption_path = corruption_path_;
    vo.multiplayer = false;  // [port] multiplayer is deferred: every session is single-player
    vo.rings = false;        // the game's planet ellipse is the HUD's g_planet_ring (C2_galactic_silent.png); the 3D ring is the viewer's
    local_ = local_player;
    builder_ = std::make_unique<render::GalacticSceneBuilder>(types_, style_, vo);
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
    set_camera_bounds({x0, y0, x1, y1});  // planet centres; galactic_input widens it to the planet spheres every frame
    // [port] start on the local player's first planet (row order), else the galaxy centre.
    Vec3 look{0.5f * (x0 + x1), 0.5f * (y0 + y1), 0};
    for (const galactic::PlanetState& p : sim.planets())
        if (p.is_planet && p.owner == local_player) {
            look = {p.position.x, p.position.y, 0};
            break;
        }
    camera_.set_look_at({look.x, look.y, 0});
    buffer_.clear();
}

void GalacticView::capture(const sim::SimHost& host) {
    auto snap = std::make_shared<render::GalacticSnapshot>();
    if (!render::capture_galactic_snapshot(host, types_, palette_, *snap, jobs::make_parallel_for(*gpu_->js))) return;
    // Fog globes: planets the local player has not revealed (PlayerView) draw fogged.
    if (const galactic::GalacticSim* g = render::find_galactic_sim(host)) render::apply_galactic_fog(*g, local_, *snap);
    buffer_.publish(std::move(snap));
}

void GalacticView::prepare(float alpha, double now_s, uint32_t selected_planet, const ui::CameraParams* cam) {
    models_->update();
    main.clear();
    background.clear();
    const render::GalacticSnapshotBuffer::Pair p = buffer_.latest();
    if (!p.curr || !builder_) return;
    render::interpolate_galactic(p.prev.get(), *p.curr, alpha, render::InterpMode::Smooth, frame_);  // alpha 1 = the current snapshot (Original)
    render::GalacticSelection sel;
    sel.planet = selected_planet ? static_cast<objects::ObjectId>(selected_planet) : objects::kNoObjectId;
    if (cam) {
        sel.eye = {cam->eye.x, cam->eye.y, cam->eye.z};
        sel.target = {cam->target.x, cam->target.y, cam->target.z};
        sel.up = {cam->up.x, cam->up.y, cam->up.z};
    }
    builder_->set_selection(sel);
    builder_->set_route_zoom(camera_.zoom_blend());  // FIXPASS_GALACTIC §3.3: route tint blend with the zoom
    builder_->build(frame_, *models_, main, background);
    // Corruption smoke: kept alive by key; the particles advance on wall time (presentation only, the galactic map
    // animates while paused too [port]).
    corruption_fx_.clear();
    render::galactic_corruption_effects(frame_, corruption_, corruption_fx_);
    corruption_live_ = 0;
#ifdef ALAMO_PARTICLES_HAVE_SIM
    if (pattach_) {
        pattach_->begin();
        for (const render::GalacticCorruptionEffect& e : corruption_fx_) {
            if (!e.type) continue;
            const objects::TypeIndex t = types_.find(*e.type);
            if (!types_.valid(t) || types_[t].model.empty()) continue;
            render::particles::SystemPtr def = plib_->get(types_[t].model);
            if (!def) continue;
            pattach_->sync_effect(e.key, std::move(def), e.transform, 1.0f);
            if (e.scalars) pattach_->set_effect_scalars(e.key, e.brightness, e.scale, e.energy);  // GRC 4.5 transition ramp
            ++corruption_live_;
        }
        pattach_->end();
        const double dt = now_s >= 0.0 && last_frame_s_ >= 0.0 ? std::clamp(now_s - last_frame_s_, 0.0, 0.1) : 1.0 / 30.0;
        if (now_s >= 0.0) last_frame_s_ = now_s;
        pworld_->update(static_cast<float>(dt));
    }
#endif
}

void GalacticView::use_scene(GpuCore& gpu) {
    // --- OI render: the galactic light record from GameConstants; R2 t03: the planet-zoom lerp (STRATEGIC_GAPS §6.1,
    // 0x3c3bf0: intensity -> Galactic_Zoom_Light_Level, angles -> Galactic_Zoom_In_Light_Angle over GMC_ZoomTime of wall
    // time, linear in the progress) follows the camera's zoom blend: 0 zoomed out, p zooming in, 1 - p zooming out. ---
    gpu.renderer.set_lighting(render::galactic_lighting(galactic_light_settings(gpu.data && gpu.data->files ? gpu.data->files.get() : nullptr),
                                                        camera_.zoom_blend()));
    // --- end OI render ---
    gpu.renderer.set_object_fade(-1.0f, -1.0f);  // SPACE_AND_OPEN.md B.8: no distance fade on the galactic map
#ifdef ALAMO_PARTICLES_HAVE_SIM
    gpu.renderer.set_scene_hook(prender_.initialized() ? &prender_ : nullptr);  // corruption smoke
#else
    gpu.renderer.set_scene_hook(nullptr);
#endif
}

void GalacticView::destroy() {
#ifdef ALAMO_PARTICLES_HAVE_SIM
    if (gpu_) {
        gpu_->ring.wait_idle();
        gpu_->ctx->wait_idle();
        gpu_->renderer.set_scene_hook(nullptr);
    }
    if (pattach_) pattach_->clear();
    pattach_.reset();
    if (prender_.initialized()) prender_.shutdown();
    plib_.reset();
    pworld_.reset();
#endif
    if (models_) models_->destroy();
    models_.reset();
    if (portraits_) portraits_->destroy();  // before its models
    portraits_.reset();
    if (portrait_models_) portrait_models_->destroy();
    portrait_models_.reset();
    builder_.reset();
    buffer_.clear();
}

void GalacticView::record_portraits(rhi::Frame& frame, VkCommandBuffer cmd) {
    if (!portraits_ || !portrait_models_) return;
    portrait_models_->update();
    portraits_->record(frame, cmd);
}

ui::CameraParams GalacticView::camera_params(uint32_t w, uint32_t h) const {
    ui::CameraParams p = camera_.params(static_cast<float>(w) / static_cast<float>(std::max(1u, h)));
    p.zfar = std::max(p.zfar, p.distance * 2.5f);
    return p;
}

float GalacticView::planet_radius(objects::TypeIndex t) const { return builder_ ? builder_->planet_radius(t) : render::kPlanetSphereRadius; }

Vec3 GalacticView::fleet_position(const render::GalacticFleetSnap& f) const {
    return builder_ ? builder_->fleet_draw_position(f, frame_.planets) : f.position;
}

std::vector<ui::PickSphere> GalacticView::pick_items() const {
    std::vector<ui::PickSphere> out;
    for (const render::GalacticPlanetSnap& p : frame_.planets) {
        if (!(p.flags & render::kGalPlanet) || (p.flags & render::kGalHidden) || p.id == objects::kNoObjectId) continue;
        ui::PickSphere s;
        s.id = static_cast<uint32_t>(p.id);
        s.center = {p.position.x, p.position.y, p.position.z};
        s.radius = planet_radius(p.type);
        s.owner = p.owner;
        s.kind = ui::PickKind::Planet;
        out.push_back(s);
    }
    // GALAXY_MAP_PRESENTATION §2: only fleets in transit are drawn (and picked); orbiting fleets are the HUD's planet
    // fleet buttons.
    for (const render::GalacticFleetSnap& f : frame_.fleets) {
        if (!render::GalacticSceneBuilder::fleet_in_transit(f, frame_.planets.size())) continue;
        const Vec3 c = fleet_position(f);
        ui::PickSphere s;
        s.id = static_cast<uint32_t>(f.id);
        s.center = {c.x, c.y, c.z};
        s.radius = 2.5f;  // [port] fleet pick radius (model ~3 units long)
        s.owner = f.owner;
        s.kind = ui::PickKind::Fleet;
        out.push_back(s);
    }
    return out;
}

uint32_t GalacticView::fleet_planet_id(uint32_t fleet_id) const {
    for (const render::GalacticFleetSnap& f : frame_.fleets)
        if (static_cast<uint32_t>(f.id) == fleet_id) {
            if (f.planet == kInvalidPlanet || f.planet >= frame_.planets.size()) return 0;
            const objects::ObjectId p = frame_.planets[f.planet].id;
            return p == objects::kNoObjectId ? 0 : static_cast<uint32_t>(p);
        }
    return 0;
}

bool GalacticView::settled() const { return models_ && models_->all_finished(); }

// ------------------------------------------------------------------------------------------------ battle
bool BattleView::start(GpuCore& gpu, const GameData& data, const GameSession& session, uint32_t width, uint32_t height, std::string* err) {
    gpu_ = &gpu;
    local_ = session.local_player();
    const sim::TacticalBattle* b = session.battle();
    const sim::TacticalMode* tm = session.host().modes().top_tactical();
    if (!b || !tm) {
        if (err) *err = "no battle running";
        return false;
    }
    space_ = tm->kind() == objects::ModeKind::Space;
    table_.build(data.types, space_ ? objects::ModeKind::Space : objects::ModeKind::Land, gpu.resolver.get());
    builder_ = std::make_unique<render::SimInstanceBuilder>(table_, gpu.resolver.get());
    const std::string map = session.spec().kind == SessionKind::Skirmish ? session.spec().skirmish_map : b->request().map;
    scene_ = std::make_unique<render::MapScene>();
    scene_->set_objects_enabled(false);  // the battle's objects replace the placements
    scene_->set_runtime_decals_enabled(!space_);
    std::string e;
    if (!scene_->load(*data.files, map, gpu.resolver.get(), jobs::make_parallel_for(*gpu.js), &e) ||
        !scene_->start(gpu.renderer, *gpu.streamer, *gpu.cache, {}, &e)) {
        if (err) *err = "battle map " + map + ": " + e;
        scene_.reset();
        return false;
    }
    ropt = render::RecordOptions{};
    ropt.threads = gpu.js->worker_count();
    scene_->apply(ropt);
    models_ = std::make_unique<render::StreamedSimModels>(table_, *gpu.streamer);
#ifdef ALAMO_RENDER_HAVE_ANIM_DRIVER
    pose_fn_ = [this](objects::ObjectId id, const render::GpuModel& m) {
        if (!m.asset) return std::span<const render::Mat3x4>{};
        return poses_.pose(id, m.asset->skeleton.size());
    };
    builder_->set_pose_source(pose_fn_);
    builder_->set_visibility_source([this](objects::ObjectId id, const render::GpuModel& m) {
        if (!m.asset) return std::span<const uint8_t>{};
        return poses_.visibility(id, m.asset->skeleton.size());
    });
#endif
#ifdef ALAMO_PARTICLES_HAVE_SIM
    pworld_ = std::make_unique<render::particles::ParticleWorld>(1u << 21);
    plib_ = std::make_unique<render::particles::ParticleLibrary>(data.files.get());
    render::particles::ParticleTextureSource tex = [&gpu](std::string_view n) { return gpu.cache->texture(n, assets::TextureRole::Color); };
    if (!prender_.init(*gpu.ctx, gpu.alloc, gpu.up, *pworld_, std::move(tex))) {
        if (err) *err = "particle renderer init failed";
        return false;
    }
    prender_.set_sort_range(render::MapScene::kFar);
    pattach_ = std::make_unique<render::particles::ProxyAttachments>(*pworld_, *plib_);
#endif
    // R5: fog colours (GameConstants), land decorations (tracks, scorch marks).
    {
        std::vector<uint8_t> gc;
        render::FogColors fc;
        if (data.files->read("Data\\XML\\GameConstants.xml", gc)) render::parse_fog_colors(gc, fc);
        fog_.set_colors(fc);
    }
    // --- R3/R5 render: decorations on every map (space: no terrain - only spin-away explosions) ---
    surface_fx_.load(*data.files);
    decorations_ = std::make_unique<render::SimDecorations>(surface_fx_, table_, space_ ? nullptr : &scene_->map().terrain);
    decorations_->set_bone_source(render::model_bone_source(table_, *models_, pose_fn_ ? &pose_fn_ : nullptr));
    // --- end R3/R5 render ---
    // --- R5 render: ability beams (ABILITY_VISUALS 2 / 4): GameConstants fallbacks, LightningEffectTypes, per-special styles ---
    {
        render::BeamConstants beam_gc;
        std::vector<uint8_t> xml;
        if (data.files->read("Data\\XML\\GameConstants.xml", xml)) render::parse_beam_constants(xml, beam_gc);
        beam_styles_.build(data.types, beam_gc);
        render::LightningLibrary lightning = render::LightningLibrary::builtin();
        xml.clear();
        if (data.files->read("Data\\XML\\LightningEffectTypes.xml", xml)) lightning.load(xml);
        const render::TextureSource beam_tex = [&gpu](std::string_view n, assets::TextureRole role) { return gpu.cache->texture(n, role); };
        beams_on_ = gpu.renderer.init_beams(beam_tex, &lightning);
        beam_bone_ = render::model_bone_source(table_, *models_, pose_fn_ ? &pose_fn_ : nullptr);
    }
    // --- end R5 render ---
    // Laser projectiles (Projectile_Custom_Render 1 / 2, PROJECTILE_NEBULA_DRAW.md 1): procedural beams / kites.
    {
        std::vector<uint8_t> xml;
        if (data.files->read("Data\\XML\\GameConstants.xml", xml)) render::parse_laser_constants(xml, laser_k_);
        if (!gpu.renderer.init_lasers([&gpu](std::string_view n, assets::TextureRole role) { return gpu.cache->texture(n, role); }))
            std::fprintf(stderr, "battle: laser projectiles not drawn (PrimAdditive / textures unavailable)\n");
    }
    // --- R3/R5 render: T6 scene models (hardpoint Model_To_Attach idle clips) ---
    scene_anim_ = std::make_unique<render::SceneModelAnimator>(data.files.get());
    builder_->set_attachment_pose_source(
        [this](const render::GpuModel& m, uint64_t tick) { return scene_anim_->pose(m, tick); });
    // --- end R3/R5 render ---
    // Camera: TacticalCameras.xml Land_Mode / Space_Mode, bounds = the mode's playable box, look at the local units.
    {
        std::vector<ui::TacticalCameraConstants> cams;
        const data::XmlDocument doc = read_xml(*data.files, "Data\\XML\\TacticalCameras.xml");
        if (doc.ok()) ui::parse_tactical_cameras(doc.root(), cams);
        const ui::TacticalCameraConstants* c = ui::find_camera(cams, space_ ? "Space_Mode" : "Land_Mode");
        const ui::UiConstants uc = ui_constants(*data.files);
        camera_ = ui::TacticalCameraController(c ? *c : (space_ ? ui::space_mode_defaults() : ui::land_mode_defaults()), uc.tactical);
        const tactical::Aabb& bb = tm->bounds();
        if (bb.max.x - bb.min.x < 1.0e5f) camera_.set_bounds({bb.min.x, bb.min.y, bb.max.x, bb.max.y});
        if (!space_) {  // land: the look-at follows the terrain (P1)
            const terrain::MapData* m = &scene_->map();
            camera_.set_terrain_height([m](float x, float y) { return m->terrain.sample_height(x, y); });
        }
        const objects::ObjectStore& st = tm->primary();
        double sx = 0, sy = 0;
        uint32_t n = 0;
        for (const objects::ObjectId id : b->movement().unit_ids()) {
            const objects::ObjectHandle h = st.find(id);
            if (!st.alive(h) || st.owner(h) != local_) continue;
            sx += st.position(h).x;
            sy += st.position(h).y;
            ++n;
        }
        if (n) camera_.set_look_at({static_cast<float>(sx / n), static_cast<float>(sy / n), 0.0f});
        else camera_.set_look_at({0.5f * (bb.min.x + bb.max.x), 0.5f * (bb.min.y + bb.max.y), 0.0f});
    }
    // Selection circle: one blob model from the selection material ([port] the first Selection* material of
    // ShadowBlobMaterials.xml / the built-in list; per-faction materials not distinguished yet). Land uses the same flat
    // instances at the unit's ground height: the terrain shadow-blob path drew the selection material's grid patches
    // with the decal shader (dark patch borders between neighbouring units).
    {
        std::vector<terrain::ShadowBlobMaterial> mats = terrain::builtin_blob_materials();
        std::vector<uint8_t> xml;
        if (data.files->read("Data\\XML\\ShadowBlobMaterials.xml", xml)) terrain::parse_shadow_blob_materials(xml, mats);
        jam_models_.set_materials(mats);  // R5 sensor jamming blobs use the same ShadowBlobMaterials table
        const terrain::ShadowBlobMaterial* m = mats.empty() ? nullptr : &mats.front();
        for (const terrain::ShadowBlobMaterial& x : mats)
            if (x.name.find("Selection") != std::string::npos || x.name.find("selection") != std::string::npos) {
                m = &x;
                break;
            }
        if (m) {
            const render::TextureSource blob_tex = [&gpu](std::string_view n, assets::TextureRole role) { return gpu.cache->texture(n, role); };
            std::string e2;
            blob_ok_ = gpu.renderer.upload_model(ui::selection_blob_model(*m), blob_tex, blob_model_, nullptr, &e2);
        }
    }
    (void)width;
    (void)height;
    buffer_.clear();
    captured_any_ = false;
    pending_ticks_ = 0;
    stats_ = {};
    return true;
}

bool BattleView::spawn_move_ack(std::string_view type, ui::Vec3 p, float scale, bool normal) {
#ifdef ALAMO_PARTICLES_HAVE_SIM
    if (!pworld_ || !plib_ || type.empty()) return false;
    const auto ieq = [](std::string_view a, std::string_view b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            const char x = a[i] >= 'a' && a[i] <= 'z' ? static_cast<char>(a[i] - 32) : a[i];
            const char y = b[i] >= 'a' && b[i] <= 'z' ? static_cast<char>(b[i] - 32) : b[i];
            if (x != y) return false;
        }
        return true;
    };
    std::string model(type);  // a particle file name when no type of that name exists
    for (size_t t = 0; t < table_.size(); ++t)
        if (ieq(table_[static_cast<objects::TypeIndex>(t)].name, type)) {
            model = table_[static_cast<objects::TypeIndex>(t)].model;
            break;
        }
    if (model.empty()) return false;
    render::particles::SystemPtr def = plib_->get(model);
    if (!def) return false;
    render::Mat3x4 m;
    for (int i = 0; i < 3; ++i) m.m[i][i] = scale;
    m.m[0][3] = p.x;
    m.m[1][3] = p.y;
    m.m[2][3] = p.z;
    const render::particles::ParticleWorld::Handle h = pworld_->create(std::move(def), m);
    if (h == 0) return false;
    std::erase_if(acks_, [this](const MoveAckFx& a) { return !pworld_->valid(a.handle); });
    acks_.push_back({h, p, normal});
    ++acks_spawned_;
    return true;
#else
    (void)type, (void)p, (void)scale, (void)normal;
    return false;
#endif
}

bool BattleView::kill_nearest_move_ack(ui::Vec3 p) {
#ifdef ALAMO_PARTICLES_HAVE_SIM
    if (!pworld_) return false;
    std::erase_if(acks_, [this](const MoveAckFx& a) { return !pworld_->valid(a.handle); });
    size_t best = acks_.size();
    float best_d = 0.0f;
    for (size_t i = 0; i < acks_.size(); ++i) {
        if (!acks_[i].normal) continue;
        const float dx = acks_[i].p.x - p.x, dy = acks_[i].p.y - p.y, dz = acks_[i].p.z - p.z;
        const float d = dx * dx + dy * dy + dz * dz;
        if (best == acks_.size() || d < best_d) best = i, best_d = d;
    }
    if (best == acks_.size()) return false;
    pworld_->set_visible(acks_[best].handle, false);  // silently: no lingering particles shown
    pworld_->release(acks_[best].handle);
    acks_.erase(acks_.begin() + static_cast<std::ptrdiff_t>(best));
    return true;
#else
    (void)p;
    return false;
#endif
}

void BattleView::stop(GpuCore& gpu) {
    acks_.clear();
    gpu.ring.wait_idle();
    gpu.ctx->wait_idle();
    gpu.renderer.set_scene_hook(nullptr);
#ifdef ALAMO_PARTICLES_HAVE_SIM
    if (pattach_) pattach_->clear();
    pattach_.reset();
    if (prender_.initialized()) prender_.shutdown();
    plib_.reset();
    pworld_.reset();
#endif
    decorations_.reset();
    scene_anim_.reset();  // R3/R5 render
    jam_models_.shutdown(gpu.renderer);
    gpu.renderer.release_beams();  // R5: no beams outside battles (GPU idle above)
    gpu.renderer.set_lasers({});   // no bolts outside battles
    beams_on_ = false;
    beam_bone_ = nullptr;
    beam_styles_ = render::BeamStyleTable{};
    if (blob_ok_) gpu.renderer.destroy_model(blob_model_);
    blob_ok_ = false;
    blobs_.clear();
    if (scene_) scene_->stop(gpu.renderer);
    gpu.renderer.set_world(nullptr);
    scene_.reset();
    models_.reset();
    builder_.reset();
    buffer_.clear();
    frame_ = render::InterpolatedFrame{};
    main.clear();
    sky.clear();
    bg_.clear();
}

void BattleView::capture(const GameSession& session) {
    const sim::SimHost& host = session.host();
    auto snap = std::make_shared<render::SimSnapshot>();
    const ParallelFor pf = jobs::make_parallel_for(*gpu_->js);
    if (!render::capture_snapshot(host, table_, *snap, pf)) return;
    if (const sim::TacticalBattle* b = session.battle()) {
        render::apply_battle_state(*b, *snap, &table_, &beam_styles_);  // R5: + the ability beam records
        render::apply_battle_projectiles(*b, *snap, table_);  // live projectiles (lasers, missiles) as plain data
        render::apply_battle_zaps(*b, *snap, buffer_.latest().curr.get(), &beam_styles_);  // R5: laser-defense zaps (the tick clears the sim event)
        render::apply_battle_jamming(*b, local_, *snap, &table_);  // R5: Is_Jammed ids, jammer blobs / owner particles
        // Int-final: garrisoned objects (the battle's garrison host) are not drawn (R3 kSnapContained).
        render::apply_contained(*snap, [b](const render::SnapshotObject& o) { return b->contained_by(o.id) != objects::kNoObjectId; });
#ifdef ALAMO_SIM_HAVE_FOG
        render::apply_battle_fog(*b, local_, *snap);  // R5: the local player's FOW grid + HideWhenFogged marks
#endif
        // --- OI render: space reinforcement overlay cells (TACTICAL_GAPS §5.5) while the overlay is shown; Lua
        // Attach_Particle_Effect requests of the story VMs (lua::PresentationLog::particle_effects, made since this
        // battle's first frame [port: the request frame is the VM's sim frame]) as attached effects ---
#ifdef ALAMO_SIM_HAVE_FOG
        if (space_ && fog_.space_reinforce()) render::apply_battle_fog_reinforce(*b, local_, *snap);
#endif
#ifdef ALAMO_SIM_HAVE_STORY
        if (const sim::StorySession* story = host.story())
            if (const sim::TacticalMode* tm = host.modes().top_tactical())
                // presentation() is a non-const getter; the log is only read here (between ticks).
                if (const lua::PresentationLog* log = const_cast<sim::StorySession*>(story)->presentation()) {
                    const uint64_t since = host.frame() >= tm->frame() ? host.frame() - tm->frame() : 0;
                    const std::vector<render::SnapshotEffect> lua_fx = render::snapshot_lua_effects(log->particle_effects(), table_, since);
                    render::append_snapshot_effects(*snap, lua_fx);
                }
#endif
        // --- end OI render ---
    }
#ifdef ALAMO_SIM_HAVE_ANIM
    // R3 corpses: detached ones (the battle removed the store object at death) exist only in SimAnimation.
    if (const sim::SimAnimation* a = session.animation())
        if (const sim::GameMode* top = host.modes().top()) {
            const std::vector<render::SnapshotCorpse> corpses = render::snapshot_corpses(a->corpses(*top), host.frame());
            render::add_snapshot_corpses(*snap, table_, corpses);
            stats_.corpses += static_cast<uint32_t>(corpses.size());
            // --- R3/R5 render: muzzle flashes (shown while frame < until) and SURFACE animation events (foot plants) ---
            render::set_snapshot_muzzles(*snap, render::snapshot_muzzles(a->muzzles(*top), host.frame()));
            stats_.muzzles += snap->muzzles.size();
            render::set_snapshot_foot_plants(*snap, render::snapshot_foot_plants(a->events(*top)));
            // --- end R3/R5 render ---
        }
#endif
    if (captured_any_ && snap->tick > last_tick_) pending_ticks_ += static_cast<uint32_t>(snap->tick - last_tick_);
    captured_any_ = true;
    last_tick_ = snap->tick;
#ifdef ALAMO_RENDER_HAVE_ANIM_DRIVER
    const anim::AnimWorld* world = nullptr;
#ifdef ALAMO_SIM_HAVE_ANIM
    if (const sim::SimAnimation* a = session.animation())
        if (const sim::GameMode* top = host.modes().top()) world = a->find_world(*top);
#endif
    poses_.update(world, pf);
#endif
    stats_.objects = static_cast<uint32_t>(snap->objects.size());
    stats_.effects = static_cast<uint32_t>(snap->effects.size());
    stats_.fog = snap->fog != nullptr;
    // --- OI render: this tick's ability events = state changes against the previous snapshot (the sim clears its own) ---
    render::set_snapshot_ability_events(*snap, buffer_.latest().curr.get());
    // --- end OI render ---
    buffer_.publish(std::move(snap));
}

void BattleView::prepare(GpuCore& gpu, float alpha) {
    gpu.streamer->update();
    main.clear();
    sky.clear();
    bg_.clear();
    const render::SnapshotBuffer::Pair p = buffer_.latest();
    if (p.curr && scene_) {
        render::interpolate(p.prev.get(), *p.curr, alpha, render::InterpMode::Smooth, frame_);  // alpha 1 = the current snapshot (Original)
        models_->request(frame_);
        builder_->build(frame_, *models_, main, bg_, &istats_);
        stats_.drawn = istats_.drawn;
        stats_.fog_hidden = istats_.fog_hidden;
        // --- R3/R5 render: decorations first (their footprint puffs / wakes join the particle pass) ---
        if (decorations_) {
            decorations_->update(frame_);
            decorations_->apply(scene_->world());
            stats_.decals = decorations_->stats().scorch_spawned;
        }
        // --- end R3/R5 render ---
        // R5 ability beams: the frame's tractor / energy / super laser / zap records with world endpoints (render pass: transparent bucket)
        if (beams_on_) gpu.renderer.set_beams(render::beams_from_frame(frame_, &beam_styles_, beam_bone_), p.curr->tick);
        gpu.renderer.set_lasers(builder_->lasers(), laser_k_);  // the frame's laser projectiles (Transparent bucket, main scene)
        stats_.lasers_peak = std::max(stats_.lasers_peak, static_cast<uint32_t>(builder_->lasers().size()));
#ifdef ALAMO_PARTICLES_HAVE_SIM
        if (pattach_) {
            render::particles::sync_sim_frame(*pattach_, frame_, table_, *models_, *builder_, pose_fn_ ? &pose_fn_ : nullptr, &popt_,
                                              decorations_ ? decorations_->effects() : std::span<const render::SimWorldEffect>());
            pworld_->update(render::particles::ParticleWorld::tick_delta(pending_ticks_));
            pending_ticks_ = 0;
            stats_.particles_spawned = pworld_->stats().spawned;
        }
#endif
    }
    // Selection circles (P2): flat blob instances (land: at the unit's ground height).
    if (scene_ && blob_ok_) {
        gpu.renderer.make_resident(blob_model_);
        for (ui::SelectionBlob b : blobs_) {
            if (!space_) b.center.z = scene_->map().terrain.sample_height(b.center.x, b.center.y);
            main.push_back(ui::selection_blob_instance(blob_model_, b));
        }
    }
    // R5 sensor jamming (ABILITY_VISUALS 3): radius blobs of the allied jammers (jam_fx.hpp), flat at the jammer's height
    // (land: the ground under it). The Owner_Particle_Effect jammers use the particle pass above (SnapshotEffect).
    if (scene_ && p.curr && !frame_.jamming.jammers.empty()) {
        const render::TextureSource jam_tex = [&gpu](std::string_view n, assets::TextureRole role) { return gpu.cache->texture(n, role); };
        std::function<float(float, float)> ground;
        if (!space_) {
            const terrain::MapData* map = &scene_->map();
            ground = [map](float x, float y) { return map->terrain.sample_height(x, y); };
        }
        jam_models_.build(gpu.renderer, jam_tex, frame_, ground, main);
    }
    if (scene_) {
        scene_->update(gpu.renderer, *gpu.streamer);
        if (p.curr) scene_->set_sim_tick(p.curr->tick);
        sky.assign(scene_->sky().begin(), scene_->sky().end());
    }
    sky.insert(sky.end(), bg_.begin(), bg_.end());
}

// R5 fog of war: the latest snapshot's copy of the local player's grid (an empty input disables the world's fog).
// Before FrameRing::begin_frame / Uploader::record_acquires: WorldRenderer::set_fog_of_war supersedes an upload still in
// flight by destroying its image, which must not happen after this frame's acquire barriers were recorded.
void BattleView::pre_frame() {
    if (!scene_) return;
    const render::SnapshotBuffer::Pair p = buffer_.latest();
    if (!p.curr || p.curr->tick == fog_tick_) return;  // once per new snapshot
    fog_tick_ = p.curr->tick;
    if (p.curr->fog) fog_.update(scene_->world(), *p.curr->fog, space_);
    else if (fog_.active()) fog_.update(scene_->world(), render::FogFrameInput{}, space_);
    stats_.fog_drawn = fog_.active();
}

// --- R3/R5 render ---
void BattleView::set_space_reinforcement_overlay(bool on) {
    if (fog_.space_reinforce() == on) return;
    fog_.set_space_reinforce(on);
    fog_tick_ = ~uint64_t{0};  // re-convert (palette 3) at the next pre_frame
}
// --- end R3/R5 render ---

float BattleView::type_radius(objects::TypeIndex t) {
    if (!models_ || t >= table_.size()) return 0.0f;
    const render::GpuModel* m = models_->type_model(t);
    if (!m) return 0.0f;
    return m->drawn_sphere().radius * table_[t].scale;
}

ui::Vec3 BattleView::type_half_extents(objects::TypeIndex t) {
    if (!models_ || t >= table_.size()) return {};
    const render::GpuModel* m = models_->type_model(t);
    if (!m) return {};
    const assets::Aabb b = m->drawn_bounds();
    if (!b.valid()) return {};
    const float s = 0.5f * table_[t].scale;
    return {(b.max.x - b.min.x) * s, (b.max.y - b.min.y) * s, (b.max.z - b.min.z) * s};
}

void BattleView::use_scene(GpuCore& gpu) {
    if (!scene_) return;
    gpu.renderer.set_lighting(scene_->lighting());
    gpu.renderer.set_world(&scene_->world());
#ifdef ALAMO_PARTICLES_HAVE_SIM
    gpu.renderer.set_scene_hook(prender_.initialized() ? &prender_ : nullptr);
#endif
}

ui::CameraParams BattleView::camera_params(uint32_t w, uint32_t h) const {
    return camera_.params(static_cast<float>(w) / static_cast<float>(std::max(1u, h)));
}

float BattleView::clear_color(int i) const {
    if (!scene_) return i == 3 ? 1.0f : 0.0f;
    const render::Float3 c = scene_->clear_color();
    return i == 0 ? c.x : i == 1 ? c.y : i == 2 ? c.z : 1.0f;
}

float BattleView::load_progress() const {
    if (!scene_) return 0.0f;
    float f = scene_->ready() ? 0.5f : 0.1f;
    if (models_ && models_->requested() > 0)
        f += 0.5f * static_cast<float>(models_->ready() + models_->failed()) / static_cast<float>(models_->requested());
    else if (scene_->ready())
        f += 0.5f;
    return std::min(f, 1.0f);
}

bool BattleView::settled(const render::DrawStats& ds) const {
    if (!scene_) return false;
    const auto& ws = scene_->world().stats();
    bool fx = true;
#ifdef ALAMO_PARTICLES_HAVE_SIM
    fx = !prender_.initialized() || prender_.last_stats().textures_pending == 0;
#endif
    return scene_->ready() && ws.terrain_pending == 0 && ws.water_pending == 0 && ws.decorations.pending == 0 && (!models_ || models_->all_finished()) &&
           fx && ds.pending_pipeline == 0 && ds.translation_pending == 0 && ds.post_pending == 0;
}

}  // namespace alamo::game
