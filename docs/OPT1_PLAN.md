# OPT1 plan — tick profiler and budgets

Written 2026-10-30. Status 2026-10-30: steps 1-4 and 6 are built (profiler in `engine/core/include/alamo/core/profile.hpp`, baselines and budget in `docs/PHASE4.md`); only the `alamo_game` overlay is open. Deviations from the design below: the ring stores full per-tick records and the report computes exact percentiles from them (no histograms); zone values are inclusive thread time summed across threads; worker utilisation is busy time per job slot as a share of tick time. Roadmap group `opt1`
(box OPT, preset `high`, estimate ~1–2 days, no hard prerequisites). Goal from the roadmap: use the cores better and stop
big game / AI updates from landing on a single tick. Nothing in the sim times itself today, so this group measures first.
Background: `docs/OPT_RESEARCH.md` (techniques ranked by fit). Every later performance group (OPT2/3, CP1–6, LP1/4/5,
BM1–4) compares against the baseline this group records.

## Constraints (CLAUDE.md rule 3, determinism)
- Timers read the wall clock, so their values must never reach game logic, saves, replays or the per-tick CRC.
  Design rule: the profiler is write-only from the sim's point of view; no sim code branches on a timing value.
- Results must stay bit-identical for any worker count, run and compiler. The CRC selfchecks and
  `alamo_headless_skirmish_ai_foc_land_outcome_w1/_w4/_w15` are the proof; run them (`build_gate.cmd`) after every step.
- Windows x64 only; MSVC ships, clang checks. Both builds, 0 warnings. No third-party dependency (Tracy needs Gabriel's
  approval under rule 2 and stays out of this group).
- Profiling is compiled in but cheap when off: a disabled scope costs one predictable branch on a flag read once per tick.

## Where the code is (read 2026-10-30)
- `engine/jobs/include/alamo/jobs/tick.hpp`: `SimClock` (30 ticks/s, 33 ms), `TickScheduler` (systems with period/phase
  run in registration order on the sim thread), `Deferred<T>`. This is the natural home of per-system timers because every
  registered system already passes through it.
- `engine/jobs/include/alamo/jobs/job_system.hpp`: `JobSystem` has `current_slot()` and `executed_per_slot()` (job counts only,
  no time). Worker busy/idle time has to be added here.
- `engine/apps/sim/include/alamo/sim/game_modes.hpp`: `ModeTickContext` is handed to every mode tick; `GameMode::tick`,
  `common_tick`. `engine/apps/sim/src/tactical_battle.cpp` (2.3 K lines) runs the tactical tick in numbered steps
  (movement, combat, fog, garrison, hangar, abilities, ...), each a candidate scope.
- `engine/apps/sim/src/sim_host.cpp`: the host frame loop; where the per-tick total and the frame-time ring buffer belong.
- `engine/apps/headless/main.cpp`: the flag parser (`--synthetic`, `--ais`, `--ticks`, `--workers`, `--game`, `--battles`, ...).
  `--profile` is new here.
- `bench_msvc.cmd`: today runs headless selfcheck, all campaigns, 400-planet synthetic (with and without `--latency 30`),
  `alamo_galactic_bench`, `alamo_jobs_bench`. Output `logs\bench.txt`. It records no per-tick timing.
- Not yet seen and to be checked at step 1: where Lua, story and the galactic AI are ticked from (they are serial per
  `OPT_RESEARCH.md`), and whether the render host (`engine/apps/game`) calls the same tick entry point.

## Design

### Profiler core (new, in `engine/core` or `engine/jobs`; decide at step 1)
- `ProfileZone` ids registered once at startup (string literal + small integer), so the hot path never touches strings.
- `ScopedZone zone(id)`: reads a monotonic clock (`QueryPerformanceCounter`) at construction and destruction, adds the
  elapsed ticks to the current tick's slot for that zone. Nested zones are allowed; each zone reports inclusive time.
- Storage: a per-thread (per-`current_slot()`) array of accumulators, indexed by zone id, so parallel jobs never share a
  cache line; merged on the sim thread at end of tick in slot order (fixed order, not that it matters for timing data).
- Per tick the profiler closes a `TickRecord { frame, total_ns, zone_ns[N], worker_busy_ns[W] }` into a fixed-size ring
  (default 4096 ticks, ~2 minutes at 30/s, allocated once). No allocation after startup.
- Histograms: per zone, log-spaced buckets (e.g. 16 µs to 1 s), updated at tick close. Percentiles come from buckets for the
  long run; the exact worst-20 list comes from a small fixed top-K kept alongside.
