// Galactic victory monitor (mode +0x28; MAIN_LOOP T11 opens the campaign-end dialog when it reports a result),
// docs/re/STRATEGIC_GAPS.md §1.4 (OI galactic), docs/re/Q6_T15_RETAIL_RUN.md ("ELIM C", "q6 t25").
//
// VictoryType enum 0..10 (parser 0x341ff0) with the campaign fields <side>_Victory_Conditions / _Credit_Target /
// _Galactic_Control / _Cycle_Limit / _Planet_Names / _Faction_Names (xml_field_tables). The records (q6 t22, setup
// 0x289a90; GalacticSim::set_victory_rules): none for an Is_Neutral faction, the Human_ list for the
// Starting_Active_Player faction, the AI_ list plus one implicit Opponent_Controls_No_Planets (no faction filter) for
// every other player when that list is non-empty; countdown 30 frames each. Players whose faction is not Is_Playable
// never pass.
// Tests (0x344710):
//   0 All_Planets_Controlled        every planet's owner is the player or allied with it (relation self / ally)
//   1 Opponent_Controls_No_Planets  no planet is owned by a hostile player with a playable faction (a non-empty
//                                   _Faction_Names list: only those factions count, case-insensitive); a planet whose
//                                   owner and the tested player are both AI-controlled non-humans does not block (q6 t22)
//   2 Credits_Accrued               credits >= _Credit_Target
//   4 Percentage_Control            owned planets / all planets >= _Galactic_Control
//   5 Conquer_Enemy_Homeworld_And_Leader  the Primary_Enemy faction's Home_Planet owned by the player and its
//                                   Faction_Leader_Company on no planet [M: tag <-> offset by field order]
//   8 Kill_Enemy_Leader             every hostile playable player's leader (Faction_Leader_Company's first company
//                                   member) is on no planet (the dying-with-final-blow clause is tactical: not modelled)
//   9 Control_Named_Planets         the player owns every _Planet_Names planet
//   10 Cycles_Elapsed               galactic_day() > _Cycle_Limit (> 0)
// q6 t21 [H]: nothing is polled. GalacticSim runs the tests on events: 0 / 1 / 4 / 5 / 9 at a planet's owner change
// (on_owner_change 0x3fb040; never at a planet's first owner assignment), every player in player order, first pass
// wins; 10 at each fiscal-cycle (week) rollover; 8 every 30 mode frames; 2 in add_credits (0x27f370) for that player.
// A pass declares {type, player} (a VictoryClaim, countdown 30 frames); the declared claim wins once its countdown ran
// out and the monitor is not held. There is no elimination test (the former [port] EliminationLatch /
// player_eliminated are gone): a human who owns nothing goes on until an owner change lets an AI's implicit
// Opponent_Controls_No_Planets pass (planets only; units do not count).
// Event-driven (3 Super_Weapon_Destruction, 6 Super_Weapon_Destroys_Last_Enemy_Planet, 7 Super_Weapon_Kills_Enemy_
// Leader): GalacticSim::victory_claims(); such a claim counts when the player has a record of that type.
// Story rewards (story t11, GalacticSim::victory_hold / story_victory): while the monitor is held nobody wins and the
// tests are deferred (replayed at release); a story claim (type 0x10, the VICTORY reward) is always registered.
//
// Pure queries on the sim: nothing here changes or hashes state (GalacticSim's victory members live in victory.cpp).
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "alamo/galactic/campaign.hpp"
#include "alamo/galactic/victory_rules.hpp"

namespace alamo::galactic {

// From the campaign definition; planet names resolve through `map`. No campaign = empty rules (no records).
// `starting_active_faction` overrides the campaign's Starting_Active_Player (empty: keep it).
VictoryRules load_victory_rules(const data::GameDatabase& db, std::string_view campaign, const GalaxyMap& map,
                                std::vector<std::string>* diagnostics = nullptr, std::string_view starting_active_faction = {});

struct VictoryResult {
    int32_t winner = kNoPlayer;  // the declared claim's player, once its countdown ran out and the monitor is not held
    VictoryCondition how = VictoryCondition::Unknown;
    bool finished() const { return winner != kNoPlayer; }
};
VictoryResult check_victory(const GalacticSim& sim);
// One test for one player (side = the player's list) as the monitor runs it. Event-driven types (3 / 6 / 7): a claim
// of that type for the player whose countdown ran out.
bool victory_condition_met(const GalacticSim& sim, const VictorySide& side, VictoryCondition k, int32_t player);
// Host helper: +1 = `player` won, -1 = another player won, 0 = going on.
int32_t victory_outcome(const GalacticSim& sim, int32_t player);

}  // namespace alamo::galactic
