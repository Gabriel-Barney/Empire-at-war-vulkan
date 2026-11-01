// .ted map -> movement world (built when the terrain module is configured;
// ALAMO_MOVEMENT_HAVE_TERRAIN is defined then).
//
// grid_from_map: one cell per terrain vertex; base = 0x10A passability (row-major in MapData),
//   or the legacy 0x104 bytes (0 Clear, 1 Steep, 2 Obstacle), or all Clear; cells whose centre is
//   outside the land playable box (0x103 05/06) get Obstacle (§1.1); water plane height for the
//   stamping water rule.
// stamp_map_obstacles: LAND_OBSTACLE placements; footprint from Obstacle_Width / Obstacle_Height, else
//   (OI tactical, §1.3) from the model bounding box when `hooks.model_half_extents` supplies it
//   (2 x |half extent| x Scale_Factor, offset -half extent x scale, vertical height = model z x scale for
//   the water rule); types with neither are skipped. Fine_Tune_Occupied_Passability types trim their
//   samples with `hooks.mesh_ray_hit` when supplied (else untrimmed). bits = OverridePassability or
//   Obstacle. Stamp ids are -(placement index + 1) (map_stamp_id) so they never collide with object ids.
// space_from_map: playable box (0x103 04/05) and SPACE_OBSTACLE placements as static entries;
//   radius = first positive Custom_Soft_Footprint_Radius / Space_Obstacle_Radius x Scale_Factor
//   (PROVISIONAL [M]: the retail getter's "2R/20" conversion is unexplained and not applied).
// space_obstacle_footprint: that static entry's radius and SpaceCollisionType bits for one type; false when the type has no
//   SPACE_OBSTACLE behaviour or no positive radius. refixf t02: hosts use it for SPACE_OBSTACLE objects spawned at run time
//   (MOVEMENT §2.2: SpaceObstacleBehaviorClass inserts the object as a static entry on attach, removes it on detach).
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
// OI tactical: host-supplied model data (the sim loads no models; both must be deterministic, const data).
struct MapStampHooks {
    // Model-space bounding box of type `type`'s land model (before Scale_Factor): x / y half extents and the z extent
    // (max - min, the water rule's "model bbox z" [inferred]); false when unknown.
    std::function<bool(uint32_t type, float& half_x, float& half_y, float& height_z)> model_half_extents;
    // Fine_Tune_Occupied_Passability: does the vertical ray (z +1000 -> -1000) at world XY hit the collision mesh of
    // map placement `placement` (index into MapData::objects)?
    std::function<bool(size_t placement, V2 world_xy)> mesh_ray_hit;
};
uint32_t stamp_map_obstacles(PassabilityGrid& grid, const terrain::MapData& map, const objects::GameObjectTypes& types,
                             const MapStampHooks* hooks = nullptr);
// S8 t01: the stamp id stamp_map_obstacles gives map placement `placement` (index into MapData::objects). A host that
// spawns an object for the placement moves the stamp to the object id (PassabilityGrid::rekey), so the per-object cell
// query (PassabilityGrid::occupies) answers for that object.
inline ObjectId map_stamp_id(size_t placement) { return -static_cast<ObjectId>(placement) - 1; }
uint32_t space_from_map(SpaceOccupancy& occ, const terrain::MapData& map, const objects::GameObjectTypes* types);
bool space_obstacle_footprint(const objects::GameObjectTypes& types, uint32_t type, float& radius, uint32_t& bits);

}  // namespace alamo::tactical::movement
