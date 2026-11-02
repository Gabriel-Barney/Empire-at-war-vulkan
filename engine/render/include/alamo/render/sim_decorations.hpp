// alamo_render (lib alamo_render_sim) - R5 sim-driven terrain decorations: dynamic unit tracks and scorch decals from
// the R1 snapshot frames (docs/re/DECORATIONS.md §4.2, §5.2; data in surface_fx.hpp). Render-side only: it reads
// InterpolatedFrames and the type table, keeps its own state and random generator, and never touches the sim.
//
// Per new sim tick (InterpolatedFrame::tick advanced; dt = ticks / 30 s, at most 1 s):
//   * Tracks: every object whose type lists SurfaceFX_Name triggers gets, from the first trigger whose Surface_Settings
//     for the surface under the object (terrain_surface_type; "default" fallback) name a DynamicTrackFX entry with
//     min_geometry_lod <= the geometry detail, one DynamicTrackGenerator per side: the Dynamic_Track_Left_Bone /
//     Right_Bone world positions when both bones resolve through the bone source, else one track at the object
//     position. update(x, y, dt) each tick; on a surface without a track (e.g. concrete "NONE") or while the object is
//     dying the generator only fades (advance(dt)); a different track type restarts it; generators of objects that left
//     the frame are dropped [port: whether a trail outlives its unit is not traced].
//   * Scorch marks: an object that appears in a frame (id not in the previous processed frame) whose type has
//     Explosion_Scorch_Mark spawns that trigger's Decal_Name (for the surface at its position) through RuntimeDecals at
//     its position and heading - the PARTICLE objects the battle spawns for detonations / deaths carry it.
//   * Runtime decals fade on the decal clock (sim seconds = tick / 30).
//   * SurfaceFX particles (wakes / splashes): the first SurfaceFX_Name trigger whose settings for the surface under the
//     object name a Particle_Name keeps that particle attached to the object while energy >= Particle_Min_Energy, energy
//     = speed (units / s, from the last tick's movement) / the type's Max_Speed [port: the original's energy measure is
//     untraced; Max_Speed unknown -> 1 when moving, 0 when still]. The name is resolved as a GameObjectType (its model)
//     or used as an ALO name. Not for dying objects.
//   * Foot plants (InterpolatedFrame::foot_plants, the sim's per-tick events): the event's trigger, else the type's
//     SurfaceFX_Name triggers [port: the first one whose settings for the surface have a Decal_Name or Particle_Name;
//     *_LEFT / *_RIGHT triggers by foot], settings for the surface at the foot -> Decal_Name through RuntimeDecals at the
//     foot (the object's heading) and Particle_Name as a one-shot world effect at the foot, listed for
//     kFootEffectSeconds then dropped.
//   (R3f t01: the Spin_Away_On_Death_Explosion is no longer a decoration; the battle spawns it as a sim object.)
// Output: track_meshes() / decal_geometry() for WorldRenderer::set_dynamic_tracks / set_runtime_decals (apply());
// effects() for particles::sync_sim_frame's world effects.
// Space maps (no terrain) produce nothing. Foot-plant triggers (AT_AT_FOOTPRINT, INFANTRY_FOOT_PLANT, ...) need an
// animation event source the sim does not have (ALA has no events; open item).
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "alamo/render/sim_snapshot.hpp"
#include "alamo/render/sim_view.hpp"
#include "alamo/render/surface_fx.hpp"
#include "alamo/render/world.hpp"

namespace alamo::render {

struct SimDecorationOptions {
    float geometry_detail = 1.0f;  // tracks whose min_geometry_lod exceeds it are not created
    bool tracks = true, scorch = true;
};

struct SimDecorationStats {
    uint64_t ticks = 0;
    uint32_t track_objects = 0, generators = 0, faded_only = 0;  // last tick
    uint64_t scorch_seen = 0, scorch_spawned = 0, scorch_rejected = 0, scorch_no_decal = 0;
    uint32_t live_decals = 0;
    uint32_t surface_particles = 0;  // last tick: objects with a SurfaceFX particle attached
    uint64_t foot_plants = 0, foot_decals = 0, foot_puffs = 0, foot_unmatched = 0;
};

inline constexpr float kFootEffectSeconds = 4.0f;  // [port] lifetime of a footprint puff's world-effect entry

class SimDecorations {
public:
    // `terrain` may be null (space: nothing is produced). `lib` and `table` must outlive this object.
    SimDecorations(const SurfaceFxLibrary& lib, const SnapshotTypeTable& table, const terrain::Terrain* terrain,
                   uint64_t seed = 0x52355235ull);
    void set_options(const SimDecorationOptions& o) { options_ = o; }
    // World position of a named bone of an object (Dynamic_Track_Left/Right_Bone); false when unknown.
    using BoneWorldFn = std::function<bool(const SnapshotObject& o, std::string_view bone, Vec3& out)>;
    void set_bone_source(BoneWorldFn fn) { bones_ = std::move(fn); }
    // Steps once per new sim tick of `frame` (no-op otherwise).
    void update(const InterpolatedFrame& frame);
    std::vector<DynamicTrackMesh> track_meshes() const;
    std::vector<terrain::DecalGeometry> decal_geometry() const { return decals_.geometry(); }
    // set_dynamic_tracks + set_runtime_decals.
    void apply(WorldRenderer& world) const;
    // R5 particle effects of the current tick (SurfaceFX wakes on objects, footprint puffs at world points), key order.
    std::span<const SimWorldEffect> effects() const { return effects_; }
    const RuntimeDecals& decals() const { return decals_; }
    RuntimeDecals& decals() { return decals_; }
    size_t generator_count() const { return gens_.size(); }
    const SimDecorationStats& stats() const { return stats_; }

private:
    struct Gen {
        std::unique_ptr<terrain::DynamicTrackGenerator> gen;
        std::string track;  // DynamicTrackFX name it was made for (upper case)
        uint64_t seen = 0;
    };
    const SurfaceFxLibrary* lib_;
    const SnapshotTypeTable* table_;
    const terrain::Terrain* terrain_;
    SimDecorationOptions options_;
    BoneWorldFn bones_;
    RuntimeDecals decals_;
    std::map<std::pair<objects::ObjectId, int>, Gen> gens_;  // (id, side 0 centre / 1 left / 2 right), ordered
    std::vector<objects::ObjectId> prev_ids_;                // ascending
    std::vector<Vec3> prev_pos_;                             // same order: positions of the previous tick (speed)
    struct Puff {
        SimWorldEffect fx;
        float until = 0.0f;
    };
    std::vector<Puff> puffs_;          // live footprint puffs (spawn order)
    std::vector<SimWorldEffect> effects_;
    uint64_t next_puff_ = 0;
    std::string particle_asset(std::string_view name) const;  // type name -> model, else the name
    const SurfaceSettings* foot_settings(const SnapshotTypeInfo& ti, const SnapshotFootPlant& f, std::string_view surface) const;
    uint64_t last_tick_ = 0, generation_ = 0;
    bool started_ = false;
    SimDecorationStats stats_;
};

// Bone source over the streamed models: object_transform(position, facing, Scale_Factor) x the bone's model-space matrix
// (the pose source's current pose when given, else the bind pose); bone names case-insensitive.
SimDecorations::BoneWorldFn model_bone_source(const SnapshotTypeTable& table, SimModelSource& models,
                                              const SimInstanceBuilder::PoseSource* pose = nullptr);

}  // namespace alamo::render
