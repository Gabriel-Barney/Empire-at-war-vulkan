// S4 (owner: S4 agent) - FoC corruption options of a planet type and the corruption-mission rule.
//
// Data (PLANETS.XML, OBJECT_TYPES.md enum fields Corruption_N_Type / _Required_Mission_Type = CorruptionTypeEnum,
// Corruption_N_Tactical_Game_Mode = tSubGameModeType): up to three options per planet type, Corruption_1..3_*:
// Type, Credit_Cost, Required_Unit_Type / _Count, Required_Mission_Type / _Count, Tactical_Game_Mode (Land / Space),
// Tactical_Map_Name, Tactical_Script_Name (a Story_Plots file), plus presentation fields (speech events, briefing, Bink
// holograms, Success_Unlock_Unit_Type) that the sim does not read.
//
// Corruption mission [inferred until docs/re/L1_S4_REMAINING.md section C: the Underworld corruption action is "not
// traced" in GALACTIC_ECONOMY.md]: choosing an option whose Tactical_Game_Mode and Tactical_Map_Name are set starts a
// tactical battle at the planet on that map (attacker = the corrupting player, defender = the planet's owner) with
// the option's Tactical_Script_Name as the tactical story, and records the pending mission (the original's GMM
// +0x1f4 corrupting player / +0x1f8 corruption type, SCRIPT_GAPS.md §4.7); the GMM battle end then applies the
// corruption and dispatches STORY_CORRUPTION_TACTICAL_COMPLETE when that player won, else _FAILED (StorySession). An
// option without a tactical mission corrupts the planet at once (GalacticSim::set_corruption). Credit cost and the
// unit / mission requirements are reported, not enforced here [port: the action's GUI checks are not traced].
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "alamo/galactic/conflicts.hpp"
#include "alamo/galactic/economy.hpp"
#include "alamo/objects/object_types.hpp"

namespace alamo::objects {
class GameObjectTypes;
}

namespace alamo::galactic {

struct CorruptionOption {
    uint32_t slot = 0;                       // 1..3 (Corruption_<slot>_*)
    int8_t type = kCorruptionNone;           // Corruption_N_Type
    int32_t credit_cost = 0;                 // Corruption_N_Credit_Cost
    std::string required_unit;               // Corruption_N_Required_Unit_Type (trimmed, as written)
    int32_t required_unit_count = 0;
    int8_t required_mission = kCorruptionNone;  // Corruption_N_Required_Mission_Type
    int32_t required_mission_count = 0;
    bool tactical = false;                   // Tactical_Game_Mode is Land / Space and Tactical_Map_Name is set
    ConflictKind kind = ConflictKind::Land;  // Corruption_N_Tactical_Game_Mode
    std::string map;                         // Corruption_N_Tactical_Map_Name (trimmed)
    std::string plots;                       // Corruption_N_Tactical_Script_Name (trimmed)
};

// The options of a planet type in slot order (slots without a Corruption_N_Type are skipped).
std::vector<CorruptionOption> corruption_options(const objects::GameObjectTypes& types, objects::TypeIndex planet_type);
// The first option of `type` (nullptr when the planet type offers none).
const CorruptionOption* find_corruption_option(const std::vector<CorruptionOption>& options, int8_t type);

}  // namespace alamo::galactic
