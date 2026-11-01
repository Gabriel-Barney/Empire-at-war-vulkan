// alamo_game render side (P host): the GPU objects shared by every scene, the galactic map view (R2) and the battle
// view (R1 + R3 + R4 + R5 over a running S10 battle). Both views only read the sim through immutable snapshots taken
// between ticks (capture*), so the per-tick CRC is the same with presentation on or off.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "alamo/assets/asset_cache.hpp"
#include "alamo/game/game_data.hpp"
#include "alamo/game/game_session.hpp"
#include "alamo/jobs/job_system.hpp"
#include "alamo/render/fleet.hpp"
#include "alamo/render/beams.hpp"       // R5 ability beams
#include "alamo/render/fog_view.hpp"
#include "alamo/render/galactic/galactic_snapshot.hpp"
#include "alamo/render/galactic/galactic_view.hpp"
#include "alamo/render/galactic/planet_portrait.hpp"
#include "alamo/render/jam_fx.hpp"  // R5 sensor jamming blobs
#include "alamo/render/map_scene.hpp"
#include "alamo/render/renderer.hpp"
#include "alamo/render/scene_anim.hpp"  // R3/R5 render: T6 scene models
#include "alamo/render/sim_beams.hpp"
#include "alamo/render/sim_decorations.hpp"
#include "alamo/render/sim_snapshot.hpp"
#include "alamo/render/sim_view.hpp"
#include "alamo/render/surface_fx.hpp"
#include "alamo/rhi/rhi.hpp"
#include "alamo/ui/camera.hpp"
#include "alamo/ui/hud_renderer.hpp"
#include "alamo/ui/picking.hpp"
#include "alamo/ui/selection_blob.hpp"
#ifdef ALAMO_RENDER_HAVE_ANIM_DRIVER
#include "alamo/render/anim/anim_pose_cache.hpp"
#endif
#ifdef ALAMO_PARTICLES_HAVE_SIM
#include "alamo/render/particles/particle_attach.hpp"
#include "alamo/render/particles/particle_data.hpp"
#include "alamo/render/particles/particle_gpu.hpp"
#include "alamo/render/particles/particle_sim.hpp"
#include "alamo/render/particles/particle_sim_attach.hpp"
#endif

namespace alamo::game {

inline constexpr VkFormat kColorFormat = VK_FORMAT_R8G8B8A8_UNORM;
inline constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;

// Per frame-in-flight colour (RGBA8, world + HUD) and depth (HUD 3D shells) images; one readback buffer for PNGs.
struct FrameTargets {
    struct Slot {
        rhi::Image color, depth;
    };
    std::vector<Slot> slots;
    rhi::Buffer readback;
    uint32_t width = 0, height = 0;
    bool create(rhi::Allocator& a, uint32_t slots, uint32_t w, uint32_t h);
    void destroy(rhi::Allocator& a);
};

// Renderer + HUD pass + asset streaming, shared by the galactic and battle views.
class GpuCore {
public:
    bool init(rhi::Context& ctx, const GameData& data, jobs::JobSystem& js, const std::string& cache_dir, std::string* err);
    void shutdown();
    rhi::Context* ctx = nullptr;
    rhi::Allocator alloc;
    rhi::Uploader up;
    rhi::FrameRing ring;
    render::Renderer renderer;
    ui::HudRenderer hud;
    std::unique_ptr<render::EffectCache> effects;
    std::unique_ptr<assets::AssetCache> cache;
    std::unique_ptr<render::ModelStreamer> streamer;
    std::unique_ptr<render::GameTypeResolver> resolver;
    jobs::JobSystem* js = nullptr;
    const GameData* data = nullptr;
};

// ---- galactic map -------------------------------------------------------------------------------------------
class GalacticView {
public:
    void init(GpuCore& gpu, const GameData& data, std::vector<render::FactionColor> colors, render::RouteLineStyle style);
    void attach(const galactic::GalacticSim& sim, int32_t local_player);  // palette, builder, camera start
    void capture(const sim::SimHost& host);                               // after every tick (const reads)
    // frame thread (now_s: wall clock, particles). GALAXY_MAP_PRESENTATION §3: `selected_planet` (planet object id, 0 =
    // none) and the camera (`cam`, null = the builder's default) drive the selection model (GalacticSceneBuilder::set_selection).
    void prepare(float alpha, double now_s = -1.0, uint32_t selected_planet = 0, const ui::CameraParams* cam = nullptr);
    void use_scene(GpuCore& gpu);                                         // lighting / fade / hooks for galactic frames
    void destroy();
    bool attached() const { return builder_ != nullptr; }
    // Particle detail option (0..1, ANIM_PARTICLES B.4): active slots of every emitter (presentation only).
    void set_particle_detail(float detail) {
        if (pworld_) pworld_->set_detail(detail);
    }

