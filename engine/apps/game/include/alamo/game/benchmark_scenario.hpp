// bm2 (side goal "repeatable benchmark"): BM1 scenario files in alamo_game, the render-free half (used by the exe and its
// tests; the exe side is app/app_bench_scenario.cpp).
//
//   RUN   alamo_game --bench-scenario S.abench [other --bench-* flags]: the manifest (alamo/sim/bench_scenario.hpp) gives
//         the save, the recorded orders (replayed: the run's own commands are ignored), the tick count, the mode, preset
//         and frames per tick (format 2), and the scripted camera; flags given on the command line win over the file
//         (--benchmark, --bench-preset, --bench-ticks up to the scenario's, --bench-frames-per-tick, --width / --height).
//         Every pinned file is checked first. At the end the run's CRC log must match the fingerprint, else the run is
//         "scenario changed, not comparable" (exit 3).
//   MAKE  alamo_game --bench-make S.abench --bench-save SAVE [flags]: a normal benchmark run from the save; when it passes
//         (presentation check included) it writes S as a `host game` scenario next to its files: <name>.alsv (the save's
//         bytes), <name>.acmd (the run's recorded command stream, continuing the save), <name>.crc (the run's CRC log =
//         the fingerprint) and the manifest with the run's mode, preset, frames per tick and ticks. When S already exists
//         it is read first (a recipe or a full scenario): its name, description, camera and settings are kept, its save is
//         the default --bench-save, and the version stays when nothing changed (else +1).
//   HOST  alamo_game runs only `host game` scenarios: a BM1 headless scenario (tests/benchmark/space_battle.abench) holds
//         an alamo_headless --save-session harness session, which is not an alamo_game session (GameSession::spec_from_save
//         refuses it), so it is refused with that reason before anything loads.
#pragma once
#include <cstdint>
#include <span>
#include <string>

#include "alamo/commands/recording.hpp"
#include "alamo/core/sim_crc.hpp"
#include "alamo/game/benchmark.hpp"
#include "alamo/sim/bench_scenario.hpp"

namespace alamo::game {

// Finds --bench-scenario / --bench-make FILE in args (call before the other flags are parsed; parse_benchmark_arg then
// overrides what the file set). 0 = neither flag; 1 = the file was read, checked and applied to `o`; -1 = *err says why.
int load_bench_scenario_arg(std::span<const std::string> args, BenchmarkOptions& o, std::string* err = nullptr);
// What the file sets into `o`: save, ticks, mode (fixed frames without a mode key), preset, frames per tick.
void apply_bench_scenario(const sim::bench::Scenario& sc, BenchmarkOptions& o);
// After all flags: "" when a scenario run / make can go ahead, else why not.
std::string check_bench_scenario(const BenchmarkOptions& o);

// The fingerprint check of a scenario run: the run's CRC log against the scenario's, both cut at the last fingerprint
// record at or before end_tick. "" = match, else the first divergence (or why the fingerprint could not be read).
std::string check_bench_fingerprint(const sim::bench::Scenario& sc, const CrcLog& run, uint64_t end_tick);

// --bench-make: writes the scenario (see MAKE above). `base` = the file read before the run (a recipe or a full scenario;
// null = none), `save` = the save the run started from, `orders` / `log` = the run's command stream and CRC log,
// `start_tick` = the save's tick, `ticks` = the ticks run, `o` = the run's options. *summary = one line for the log.
bool write_game_scenario(const std::string& path, const sim::bench::Scenario* base, const std::string& save,
                         const commands::CommandRecording& orders, const CrcLog& log, uint64_t start_tick, uint64_t ticks,
                         const BenchmarkOptions& o, std::string* summary = nullptr, std::string* err = nullptr);

}  // namespace alamo::game
