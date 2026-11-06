// alamo_vfs STEAMMOD= resolver without the Steam API (roadmap modc t10, docs/MODS.md §3.1).
//
// Retail asks the Steam client for a subscribed workshop item's folder; the port looks on disk instead: a workshop item
// counts as installed when <library>\steamapps\workshop\content\<app id>\<id>\ is a folder in one of the Steam library
// folders. Nothing here writes anything, calls Steam or needs Steam to run.
//
// Library folders, in search order (first folder holding the item wins; duplicates dropped, compared
// case-insensitively with '/' == '\\'):
//   1. the library that holds the FoC folder: the parent of the nearest ancestor of foc_root named "steamapps"
//      (<library>\steamapps\common\<game>\corruption), so a Steam install of the game finds its own workshop items
//      even with no Steam client folder known;
//   2. for each Steam client folder ("steam root"), in order: the root itself, then every "path" entry of
//      <root>\steamapps\libraryfolders.vdf (file order; the pre-2021 form with numeric keys also), then those of
//      <root>\config\libraryfolders.vdf.
// default_steam_roots() gives the client folders of this machine: ALAMO_STEAM_ROOT (';'-separated, for tests and
// unusual setups; when set, nothing else), else the SteamPath / InstallPath values Steam keeps in the registry (read
// only), else the default install folder.
//
// Determinism: the resolved folder depends only on the files on disk, exactly like MODPATH=; the order above is fixed
// (no directory enumeration decides a result). Thread safety: the free functions are reentrant; the process-wide
// lookup switch (install_steam_workshop_lookup) is guarded by a mutex and meant to be set once at startup.
#pragma once
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "alamo/vfs/vfs.hpp"

namespace alamo::vfs {

// Steam app id of Star Wars: Empire at War - Gold Pack, whose workshop holds FoC mods (the original's STEAMMOD= items
// live under workshop\content\32470; docs/MODS.md §3.1). Not a mod id.
inline constexpr std::string_view kFocSteamAppId = "32470";

// Every library "path" value of a libraryfolders.vdf text (Valve KeyValues), in file order, with the KeyValues escapes
// (\\, \", \n, \t) decoded. Both forms: "<n>" { "path" "D:\\SteamLibrary" ... } and the older "<n>" "D:\\SteamLibrary".
// Malformed input yields what was parsed up to the error.
std::vector<std::string> parse_steam_library_folders(std::string_view vdf);

// Library folders to search (see the file comment), host paths without a trailing separator.
std::vector<std::string> find_steam_libraries(std::string_view foc_root, std::span<const std::string> steam_roots);

// Steam client folders of this machine (see the file comment). Existing folders only.
std::vector<std::string> default_steam_roots();

// <library>\steamapps\workshop\content\<app_id>\<id> of the first library where it is a folder, else nullopt. `id` must
// be a non-empty run of decimal digits (a workshop id); anything else is never found (no path tricks through STEAMMOD=).
std::optional<std::string> find_workshop_item(std::span<const std::string> libraries, std::string_view id,
                                              std::string_view app_id = kFocSteamAppId);

// A VfsConfig::steam_resolver over a fixed library list.
SteamModResolver make_steam_workshop_resolver(std::vector<std::string> libraries, std::string app_id = std::string(kFocSteamAppId));

// Process-wide default: once installed, Vfs::create resolves STEAMMOD= entries of a config whose steam_resolver is unset
// with make_steam_workshop_resolver(find_steam_libraries(cfg.foc_root, roots)) (roots: default_steam_roots(), read when
// the first such config is created; or the given list). Apps call install_steam_workshop_lookup() once in main; tests
// and libraries leave it off, so a bare Vfs keeps warning "no workshop resolver".
void install_steam_workshop_lookup();
void install_steam_workshop_lookup(std::vector<std::string> steam_roots);
void uninstall_steam_workshop_lookup();
// The default resolver for `foc_root` if the lookup is installed, else an empty function.
SteamModResolver default_steam_resolver(std::string_view foc_root);

}  // namespace alamo::vfs
