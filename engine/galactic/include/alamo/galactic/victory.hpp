// Galactic victory monitor (mode +0x28; MAIN_LOOP T11 opens the campaign-end dialog when it reports a result),
// docs/re/STRATEGIC_GAPS.md §1.4 (OI galactic).
//
// VictoryType enum 0..10 (parser 0x341ff0) with the campaign fields <side>_Victory_Conditions / _Credit_Target /
// _Galactic_Control / _Cycle_Limit / _Planet_Names / _Faction_Names (xml_field_tables). Human players use the Human_
// side, AI players the AI_ side. Players whose faction is not Is_Playable never win.
// Polled tests (0x344710, every check):
//   0 All_Planets_Controlled        every planet's owner is the player or allied with it (relation self / ally)
//   1 Opponent_Controls_No_Planets  no planet is owned by a hostile player with a playable faction (a non-empty
//                                   _Faction_Names list: only those factions count, case-insensitive)
//   2 Credits_Accrued               credits >= _Credit_Target
//   4 Percentage_Control            owned planets / all planets >= _Galactic_Control
//   5 Conquer_Enemy_Homeworld_And_Leader  the Primary_Enemy faction's Home_Planet owned by the player and its
//                                   Faction_Leader_Company on no planet [M: tag <-> offset by field order]
//   8 Kill_Enemy_Leader             every hostile playable player's leader (Faction_Leader_Company's first company
//                                   member) is on no planet (the dying-with-final-blow clause is tactical: not modelled)
//   9 Control_Named_Planets         the player owns every _Planet_Names planet
//   10 Cycles_Elapsed               galactic_day() > _Cycle_Limit (> 0)
// Event-driven (3 Super_Weapon_Destruction, 6 Super_Weapon_Destroys_Last_Enemy_Planet, 7 Super_Weapon_Kills_Enemy_
// Leader): GalacticSim::victory_claims(); a claim wins for its player when the player's side lists the type and the
// countdown (mode frames) has run out.
// Story rewards (story t11, GalacticSim::victory_hold / story_victory): while the monitor is held nobody wins; a story
// claim (type 0x10, the VICTORY reward) is a declaration: the first one in claim order wins for its player (playable or
// not) once its countdown ran out, and until then no other condition can win.
// [port] a playable player is eliminated when it owns no planet and no live unit (fleet member, surface unit,
// structure, starbase); elimination is reported even while the monitor is held.
//
// Pure queries on the sim: nothing here changes or hashes state.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "alamo/galactic/campaign.hpp"

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
};
// From the campaign definition; planet names resolve through `map`. No campaign = empty rules (never fires).
VictoryRules load_victory_rules(const data::GameDatabase& db, std::string_view campaign, const GalaxyMap& map,
                                std::vector<std::string>* diagnostics = nullptr);

struct VictoryResult {
    int32_t winner = kNoPlayer;                 // first player (player order) meeting one of its side's conditions
    VictoryCondition how = VictoryCondition::Unknown;
    std::vector<int32_t> eliminated;            // playable players with nothing left, player order
    bool finished() const { return winner != kNoPlayer; }
};
VictoryResult check_victory(const GalacticSim& sim, const VictoryRules& rules);
bool player_eliminated(const GalacticSim& sim, int32_t player);
// One test for one player (side = the player's list), polled or event-driven.
bool victory_condition_met(const GalacticSim& sim, const VictorySide& side, VictoryCondition k, int32_t player);
// Host helper: +1 = `player` won, -1 = another player won or `player` is eliminated, 0 = going on.
int32_t victory_outcome(const GalacticSim& sim, const VictoryRules& rules, int32_t player);

}  // namespace alamo::galactic
