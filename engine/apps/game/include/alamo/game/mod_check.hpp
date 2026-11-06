// Q5 / modc t05 mod load report: what the port makes of a mod's data. Loads the data exactly as alamo_game does
// (load_game_data with the MODPATH= / STEAMMOD= / FALLBACKPATH= arguments) and reports, without running a session:
//   files      the VFS layers and warnings, and per Data\<folder> of the mod dirs how many files are new and how many
//              replace a base / MEG / fallback file
//   loaders    every load_game_data step (GameData::stages) with its time, result and diagnostics; an exception thrown by
//              a step is caught and reported as a failed step
//   content    counts the session will see (campaigns, planets, object types, factions, AI plans, skirmish maps)
//   campaigns  build_galaxy_map of every campaign: the campaigns with no planets or with skipped locations / routes
//   missing    files the data names that no layer has: entries of the Data\XML\*Files.xml lists, and the
//              Space_ / Land_ / Galactic_Model_Name models of the object types (Data\Art\Models)
//   xml tags   element tags below an object type that neither the original's field tables nor its sub-object elements
//              know (objects/coverage.hpp): the original ignores them, so does the port; listed per tag with counts
//   lua        every Data\Scripts\**\*.lua compiled by the engine's Lua 5.0 VM; each entry script (under a script root
//              other than Library) that compiles also has its main chunk run once in its own VM (require through the VFS,
//              no session: what loading the script runs); and a static scan of every script for engine functions /
//              methods the port does not bind yet
// Every finding of the last three sections says whether it comes from a file in a mod folder (in_mod), so a mod's own
// problems can be told from the base game's (base FoC itself names missing cinematic models and unread tags).
// It does not run a session: run-time gaps past a script's main chunk (functions the session calls later) show up in
// alamo_headless --story / --galactic-ai runs.
// format_mod_report_json: the machine-readable summary, schema "alamo.mod_report" version 1 (docs/MODS.md §5.1).
// Used by alamo_mod_check (tools/mod_check_main.cpp) and its tests. Render-free.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "alamo/game/game_data.hpp"

namespace alamo::game {

struct ModCheckOptions {
    bool campaigns = true;  // build every campaign's galaxy map
    bool lua = true;        // compile every script and scan it for unbound engine calls
    bool lua_run = true;    // with lua: run each compiling script's main chunk in its own VM
    bool files = true;      // missing files named by list files and object type model tags
    bool xml_tags = true;   // unknown XML tags of the object types
};

// A file the data names that no VFS layer has. Grouped by path: `references` counts the naming places, `referenced_by`
// is the first ("Data\XML\GameObjectFiles.xml" for a list entry, "<type> (Space_Model_Name)" for a model).
struct MissingFile {
    std::string path;  // game-relative as looked up, upper case ("DATA\ART\MODELS\X.ALO")
    std::string referenced_by;
    uint32_t references = 0;
    bool in_mod = false;  // at least one naming file is in a mod folder
};

// An element tag below an object type that the original does not read (TagCoverage::unknown()).
struct UnknownXmlTag {
    std::string tag;          // first spelling seen
    uint32_t occurrences = 0;
    uint32_t types = 0;       // distinct type definitions using it
    std::string first_file;   // first file using it
    bool in_mod = false;      // at least one file using it is in a mod folder
};

// A script that does not compile, or whose main chunk raises an error.
struct LuaIssue {
    std::string path;     // game-relative, upper case
    std::string error;    // the VM's message
    bool in_mod = false;  // the script file is in a mod folder
    std::string text() const { return path + ": " + error; }
};

// An engine global / wrapper method the scripts call that the port registers as an "unbound" stub (lua/bindings.hpp).
struct UnboundLuaCall {
    std::string name;
    bool method = false;
    uint32_t call_sites = 0;  // over every script (static scan)
    uint32_t scripts = 0;     // scripts with at least one call site
};

// Files of the mod dirs under one Data\<folder> (the first folder below Data; "" for files directly in Data).
struct ModFolderStats {
    std::string folder;      // upper case, e.g. "XML", "SCRIPTS", "ART"
    uint32_t added = 0;      // no lower layer (FoC loose, MEGs, fallbacks) has the path
    uint32_t overrides = 0;  // a lower layer has the path: the mod's file wins
};

struct ModCheckReport {
    std::string game;                        // the FoC folder checked
    std::vector<std::string> args;           // the VFS command line (MODPATH= ...)
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
    std::vector<MissingFile> missing_files;      // sorted by path
    std::vector<UnknownXmlTag> unknown_xml_tags;  // most occurrences first, then by tag
    size_t lua_scripts = 0;
    std::vector<LuaIssue> lua_errors;            // does not compile
    size_t lua_run = 0;                          // main chunks run (entry scripts that compile)
    std::vector<LuaIssue> lua_run_errors;        // compiles, its main chunk raises an error
    std::vector<UnboundLuaCall> unbound_lua_calls;  // most call sites first, then by name
    std::vector<std::string> check_errors;       // a check above that threw: "<check>: exception: <what>"

    // Errors: the load failed or a step threw / failed, a campaign has no planets, a check threw. Warnings: loader and
    // VFS diagnostics, skipped campaign rows, missing files, unknown XML tags (one per tag), scripts that do not compile
    // or whose main chunk errors, unbound engine calls (one per name). The original loads with all of these; its Lua
    // 5.0 rejects the same scripts, and mods ship unused ones, e.g. Thrawn's Revenge's Lua 5.1 unit-test specs.
    size_t error_count() const;
    size_t warning_count() const;
};

ModCheckReport check_mod_data(const std::string& root, const std::vector<std::string>& args, const ModCheckOptions& options = {});
// Human-readable report (sections in the order of the file comment). verbose: every diagnostic, else the first 20 of each
// step and campaign / Lua list.
std::string format_mod_report(const ModCheckReport& report, bool verbose = false);
// The machine-readable summary (docs/MODS.md §5.1): one JSON object, every list complete, keys in a fixed order.
std::string format_mod_report_json(const ModCheckReport& report);

}  // namespace alamo::game
