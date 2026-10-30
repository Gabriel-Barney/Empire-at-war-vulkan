// A1 - optional passability prefilter for land reachability (built when engine/tactical/movement
// is configured; ALAMO_AI_TACTICAL_HAVE_MOVEMENT).
//
// [port] Not a retail rule: the retail ZoneReachability answers from the zone graph only (3.4).
// This helper lets a host refuse a land region whose centre lies in a different passability
// component of the movement region map than the source (read-only use of RegionMap). It is OFF
// unless the caller applies it (e.g. as an extra Reachable_Target filter while the land zone graph
// edge weights are still provisional).
#pragma once
#include <cstdint>

#if defined(ALAMO_AI_TACTICAL_HAVE_MOVEMENT)
#include "alamo/tactical/movement/land_path.hpp"

namespace alamo::ai::tactical {

// True when both world points lie on passable cells of the same component for `mask` (the mask
// must have been passed to RegionMap::build). False otherwise (blocked / uncovered / unknown mask).
bool land_points_connected(const ::alamo::tactical::movement::RegionMap& regions, uint32_t mask, float ax, float ay, float bx,
                           float by);

}  // namespace alamo::ai::tactical
#endif
