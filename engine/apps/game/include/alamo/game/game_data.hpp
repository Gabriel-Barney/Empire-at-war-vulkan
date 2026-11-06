// P host (alamo_game): everything the game session needs from the data files, loaded once per process.
//   GameData       VFS (or a directory provider), GameDatabase, object types, galactic rules, conflict rules and the
//                  S10 battle catalog - exactly what alamo_headless --battles loads, in the same order.
//   skirmish       a skirmish is hosted as a three-planet "galaxy" (alamo_headless --skirmish-ai's layout): the human
//                  player's fleet at planet 2, the AI's fleet holding planet 1; the human fleet's opening hop to
//                  planet 1 (a command in the stream) starts the space conflict whose tactical battle is the skirmish,
//                  on the map picked in the menu (the map loader ignores the conflict's own map choice).
// Render-free (no GPU): used by alamo_game and its headless tests / replays.
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "alamo/core/file_provider.hpp"
#include "alamo/data/game_database.hpp"
#include "alamo/galactic/campaign.hpp"
#include "alamo/galactic/conflicts.hpp"
#include "alamo/galactic/galactic_rules.hpp"
#include "alamo/game/skirmish_rules.hpp"  // q6 t03: AI types, default forces, map planet type (shared with alamo_headless)
#include "alamo/objects/game_object_types.hpp"
#include "alamo/objects/object_types.hpp"
#include "alamo/sim/battle_catalog.hpp"

#ifdef ALAMO_GAME_HAVE_GALACTIC_AI
#include "alamo/ai/ai_definitions.hpp"
#include "alamo/ai/galactic_ai.hpp"
#include "alamo/ai/perception_equations.hpp"
#endif
#ifdef ALAMO_SIM_HAVE_TACTICAL_AI
#include "alamo/sim/battle_ai.hpp"
#endif

namespace alamo::vfs {
class Vfs;
}

