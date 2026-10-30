// alamo_render - a land map as a render scene: .ted through the VFS (engine/terrain), terrain mesh and
// water built on the job system, object placements resolved through the game database and streamed
// through ModelStreamer, the start environment as lighting, the tactical start camera. Used by
// alamo_render --map and the tests.
#pragma once
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "alamo/assets/skeleton.hpp"
#include "alamo/core/parallel.hpp"
#include "alamo/render/camera.hpp"
#include "alamo/render/hardpoints.hpp"
#include "alamo/render/lighting.hpp"
#include "alamo/render/renderer.hpp"
#include "alamo/render/world.hpp"
#include "alamo/terrain/mesh.hpp"
#include "alamo/terrain/placement.hpp"
#include "alamo/terrain/ted.hpp"
#include "alamo/terrain/water.hpp"

namespace alamo::data {
class GameDatabase;
struct GameObjectRecord;
}  // namespace alamo::data

namespace alamo::render {

// terrain::TypeResolver over the game database (the reference resolver of engine/terrain's tests):
// first definition by CRC; model = Land_Model_Name for land maps, Space_Model_Name for space maps,
// falling back to the other and to Galactic_Model_Name, following Variant_Of_Existing_Type when the
// variant has no model of its own; Scale_Factor likewise.
// Render-relevant fields of a GameObjectType (variant chain followed like the model).
struct TypeRenderInfo {
    float lod_bias = kDefaultLodBias;             // <LOD_Bias> (SPACE_AND_OPEN.md B.4)
    bool hardpoints_known = false;                // the hardpoint XML was loaded (else: name-rule fallback)
    std::vector<std::string> hardpoints;          // <HardPoints> names in order
    std::vector<std::string> hardpoint_decal;     // per hardpoints entry: its <Damage_Decal> ("" if none / unknown)
    std::vector<std::string> damage_decals;       // the non-empty ones
    std::vector<HardpointDef> hardpoint_defs;     // per hardpoints entry: its <HardPoint> record (known = false if absent)
    // Index into hardpoints of `name` (case-insensitive), -1 if the type has no such hardpoint.
    int32_t hardpoint_index(std::string_view name) const;
    bool has_no_colorization = false;             // the type's own <No_Colorization_Color>
    materials::float4 no_colorization{1, 1, 1, 1};
    bool in_background = false;                   // <In_Background> (space: background scene, SPACE_AND_OPEN.md A.3)
    bool loop_idle = false;                       // <Loop_Idle_Anim_00>
    float idle_rate = 1.0f;                       // <Idle_Anim_00_Rate_Mod> (0 = frozen at frame 0)
};

// The 9 MP colours (SPACE_AND_OPEN.md B.3, GameConstants MP_Color_Blue, Red, Green, Orange, Cyan,
// Purple, Yellow, Gray, Eight; byte RGB / 255, alpha 1).
inline constexpr uint32_t kMpPaletteSize = 9;
struct MpPalette {
    materials::float4 color[kMpPaletteSize];
};
MpPalette default_mp_palette();  // the vanilla values

class GameTypeResolver {
public:
    explicit GameTypeResolver(const data::GameDatabase& db);
    bool resolve(uint32_t type_crc, bool space_map, terrain::ResolvedType& out) const;
    terrain::TypeResolver function() const {
        return [this](uint32_t crc, bool space, terrain::ResolvedType& out) { return resolve(crc, space, out); };
    }
    // Model of a type by name (environment skydome objects), "" if unknown.
    std::string model_of(std::string_view type_name, bool space_map) const;

