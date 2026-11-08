// BM3 (side goal "repeatable benchmark"): game-made benchmark saves, render-free (the exe's `--bench-new-save` and
// `--bench-maps`, and their tests).
//
//   alamo_game --bench-new-save FILE --bench-map MAP [--bench-human FACTION] [--bench-ai FACTION] [--bench-scale N]
//              [--bench-save-after T] [--bench-seed S] [--workers N] [--game DIR] [MODPATH=..]
//       Builds a skirmish session (the human's idle fleet or army against the AI's, both repeated N times: the map decides
//       space or land), runs it until the battle is on the mode stack plus T more ticks, and writes the save to FILE.
//       The save's descriptor leaves out the machine's game-data folder and the file carries no wall-clock time, so the same
//       arguments give the same bytes on any machine. `alamo_game --bench-make S.abench --bench-save FILE` turns it into a
//       scenario (benchmark_scenario.hpp); the manifest's `make` line records these arguments so
//       `alamo_game --bench-regen S.abench` can remake the save when the sim, the save format or the game data changed.
//       The human side takes no orders in a benchmark run, so it only defends: the AI side attacks.
//   alamo_game --bench-maps
//       One line per official skirmish map: space / land, the size, water (type, height, share of the terrain under it,
//       reflection / refraction settings) and the environment's sky domes, with the placed nebula / cloud / field objects.
//       The way the nebula and water scenes were picked.
#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "alamo/game/game_data.hpp"
#include "alamo/jobs/job_system.hpp"

namespace alamo::game {

struct BenchNewSave {
    std::string out;                  // --bench-new-save FILE ("" = not requested)
    std::string map;                  // --bench-map (a _MP_SPACE_* or _MP_LAND_* skirmish map)
    // [name-ok: the benchmark command line's default factions (any faction name is accepted), recorded in the scenes' make lines]
    std::string human = "Empire", ai = "Rebel";  // --bench-human / --bench-ai
    uint32_t scale = 1;               // --bench-scale N: the default forces of both sides repeated N times (1..16)
    uint64_t save_after = 300;        // --bench-save-after T: ticks after the battle started
    uint32_t seed = 1;                // --bench-seed S
    bool maps = false;                // --bench-maps
    bool active() const { return !out.empty() || maps; }
};

// Parses the flag at args[i] (advancing i past its value). 1 = consumed, 0 = not one of these flags, -1 = bad value.
int parse_bench_new_arg(std::span<const std::string> args, size_t& i, BenchNewSave& o, std::string* err = nullptr);
// "" when the request can run, else why not.
std::string check_bench_new_save(const BenchNewSave& o);

// Builds the session, runs it to the save point and writes `o.out`. False + *err. *summary: one line for the log.
bool make_bench_save(const GameData& data, jobs::JobSystem& js, const BenchNewSave& o, std::string* summary, std::string* err);

// --bench-regen S.abench (a `host game` scenario whose `make` line holds the --bench-new-save arguments, without the output file):
// makes the session again as the manifest says and writes it next to the manifest as <name>.regen.alsv (*tmp_save). The exe then runs
// `--bench-make S --bench-save <that file>` (the scenario's own settings and camera) and deletes the file. False + *err.
bool bench_regen_save(const GameData& data, jobs::JobSystem& js, const std::string& manifest, std::string* tmp_save, std::string* summary,
                      std::string* err);

// --bench-maps: the listing, one line per map, in the skirmish list's order (space maps, then land maps).
std::vector<std::string> bench_map_lines(const GameData& data);

}  // namespace alamo::game