- Compile-time switch `ALAMO_PROFILE` (default on in all configs; the runtime flag decides) with a `constexpr` off mode
  that removes the zones entirely if a measurement shows the on-but-idle cost is not negligible.

### Zones (t01)
Sim and AI systems, Lua, story, commit. First cut, adjusted at step 1 once the call sites are read:
`tick.total`, `galactic.economy`, `galactic.ai.<n>` (per AI), `galactic.allpairs`, `galactic.perception`, `story`, `lua`,
`tactical.movement`, `tactical.combat`, `tactical.fog`, `tactical.garrison`, `tactical.hangar`, `tactical.abilities`,
`tactical.ai`, `commands`, `objects.commit`, `crc`.

### Worker utilisation (t02, `engine/jobs`)
- Per worker slot: nanoseconds spent running a job vs. waiting, per tick; sim-thread-alone time = tick total minus the time
  any worker was busy. Workers add timestamps around job execution only (two clock reads per job), not around the steal loop.
- Published through a `JobSystem::utilisation()` snapshot taken at tick close; nothing in the job scheduling decisions reads it.
- The existing `alamo_jobs_tests` / `alamo_jobs_crc_tests` plus a new test: with `--profile` on and off, the CRC log is identical.

### Reports (t03)
- `alamo_headless --profile [file]`: after the run print p50 / p99 / max tick ms, tick-over-budget count, per-zone p50 / p99 /
  share of total, worker utilisation, and the 20 worst ticks each with its per-zone split. Optional CSV/JSON for diffs.
- `alamo_game`: frame-time overlay (graph of the last N ticks, current worst zone). Lives in the render/ui side, reads the same
  ring buffer; no sim dependency in the other direction.

### Baselines (t04)
On Gabriel's PC (16 logical processors, `--workers` at default and at 0 for the serial reference):
1. FoC HotM galactic with all AIs (`--game ... --campaign` run, exact flags chosen at step 1).
2. 400-planet synthetic, 4 AIs (`--synthetic 400 --ais 4`), with and without `--latency 30`.
3. A large space battle and a large land battle (`--battles` / the skirmish-AI runs; pick the biggest shipped scenes).
Each run is repeated 3 times; report the median run, and record machine, build, flags and date in `docs/PHASE4.md`.

### Budget (t05)
- Targets from the baseline, not guessed: a p99 sim-tick target and a spike limit (no tick above a set share of the 33 ms
  tick at 30 ticks/s), per scenario. Initial proposal for review: p99 under 50 % of 33 ms, and no tick above 100 % in the
  baseline set.
- List the top spike sources (worst-20 tables) as the input list for OPT2.

### Bench integration (t06)
`bench_msvc.cmd` gains the profile runs, writing `logs\bench_profile_<scenario>.txt` and a one-line summary per scenario;
a small script compares a new run to the stored baseline and flags regressions (this also feeds the LP5 regression gate later).

## Steps and order
1. Read the call sites above (Lua, story, galactic AI, render host entry point); decide module placement; write the zone list.
2. Profiler core + `TickScheduler` hooks + unit tests (zones accumulate, ring wraps, no allocation after start, disabled mode). Gate.
3. Add zones in `apps/sim` (galactic and tactical ticks, Lua, story, commit). CRC-identical check with profiling on/off. Gate.
4. Worker utilisation in `engine/jobs` + tests. Gate including `alamo_jobs_tests`, and the sanitizer run (`build_asan.cmd`).
5. `alamo_headless --profile` report, then the `alamo_game` overlay (render-side, needs a visual check by Gabriel or a screenshot).
6. Baselines, budget, `bench_msvc.cmd` additions, `docs/PHASE4.md` record, roadmap update (close tasks, mark group done,
   open the OPT2 inputs).

## Risks
- Clock-read cost in hot parallel loops: put zones at system level, not per object; measure on-but-idle overhead at step 2.
- Per-tick numbers on a shared dev PC are noisy: take the median of 3 runs, close other heavy programs, and keep
  `--workers 0` as the stable reference.
- Touching `engine/core` / `engine/jobs` headers goes through the integrating agent (CLAUDE.md parallel-work rule): do the
  profiler core and the jobs change as one reviewed pass before module agents build on them.
- The GPU is out of scope here (CPU tick only); GP1–GP3 cover it.

## Done when
`alamo_headless --profile` prints per-system percentiles and the 20 worst ticks for the four baseline scenarios, the baseline and
budget are in `docs/PHASE4.md`, `bench_msvc.cmd` re-runs them, CRCs are unchanged with profiling on and off at 0/1/4/15
workers, and the gate (`build_gate.cmd`) passes with 0 warnings.
