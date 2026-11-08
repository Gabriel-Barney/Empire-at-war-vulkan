// L1 systems (owner: tactical-systems agent): the faction / GameConstants loaders of the tactical systems, defined in
// tactical_battle_l1.cpp and tactical_battle_l1_strikes.cpp (target alamo_sim_battle). Split out of campaign_battles.hpp
// (WF1b t01) so those battle sources no longer include the campaign layer; campaign_battles.hpp includes this header.
// cu5 t01: the faction loaders read the definitions `rules` holds (FactionRules::xml_fields, list order, a later
// definition's non-empty tag wins), not FactionFiles.xml again.
#pragma once

#include <cstdint>
#include <vector>

#include "alamo/data/game_database.hpp"
#include "alamo/galactic/galactic_rules.hpp"
#include "alamo/sim/tactical_battle.hpp"

namespace alamo::sim {

// Space_Retreat_Unit_Increment_Wait_Frames of every faction (by GalacticRules faction index; unset = 0) for
// CampaignBattleOptions::retreat_increment_frames.
std::vector<int32_t> load_retreat_increments(const galactic::GalacticRules& rules);
std::vector<float> load_retreat_flight_increments(const galactic::GalacticRules& rules);  // unset = 500 [port]
TacticalBattle::L1Constants load_l1_constants(const data::GameConstants& constants);  // missing tags keep the defaults
std::vector<TacticalBattle::L1Faction> load_l1_factions(const galactic::GalacticRules& rules);

}  // namespace alamo::sim
