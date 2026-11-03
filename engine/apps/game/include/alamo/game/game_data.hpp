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
// [port] AIPlayerType per faction (the campaign's AI_Player_Control pairs are not carried by GalacticSetup):
// Empire -> BasicEmpire, Rebel -> BasicRebel, Underworld -> AI_Player_Underworld; "" otherwise.
std::string ai_type_for(const std::string& faction);

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
// neutral faction. `units_human` / `units_ai` = type names of the starting fleets (unknown names are skipped);
// empty = the faction's default fleet (default_skirmish_fleet). `planet_type` = a planet type name ("" = the first of
// Kuat / Coruscant / any PLANET type). False + err when a faction or every unit is unknown.
// Land skirmish (land = true): the AI's ground forces hold planet 1's surface (no space units, so no space conflict);
// the human's ground forces ride a fleet (one space unit + the ground units launched at session start) that hops to
// planet 1 and invades on arrival (the session submits the Invade order): the land battle on the chosen land map.
struct SkirmishSetup {
    std::string human_faction = "Rebel", ai_faction = "Empire";
    std::vector<std::string> units_human, units_ai;    // space fleets ("" = default_skirmish_fleet)
    bool land = false;
    // Land: ground forces ("" = default_skirmish_ground). Space (P4 host, tests): a non-empty ground_human stands on the
    // human's planet surface (FleetManagement launch / land); empty = none, as before.
    std::vector<std::string> ground_human, ground_ai;
    std::string planet_type;
    // p4uia t02: the colour index (0..8, GameConstants MP_Color_* entry) of player 0 (human) and 1 (AI) chosen on the
    // staging screen; players without an entry keep their index. Presentation only: the setup and the sim ignore it.
    std::vector<uint8_t> colors;
};
bool make_skirmish_galaxy(const GameData& data, const SkirmishSetup& s, galactic::GalacticSetup& out, std::string* err = nullptr);
// [port] default skirmish fleets for the stock factions (Empire / Rebel / Underworld); empty for others.
std::vector<std::string> default_skirmish_fleet(const std::string& faction);
std::vector<std::string> default_skirmish_ground(const std::string& faction);
inline constexpr PlanetIndex kSkirmishHumanPlanet = 2, kSkirmishBattlePlanet = 1;

// Space skirmish maps of the data (Data\Art\Maps\_MP_SPACE_*.ted through the VFS), sorted, "_MP_SPACE_KUAT" first
// when present (the A1 skirmish golden's map). Empty without a VFS.
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
    // maps run 0..width / 0..height from the lower left corner, space maps are centred on the origin [M]; world +Y is up.
    std::vector<std::pair<float, float>> starts;
};
SkirmishMapPreview read_skirmish_map_preview(const FileProvider& files, std::string_view map, bool space);

// [port] (no RE rule) the planet type a skirmish map stands for: the name after "_MP_SPACE_" / "_MP_LAND_" (path and
// extension dropped, case ignored) when it names a PLANET type, e.g. _MP_LAND_TATOOINE -> "Tatooine"; "" otherwise.
// Presentation only (load screen portrait, tactical HUD planet name): the skirmish galaxy's planets keep the
// Kuat / Coruscant type, because a map's planet type may forbid the setup (Alderaan: Planet_Surface_Accessible No).
std::string skirmish_map_planet_type(const galactic::GalacticRules& rules, std::string_view map);

}  // namespace alamo::game