namespace alamo::game {

// AI data shared by the galactic AI (A2) and the tactical AI (A1): definitions, perception equations, plan
// scripts, difficulty adjustments, per-type AI facts. Loaded once with the game data (VFS only).
struct AiData;
#ifdef ALAMO_GAME_HAVE_GALACTIC_AI
struct AiData {
    ai::AiDefinitions defs;
    ai::EquationSet equations;
    std::vector<std::string> plan_files;                 // galactic plan scripts (ai::list_plan_files)
    std::vector<ai::GalacticAiDifficulty> difficulties;
    std::vector<ai::GalacticAiTypeInfo> type_info;
#ifdef ALAMO_SIM_HAVE_TACTICAL_AI
    // A1: the tactical AI of AI players in battles (plans per mode, difficulties); setup points at defs / equations.
    std::shared_ptr<sim::TacticalAiSetup> tactical;
#endif
};
#endif
// resolve_ai_type: alamo/game/skirmish_rules.hpp (q6 t03, modc t03).

struct GameData {
    std::string root;                      // game folder (FoC "corruption") or synthetic data folder
    std::unique_ptr<FileProvider> files;   // VFS for real data, DirectoryProvider for synthetic data
    vfs::Vfs* vfs = nullptr;               // = files when it is a VFS (audio archives, map lists), else null
    data::GameDatabase db;
    objects::ObjectTypeDatabase raw;
    objects::GameObjectTypes types;
    galactic::GalacticRules rules;
    galactic::ConflictRules crules;
    std::unique_ptr<sim::BattleCatalog> catalog;
    std::shared_ptr<AiData> ai;  // null: no AI data / built without engine/ai galactic AI
    // q6 t08: Faction.Basic_AI (ai::load_faction_basic_ai: upper-case faction name -> AIPlayerType; empty without engine/ai).
    std::map<std::string, std::string> basic_ai;
    // q6 t08: Faction Space_ / Land_Skirmish_AI_Default_Forces (SKIRMISH_SETUP §2 step 4) as written, per upper-case faction
    // name (FactionFiles.xml order; the last definition with a list wins, as alamo_headless's skirmish host).
    using SkirmishForces = game::SkirmishForces;  // skirmish_rules.hpp load_skirmish_forces
    SkirmishForceTable skirmish_forces;
    std::vector<std::string> diagnostics;
    // Q5: one record per loader step, in load order; the step's diagnostics are diagnostics[first_diag, + diag_count).
    // ms is wall clock (reports only, never read by the sim).
    struct LoadStage {
        std::string name;
        size_t first_diag = 0, diag_count = 0;
        double ms = 0;
        bool ok = true;
    };
    std::vector<LoadStage> stages;
    std::vector<std::string> args;  // N1 / t09: the VFS command line the data was loaded with (save descriptors)
    // modc t03: a MODPATH= / STEAMMOD= layer is mounted (Vfs::mod_dirs non-empty). Keys the [port] rules that apply only
    // to mods (docs/MODS.md §6): campaign AI_Player_Control pairs, no base-game skirmish lists. Synthetic data: false
    // (tests set it to stand for a mod).
    bool mod_layer = false;
    // N1 / t09: sim::fingerprint_types of `types`, computed on first use (saves / loads only; one process-wide cache).
    uint64_t types_fingerprint() const;

private:
    mutable uint64_t types_fp_ = 0;
    mutable bool types_fp_done_ = false;
};

// Real game data under `root` (the FoC folder; MODPATH= / FALLBACKPATH= style `args` as alamo_headless). False + err.
bool load_game_data(const std::string& root, const std::vector<std::string>& args, GameData& out, std::string* err = nullptr);
// Any folder with Data\XML (tests: synthetic data written by the test). False + err.
bool load_game_data_dir(const std::string& dir, GameData& out, std::string* err = nullptr);

// Skirmish galaxy (see the file comment). Players: 0 = human (`human_faction`), 1 = AI (`ai_faction`), 2 = the first
// neutral faction, then the native-owner faction (GalacticRules::native_owner_faction). An empty faction name is the
// default slot faction (default_skirmish_faction). `units_human` / `units_ai` = type names of the starting fleets (unknown names are skipped);
// empty = the faction's default fleet (default_skirmish_fleet). `planet_type` = a planet type name ("" = the first of
// Kuat / Coruscant / any PLANET type). False + err when a faction or every unit is unknown.
// Land skirmish (land = true): the AI's ground forces hold planet 1's surface (no space units, so no space conflict);
// the human's ground forces ride a fleet (one space unit + the ground units launched at session start) that hops to
// planet 1 and invades on arrival (the session submits the Invade order): the land battle on the chosen land map.
struct SkirmishSetup {
    std::string human_faction, ai_faction;  // "" = default_skirmish_faction (modc t03: from the data, not by name)
    std::vector<std::string> units_human, units_ai;    // space fleets ("" = default_skirmish_fleet)
    bool land = false;
    // Land: ground forces ("" = default_skirmish_ground). Space (P4 host, tests): a non-empty ground_human stands on the
    // human's planet surface (FleetManagement launch / land); empty = none, as before.
    std::vector<std::string> ground_human, ground_ai;
    std::string planet_type;
    // p4uia t02: the colour index (0..8, GameConstants MP_Color_* entry) of player 0 (human) and 1 (AI) chosen on the
    // staging screen; players without an entry keep their index. Presentation only: the setup and the sim ignore it.
    std::vector<uint8_t> colors;
    // q6 t02 [port]: no starting forces and no "unknown unit" failure - the attract demo clears the forces anyway (every unit
    // comes from its map), and a mod's Underworld may have no default fleet (Thrawn's Revenge). Not saved (the demo is never).
    bool no_forces = false;
};
bool make_skirmish_galaxy(const GameData& data, const SkirmishSetup& s, galactic::GalacticSetup& out, std::string* err = nullptr);
// modc t03 [port] (retail's lobby always names both slots): the faction of an unnamed skirmish slot. Human slot: the first
// Is_Playable faction in FactionFiles.xml order; AI slot: the first Is_Playable faction other than `human` (FoC: Rebel,
// Empire, the port's old built-in defaults). "" when the data has none.
std::string default_skirmish_faction(const galactic::GalacticRules& rules, bool ai_slot, std::string_view human = {});
// `s` with its empty slot factions replaced by default_skirmish_faction.
SkirmishSetup resolve_skirmish_factions(const galactic::GalacticRules& rules, SkirmishSetup s);
// q6 t08: the default forces make_skirmish_galaxy uses, as type names the data defines (skirmish_rules.hpp
// skirmish_default_forces with the alamo_game options: base-game lists unless data.mod_layer, hyperspace leader):
//   base-game list, no mod layer      default_skirmish_fleet / _ground(faction), unchanged (names unknown to the data are
//                                     kept; the galaxy skips them). [port] MODS.md §6; replacement modc t17.
//   other factions                    the faction's Space_ / Land_Skirmish_AI_Default_Forces (SKIRMISH_SETUP §2 step 4),
//                                     names the data does not define, and types the galaxy cannot hold (space: not a
//                                     SpaceUnit / Squadron kind; land: not a GroundUnit kind), dropped. [port] none left: up
//                                     to three of the faction's buildable units (data_fallback_forces).
//   space, other factions             [port] the port hosts a skirmish as a galactic hop, so the fleet needs a hyperspace
//                                     member: the first one moves to the front; without one the faction's cheapest buildable
//                                     hyperspace ship that holds the list's non-hyperspace squadrons (else the cheapest) is
//                                     put in front. Retail places these squadrons directly at the map's spawn markers.
std::vector<std::string> default_skirmish_fleet(const GameData& data, const std::string& faction);
std::vector<std::string> default_skirmish_ground(const GameData& data, const std::string& faction);
// q6 t08 [port] (no RE rule: retail places nothing without a default force list): the faction's buildable units of one
// mode - land: GroundUnit types, space: hyperspace SpaceUnit types with Max_Speed > 0 (no stations) - with
// Build_Cost_Credits > 0, Tech_Level >= 1, Affiliation naming the faction, no Required_Planets, no build limit of 0, not a
// hero, not cinematic-only, not a company / squadron member and not Build_Initially_Locked; ordered by (build cost, name).
// Returns the two cheapest and the median one (distinct, in that order), fewer when there are fewer.
std::vector<std::string> data_fallback_forces(const GameData& data, const std::string& faction, bool land);
inline constexpr PlanetIndex kSkirmishHumanPlanet = 2, kSkirmishBattlePlanet = 1;

// Space skirmish maps of the data (Data\Art\Maps\_MP_SPACE_*.ted through the VFS), sorted, "_MP_SPACE_KUAT" first
// when present (the A1 skirmish golden's map). Empty without a VFS. [port] Both lists leave out a map whose loose file (through
// the normal lookup) is empty or cannot be sized (Thrawn's Revenge ships a 0-byte _MP_SPACE_KUAT.ted); MEG entries are kept,
// so vanilla lists are unchanged.
std::vector<std::string> skirmish_space_maps(const GameData& data);
// Land skirmish maps (Data\Art\Maps\_MP_LAND_*.ted), sorted. Empty without a VFS.
std::vector<std::string> skirmish_land_maps(const GameData& data);
// ---- Skirmish map catalog (docs/re/UI.md section 9, roadmap p4ui t06) --------------------------------------------------
// The menu's map table: three scans over the layers, each map's header (top chunk 0 of the .ted) read through the normal
// VFS lookup (mod > FoC loose > MEGs > fallbacks, so a mod's copy of an official map name decides its header).
//   official      Data\Art\Maps\*.TED   from the FoC-loose and MEG layers; every entry kept, readable or not
//   custom scan   Data\CustomMaps\*.TED from the FoC-loose and MEG layers, plus Data\Art\Maps\*.TED from the mod layer;
//                 an entry is kept only when its custom flag (mini 0B) is 1
// Entries are keyed by the logical path (one entry per path per list; the same path can sit in both lists), in path order.
struct SkirmishMapEntry {
    std::string path;      // normalised logical path, e.g. "DATA\ART\MAPS\_MP_LAND_ENDOR.TED"
    std::string map;       // map id for SessionSpec::skirmish_map / terrain::map_path: the file stem when the file is directly
                           // under DATA\ART\MAPS\ ("_MP_LAND_ENDOR"), else the full logical path
    uint32_t kind = 0;     // mini 01: 1 land, 2 space (0 = absent or unreadable)
    uint32_t players = 1;  // mini 02, 1 when absent
    bool custom = false;   // mini 0B == 1: the Custom tab shows it, else the Official tab
    std::string name_key;  // mini 08 (UTF-8 text key), "" when absent
    bool readable = false; // the file was read and its map-info chunk decoded
};
struct SkirmishMapCatalog {
    std::vector<SkirmishMapEntry> official;
    std::vector<SkirmishMapEntry> custom_scan;  // kept entries only (custom flag 1)
};
// Builds the catalog (reads every candidate's header once). Empty over a null VFS / GameData without one.
SkirmishMapCatalog build_skirmish_map_catalog(const vfs::Vfs& vfs);
SkirmishMapCatalog build_skirmish_map_catalog(const GameData& data);
// The skirmish list of one tab: entries of `official` then `custom_scan` with kind 1 or 2, players >= 2 and
// custom == `custom_tab` (UI.md 9.4); a logical path listed in both lists appears once.
std::vector<SkirmishMapEntry> skirmish_map_rows(const SkirmishMapCatalog& catalog, bool custom_tab);
// The entry for a map id (a stem, a file name or a path: the terrain::map_path rule, case-insensitive), nullptr when absent.
const SkirmishMapEntry* find_skirmish_map(const SkirmishMapCatalog& catalog, std::string_view map);

// p4uim t03 (UI.md 9.3 / 9.6): what the staging preview needs from a map file, read through the normal VFS lookup.
struct SkirmishMapPreview {
    bool found = false;                    // the file was read
    std::vector<uint8_t> embedded_image;   // top chunk 0x13 (a TGA file), empty = none
    // Top chunk 3 start positions as fractions of the map (x right, y down), only when the header's mini 12 is set. Land
    // maps run 0..width / 0..height from the lower left corner, space maps are centred on the origin; world +Y is up.
    std::vector<std::pair<float, float>> starts;
};
SkirmishMapPreview read_skirmish_map_preview(const FileProvider& files, std::string_view map, bool space);

// skirmish_map_planet_type (skirmish_rules.hpp): presentation only here (load screen portrait, tactical HUD planet name):
// the skirmish galaxy's planets keep the Kuat / Coruscant type, because a map's planet type may forbid the setup (Alderaan:
// Planet_Surface_Accessible No).

}  // namespace alamo::game
