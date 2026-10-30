// Behaviours (docs/re/OBJECT_RUNTIME.md §4).
//
// Runtime behaviour TYPE ids are the slots of the original's 0x6A-entry type -> behaviour index
// table: 0 = the object itself, 0x01..0x69 behaviours. XML names map to ids through the doc's
// §4.3 table (116 names): all 13 locomotors share id 0x01 (an object has at most one locomotor),
// TARGETING / TEAM_TARGETING share 0x09, INDIGENOUS_UNIT / EXPLOSION share 0x20, the 15 DUMMY_*
// names are 0x5B..0x69 in doc order; 0x1F, 0x24 and 0x35 have no XML name (internal).
// Unknown names are ignored (the original drops them silently).
//
// behavior_classes() keeps the 121 RTTI classes deriving from BehaviorClass (docs/re/rtti_classes.txt)
// for reference / naming only.
#pragma once
#include <cstdint>
#include <span>
#include <string_view>

namespace alamo::objects {

using BehaviorTypeId = uint8_t;
inline constexpr uint32_t kBehaviorTypeCount = 0x6A;  // ids 0..0x69
inline constexpr BehaviorTypeId kNoBehavior = 0;

namespace beh {
inline constexpr BehaviorTypeId Locomotor = 0x01, Selectable = 0x02, Planet = 0x03, Fleet = 0x04, Transport = 0x05,
                                Production = 0x06, Idle = 0x07, Projectile = 0x08, Targeting = 0x09, Weapon = 0x0A,
                                Particle = 0x0C, Shielded = 0x10, Marker = 0x11, StarshipCombatant = 0x13,
                                Team = 0x16, TacticalSuperWeapon = 0x2C, TransportLanding = 0x36, Hint = 0x4A,
                                DummyUpgrade = 0x65, Powered = 0x0F, LandObstacle = 0x14, DeployTroopers = 0x1C,
                                Barrage = 0x1D, Landing = 0x1E, IndigenousUnit = 0x20, Hunt = 0x21,
                                GuardSpawnHouse = 0x22, Lure = 0x23, BaseShield = 0x39, SelfDestruct = 0x43;
}

// XML behaviour name (ASCII case-insensitive) -> type id; kNoBehavior if unknown.
BehaviorTypeId behavior_type_id(std::string_view xml_name);

// Service class of a behaviour (OBJECT_RUNTIME.md §9.1): whether it is serviced at all and its
// service interval in frames. Never serviced: TACTICAL_BUILD_OBJECTS, MARKER, HINT, TRANSPORT,
// TACTICAL_SELL, TERRAIN_TEXTURE_MODIFICATION, WALL, SPACE_OBSTACLE, STARSHIP_COMBATANT and the 15
// DUMMY_*. Interval 0: EXPLOSION; 4: CAPTURE_POINT, CASH_POINT; 30: SPAWN_INDIGENOUS_UNITS,
// DAMAGE_TRACKING, HARASS, AVOID_DANGER, BARRAGE, SPAWN_SQUADRON, BURNING, IMPOSING_PRESENCE,
// AMBIENT_SFX, INVULNERABLE; 60: GRAVITY_CONTROL_FIELD; every other behaviour 1.
// "Computed at attach" in the original (LAND_OBSTACLE, SHIELDED, POWERED, BASE_SHIELD,
// INDIGENOUS_UNIT, HUNT, GUARD_SPAWN_HOUSE): `computed` = true and `interval` = 1 here; the store
// replaces it at attach with computed_behavior_interval() (TACTICAL_GAPS.md §6.4).
struct BehaviorService {
    bool serviced = false;
    uint32_t interval = 0;
    bool computed = false;
};
BehaviorService behavior_service(std::string_view xml_name);  // unknown name: not serviced

// Inputs of the "computed at attach" intervals (TACTICAL_GAPS.md §6.4): FPS (global 0x140b0a340,
// 30) and the GameConstants ShieldRechargeIntervalInSecs (+0x354) / EnergyRechargeIntervalInSecs
// (+0x358).
struct BehaviorIntervalConfig {
    uint32_t fps = 30;
    float shield_recharge_interval_secs = 3.0f;
    float energy_recharge_interval_secs = 5.0f;
};
// Interval of a computed behaviour (TACTICAL_GAPS.md §6.4, [H]):
//   LAND_OBSTACLE 1 (0 or 1 in the original: every frame either way);
//   SHIELDED  int(FPS x `multiplier` x ShieldRechargeIntervalInSecs + 0.5) (multiplier = the
//             SHIELD_REGEN_INTERVAL modifier product, 1 without modifiers);
//   POWERED   int(FPS x `multiplier` x EnergyRechargeIntervalInSecs + 0.5) (ENERGY_REGEN_INTERVAL);
//   BASE_SHIELD FPS / 2 (15);
//   INDIGENOUS_UNIT, HUNT, GUARD_SPAWN_HOUSE: 1 while `burning` (obj +0x3a1 & 2), else 30.
// Negative / NaN products give 0. Any other id: 1.
uint32_t computed_behavior_interval(BehaviorTypeId id, const BehaviorIntervalConfig& cfg, float multiplier = 1.0f,
                                    bool burning = false);
// Behaviours whose Initialise disables them (disable counter 1, TACTICAL_GAPS.md §6.6): HUNT, LURE,
// SELF_DESTRUCT (0x628800); DEPLOY_TROOPERS, BARRAGE, LANDING (0x58f870).
bool behavior_starts_disabled(BehaviorTypeId id);
// By id: the id's canonical name (0x20 -> INDIGENOUS_UNIT; EXPLOSION shares the id but has
// interval 0, so attach by name where the name is known).
BehaviorService behavior_service(BehaviorTypeId id);
// Canonical XML name of an id (the first name of its row; "LOCOMOTOR" for 0x01); "" if none.
std::string_view behavior_type_name(BehaviorTypeId id);
// Number of XML names in the table (116).
uint32_t behavior_name_count();

std::span<const std::string_view> behavior_classes();
// "Behavior", "LandBehavior", "SpaceBehavior", "GalacticBehavior", "TacticalBehavior",
// "DeployedBehavior" (ASCII case-insensitive).
bool is_behavior_list_tag(std::string_view tag);

}  // namespace alamo::objects