    // Reads Data\XML\HardPointDataFiles.xml and its files (HardPoint Name -> Damage_Decal) and
    // FactionFiles.xml (the neutral faction's No_Colorization_Color). False when the hardpoint list is
    // missing (render_info then reports hardpoints_known = false). Read-only over engine/data's XML.
    bool load_render_xml(const FileProvider& files);
    bool hardpoints_loaded() const { return hardpoints_loaded_; }
    TypeRenderInfo render_info(uint32_t type_crc) const;
    // <HardPoint> record by name (case-insensitive; first definition wins), null if unknown.
    const HardpointDef* hardpoint(std::string_view name) const;
    // GameConstants MP_Color_* (doc values for absent tags).
    MpPalette palette() const;
    // Factions in FactionFiles.xml order. A map placement's owner (0x4B0 mini 02) is read as an index into
    // this list [inferred from the data: vanilla land maps own ~99% of their objects - props, trees - by 3 =
    // Neutral and a few by 4 = Hostile, in the order Rebel, Empire, Pirates, Neutral, Hostile, ...].
    struct Faction {
        std::string name;
        bool neutral = false;                       // <Is_Neutral>
        materials::float4 no_colorization{1, 1, 1, 1};  // <No_Colorization_Color>
    };
    const std::vector<Faction>& factions() const { return factions_; }
    bool owner_neutral(uint32_t owner) const { return owner < factions_.size() && factions_[owner].neutral; }
    // SPACE_AND_OPEN.md B.3 colour choice: owner faction not neutral -> the owner's player colour (here the MP
    // palette entry of the owner index; no lobby); neutral -> the type's No_Colorization_Color when set, else
    // the faction's.
    materials::float4 owner_color(uint32_t owner, const TypeRenderInfo& t, const MpPalette& palette) const;

private:
    const data::GameDatabase& db_;
    std::unordered_map<uint32_t, const data::GameObjectRecord*> by_crc_;
    std::unordered_map<std::string, HardpointDef> hardpoint_defs_;  // upper-cased hardpoint name -> record (lookup only)
    bool hardpoints_loaded_ = false;
    std::vector<Faction> factions_;
};

// Player colour (Colorization = (RGB, 1)) of an owner index: MP palette index = owner % 9 (no lobby, so
// index k shows colour k) [inferred].
materials::float4 owner_team_color(uint32_t owner, const MpPalette& palette = default_mp_palette());

struct MapSceneStats {
    uint32_t objects = 0, placements = 0, unknown_type = 0, no_model = 0, distinct_models = 0;
    uint32_t models_ready = 0, models_failed = 0, instances = 0;
    uint32_t attach_models = 0, attachments = 0;  // distinct Model_To_Attach models / attached instances drawn
    double parse_ms = 0, mesh_ms = 0, water_ms = 0, resolve_ms = 0;
    terrain::TerrainMeshStats mesh;
    uint32_t water_patches = 0;
    uint64_t water_triangles = 0;
    // Decorations built at load (DECORATIONS.md): map tracks with a ribbon, decals with geometry.
    uint32_t tracks = 0, tracks_built = 0, water_tracks = 0, decals = 0, decals_built = 0;
    uint64_t decoration_triangles = 0;
    double decoration_ms = 0;
    uint32_t fow_width = 0, fow_height = 0;
    uint32_t background_placements = 0;  // space: In_Background placements (background scene)
    uint32_t background_animated = 0;    // background placements + skydomes with an <model>_IDLE_00 clip
    // Placements whose starting health fraction (0x4B0 mini 03, ResolvedPlacement::initial_health_fraction)
    // is not 1. Not a scale (the transform uses the type's Scale_Factor only); diagnostic for the sim.
    uint32_t health_not_full = 0;
};

// Land maps: terrain + water + decorations (map tracks, decals; per-frame dynamic tracks and shadow blobs) + fog
// of war. Space maps (SpaceMapDesc, SPACE_AND_OPEN.md A): no terrain; placements of In_Background types and the
// environment skydomes (at the origin, type Scale_Factor, TEE 1D/1F and 1E/20 angles) form the background scene
// (sky(); draw with RecordOptions::space), the rest the main scene; Space_Mode camera; black clear.
class MapScene {
public:
    // CPU part: parse the map, build the terrain mesh + water + decorations on `parallel`, resolve the placements.
    // False (with `error`) on any failure, including exceptions such as std::bad_alloc; the scene is then
    // left empty (nothing loaded).
    bool load(const FileProvider& files, std::string_view map, const GameTypeResolver* resolver,
              const ParallelFor& parallel = serial_for, std::string* error = nullptr);
    // GPU part: world buffers / textures / water targets on `r`, model and sky-object requests on the
    // streamer, the start environment as the renderer's lighting, the world attached to `r`.
    bool start(Renderer& r, ModelStreamer& streamer, assets::AssetCache& cache, const WorldOptions& options = {},
               std::string* error = nullptr);
    // Per frame after ModelStreamer::update: residency, instances of the models that are ready, sky.
    void update(Renderer& r, ModelStreamer& streamer);
    // Every model finished (ready or failed) and the world resident.
    bool ready() const { return ready_; }
    void stop(Renderer& r);  // GPU idle: detach + free the world (models belong to the streamer)

