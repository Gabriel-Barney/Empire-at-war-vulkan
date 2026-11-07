// q6 t03: skirmish rules shared by alamo_game (alamo_game_core, game_data.hpp) and alamo_headless's skirmish host
// (headless/skirmish_run.cpp), which does not link alamo_game_core (it pulls in UI / audio). Data in, names / types out:
// no UI, audio, session or sim code. The hosts' deliberate differences are SkirmishForceOptions fields.
//   AI type       resolve_ai_type (SKIRMISH_SETUP §2.1, AI_GALACTIC §1.2): the faction's Basic_AI / campaign pairs.
//   forces        the faction's Space_ / Land_Skirmish_AI_Default_Forces (SKIRMISH_SETUP §2 step 4) and the [port]
//                 cheapest-buildable fallback, for base FoC and mods alike (modc t17).
// modc t03 / t17 / t19 (docs/MODS.md §1 rule 4): nothing here tests a faction, unit, planet or map name to pick a rule.
//   map planet    skirmish_map_planet_type (the planet type a _MP_SPACE_ / _MP_LAND_ map stands for) and
//                 skirmish_galaxy_planet (the planet type of the port's skirmish galaxy).
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace alamo {
class FileProvider;
}
namespace alamo::galactic {
struct GalacticRules;
}
namespace alamo::objects {
class GameObjectTypes;
}
#ifdef ALAMO_GAME_RULES_HAVE_AI
namespace alamo::ai {
class AiDefinitions;
}
#endif

