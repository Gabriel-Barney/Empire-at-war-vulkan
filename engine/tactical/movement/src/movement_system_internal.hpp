#pragma once
// Shared includes, helpers and types of movement_system.cpp (bo3 t03 split); its member definitions are in movement_system.cpp and movement_system_partN.cpp.
#include "alamo/tactical/movement/movement_system.hpp"
#include "alamo/core/sim_crc.hpp"

#include <algorithm>
#include <bit>
#include <cmath>

#include "alamo/core/dmath.hpp"
#include "alamo/objects/object_store.hpp"

namespace alamo::tactical::movement {

namespace bo3split {

inline constexpr float kInf = 3.0e38f;
uint64_t cell_key(int32_t x, int32_t y);
bool contains_key(const std::vector<uint64_t>& v, uint64_t k);
void insert_key(std::vector<uint64_t>& v, uint64_t k);
float slowdown_for_layer(const MovementConstants& k, uint32_t layer);

}  // namespace
using namespace bo3split;

// ---- s78n t20 (MOVEMENT §1.5.1 item 5): the collision-owned record on top of the path stack ----
namespace bo3split {uint8_t record_weight(CollisionKind k);
bool record_finished(const std::vector<V2>& pts, uint32_t& next, V2 pos);
}  // namespace
using namespace bo3split;

namespace bo3split {bool infantry_class(int32_t k);
}  // namespace
using namespace bo3split;

namespace bo3split {void hash_f3(StateHasher& h, F3 v);
void hash_att(StateHasher& h, FAttitude o);
void hash_cell(StateHasher& h, FighterCell c);
void hash_target(StateHasher& h, const FighterTarget& t);
}  // namespace
using namespace bo3split;
}  // namespace alamo::tactical::movement
