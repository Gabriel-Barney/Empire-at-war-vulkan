// .ted map -> movement world (built when the terrain module is configured;
// ALAMO_MOVEMENT_HAVE_TERRAIN is defined then).
//
// grid_from_map: one cell per terrain vertex; base = 0x10A passability (row-major in MapData),
//   or the legacy 0x104 bytes (0 Clear, 1 Steep, 2 Obstacle), or all Clear; cells whose centre is
//   outside the land playable box (0x103 05/06) get Obstacle (§1.1); water plane height for the
//   stamping water rule.
// stamp_map_obstacles: LAND_OBSTACLE placements, each stamped by land_obstacle_footprint at the placement's position / yaw /
//   height (MOVEMENT §1.3: one stamper serves map placements and run-time objects). Stamp ids are -(placement index + 1)
//   (map_stamp_id) so they never collide with object ids.
// land_obstacle_footprint: the LAND_OBSTACLE footprint of one object of type `type` at `position` / yaw / terrain height (no
//   behaviour test). S8 t09 (MOVEMENT §1.3 [H]): the footprint type F is the type's Obstacle_Proxy_Type when it names a type,
//   else the type itself; F supplies Obstacle_Width / _Height / _X_Offset / _Y_Offset, else (OI tactical) the model bounding
//   box from `hooks.model_half_extents` (2 x |half extent| x F's Scale_Factor, offset -half extent x scale, vertical height
//   = model z x scale for the water rule), and F's Fine_Tune_Occupied_Passability (default true). bits = the object type's
//   OverridePassability or Obstacle [inferred: the cell recompute reads the occupant's own type]. Fine-tune: each sample
//   needs the vertical segment (x, y, -1000) -> (x, y, +1000) to hit F's land model placed at the object's position / yaw,
//   scaled by F's Scale_Factor (proxy: the object's transform x proxy scale / object scale) - hooks.mesh_ray_hit. Ray model
//   (hooks.ray_model(F)): Model -> trimmed; None (F has no land model) -> with a proxy no ray (every sample stamps), without
//   a proxy NOT STAMPED (false: retail returns before the loop and retries at the next service; the port's model data is
//   const, so hosts treat it like "no footprint"); Unknown (no model data in this run, no hook) -> untrimmed [port].
//   False also when F has neither XML size nor model extents. S8 t02: hosts stamp LAND_OBSTACLE objects spawned at run time
//   with it under their object id (LandObstacleBehaviorClass stamps on its first service, unstamps on detach), and re-stamp
//   a Map_Load_Spawn_Table placement with the type actually spawned.
// space_from_map: playable box (0x103 04/05) and SPACE_OBSTACLE placements as static entries;
//   radius = first positive Custom_Soft_Footprint_Radius / Space_Obstacle_Radius, else (S7 t07, MOVEMENT §2.2) the larger
//   x / y half extent of the type's space-model collision bound from `hooks.model_half_extents`, x Scale_Factor
//   (PROVISIONAL [M]: the retail getter's "2R/20" conversion is unexplained and not applied). Centre = placement position +
//   Space_Obstacle_Offset (x along the placement facing Rz(yaw) (0, -1), y to its left [M: rotation sense], as the host's
//   space_obstacles_sync does for units). Entry ids are map_stamp_id(placement) (S7 t13: a host that spawns an object for
//   the placement moves the entry to the object id, SpaceOccupancy::rekey_static; removed when it is a battle unit that left).
// space_obstacle_center: that centre for an object at `position` facing the unit XY direction `facing`.
// space_obstacle_footprint: that static entry's radius and SpaceCollisionType bits for one type; false when the type has no
//   SPACE_OBSTACLE behaviour, or no positive radius and no model extent. refixf t02: hosts use it for SPACE_OBSTACLE objects
//   spawned at run time (MOVEMENT §2.2: SpaceObstacleBehaviorClass inserts the object as a static entry on attach, removes
//   it on detach). The model's collide switch is not consulted (SPACE_AVOIDANCE §5.5: capture points and orbital props
//   stay planner obstacles).
#pragma once
#include <cstdint>
#include <functional>