    const terrain::MapData& map() const { return map_; }
    const terrain::TerrainMesh& mesh() const { return mesh_; }
    const terrain::WaterSurface& water() const { return water_; }
    const terrain::PlacementResult& placements() const { return placements_; }
    const Lighting& lighting() const { return lighting_; }
    std::span<const Instance> instances() const { return instances_; }
    // Land: the environment skydomes (camera-relative background). Space: the whole background scene
    // (skydomes + In_Background placements, world space).
    std::span<const Instance> sky() const { return space_ ? std::span<const Instance>(background_) : std::span<const Instance>(sky_instances_); }
    bool space() const { return space_; }
    // Frame clear colour: the environment's (TEE 15) on land, black in space (SPACE_AND_OPEN.md 0 item 3).
    Float3 clear_color() const { return space_ ? Float3{0, 0, 0} : lighting_.clear_color; }
    // RecordOptions for this map (space flag).
    void apply(RecordOptions& o) const { o.space = space_; }

    // ---- Fog of war (DECORATIONS.md 2): grid over the playable box (land 15, space 100 units per cell).
    terrain::FowGrid& fow() { return fow_; }
    // Border / blur / palette (land: kLandFowVisible / kLandFowColor, space: kSpaceFowVisible / kSpaceFowColor)
    // over fow().intensity(), then uploads (after start()). False before start or on an empty grid.
    bool apply_fog_of_war();
    void set_fow_enabled(bool on) { world_.set_fow_enabled(on); }
    // Clears the grid to fog and reveals a disc (intensity 255 inside `radius`, a 2-cell soft edge): a synthetic
    // visibility input for tests and the app (--fow).
    void reveal_circle(float x, float y, float radius);
    // ---- Per-frame decorations (after start()).
    const std::vector<terrain::DynamicTrackParams>& dynamic_track_params() const { return track_fx_; }
    const WorldDecorations& decorations() const { return deco_; }  // static decorations built at load
    const std::vector<terrain::ShadowBlobMaterial>& blob_materials() const { return deco_.blob_materials; }
    void set_dynamic_tracks(std::span<const terrain::DynamicTrackGenerator> tracks);
    void set_shadow_blobs(std::span<const terrain::ShadowBlob> blobs);
    WorldRenderer& world() { return world_; }
    const MapSceneStats& stats() const { return stats_; }
    // The tactical start camera: TacticalCameras.xml Land_Mode pitch 50 / distance 460 / FOV 55 (4:3) / near
    // 10 / far 7000 (or the map's 0x10C minis 12 / 14 / 15 when plausible, a mapping inferred from their
    // values), looking north at the centre of the playable box on the terrain surface (RENDER.md 3.4).
    // distance_override > 0 replaces the distance (alamo_render --camera-distance).
    OrbitCamera tactical_camera(float aspect, float distance_override = -1.0f) const;
    static constexpr float kNear = 10.0f, kFar = 7000.0f;
    static constexpr float kLandPitch = 50.0f, kLandDistance = 460.0f, kLandFov = 55.0f;
    static constexpr float kLandObjectFadeBegin = 1100.0f, kLandObjectFadeEnd = 1300.0f;  // Object_Fade_Begin / End
    // Space_Mode (SPACE_AND_OPEN.md A.2): FOV 55, pitch 50, distance 1000, near 10 / far 7000, fade 6000 -> 7000.
    static constexpr float kSpacePitch = 50.0f, kSpaceDistance = 1000.0f, kSpaceFov = 55.0f;
    static constexpr float kSpaceObjectFadeBegin = 6000.0f, kSpaceObjectFadeEnd = 7000.0f;
    // Fog always comes from the active (start) environment (SPACE_AND_OPEN.md B.1 [H]); the map's 0x100/2
    // scene-fog block is parsed by engine/terrain but never used.
    // Before start(): no environment skydome objects (main pass and sky cube; --no-sky).
    void set_sky_enabled(bool on) { sky_enabled_ = on; }
    // Before load(): draw the hidden damage-decal / state meshes too (alamo_render --show-damage).
    void set_show_damage(bool on) { show_damage_ = on; }
    // Before start(): false = the map's object placements are neither streamed nor drawn (instances() empty,
    // sky() = the environment skydomes only) - the sim's objects replace them (alamo_render --sim, R1).
    void set_objects_enabled(bool on) { objects_enabled_ = on; }
    // Before start(): the map will get R5 runtime decals (WorldDecorations::runtime_decals: the decal atlas is loaded).
    void set_runtime_decals_enabled(bool on) { runtime_decals_ = on; }
    bool objects_enabled() const { return objects_enabled_; }
    // Per-placement hardpoint state (placement index into placements().placements; hardpoint = index into the
    // type's TypeRenderInfo::hardpoints, or its name, case-insensitive). Destroying applies the B.10 rule on the
    // placement's model (hardpoints.hpp); destroyed = false is a DEBUG RESET only (the game never repairs
    // hardpoint visuals). Valid before the model is ready (applied when it is). Frame thread only, outside
    // Renderer builds; masks change at once, attachment instances on the next update(). False for an unknown
    // placement / hardpoint index / name.
    bool set_hardpoint_destroyed(size_t placement, size_t hardpoint, bool destroyed);
    bool set_hardpoint_destroyed(size_t placement, std::string_view hardpoint, bool destroyed);
    // State of a placement (null if out of range): destroyed bits, effective mesh mask once the model is ready.
    const HardpointVisibility* hardpoint_state(size_t placement) const {
        return placement < placement_vis_.size() ? &placement_vis_[placement] : nullptr;
    }
    const TypeRenderInfo* type_info(uint32_t type_crc) const {
        const auto it = type_info_.find(type_crc);
        return it == type_info_.end() ? nullptr : &it->second;
    }
    struct OwnerCount {
        uint32_t owner = 0, objects = 0, neutral = 0;
    };
    // Placements per owner slot (ascending slot) - diagnostics for the team-colour rule.
    std::vector<OwnerCount> owner_histogram() const;

