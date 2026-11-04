// q6 t03: skirmish rules shared by alamo_game (alamo_game_core, game_data.hpp) and alamo_headless's skirmish host
// (headless/skirmish_run.cpp), which does not link alamo_game_core (it pulls in UI / audio). Data in, names / types out:
// no UI, audio, session or sim code. The hosts' deliberate differences are SkirmishForceOptions fields.
//   AI type       ai_type_for / is_stock_faction / resolve_ai_type (SKIRMISH_SETUP §2.1, AI_GALACTIC §1.2).
//   forces        the faction's Space_ / Land_Skirmish_AI_Default_Forces (SKIRMISH_SETUP §2 step 4), the [port] stock
//                 lists and the [port] cheapest-buildable fallback.
//   map planet    skirmish_map_planet_type (the planet type a _MP_SPACE_ / _MP_LAND_ map stands for).
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

// [port] the stock factions' AIPlayerType as the port has always assigned it: Empire -> BasicEmpire, Rebel -> BasicRebel,
// Underworld -> AI_Player_Underworld; "" otherwise. FoC's Factions.xml / Expansion_Factions.xml Basic_AI agree.
std::string ai_type_for(const std::string& faction);
// q6 t08: a "stock" faction keeps the port's hard-coded AI type and skirmish forces: ai_type_for(faction) is set and the
// data's Basic_AI (`basic_ai`: upper-case faction name -> type, ai::load_faction_basic_ai) is absent or the same name
// (case-insensitive). A mod that redefines Empire with another Basic_AI (Thrawn's Revenge: GenericAI) is not stock.
bool is_stock_faction(const std::string& faction, const std::map<std::string, std::string>& basic_ai);
#ifdef ALAMO_GAME_RULES_HAVE_AI
// q6 t08: the AIPlayerType a non-human player of `faction` gets ("" = no AI; AI_GALACTIC §1.2):
//   stock faction (is_stock_faction)   ai_type_for(faction), unchanged. [port] FoC campaigns' AI_Player_Control pairs
//                                      are not read for them (e.g. "Empire, None" / "Rebel, BasicRebel_NoOpenings" in the
//                                      shipped campaigns, the port's sessions have always used Basic_AI).
//   no Basic_AI                        "" (as before for Pirates / Hostile / Hutts ...).
//   skirmish (campaign_control null)   the faction's Basic_AI (SKIRMISH_SETUP §2.1: AI slots get Faction.Basic_AI).
//   campaign                           ai::campaign_ai_type(*campaign_control, basic_ai, faction, false): the campaign's
//                                      AI_Player_Control pair; no pair = no AI (only the pairs call Set_AI_Type [M]).
// A type `defs` does not define is no AI (Set_AI_Type: "Unrecognized AI player type"); defs null = not checked.
std::string resolve_ai_type(const std::string& faction, const std::map<std::string, std::string>& basic_ai,
                            const std::vector<std::string>* campaign_control, const ai::AiDefinitions* defs);
#endif

// [port] default skirmish fleets / ground forces for the stock factions (Empire / Rebel / Underworld); empty for others.
std::vector<std::string> default_skirmish_fleet(const std::string& faction);
std::vector<std::string> default_skirmish_ground(const std::string& faction);

struct SkirmishForceData {
    const galactic::GalacticRules& rules;
    const objects::GameObjectTypes& types;  // Max_Speed (the fallback's space list leaves stations out)
    const SkirmishForceTable& forces;
    const std::map<std::string, std::string>& basic_ai;  // for is_stock_faction
};
struct SkirmishForceOptions {
    // alamo_game: true. alamo_headless: false (its skirmish host has always started every faction from the data's list,
    // which the checked-in skirmish goldens hold).
    bool stock_lists = true;
    // alamo_game: true ([port] it hosts a space skirmish as a galactic hop, so the fleet needs a hyperspace member).
    // alamo_headless: false (its battle starts on the map, no hop).
    bool hyperspace_leader = true;
    // Optional notes: dropped names, the fallback's picks (faction first, no host prefix).
    std::vector<std::string>* diag = nullptr;
};
// The default forces of `faction`, as type names:
//   stock faction (is_stock_faction), stock_lists   the lists above, unchanged when the data defines at least one of
//                                                   their names (names unknown to the data are kept; the galaxy skips them).
//   otherwise                                       the faction's Space_ / Land_Skirmish_AI_Default_Forces; names the data
//                                                   does not define, and types the galaxy cannot hold (space: not a
//                                                   SpaceUnit / Squadron kind; land: not a GroundUnit kind), dropped.
//                                                   [port] none left: data_fallback_forces.
//   space, hyperspace_leader, not the stock list   [port] the first hyperspace member moves to the front; without one
//                                                   the faction's cheapest buildable hyperspace ship that holds the list's
//                                                   non-hyperspace squadrons (else the cheapest) is put in front. Retail
//                                                   places these squadrons directly at the map's spawn markers.
std::vector<std::string> skirmish_default_forces(const SkirmishForceData& data, const std::string& faction, bool land,
                                                 const SkirmishForceOptions& options = {});
// The data path above (never the stock lists, options.stock_lists is not read) as type indices of `data.rules`.
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

// [port] (no RE rule) the planet type a skirmish map stands for: the name after "_MP_SPACE_" / "_MP_LAND_" (path and
// extension dropped, case ignored) when it names a PLANET type, e.g. _MP_LAND_TATOOINE -> "Tatooine"; "" otherwise.
std::string skirmish_map_planet_type(const galactic::GalacticRules& rules, std::string_view map);

}  // namespace alamo::game