#include "alamo/tactical/movement/passability.hpp"
#include "alamo/tactical/movement/space_path.hpp"
#include "alamo/terrain/ted.hpp"

namespace alamo::objects {
class GameObjectTypes;
}

namespace alamo::tactical::movement {

PassabilityGrid grid_from_map(const terrain::MapData& map);
// OI tactical: host-supplied model data (the sim loads no models; every hook must be deterministic, const data).
enum class StampRayModel : uint8_t {
    Unknown,  // no model data in this run: no ray test [port]
    None,     // the type has no land model
    Model,    // the ray test applies (mesh_ray_hit)
};
// Where the fine-tune ray's model sits: the object's position (z = its height), yaw (the footprint frame: model +x along
// dir_from_deg(yaw_deg), as space_avoid's transform with facing Rz(yaw) (0, -1)) and the model scale.
struct StampPose {
    V2 position;
    float z = 0.0f;
    float yaw_deg = 0.0f;
    float scale = 1.0f;
};
struct MapStampHooks {
    // Model-space bounding box of type `type`'s land model (before Scale_Factor): x / y half extents and the z extent
    // (max - min, the water rule's "model bbox z" [inferred]); false when unknown.
    std::function<bool(uint32_t type, float& half_x, float& half_y, float& height_z)> model_half_extents;
    // S8 t09: Fine_Tune_Occupied_Passability (MOVEMENT §1.3). ray_model(type): does type `type` have a land model to test
    // (unset hook: Unknown)? mesh_ray_hit: does the vertical segment (x, y, -1000) -> (x, y, +1000) at `world_xy` hit the
    // collision mesh of type `model_type`'s land model placed at `pose`? Called only when ray_model gave Model.
    std::function<StampRayModel(uint32_t type)> ray_model;
    std::function<bool(uint32_t model_type, const StampPose& pose, V2 world_xy)> mesh_ray_hit;
};
uint32_t stamp_map_obstacles(PassabilityGrid& grid, const terrain::MapData& map, const objects::GameObjectTypes& types,
                             const MapStampHooks* hooks = nullptr);
// S8 t02 / t09: the LAND_OBSTACLE footprint of one object (rules above). `out.fine_tune` (when set) holds copies of the
// hooks it needs, so it stays valid after `hooks` is gone.
bool land_obstacle_footprint(const objects::GameObjectTypes& types, uint32_t type, V2 position, float yaw_deg, float terrain_z,
                             const MapStampHooks* hooks, ObstacleFootprint& out);
// S8 t09: the footprint type of `type`: its Obstacle_Proxy_Type when that names a type, else `type`.
uint32_t land_obstacle_footprint_type(const objects::GameObjectTypes& types, uint32_t type);
// S8 t01: the stamp id stamp_map_obstacles gives map placement `placement` (index into MapData::objects). A host that
// spawns an object for the placement moves the stamp to the object id (PassabilityGrid::rekey), so the per-object cell
// query (PassabilityGrid::occupies) answers for that object.
inline ObjectId map_stamp_id(size_t placement) { return -static_cast<ObjectId>(placement) - 1; }
// S7 t07: host-supplied model data for SPACE_OBSTACLE footprints (deterministic, const data).
struct SpaceObstacleHooks {
    // Half x / y extents of the collision bound of type `type`'s space model (bind-pose union of the collision-enabled
    // meshes, 1 / 1 when it has none; before Scale_Factor); false when the model is unknown.
    std::function<bool(uint32_t type, float& half_x, float& half_y)> model_half_extents;
};
uint32_t space_from_map(SpaceOccupancy& occ, const terrain::MapData& map, const objects::GameObjectTypes* types,
                        const SpaceObstacleHooks* hooks = nullptr);
bool space_obstacle_footprint(const objects::GameObjectTypes& types, uint32_t type, float& radius, uint32_t& bits,
                              const SpaceObstacleHooks* hooks = nullptr);
V2 space_obstacle_center(const objects::GameObjectTypes& types, uint32_t type, V2 position, V2 facing);

}  // namespace alamo::tactical::movement
