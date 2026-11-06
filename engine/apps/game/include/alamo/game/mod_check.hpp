// Q5 mod diagnostics: what the port makes of a mod's data. Loads the data exactly as alamo_game does (load_game_data with
// the MODPATH= / STEAMMOD= / FALLBACKPATH= arguments) and reports, without running a session:
//   files      the VFS layers and warnings, and per Data\<folder> of the mod dirs how many files are new and how many
//              replace a base / MEG / fallback file
//   loaders    every load_game_data step (GameData::stages) with its time, result and diagnostics; an exception thrown by
//              a step is caught and reported as a failed step
//   content    counts the session will see (campaigns, planets, object types, factions, AI plans, skirmish maps)
//   campaigns  build_galaxy_map of every campaign: the campaigns with no planets or with skipped locations / routes
//   lua        every Data\Scripts\**\*.lua compiled (syntax only, not run) by the engine's Lua 5.0 VM
// It does not run a session: Lua run-time gaps (a missing engine function, a script the port never starts) show up only in
// alamo_headless --story / --galactic-ai runs.
// Used by alamo_mod_check (tools/mod_check_main.cpp) and its tests. Render-free.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "alamo/game/game_data.hpp"

namespace alamo::game {

struct ModCheckOptions {
    bool campaigns = true;  // build every campaign's galaxy map
    bool lua = true;        // compile every script
};

// Files of the mod dirs under one Data\<folder> (the first folder below Data; "" for files directly in Data).
struct ModFolderStats {
    std::string folder;      // upper case, e.g. "XML", "SCRIPTS", "ART"
    uint32_t added = 0;      // no lower layer (FoC loose, MEGs, fallbacks) has the path
    uint32_t overrides = 0;  // a lower layer has the path: the mod's file wins
};

struct ModCheckReport {
    bool loaded = false;  // load_game_data succeeded and no step threw
    std::string error;    // why not
    std::string vfs_layers;                  // Vfs::describe()
    std::vector<std::string> vfs_warnings;   // Vfs::warnings()
    std::vector<std::string> mod_dirs;       // effective mod search list
    std::vector<ModFolderStats> mod_folders; // sorted by folder
    std::vector<GameData::LoadStage> stages;
    std::vector<std::string> diagnostics;    // GameData::diagnostics (stages index into it)
    struct Counts {
        size_t campaigns = 0, planets = 0, trade_routes = 0, object_types = 0, factions = 0;
        size_t ai_plans = 0, tactical_space_plans = 0, tactical_land_plans = 0;
        size_t official_maps = 0, custom_maps = 0;
        bool ai = false;  // AI data loaded
    } counts;
    size_t campaigns_checked = 0;
    std::vector<std::string> campaigns_without_planets;
    std::vector<std::string> campaign_warnings;  // build_galaxy_map diagnostics, one line per message with its campaigns
    size_t campaign_warning_count = 0;           // the diagnostics behind campaign_warnings (per campaign)
    size_t lua_scripts = 0;
    std::vector<std::string> lua_errors;         // "<path>: <error>"
    std::vector<std::string> check_errors;       // a check above that threw: "<check>: exception: <what>"

    // Errors: the load failed or a step threw / failed, a campaign has no planets, a check threw. Warnings: loader and
    // VFS diagnostics, skipped campaign rows, scripts that do not compile (the original loads with all of these; its Lua 5.0
    // rejects the same scripts, and mods ship unused ones, e.g. Thrawn's Revenge's Lua 5.1 unit-test specs).
    size_t error_count() const;
    size_t warning_count() const;
};

ModCheckReport check_mod_data(const std::string& root, const std::vector<std::string>& args, const ModCheckOptions& options = {});
// Human-readable report (sections in the order of the file comment). verbose: every diagnostic, else the first 20 of each
// step and campaign / Lua list.
std::string format_mod_report(const ModCheckReport& report, bool verbose = false);

}  // namespace alamo::game
