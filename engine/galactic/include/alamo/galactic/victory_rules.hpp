// Galactic victory conditions of a campaign (data only; the monitor is victory.hpp). Split from victory.hpp so that
// GalacticSetup / GalacticSim (campaign.hpp) can hold the rules and run the event-driven tests themselves (q6 t21).
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "alamo/core/galaxy.hpp"

namespace alamo::galactic {

// Values = the original VictoryType enum.
enum class VictoryCondition : uint8_t {
    AllPlanetsControlled = 0, OpponentControlsNoPlanets = 1, CreditsAccrued = 2, SuperWeaponDestruction = 3,
    PercentageControl = 4, ConquerEnemyHomeworldAndLeader = 5, SuperWeaponDestroysLastEnemyPlanet = 6,
    SuperWeaponKillsEnemyLeader = 7, KillEnemyLeader = 8, ControlNamedPlanets = 9, CyclesElapsed = 10,
    StoryTriggered = 0x10,  // SUB_TACTICAL_STORY_MODE_TRIGGERED_VICTORY: the story VICTORY reward (never in a side's list)
    Unknown = 0xFF
};
VictoryCondition parse_victory_condition(std::string_view name);

// Every galactic victory record has countdown 30 frames (setup 0x289a90 writes 0x1E; BKC1_RE_ANSWERS Q3) [H].
inline constexpr int32_t kGalacticVictoryCountdown = 30;

struct VictorySide {
    std::vector<VictoryCondition> conditions;  // document order
    std::vector<PlanetIndex> planets;          // <side>_Victory_Planet_Names (rows found in the map)
    int32_t cycle_limit = 0;                   // <side>_Victory_Cycle_Limit, 0 = none
    int32_t credit_target = 0;                 // <side>_Victory_Credit_Target (+0xC)
    float galactic_control = 0.0f;             // <side>_Victory_Galactic_Control (+0x10)
    std::vector<std::string> faction_names;    // <side>_Victory_Faction_Names (type 1 filter)
};
struct VictoryRules {
    VictorySide human, ai;
    // q6 t22: the faction that gets the Human_ list (campaign Starting_Active_Player, or the faction the player picked
    // when the host overrides it). Empty: the players marked human get it [port: hand-built setups].
    std::string starting_active_faction;
};

// One condition record of one player (monitor vector +0x08, 0x30 bytes; built by GalacticSim::set_victory_rules from
// the rules and the player list, so not saved). `implicit`: the extra Opponent_Controls_No_Planets every AI-list player
// gets (no faction filter).
struct VictoryRecord {
    int32_t player = -1;
    VictoryCondition type = VictoryCondition::Unknown;
    int32_t countdown = kGalacticVictoryCountdown;
    bool human_side = false;  // tested with VictoryRules::human (else ::ai)
    bool implicit = false;
};

}  // namespace alamo::galactic
