// alamo_headless BM1 benchmark (docs/PHASE4.md "BM - repeatable benchmark"; scenario format: alamo/sim/bench_scenario.hpp).
//
//   --bench SCENARIO.abench [--runs R] [--warmup W] [--workers N] [--ticks K] [--bench-json FILE|-] [--bench-baseline OLD.json]
//           [--crc-out F] [--golden F] [--bench-allow-changed] [--quiet]
//       Sim-only mode: loads the scenario's save into a fresh session for every run, replays its recorded orders and runs its
//       ticks as fast as possible (no rendering, no pacing). W warm-up runs (default 1) are not timed into the result; R timed
//       runs (default 3) follow. Each tick is timed around SimHost::tick_once (+ the harness step), and OPT1's per-system
//       zones (alamo/core/profile.hpp) are recorded during the timed runs. Prints median / p99 / worst tick per run and over
//       all timed ticks, and the zones with the largest share.
//       Every run's per-tick CRCs are compared with the scenario's fingerprint: on a mismatch (or when a scenario file no
//       longer matches its manifest pin, or the save no longer loads) the result is "scenario changed, not comparable" and
//       the exit code is 3 (0 with --bench-allow-changed); no speedup or slowdown is reported for it.
//       --ticks K (< the scenario's ticks) runs the first K ticks only (compared on those ticks).
//       --bench-json writes the machine-readable result (CPU, GPU, driver, OS, settings, build, scenario version, runs,
//       summary, zones; "-" = stdout). --bench-baseline compares with an earlier result: a speedup / slowdown only when both
//       matched the same scenario version and fingerprint, else "scenario changed, not comparable".
//       --crc-out writes the first run's CRC log; --golden compares every run's log with a CRC log (exit 1 on a difference:
//       the OPT3 t07 worker-count check, alamo_add_worker_count_check). Runs must also agree with each other (exit 1).
//       Without the game data the scenario needs: "skipped", exit 0.
//       bm2c t02: a `host game` scenario (made by alamo_game --bench-make) runs the same way through alamo_game_core's
//       GameSession (open_game_save_source below): its fingerprint is the rendered run's CRC log, so a match proves the
//       sim-only run did the rendered run's sim work.
//   --bench-make SCENARIO.abench --save-session KIND [--campaign C] [--map M] [--battles B] [--story] [--seed S]
//           --bench-save-at T [--save-when W] --ticks K [--workers N] [--bench-every N] [--bench-desc TEXT]
//       Makes (or remakes) a scenario: builds the session as --save-session does, runs it to the save point, saves
//       <name>.alsv; loads it and runs K ticks recording the orders (<name>.acmd); loads it again replaying them and
//       requires the same per-tick CRCs, which become the fingerprint (<name>.crc; every tick, or a checkpoint log with
//       --bench-every N > 1). Writes the manifest last. The version is kept when the files come out identical and bumped
//       otherwise. The arguments are recorded in the manifest's "make" line.
//   --bench-regen SCENARIO.abench
//       Remakes the scenario with the arguments its manifest recorded (one command after a save-format or game-data change;
//       tools/benchmark/regen_scenario.cmd).
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "save_run.hpp"

struct BenchOptions {
    std::string scenario;            // --bench / --bench-make: the manifest path
    uint32_t runs = 3, warmup = 1;
    int32_t workers = -1;
    bool workers_set = false;
    uint64_t ticks = 0;
    bool ticks_set = false;
    std::string json_out, baseline, crc_out, golden;
    bool allow_changed = false;
    bool quiet = false;
    std::string game_dir;
    std::vector<std::string> game_args;
    // --bench-make
    SaveRunOptions make;             // session, campaign, map, battles, story, seed, save_at, save_when
    std::vector<std::string> make_args;
    uint32_t fingerprint_every = 1;
    std::string description;
};

int bench_run(const BenchOptions& opt);
int bench_make(const BenchOptions& opt);

// bm2c t02 (bench_game.cpp, only with alamo_game_core: ALAMO_HEADLESS_HAVE_GAME_CORE): the source of a `host game` scenario's
// save (alamo_game --bench-make / --bench-regen), so --bench times it sim-only like a headless one. Each load builds a fresh
// GameSession from the save (GameSession::spec_from_save + start_from_save, as alamo_game loads it) with its own job system;
// each tick is one GameSession::tick. Game data: game_dir, else ALAMO_FOC_DATA (0 = skipped when neither is set); VFS
// arguments: game_args, else the save's. 1 = opened, -1 = error (err).
int open_game_save_source(const std::string& save_path, const std::string& game_dir, const std::vector<std::string>& game_args,
                          std::unique_ptr<SaveSessionSource>& out, std::string& err);