    // ---- Background idle animation (SPACE_AND_OPEN.md D.7 4, RENDER.md 4). Space background placements and
    // skydomes play their model's `<model>_IDLE_00` clip at clip time idle_anim_seconds(tick,
    // Idle_Anim_00_Rate_Mod), looped per Loop_Idle_Anim_00. Frames advance only on 30 Hz sim ticks. Frame thread,
    // outside Renderer builds; poses are updated in place (Instance::bone_world of sky()).
    // R3 (ANIM_PARTICLES.md A.5 / A.9, corrects D.7 4): loops wrap at frame count - 1 and the last stored frame is
    // never sampled, so a 240-frame 1 fps planet clip (frame 239 = frame 0) turns once per 239 s, not 240 s;
    // one-shots hold frame count - 2. Sampled with alamo_anim's rules when engine/anim is configured.
    void set_sim_tick(uint64_t tick);
    uint64_t sim_tick() const { return sim_tick_; }
    static constexpr double kSimTickHz = 30.0;
    static float idle_anim_seconds(uint64_t tick, float rate) {
        return static_cast<float>(static_cast<double>(tick) / kSimTickHz * static_cast<double>(rate));
    }

private:
    struct IdleAnim {
        std::shared_ptr<const assets::AnimationClip> clip;
        float rate = 1.0f;
        bool loop = false;
        const assets::ModelAsset* asset = nullptr;  // bound when the instance is built
        std::vector<Mat3x4> world;                  // per skeleton bone; storage for Instance::bone_world
        std::shared_ptr<const void> r3;             // R3: alamo_anim clip + skeleton (map_scene.cpp), null without engine/anim
    };
    void load_idle_anim(assets::AssetCache& cache, std::string_view model, const TypeRenderInfo& ti, IdleAnim& out);
    std::span<const Mat3x4> bind_idle_anim(IdleAnim& a, const GpuModel* m);  // sizes + poses; empty if none
    void pose_idle_anim(IdleAnim& a) const;

