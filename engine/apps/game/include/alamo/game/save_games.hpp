// N1 / t09 (owner: n2-game): alamo_game's saved games on disk - engine-format saves (SaveFile, alamo/core/save_file.hpp)
// with the extension .alsv in one per-user folder. Render-free (used by the game, its tests and tools).
//
//   FOLDER      default_saves_dir(): %LOCALAPPDATA%\alamo\Saves, next to the video options file (game_display.hpp
//               default_video_profile_path). Smoke / test runs pass their own folder.
//   NAMES       the save name the player typed is the file stem: sanitize_save_name keeps printable ASCII except the
//               characters Windows forbids in file names (replaced by '_'), trims spaces / dots at both ends, caps the
//               length at kMaxSaveName and appends '_' to reserved device names (CON, NUL, COM1, ...).
//   DESCRIPTION SaveFile::description = encode_description(SaveMeta): line 1 = the dialog label (save_label), then the
//               marker line "alamo_game_save 1" and key=value lines (name, kind, campaign, title, faction, difficulty,
//               tick, week, game_seconds, saved_at, faction_icon, playthrough, playthrough_started). decode_description
//               reads them back (unknown keys ignored; saves from before a key have its default).
//   TREE        [port: Gabriel 2026-11-13] the dialogs list saves as a tree (save_groups): campaign or skirmish map >
//               faction (with its emblem, faction_icon) > playthrough (campaigns: the saves of one new game, playthrough
//               id) > the saves, each row showing save_row_label (the week / battle time and when it was saved).
//   LISTING     list_saves(dir): every *.alsv, newest first (SaveMeta::saved_at, else the file's write time), then by
//               name (case-insensitive, then exact) - a deterministic order. Only the file header is read when it is in
//               the format-1 layout (magic, version, tick, description); other versions fall back to SaveFile::read_file.
//               Unreadable files are listed with readable = false (so the player can delete them).
//   WRITING     write_save_file: a temporary file next to the target, then a rename over it, so a failed write never
//               destroys the save it was replacing.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "alamo/core/save_file.hpp"

namespace alamo::game {

inline constexpr const char* kSaveExtension = ".alsv";
inline constexpr size_t kMaxSaveName = 64;

std::string default_saves_dir();  // "" when %LOCALAPPDATA% is unset

// What the load / save dialogs show about a save (SaveFile::description).
struct SaveMeta {
    std::string name;          // the save name (= file stem)
    std::string kind;          // "campaign" / "skirmish"
    std::string campaign;      // CampaignDef name / skirmish map
    std::string title;         // display name of the campaign ("" = campaign)
    std::string faction;       // the local player's faction
    int32_t difficulty = 1;    // 0 easy, 1 normal, 2 hard
    uint64_t tick = 0;         // host frame the save was taken after
    uint32_t week = 0;         // in-game week (1-based; 0 = none, e.g. a skirmish)
    uint64_t game_seconds = 0; // campaign: galactic mode time (galactic frame / 30); skirmish: battle time (battle frame / 30)
    int64_t saved_at = 0;      // wall clock, seconds since 1970-01-01 UTC (0 = unknown)
    // [port: Gabriel] the local faction's emblem texture (FactionRules::icon; kept so a save from another mod still has it),
    // the playthrough the save belongs to (an id made when a new game starts and kept through saves and loads; "" = a
    // save from before playthroughs) and when that game was started (wall clock, 0 = unknown).
    std::string faction_icon;
    std::string playthrough;
    int64_t playthrough_started = 0;
};
std::string encode_description(const SaveMeta& m);
bool decode_description(std::string_view text, SaveMeta& out);
// "<name>  -  <title or campaign> (<faction>), week <w>, <local date and time>"; a skirmish shows its map and the battle
// time ("12:05") instead of a week.
std::string save_label(const SaveMeta& m);
// [port: Gabriel] a save's row in the tree: "<name>  -  Week <w>, <local date and time>" (a skirmish: its battle time
// "12:05" instead of the week).
std::string save_row_label(const SaveMeta& m);
std::string new_playthrough_id(int64_t now);          // a new SaveMeta::playthrough ("<now in hex>-<8 hex digits>")
std::string format_local_time(int64_t unix_seconds);  // "2026-10-29 14:03" ("" for 0)
int64_t unix_now();                                   // wall clock, seconds since 1970-01-01 UTC (SaveMeta::saved_at)

struct SaveEntry {
    std::string name;   // file stem: the menus' save identifier
    std::string path;
    SaveMeta meta;      // from the description (meta.name falls back to the stem)
    uint64_t tick = 0;
    int64_t sort_time = 0;  // meta.saved_at, else the file's write time (seconds since 1970)
    bool readable = false;
    std::string error;      // !readable: why
};
std::vector<SaveEntry> list_saves(const std::string& dir);

std::string sanitize_save_name(std::string_view typed);
std::string save_path(const std::string& dir, std::string_view name);  // dir \ sanitize(name) + ".alsv" ("" for an empty name)
bool save_exists(const std::string& dir, std::string_view name);
bool delete_save(const std::string& dir, std::string_view name, std::string* err = nullptr);
bool write_save_file(const SaveFile& save, const std::string& path, std::string* err = nullptr);  // creates the folder
// tick + description of a save file (header only when possible). False + *err when the file is not a readable save.
bool read_save_header(const std::string& path, uint64_t& tick, std::string& description, std::string* err = nullptr);

}  // namespace alamo::game
