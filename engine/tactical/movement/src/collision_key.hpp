// CollisionLayer's pair key, shared by collision.cpp (has_pair / add / drop_pair) and movement_save.cpp (rebuilding
// the sorted pair list on load), so the two cannot drift. Module-internal (src/), not installed.
#pragma once
#include <algorithm>
#include <cstdint>

#include "alamo/tactical/movement/collision.hpp"

namespace alamo::tactical::movement {

// Unordered pair: the smaller id in the high word.
inline uint64_t collision_pair_key(ObjectId a, ObjectId b) {
    const uint32_t x = static_cast<uint32_t>(std::min(a, b)), y = static_cast<uint32_t>(std::max(a, b));
    return (static_cast<uint64_t>(x) << 32) | y;
}

}  // namespace alamo::tactical::movement