    ui::GalacticCameraController& camera() { return camera_; }
    // The camera's planet box (ui::galactic_scroll_box; the camera widens it by 15 %).
    void set_camera_bounds(const ui::CameraBox& b) { camera_.set_bounds(b); }
    ui::CameraParams camera_params(uint32_t w, uint32_t h) const;
    const render::GalacticFrame& frame() const { return frame_; }
    const render::GalacticTypeTable& types() const { return types_; }
    const render::GalacticPalette& palette() const { return palette_; }
    const render::GalacticSceneBuilder* builder() const { return builder_.get(); }
    // Picking: planets (planet radius) and fleets (at their draw position) of the current frame.
    std::vector<ui::PickSphere> pick_items() const;
    Vec3 fleet_position(const render::GalacticFleetSnap& f) const;
    // Object id of the planet the fleet `fleet_id` orbits this frame (0: in transit / unknown).
    uint32_t fleet_planet_id(uint32_t fleet_id) const;
    float planet_radius(objects::TypeIndex t) const;
    bool settled() const;
    // Planet portraits ("planet_portrait:<type>" image pages: Planet Info IDC_PLANET_ICON, the battle load screen's
    // planet icon). Own GalacticModels so they also render while the galaxy is not drawn (battle load). Frame thread,
    // after Uploader::record_acquires and outside any render pass.
    void record_portraits(rhi::Frame& frame, VkCommandBuffer cmd);
    render::PlanetPortraits* portraits() { return portraits_.get(); }

    std::vector<render::Instance> main, background;
    render::RecordOptions ropt;

private:
    render::GalacticTypeTable types_;
    render::GalacticPalette palette_;
    render::GalacticSnapshotBuffer buffer_;
    std::unique_ptr<render::GalacticModels> models_;
    std::unique_ptr<render::GalacticSceneBuilder> builder_;
    std::unique_ptr<render::GalacticModels> portrait_models_;
    std::unique_ptr<render::PlanetPortraits> portraits_;
    render::GalacticFrame frame_;
    std::vector<render::FactionColor> colors_;
    render::RouteLineStyle style_;
    render::CorruptionPathSettings corruption_path_;  // FIXPASS_GALACTIC §3.2 (GameConstants)
    std::string evil_side_name_ = "EMPIRE";           // GameConstants Evil_Side_Name (route sides, §3.1)
    ui::GalacticCameraController camera_;
    GpuCore* gpu_ = nullptr;
    int32_t local_ = -1;  // fog viewpoint (attach)
    // Underworld corruption smoke (render agent host ask 3): the galactic map's own particle world; the effects of
    // galactic_corruption_effects are synced by key every frame (each Particle type's Galactic_Model_Name).
    render::GalacticCorruptionSettings corruption_;
    std::vector<render::GalacticCorruptionEffect> corruption_fx_;
    double last_frame_s_ = -1.0;
#ifdef ALAMO_PARTICLES_HAVE_SIM
    std::unique_ptr<render::particles::ParticleWorld> pworld_;
    std::unique_ptr<render::particles::ParticleLibrary> plib_;
    render::particles::ParticleRenderer prender_;
    std::unique_ptr<render::particles::ProxyAttachments> pattach_;
#endif
};

// ---- battle -------------------------------------------------------------------------------------------------
struct BattleViewStats {
    // corpses: detached corpses + drawable death-clone objects, summed over captures.
    uint32_t objects = 0, drawn = 0, fog_hidden = 0, effects = 0, corpses = 0;
    uint64_t particles_spawned = 0;
    bool fog = false, fog_drawn = false;
    uint64_t decals = 0;
    uint64_t muzzles = 0;  // muzzle flashes handed to the snapshots (sum over captures; R3 attack row)
    uint32_t lasers_peak = 0;  // most laser projectiles handed to the renderer in one frame (playtest R1)
};

class BattleView {
public:
    // Loads the battle's map scene (terrain / water / sky; placements off - the battle's objects replace them).
    bool start(GpuCore& gpu, const GameData& data, const GameSession& session, uint32_t width, uint32_t height, std::string* err);
    void stop(GpuCore& gpu);
    bool active() const { return scene_ != nullptr; }
    // Particle detail option (0..1, ANIM_PARTICLES B.4): active slots of every emitter (presentation only).
    void set_particle_detail(float detail) {
        if (pworld_) pworld_->set_detail(detail);
    }
    // Scripted runs: every model the frame requests is loaded and resident before it is built (ModelStreamer::
    // finish_pending), so a new type's first drawn frame and its particles do not depend on load timing; likewise each
    // snapshot's fog texture (WorldRenderer::finish_fow_upload).
    void set_blocking_models(bool on) { blocking_models_ = on; }
    void capture(const GameSession& session);  // after every battle tick (const reads)
    void pre_frame();  // before begin_frame (fog uploads)
    void prepare(GpuCore& gpu, float alpha);
    void use_scene(GpuCore& gpu);
    // World bounding radius of a type (drawn bounding sphere of its model x Scale_Factor); 0 until the model is loaded.
    float type_radius(objects::TypeIndex t);
    float load_progress() const;
    bool settled(const render::DrawStats& ds) const;

