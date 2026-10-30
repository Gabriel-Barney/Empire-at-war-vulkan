// A1 - which targets a goal can use: Applies(flags, aiPlayer), docs/re/AI_TACTICAL.md 3.3;
// AIGoalApplicationType / AIGoalReachabilityType (AI_GALACTIC.md 1.4).
//
// Region (0x725730): matches iff Tactical_Location is set. Global never matches a target.
// Unit (0x724ef0):
//   1. reject if fully fogged for the AI player (the fog test with "ignore AIUsesFogOfWar" = true),
//      unless the type has Last_State_Visible_Under_FOW or Initial_State_Visible_Under_FOW;
//   2. friendly = has an owner AND that owner is allied with the AI player (unowned = not friendly);
//   3. class by behaviour slots: locomotor -> Unit; else TACTICAL_BUILD_OBJECTS -> Build_Pad; else
//      CASH_POINT -> Cash_Point; else Reinforcement_Enabling_Radius > 0 -> Reinforcement_Point;
//      else Structure. Match iff the friendly / enemy bit of the class is set.
// The fog test is an input (callback) - the FOW system is the host's.
#pragma once
#include <cstdint>
#include <functional>
#include <string_view>

#include "alamo/ai/ai_types.hpp"
#include "alamo/ai/tactical/target_list.hpp"

namespace alamo::ai::tactical {

// Stage 2: the AIGoalApplicationType bits and AIGoalReachabilityType are the A2 framework's
// (alamo/ai/ai_types.hpp); these names alias them.
inline constexpr uint32_t kAppFriendly = app::Friendly, kAppEnemy = app::Enemy, kAppNeutral = app::Neutral,
                          kAppContested = app::Contested, kAppGlobal = app::Global, kAppFriendlyUnit = app::FriendlyUnit,
                          kAppEnemyUnit = app::EnemyUnit, kAppFriendlyStructure = app::FriendlyStructure,
                          kAppEnemyStructure = app::EnemyStructure, kAppTacticalLocation = app::TacticalLocation,
                          kAppFriendlyBuildPad = app::FriendlyBuildPad, kAppEnemyBuildPad = app::EnemyBuildPad,
                          kAppFriendlyCashPoint = app::FriendlyCashPoint, kAppEnemyCashPoint = app::EnemyCashPoint,
                          kAppFriendlyReinforcementPoint = app::FriendlyReinforcementPoint,
                          kAppEnemyReinforcementPoint = app::EnemyReinforcementPoint;

// "A | B | C" -> bits (A2's parse_application_flags); names matched case-insensitively (AI_GALACTIC 1.4 [M]).
// False on an unknown name or an empty list (bits holds what parsed). Whether Global is legal is the
// caller's rule (FindTarget errors on it, 6.1).
bool parse_goal_application(std::string_view text, uint32_t& bits);

using Reachability = ::alamo::ai::Reachability;
// AIGoalReachabilityType name (case-insensitive) -> value. False if unknown (A2's parser).
using ::alamo::ai::parse_reachability;

struct UnitAppliesInfo {
    int32_t object_id = -1;
    bool has_owner = false;             // owner record +4 != -1
    bool owner_allied = false;          // owner allied with the AI player (0x4ad9f0)
    bool has_locomotor = false;         // behaviour 0x01 (obj +0x333)
    bool has_build_objects = false;     // TACTICAL_BUILD_OBJECTS 0x29 (+0x35b) - build pad
    bool has_cash_point = false;        // CASH_POINT 0x3c (+0x36e)
    float reinforcement_enabling_radius = 0.0f;  // type +0x1e20
    bool last_state_visible_under_fow = false;   // type +0x88
    bool initial_state_visible_under_fow = false;  // type +0x89
};

struct UnitClassBits {
    uint32_t friendly = 0, enemy = 0;
};
UnitClassBits classify_unit(const UnitAppliesInfo& u);
bool is_friendly(const UnitAppliesInfo& u);

bool region_applies(uint32_t flags);
// `fully_fogged`: the object is fully fogged for the AI player.
bool unit_applies(uint32_t flags, const UnitAppliesInfo& u, bool fully_fogged);
// Fog as a callback: called only when the type is not visible-under-FOW.
using FogQuery = std::function<bool(int32_t object_id)>;
bool unit_applies(uint32_t flags, const UnitAppliesInfo& u, const FogQuery& fully_fogged);

}  // namespace alamo::ai::tactical
