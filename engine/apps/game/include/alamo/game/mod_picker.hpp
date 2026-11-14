// modc t08: the mod picker's data side (alamo_game's IDD_PORT_MODS_DIALOG; docs/MODS.md §5.2). Render-free and
// UI-free so the tests can drive it:
//   list_installed_mods   the mods a player can pick: Steam workshop items of the FoC app id in the given Steam library
//                         folders (vfs::find_steam_libraries order; the first library holding an id wins, as the STEAMMOD=
//                         resolver), then the folders of <game>\Mods. A candidate needs a Data folder (the same rule as
//                         the load report's submod search). Display name: modinfo.json "name", else the folder name.
//   mod_report_summary    a few lines of the load report (mod_check.hpp, the alamo.mod_report figures) to show before
//                         launch: result, errors / warnings, content counts, Lua, missing files, the submod hint
//   mod_launch_args       the VFS arguments that start a mod: its own MODPATH= / STEAMMOD=, or for a likely submod
//                         (SubmodHint::likely) the report's stacked arguments (the submod, then its parent)
//   relaunch_args         a command line with its mod arguments replaced (alamo_game restarts itself: the data, the
//                         text database and the GUI are loaded once per process, as in the original, which takes mods
//                         only on its command line)
// No faction or mod name is known here (docs/MODS.md §1).
#pragma once
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace alamo::game {

struct ModCheckReport;

struct InstalledMod {
    std::string id;         // workshop id (the item folder's name) or the <game>\Mods folder name
    bool workshop = false;  // a Steam workshop item (else a <game>\Mods folder)
    std::string path;       // host folder
    std::string name;       // modinfo.json "name" (UTF-8), else `id`
    std::string arg;        // "STEAMMOD=<id>" or "MODPATH=Mods\<id>"
};

// Sub-folders of `dir` that have a Data folder, by name (byte order); `digits_only`: workshop item folders only. An entry
// that cannot be read is skipped and the scan goes on. Shared by list_installed_mods and the load report's submod search
// (cu9 t03).
std::vector<std::filesystem::path> mod_folders(const std::filesystem::path& dir, bool digits_only);
// <library>\steamapps\workshop\content\<FoC app id>: where a Steam library keeps its FoC workshop items.
std::filesystem::path workshop_content_dir(std::string_view library);

// See the file comment. Order: workshop items by id, then Mods folders by name (byte order), each id once.
std::vector<InstalledMod> list_installed_mods(const std::string& game, std::span<const std::string> workshop_libraries);
// The "name" string of <folder>\modinfo.json (JSON escapes \" \\ \/ \n \t and \uXXXX decoded to UTF-8); "" when the file
// or the key is missing.
std::string modinfo_name(const std::string& folder);
// A VFS mod argument: MODPATH= or STEAMMOD= (any case). FALLBACKPATH= is not one (it stays).
bool is_mod_arg(std::string_view arg);
// `args` without its mod arguments, followed by `mod_args` (empty = the base game).
std::vector<std::string> relaunch_args(std::span<const std::string> args, std::span<const std::string> mod_args);
// The arguments that start `mod` (see the file comment); `report` may be null (not checked yet).
std::vector<std::string> mod_launch_args(const InstalledMod& mod, const ModCheckReport* report);
// The picker's report lines (UTF-8, at most about a dozen, the most important first).
std::vector<std::string> mod_report_summary(const ModCheckReport& report);
// One Windows command line from `args` (quoted for CommandLineToArgvW / the CRT: spaces, tabs and quotes; backslashes
// before a quote doubled).
std::string windows_command_line(std::span<const std::string> args);

}  // namespace alamo::game
