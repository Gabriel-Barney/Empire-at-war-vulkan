// alamo_vfs - virtual file system implementing the verified Data\ resolution order
// (docs/PHASE2.md, docs/re/SUBSYSTEMS.md "File lookup priority", docs/re/FALLBACK_CAMPAIGNS.md).
//
// Lookup of a game-relative path, first match wins:
//   1. Mod  : one search list, <dir>\Data\<rest>, earliest entry wins. MODPATH= and STEAMMOD=
//             are interleaved in command-line order (PHASE2_ANSWERS.md Q14: the original adds
//             each token to the same search vector as it parses it). See VfsConfig::mod_search.
//             Only for paths starting with Data\ (after normalisation).
//   2. Base : FoC loose <foc_root>\Data\<rest>.
//   3. Meg  : mounted MEG archives, newest-mounted first. Evidence: statically, Add_Megafile
//             appends the archive at the TAIL of its list, so newest-wins depends on Open_File
//             iterating in reverse, which was not verified statically (PHASE2_ANSWERS.md,
//             "Corrections", SUBSYSTEMS.md MEG list); in-game probe v2 confirmed at runtime that
//             the later-listed MEG wins. Mount order (later wins):
//             <EaW>\Data\MegaFiles.xml entries (file order), <EaW>\Data\Patch.meg, patch2.meg,
//             64Patch.meg, then FoC Data\MegaFiles.xml entries, then FoC Data\Patch.meg,
//             patch2.meg, 64Patch.meg. FoC's MegaFiles.xml and every FoC-side .meg are opened
//             through the loose tiers (mod -> base -> FALLBACKPATH dirs; MegaFiles.xml also via
//             already-mounted MEGs), so a mod's own MegaFiles.xml / Patch*.meg replace FoC's.
//             EaW-side files are always taken from the EaW install itself.
//             MegaFiles.xml MEGs go through the normal file open (Q14), so a mod's
//             Data\Config.meg is mounted INSTEAD of FoC's (the base copy is never mounted). A
//             requested name that is already mounted is skipped (the original de-duplicates by
//             the requested path string, case-insensitively; here: alamo::normalize_path of the
//             request, separately for the EaW and FoC sides).
//   4. Fallback: FALLBACKPATH= dirs in order, then <EaW>\Data (GameData). Data\ paths only.
// Paths are ASCII case-insensitive, '/' == '\\', leading ".\\" ignored (alamo::normalize_path).
// Loose tiers index only the <root>\Data subtree, so non-Data paths resolve from MEGs only.
//
// Thread safety: create() builds immutable indices; every const method may be called
// concurrently from any number of threads afterwards (MEG reads use the thread-safe
// alamo::meg::Archive sources).
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "alamo/core/file_provider.hpp"

namespace alamo::meg {
class Archive;
}

namespace alamo::vfs {

// One entry of the ordered mod search list.
struct ModSearchEntry {
    enum class Kind : uint8_t { Path, SteamWorkshop };
    Kind kind = Kind::Path;
    std::string value;  // Path: host dir; SteamWorkshop: the workshop id as written
};

// Maps a STEAMMOD= workshop id to the subscribed item's folder. Return nullopt (or "") when the
// item is not subscribed/installed: like the original, nothing is mounted for that entry.
using SteamModResolver = std::function<std::optional<std::string>(std::string_view workshop_id)>;

struct VfsConfig {
    std::string foc_root;                      // folder containing FoC's Data\ (the "corruption" dir)
    // The mod search list in command-line order (MODPATH= and STEAMMOD= interleaved), earliest
    // wins. parse_command_line appends here.
    std::vector<ModSearchEntry> mod_search;
    // Simple form for callers that only use plain directories: searched AFTER mod_search, in
    // order, earliest wins. parse_command_line does not touch it.
    std::vector<std::string> mod_paths;
    std::vector<std::string> fallback_paths;   // FALLBACKPATH= dirs, in order
    std::optional<std::string> eaw_root;       // nullopt = auto-detect (<foc_root>\..\GameData with
                                               // sweaw.exe), "" = no EaW, else explicit GameData dir
    bool mount_megs = true;
    // Resolves SteamWorkshop entries of mod_search when the Vfs is created. Unset: those entries
    // are skipped with a warning.
    SteamModResolver steam_resolver;
};

// Parses StarWarsG-style switches: MODPATH=, FALLBACKPATH=, STEAMMOD= (keys case-insensitive,
// values may be quoted). MODPATH= and STEAMMOD= are appended to cfg.mod_search in argument
// order; FALLBACKPATH= to cfg.fallback_paths. Relative paths are resolved against cfg.foc_root (set it first);
// backslashes are converted for the host. Unknown arguments are ignored. Returns false if a
// recognised switch had an empty value (the other switches are still applied).
bool parse_command_line(std::span<const std::string> args, VfsConfig& cfg);

class Vfs final : public alamo::FileProvider {
public:
    struct Resolution {
        enum Layer { Mod, Base, Meg, Fallback } layer;
        // Loose: host path of the file. Meg: "<archive host path>#<ENTRY NAME AS STORED>".
        std::string location;
        // Mod: index into mod_dirs(). Fallback: FALLBACKPATH dirs, then GameData last.
        // Base: 0. Meg: mount index (0 = first mounted = lowest priority), see mounts().
        int layer_index;
    };
    struct Mount {
        std::string path;    // host path of the .meg
        std::string reason;  // e.g. "EaW MegaFiles.xml", "FoC Patch", "FoC 64Patch"
    };

