#pragma once
// Shared includes, helpers and types of combat_system.cpp (bo3 t03 split); its member definitions are in combat_system.cpp and combat_system_partN.cpp.
#include "alamo/tactical/combat/combat_system.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>

#include "alamo/core/dmath.hpp"
#include "alamo/core/hash.hpp"
#include "alamo/core/sim_crc.hpp"
#include "alamo/data/game_database.hpp"

namespace alamo::tactical::combat {

namespace bo3split {

inline constexpr uint32_t kUnitRemoved = 1u << 31;  // internal: host removal (no death event)
inline constexpr float kRadToDeg = 57.295779513082320876f;
inline constexpr int32_t kBurningInterval = 30;  // OI abilities: BurningBehavior vtable +0x28 (TACTICAL_GAPS §1.8)
inline constexpr int32_t kAbilityRocketAttack = 11;    // S8 t08: UnitAbilityType ROCKET_ATTACK (0x39b950(obj, 11))
// S8 t08: Mod_Flag WEAPONS_ENABLED_FLAG (0x397f60(obj, 1); S8 t11: enum table 0x140a434a0 index 1 [H]).
inline constexpr uint8_t kFlagWeaponsEnabled = 1;
inline constexpr uint32_t kNoService = 0xFFFFFFFFu;    // S7 t15 A2: Unit::weapon_last of an older save, never serviced (= 0)
inline constexpr float kGarrisonGunHalfAngle = 60.0f;  // S8 t10: Spawn_Projectile garrison pick, |bone yaw - target yaw|
const PlayerInfo kDefaultPlayer{};
Vec3 normalize(Vec3 v);
float wrap_deg(float a);
float abs_f(float v);

}  // namespace
using namespace bo3split;
}  // namespace alamo::tactical::combat
