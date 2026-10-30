// G0 map spawning: a .ted map's object placements (engine/terrain) become real objects in a
// tactical mode's primary store with their real type stats (engine/objects GameObjectTypes).
//
// Per placement, in map object order (= the order the original's loader creates them):
//   type     = first GameObjectType definition whose name CRC (alamo::name_crc of the upper-cased
//              name) equals the placement's type CRC; unknown CRCs are counted and skipped
//   owner    = placement owner (player slot)
//   position = as stored (absolute z); facing = (cos yaw, sin yaw, 0) from the stored Euler z
//   health   = initial_health_fraction x Tactical_Health (non-finite fraction -> 1)
//   spatial  = TreeSet entry keyed by the new object id; trees rebuilt with the map-load
//              threshold at the end (Rebuild_With_Threshold)
// Mode bounds = the map's playable box (land: 0x103 / terrain extent; space: 0x103 04/05).
// Not spawned yet (open items): hardpoint sub-objects, company / squadron members, skirmish
// starting forces and the parent-mode links of campaign battles.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "alamo/sim/game_modes.hpp"
#include "alamo/terrain/ted.hpp"

namespace alamo::sim {

// Name CRC -> first type definition (lookup only, never iterated).
class TypeCrcIndex {
public:
    explicit TypeCrcIndex(const objects::GameObjectTypes& types);
    objects::TypeIndex find(uint32_t name_crc) const;
    size_t size() const { return map_.size(); }

private:
    std::unordered_map<uint32_t, objects::TypeIndex> map_;
};

struct SpawnReport {
    uint32_t placements = 0, spawned = 0, unknown_type = 0, in_trees = 0;
    uint32_t units = 0, structures = 0, props = 0;            // category counts
    float total_health = 0.0f;                               // sum of spawned health (stats check)
    std::map<int32_t, uint32_t> per_owner;                   // owner -> objects
    std::vector<std::pair<uint32_t, uint32_t>> unknown_crcs; // (crc, count), ascending crc
};

SpawnReport spawn_map(TacticalMode& mode, const terrain::MapData& map, const TypeCrcIndex& index,
                      const ParallelFor& pf = serial_for);

}  // namespace alamo::sim