    ui::TacticalCameraController& camera() { return camera_; }
    ui::CameraParams camera_params(uint32_t w, uint32_t h) const;
    const render::InterpolatedFrame& frame() const { return frame_; }
    const render::SnapshotTypeTable& table() const { return table_; }
    const render::MapScene* scene() const { return scene_.get(); }
    bool space() const { return space_; }
    const BattleViewStats& stats() const { return stats_; }
    float clear_color(int i) const;
    // P2 selection circles of this frame (TacticalHud::selection_blobs): flat blob instances in the main list (land: at
    // the terrain height under the unit). Set before prepare().
    void set_selection_blobs(std::span<const ui::SelectionBlob> blobs) { blobs_.assign(blobs.begin(), blobs.end()); }
    const render::FogRenderer& fog() const { return fog_; }
    // --- R3/R5 render ---
    // Space reinforcement overlay (palette 3 + W_Space_Reinforce_FOW_Grid): on while the local player picks a space
    // reinforcement drop point (game_main: TacticalHud BeginReinforcement until the drop / cancel).
    void set_space_reinforcement_overlay(bool on);
    // --- end R3/R5 render ---
    // P1 move acknowledgement (P1_DOUBLE_CLICK_MOVE.md §2.3; presentation only): spawns the particle object type `type`
    // (a GameConstants GUI_*_Command_Ack_Effect value: the type's model for this mode, else `type` as a particle file) at
    // `p` scaled by `scale` (GUI_Move_Acknowledge_Scale_Land / _Space). `normal`: a GUI_Move_Command_Ack_Effect object (the
    // kind a double-click kills). False when nothing was spawned (unknown type, no particle renderer).
    bool spawn_move_ack(std::string_view type, ui::Vec3 p, float scale, bool normal);
    // The nearest live normal move ack to `p` (no radius) is removed silently (hidden, no linger). False when none.
    bool kill_nearest_move_ack(ui::Vec3 p);

    std::vector<render::Instance> main, sky;
    render::RecordOptions ropt;

private:
    GpuCore* gpu_ = nullptr;
    bool space_ = true;
    bool blocking_models_ = false;
    int32_t local_ = 0;
    render::SnapshotTypeTable table_;
    render::SnapshotBuffer buffer_;
    std::unique_ptr<render::MapScene> scene_;
    std::unique_ptr<render::StreamedSimModels> models_;
    std::unique_ptr<render::SimInstanceBuilder> builder_;
    render::InterpolatedFrame frame_;
    std::vector<render::Instance> bg_;
    render::SimInstanceStats istats_;
    ui::TacticalCameraController camera_;
    render::FogRenderer fog_;
    render::SurfaceFxLibrary surface_fx_;
    std::unique_ptr<render::SimDecorations> decorations_;
    std::unique_ptr<render::SceneModelAnimator> scene_anim_;  // R3/R5 render: Model_To_Attach idle clips (T6)
    render::SimInstanceBuilder::PoseSource pose_fn_;
#ifdef ALAMO_RENDER_HAVE_ANIM_DRIVER
    render::AnimPoseCache poses_;
#endif
#ifdef ALAMO_PARTICLES_HAVE_SIM
    std::unique_ptr<render::particles::ParticleWorld> pworld_;
    std::unique_ptr<render::particles::ParticleLibrary> plib_;
    render::particles::ParticleRenderer prender_;
    std::unique_ptr<render::particles::ProxyAttachments> pattach_;
    render::particles::SimAttachOptions popt_;
#endif
    struct MoveAckFx {
        uint64_t handle = 0;  // ParticleWorld::Handle
        ui::Vec3 p{};
        bool normal = false;
    };
    std::vector<MoveAckFx> acks_;
    std::vector<ui::SelectionBlob> blobs_;
    render::GpuModel blob_model_;
    bool blob_ok_ = false;
    render::JamBlobModels jam_models_;  // R5 sensor jamming radius blobs (one model per material + colour)
    render::BeamStyleTable beam_styles_;  // R5 ability beams: per-special styles over the GameConstants fallbacks (ABILITY_VISUALS 4.2)
    render::BeamBoneFn beam_bone_;        // bone world positions (HP_trac_bone, hardpoint / muzzle bones)
    bool beams_on_ = false;               // the renderer's beam pass is initialised for this battle
    render::LaserConstants laser_k_;      // GameConstants Laser_Beam / Kite_Z_Scale_Factor
    uint64_t last_tick_ = 0, fog_tick_ = ~uint64_t{0};
    uint32_t pending_ticks_ = 0;
    bool captured_any_ = false;
    BattleViewStats stats_;
};

}  // namespace alamo::game