namespace alamo::game {

// Faction Space_ / Land_Skirmish_AI_Default_Forces as written, per upper-case trimmed faction name (FactionFiles.xml
// order; per list, the last definition that has one wins).
struct SkirmishForces {
    std::vector<std::string> space, land;
};
using SkirmishForceTable = std::map<std::string, SkirmishForces>;
SkirmishForceTable load_skirmish_forces(const FileProvider& files);

#ifdef ALAMO_GAME_RULES_HAVE_AI
// The AIPlayerType a non-human player of `faction` gets ("" = no AI; AI_GALACTIC §1.2). `basic_ai`: upper-case faction
// name -> Basic_AI (ai::load_faction_basic_ai).
//   no Basic_AI                        "" (FoC: Pirates / Hostile / Hutts ...).
//   skirmish (campaign_control null)   the faction's Basic_AI (SKIRMISH_SETUP §2.1: AI slots get Faction.Basic_AI).
//   campaign (campaign_control set)    ai::campaign_ai_type(*campaign_control, basic_ai, faction, false): the campaign's
//                                      AI_Player_Control pair (AI_GALACTIC §1.2); no pair = no AI (only the pairs call
//                                      Set_AI_Type [M]). Base FoC and mods alike (modc t17: FoC's "Empire, None" /
//                                      "Rebel, BasicRebel_NoOpenings" ... are read as written).
// "Human" = no AI. A type `defs` does not define is no AI (Set_AI_Type: "Unrecognized AI player type"); defs null = not
// checked.
std::string resolve_ai_type(const std::string& faction, const std::map<std::string, std::string>& basic_ai,
                            const std::vector<std::string>* campaign_control, const ai::AiDefinitions* defs);
#endif

struct SkirmishForceData {
    const galactic::GalacticRules& rules;
    const objects::GameObjectTypes& types;  // Max_Speed (the fallback's space list leaves stations out)
    const SkirmishForceTable& forces;
};
struct SkirmishForceOptions {
    // alamo_game: true ([port] it hosts a space skirmish as a galactic hop, so the fleet needs a hyperspace member).
    // alamo_headless: false (its battle starts on the map, no hop).
    bool hyperspace_leader = true;
    // Optional notes: dropped names, the fallback's picks (faction first, no host prefix).
    std::vector<std::string>* diag = nullptr;
};
// The default forces of `faction`, as type names (modc t17: every faction alike, base FoC included):
//   the list                                        the faction's Space_ / Land_Skirmish_AI_Default_Forces; names the data
//                                                   does not define, and types the galaxy cannot hold (space: not a
//                                                   SpaceUnit / Squadron kind; land: not a GroundUnit kind), dropped.
//                                                   [port] none left: data_fallback_forces.
//   space, hyperspace_leader, not a list above     [port] the first hyperspace member moves to the front; without one
//                                                   the faction's cheapest buildable hyperspace ship that holds the list's
//                                                   non-hyperspace squadrons (else the cheapest) is put in front. Retail
//                                                   places these squadrons directly at the map's spawn markers.
std::vector<std::string> skirmish_default_forces(const SkirmishForceData& data, const std::string& faction, bool land,
                                                 const SkirmishForceOptions& options = {});
// The same forces as type indices of `data.rules`.
std::vector<uint32_t> skirmish_data_force_types(const SkirmishForceData& data, const std::string& faction, bool land,
                                                const SkirmishForceOptions& options = {});
// [port] the hyperspace leader of the galactic-hop arrangement for a space fleet of `faction` (type indices of `rules`):
// kNoIndex when `fleet` is empty or already has a hyperspace member (SpaceUnit / Squadron with Hyperspace and a speed);
// else the faction's cheapest buildable hyperspace ship that holds the fleet's squadrons, else its cheapest one (the
// same pick as skirmish_data_force_types), kNoIndex when the faction has none. q6: alamo_game also leads a bought fleet
// with it (make_skirmish_galaxy), e.g. Thrawn's Revenge's buy list offers fighters without Hyperspace (Nantex).
uint32_t skirmish_hyperspace_leader(const galactic::GalacticRules& rules, const objects::GameObjectTypes& types, uint32_t faction,
                                    const std::vector<uint32_t>& fleet);
// q6 t08 [port] (no RE rule: retail places nothing without a default force list): the faction's buildable units of one
// mode - land: GroundUnit types, space: hyperspace SpaceUnit types with Max_Speed > 0 (no stations) - with
// Build_Cost_Credits > 0, Tech_Level >= 1, Affiliation naming the faction, no Required_Planets, no build limit of 0, not a
// hero, not cinematic-only, not a company / squadron member and not Build_Initially_Locked; ordered by (build cost, name).
// Returns the two cheapest and the median one (distinct, in that order), fewer when there are fewer.
std::vector<std::string> data_fallback_forces(const galactic::GalacticRules& rules, const objects::GameObjectTypes& types,
                                              const std::string& faction, bool land);

// q6: the buy-units list of IDD_SKIRMISH_BUY_UNITS_DIALOG (SKIRMISH_SETUP §2.7, candidate list 0xf0600 + rows 0xef9f0), as
// type indices of `rules` in display order. Every type in type-manager order (type index 0..N-1) that has
//   1. Build_Cost_Credits > 0,
//   2. `faction` in its Affiliation (an empty list never matches),
//   3. Available_In_Skirmish, OR the map kind's build tab (Build_Tab_Land_Units on land, Build_Tab_Space_Units in space),
//   4. no Cinematic_Object_Only,
// and, for the rows, no Is_Named_Hero unless `heroes` (the setup's heroes option bit). Nothing else is tested (tech level,
// locks, prerequisites, build limits, unit cap, population, behaviours, class). No cap. Order: Build_Cost_Credits
// descending (the listbox comparator 0xf0e80); the original's ties are in no defined order: [port] type index ascending.
// `multiplayer_list_only`: [port interim rule] (SKIRMISH_SETUP §2.7 port notes) also require that the type is named in
// some type's Tactical_Buildable_Objects_Multiplayer list, which drops the option / cheat / layer / ground-war marker
// types a mod lists (Thrawn's Revenge Empire space 407 -> 106 rows, land 63 -> 38). alamo_game turns it on only when a mod
// layer is loaded (Gabriel 2026-11-04: mod-only port rules keep the vanilla behaviour).
struct SkirmishBuyOptions {
    bool heroes = false;
    bool multiplayer_list_only = false;
};
std::vector<uint32_t> skirmish_buy_list(const galactic::GalacticRules& rules, const objects::GameObjectTypes& types, uint32_t faction,
                                        bool land, const SkirmishBuyOptions& options = {});

// [port] (no RE rule) the planet type a skirmish map stands for: the name after "_MP_SPACE_" / "_MP_LAND_" (path and
// extension dropped, case ignored) when it names a PLANET type, e.g. _MP_LAND_TATOOINE -> "Tatooine"; "" otherwise.
std::string skirmish_map_planet_type(const galactic::GalacticRules& rules, std::string_view map);
// modc t19 [port] (retail has no skirmish galaxy: it loads the map directly; the port hosts a skirmish as a galactic hop
// onto a planet of this type). The planet type of the skirmish galaxy, as a type index of `rules`, from the data only:
//   1. the map's planet type (skirmish_map_planet_type) when it is usable,
//   2. else the first usable PLANET type in type order,
//   3. else the first PLANET type; kNoIndex when the data has none.
// Usable: a Terrain (FoC's Galaxy_Core_Art_Model, the galaxy backdrop, is a planet type without one), no
// Planet_Restricted_Unit_Categories / _Types (a planet that forbids units may refuse the starting forces) and, for a land
// skirmish, Planet_Surface_Accessible. `map` may be "" (rule 2 then).
uint32_t skirmish_galaxy_planet(const galactic::GalacticRules& rules, std::string_view map, bool land);

}  // namespace alamo::game