    // Returns nullptr (and *err) only if foc_root is not a directory. Missing EaW, missing
    // MegaFiles.xml entries and unreadable MEGs are recorded in warnings() and skipped.
    static std::unique_ptr<Vfs> create(const VfsConfig& cfg, std::string* err = nullptr);
    ~Vfs() override;
    Vfs(const Vfs&) = delete;
    Vfs& operator=(const Vfs&) = delete;

    bool read(std::string_view path, std::vector<uint8_t>& out) const override;
    bool exists(std::string_view path) const override;
    std::string origin(std::string_view path) const override;  // Resolution::location or ""

    std::optional<Resolution> resolve(std::string_view path) const;

    // Union of all logical paths matching `glob` over every layer, each once, normalised
    // (upper-case, '\\'), sorted by byte order. Glob: '*' and '?' do not cross '\\'; '**' does.
    // Example: list("Data\\Scripts\\AI\\*.lua").
    std::vector<std::string> list(std::string_view glob) const;
    // Layer bits for the restricted list(): which layers' files are searched.
    //   kLayerMod       the mod search list's Data trees
    //   kLayerBase      FoC loose <foc_root>\Data
    //   kLayerMeg       every mounted MEG (EaW and FoC)
    //   kLayerFallback  FALLBACKPATH dirs and the EaW GameData folder
    static constexpr uint32_t kLayerMod = 1u, kLayerBase = 2u, kLayerMeg = 4u, kLayerFallback = 8u;
    static constexpr uint32_t kLayerAll = kLayerMod | kLayerBase | kLayerMeg | kLayerFallback;
    // The same listing over the selected layers only (list(glob) == list(glob, kLayerAll)): the logical paths that
    // exist in at least one selected layer, each once, normalised, sorted. A name present in a selected layer is
    // listed whether or not an unselected higher-priority layer overrides it (this lists names, not resolutions).
    // Example: list("Data\\CustomMaps\\*.TED", kLayerBase | kLayerMeg). Unknown bits are ignored.
    std::vector<std::string> list(std::string_view glob, uint32_t layers) const;

    // Multi-line dump of the layers and MEG mount order (highest priority first).
    std::string describe() const;

    const std::vector<Mount>& mounts() const;           // mount order (index 0 = lowest priority)
    // The effective mod search list (host dirs, earliest wins): resolved mod_search entries
    // (unresolved STEAMMOD ids dropped), then mod_paths. Resolution::layer_index indexes this.
    const std::vector<std::string>& mod_dirs() const;
    const std::vector<std::string>& warnings() const;
    const std::string& eaw_root() const;                // "" if none

    struct Impl;

private:
    explicit Vfs(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

// Tolerant <File>...</File> extractor for MegaFiles.xml (comments skipped, entries trimmed,
// UTF-8/ASCII or UTF-16LE with BOM). Exposed for tests/diagnostics.
std::vector<std::string> parse_megafiles_xml(std::span<const uint8_t> bytes);

// Glob used by Vfs::list on normalised paths (case-insensitive, '/' == '\\').
bool path_glob_match(std::string_view pattern, std::string_view path);

}  // namespace alamo::vfs
