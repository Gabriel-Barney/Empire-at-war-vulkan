// Launcher support (owner: n2-game): a saved game's galactic state as JSON, for tools outside the engine (the EaW Vulkan
// Launcher's campaign map). The .alsv body is the engine's own packed, versioned format; tools read this export instead.
//
//   alamo_game --game DIR [MODPATH=..] --export-save SAVE.alsv --out FILE.json
//     loads the save exactly as the Load menu does (game::load_game: the same game data and mods it was saved with,
//     else refused), without a window, Vulkan or audio, writes the JSON and exits 0. Exit 1 + a message on stderr when
//     the save does not load or has no galactic map.
//
// JSON (format 1; UTF-8; names as written in the XML, no localisation):
//   format, save {name, kind, campaign, title, faction, difficulty, week, gameSeconds, savedAt, tick},
//   galacticDay, modeFrame, localPlayer (player index, -1 none),
//   players [{index, faction, color ("#rrggbb" from Factions.xml, null without), human, ai, neutral, credits, techLevel,
//             maxTechLevel, planets (owned count)}],
//   planets [{index, name, type, isPlanet, x, y, z, owner (-1 none), control, creditValue, starbaseLevel, starbaseOwner,
//             groundLevel, revealed (by the local player), hidden, destroyed, corrupted, inBattle,
//             structures [{type, owner, count}], surface [{type, owner, count}], fleets [fleet index]}]  (row order),
//   routes [{a, b}] (planet indices, route order),
//   fleets [{index, owner, planet (-1 in transit), from, to, moving, x, y, z, units [{type, owner, count}]}]  (live only).
//   Unit lists are grouped by (type, owner) in first-seen order; dead objects are left out.
#pragma once
#include <cstdint>
#include <span>
#include <string>

#include "alamo/core/save_file.hpp"
#include "alamo/game/game_data.hpp"
#include "alamo/game/game_session.hpp"
#include "alamo/jobs/job_system.hpp"

namespace alamo::game {

inline constexpr int kSaveExportFormat = 1;

// A faction's map colour for the export (the exe fills these from Factions.xml; empty = no colours).
struct ExportColor {
    std::string faction;
    uint8_t r = 255, g = 255, b = 255;
};

// The JSON of a loaded session (`file` = the save it was loaded from, for its description). False + *err when the
// session has no galactic mode.
bool export_session_json(const GameData& data, const GameSession& session, const SaveFile& file, std::span<const ExportColor> colors,
                         std::string& out, std::string* err = nullptr);
// load_game(save_path) -> export_session_json -> written to out_path (temporary file + rename). False + *err.
bool export_save(const GameData& data, jobs::JobSystem& js, const std::string& save_path, const std::string& out_path,
                 std::span<const ExportColor> colors, std::string* err = nullptr);

}  // namespace alamo::game