    bool load_impl(const FileProvider& files, std::string_view map, const GameTypeResolver* resolver, const ParallelFor& parallel,
                   std::string* error);
    void clear_loaded();  // drops every CPU-side result of load() (failure path)

    terrain::MapData map_;
    terrain::TerrainMesh mesh_;
    terrain::WaterSurface water_;
    terrain::PlacementResult placements_;
    const GameTypeResolver* resolver_ = nullptr;
    Lighting lighting_;
    WorldRenderer world_;
    std::vector<std::string> models_;                 // distinct model names
    std::vector<uint32_t> model_handle_;              // streamer handle per distinct model
    std::vector<uint32_t> placement_model_;           // distinct model index per placement
    std::vector<std::pair<uint32_t, int>> sky_handles_;
    std::vector<Instance> instances_, sky_instances_, background_;
    bool space_ = false;
    std::vector<uint8_t> placement_bg_;               // per placement: In_Background type (space)
    WorldDecorations deco_;
    bool runtime_decals_ = false;  // R5
    std::vector<terrain::DynamicTrackParams> track_fx_;
    terrain::FowGrid fow_;
    bool started_ = false;
    float sky_scale_[2] = {1, 1};                     // skydome type Scale_Factor (space)
    std::vector<Instance> bg_placements_;              // space: In_Background placement instances
    // Per type: render info (LOD_Bias, Damage_Decal names, No_Colorization_Color) - looked up by key only.
    std::unordered_map<uint32_t, TypeRenderInfo> type_info_;
    // Per placement: hardpoint state + effective hidden-mesh mask (bound once its model is ready); stable storage
    // for Instance::mesh_hidden (the vector is sized at load only).
    std::vector<HardpointVisibility> placement_vis_;
    // Hardpoint layouts per (type CRC << 32 | distinct model index) - looked up by key only.
    std::unordered_map<uint64_t, HardpointLayoutPtr> layout_cache_;
    // Model_To_Attach models (B.10): distinct names (first-appearance order over placements x HardPoints), their
    // streamer handles, and per placement the distinct attachment model index per hardpoint (UINT32_MAX: none).
    std::vector<std::string> attach_models_;
    std::vector<uint32_t> attach_handle_;
    std::vector<std::vector<uint32_t>> placement_attach_;
    std::vector<const GpuModel*> placement_gpu_;      // model of each placement once ready (else null)
    std::vector<materials::float4> placement_color_;
    bool masks_dirty_ = false;
    const FileProvider* files_ = nullptr;             // load()'s provider (idle clip existence checks)
    std::vector<IdleAnim> placement_anim_;            // per placement (sized at start; only background ones get a clip)
    IdleAnim sky_anim_[2];
    uint64_t sim_tick_ = 0;
    uint32_t finished_seen_ = UINT32_MAX;
    bool sky_done_ = false, ready_ = false, sky_enabled_ = true, show_damage_ = false, objects_enabled_ = true;
    MapSceneStats stats_;
};

}  // namespace alamo::render
