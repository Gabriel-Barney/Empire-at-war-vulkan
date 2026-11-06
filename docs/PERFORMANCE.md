# Performance

Every performance measurement and the reasoning behind the scheduling changes, in one file (2026-11-06). The overview comes from the README; the sections after it were moved verbatim from `docs/PHASE4.md` (OPT1 / OPT2 / OPT3 results). The plans and research behind them: [`OPT1_PLAN.md`](OPT1_PLAN.md) and [`OPT_RESEARCH.md`](OPT_RESEARCH.md). Re-run the baselines with `bench_msvc.cmd`; how the tick profiler is used is in [`BUILD.md`](BUILD.md).

## Overview (moved from the README)

### How it differs from the original

| | Original engine | This port |
|---|---|---|
| Simulation | One thread, run serially inside the frame | Parallel jobs across all cores: movement, combat, spatial queries, pathfinding, perception, animation, galactic graph, AI value scoring |
| Thread library | Intel TBB shipped but unused | Own work-stealing job system with order-stable results |
| Graphics API | Direct3D 9 | Vulkan 1.3, bindless, multithreaded recording |
| AI timing | All AI players think on the same frame | Each AI player's clocks are **staggered**, so players no longer plan on the same tick |
| Long AI work | Runs inside the tick that asks for it | Path recomputes and plan solves run as **fixed-latency background jobs**: started on tick N from a copy of their inputs, applied on tick N+k |
| Lua garbage collection | Collects whenever a VM hits its threshold, mid-script | Collections scheduled by tick number at a fixed point, run in parallel; 70-85% moved out of script calls |
| Frame pacing | Fixed 30 Hz steps shown as-is | Interpolated to the display refresh rate |

Everything is scheduled **by tick number, never by wall clock or thread timing**, so the result is bit-identical for
any worker count. That constraint is why parallelism here is mostly "per-index outputs, reduce in index order".

### Method
All numbers are from this project's own profiler (`alamo_headless --profile`, wall-clock and write-only: no simulation
code ever reads a timing) on an **AMD Ryzen 7 7800X3D (8 cores, 16 threads), Windows 11, MSVC `/O2`**, at 30 ticks per
second, so **one tick is 33.3 ms**. Each scenario is the median of 3 runs on a quiet machine; runs agree within about
2%. "W15" is the default 15 workers, "W0" is the serial reference. Full tables and flags are in
the sections below.

**Budget** (set by the project owner): per tick, **p99 below 16.7 ms** (half a tick) and **no tick above 33.3 ms**.

### Tick time on real Forces of Corruption content

The baseline was recorded on 2026-10-30 (OPT1); "now" is the re-run after the OPT2/OPT3 work on 2026-11-03.

| Scenario | Baseline p50 / p99 / max (ms) | Now p50 / p99 / max (ms) |
|---|---|---|
| Galactic conquest, Heart of the Maw, every faction an AI (18,000 ticks) | 0.19 / 0.52 / 3.1 | 0.18 / 0.51 / 4.4 |
| 400-planet synthetic galaxy, 4 AIs | 0.39 / 2.99 / 11.9 | 0.38 / **1.53** / **4.7** |
| Land battle | 3.06 / 6.39 / 104.7 | **1.32 / 2.21 / 55.1** |
| Space battle with station | 0.95 / 2.17 / 66.9 | 0.80 / 2.15 / 42.4 |

- **p99 is under 16.7 ms in every scenario, by a wide margin** (worst: 2.2 ms).
- The land battle's typical tick is **2.3x faster** and its p99 **2.9x** better. The 400-planet galaxy's p99 halved
  because its first-frame all-pairs recompute now runs in the background.
- The galactic run is dominated by AI goal proposal and was already tiny, so it did not move.
- The land and space battles are not the same games as in the baseline: AI fixes since then end them at different
  tick counts (land 12,374 -> 3,014; space 9,072 -> 12,173). Compare per-tick numbers, not totals.
- The 400-planet galaxy at W15 against W0 (serial): **3.1x faster on mean and 9x on p99** (1.53 against 14.2 ms).
- The job system itself scales well: **7.9x at 8 workers and 12x at 15** on a large Dijkstra, **16.9x** on a
  16-million-element float reduction, with identical results at every worker count.

### The one remaining miss
**One tick per battle is still above 33.3 ms: the first one.** It is one-off battle setup (loading models, building
the tactical AI's plan VMs, laying down map objects), already spread over the workers. After the OPT3 fixes it takes
**55 ms** on the land campaign battle, **42 ms** in space and about **139 ms** on a land skirmish (was 75 / 57 / 193).
Getting under 33.3 ms needs setup spread over several ticks or a sim-only model load. Every other tick is under about
12 ms; the next worst are the battle-end tick (land 5.3 ms) and the second battle tick (first perception, 8-11 ms).

### What changed, and what each fix bought

**Measure first (OPT1).** A zone-based tick profiler with per-system times, worker utilisation, a worst-20 table and
an in-game overlay. It found that the tactical AI, not the battle passes, was most of a land battle's tick.

**One slow equation (OPT2 t13).** On land, a single AI perception leaf, `OpenBuildPadCount`, ran a case-insensitive scan
of every object type name for every open build pad, 66,749 times at about 380 us each: 25.5 s of the run's 28.6 s in
perception. The result fed a hook that nothing installs. Doing the lookup once per call, and only when the hook is
present, changed no output. Land skirmish on a loaded machine, main against the fix: tick p99 **19.7 -> 2.6 ms**, goal
system p99 **18.1 -> 1.0 ms**, ticks over 33.3 ms **7 -> 1**.

**Plan solve and goal values (OPT2 t03/t10).** The PotentialPlan accept was about half of a land battle's tick. Most of
it was the host's build-pad query repeated per candidate, now answered once per proposal slice. The solves then run on
their own RNG streams as a background job applied one tick later, and the AI's per-object view of the battle
(`BattleAi::fill`) is built in parallel: **p50 1.6 -> 0.70 ms, p99 19 -> 1.3-3.5 ms**.

**Spread the AI out in time (OPT2 t01/t02, OPT3 t04).** Staggered per-player AI clocks and fixed-latency background
jobs for galactic all-pairs path recomputes and tactical zone reachability, with save rules so a mid-job save loads and
continues identically.

**Battle setup (OPT3 t05/t09/t11).**
- The battle's model files load on the job system before any spawn instead of one at a time inside each spawn; the
  region build runs one job per movement mask.
- The tactical AI's 54 land / 27 space plan scripts each get their own Lua VM, now built in parallel; their shared
  `require` reads go through a read cache (**98 -> 25 ms** of file reads per battle).
- Battle tables are built incrementally; each model is parsed once.
- Map obstacle stamps run in parallel (**60 -> 17 ms** on a land skirmish).
- ALO model decode is faster (**51 -> 29 ms** of thread time on a land battle).
- Combined effect on the first battle tick under a fully loaded PC: **3-4.5x shorter** on the land campaign battle and in
  space, about 2x on a land skirmish.

**Battle end (OPT3 t11).** The tactical AI's Lua VMs used to be closed one by one inside the final tick (about 23 ms).
Their finalizers now run on the sim thread and the memory is freed by a background job: land battle-end tick
**27.0 -> 5.3 ms**, space **23.5 ms -> under 8.4 ms**.

**End-of-tick checksum (OPT3 t05).** Computing the per-tick CRC was 46-54% of every battle tick. One system alone, the
tactical AI, fed about 65,000 fields through an out-of-line byte-by-byte hasher. Typed inline feeds produce the same byte
stream and the same digests at a fraction of the cost.

**Space movement spikes (OPT3 t06).** Occupancy queries walked two `std::map`s and rebuilt every track piece per query;
they are now sorted contiguous vectors with identical iteration order.

**Lua garbage collection (OPT2 t04).** Lua 5.0.2's collector is unchanged; what changed is *when* it runs. Each VM is
due on a tick determined by a hash of its own state, collected at a fixed point before scripts run, in parallel across
VMs. **70-85% of collections moved out of script calls**; the worst remaining in-script collection is about 2.8 ms. GC
was never a large cost (under 0.5% of tick time), so this removes spikes rather than lowering the average.

### Renderer and frame loop
- The attract-demo land battle at 1280x720 runs a **mean frame of 4.5-5.3 ms** (p50 4.0-4.8 ms). On frames that run a
  sim tick, that is about 2 ms of tick plus about 5 ms of render.
- With 2,000 ships on screen (2,502 draws): draw-list build **0.88 -> 0.67 ms**, sort **0.19 -> 0.09 ms**; draw lists are
  identical at 1, 4 and 15 threads.
- TAA costs about **0.10 ms** of GPU time at 720p; SSAO about **0.4 ms** at 1080p.

### Tried and not shipped
Documented so nobody repeats them without new evidence.
- **Overlapping the sim tick with rendering.** Measured: no gain beyond run-to-run noise (it can save at most the 0.2 ms
  prepare step), and it changed what the scripted input saw. A real overlap needs a snapshot-fed HUD, a Lua VM hand-off
  between threads or a render thread; worth doing only once tick + render exceeds 16.7 ms on target hardware.
- **Running AI players' Lua in parallel.** The scripts are a small share of AI time (0.5-1.5% of the battle AI on land,
  5.5% in space, under 7% in the galactic AI); the cost is the C++ behind each player's service. Not worth the shared
  state it would need.
- **Parallel galactic perception.** Investigated; the same frame's readers use its results, so it stays serial.
- **A shared Lua bytecode cache** for plan VMs: rejected because it would change VM serials and digests.

### Honest caveats
- There is **no side-by-side benchmark against the original executable yet**. The architecture table above describes how
  the designs differ; the measured numbers compare this port with its own earlier builds. The repeatable benchmark
  (BM1-BM4) is the planned fix.
- The test battles are two-AI skirmishes, not the largest scenes the game can produce, so the budget will be re-checked
  as bigger scenes are added.
- Several "before" measurements in the project notes were taken on a PC busy with other jobs, so those are upper bounds;
  the table above uses quiet-machine runs.
- Results come from one machine (16 logical processors). CPU-topology effects on hybrid or dual-chiplet CPUs have not been
  measured.

## Performance baseline and tick budget (OPT1, 2026-10-30)

Tool: `alamo_headless --profile [--profile-out FILE] [--profile-budget MS]` (engine/core `profile.hpp`; design in `docs/OPT1_PLAN.md`).
It reports tick wall time (mean / p50 / p99 / max), per-zone thread time, job-slot busy share and the 20 worst ticks. It is
wall-clock and write-only: the CRC logs of a profiled run equal the unprofiled ones at 0 / 1 / 4 / 15 workers (checked by hand, and by the
ctest entry `alamo_headless_skirmish_ai_foc_land_outcome_profiled`). `bench_msvc.cmd` re-runs the four scenarios into `logs\bench_profile_*.txt`.

Machine: AMD Ryzen 7 7800X3D (8 cores, 16 logical), Windows 11, MSVC `/O2` build, nothing else heavy running. Each scenario 3 runs; the
three runs agree within about 3 %, the median is shown. Tick = 33.3 ms at 30 ticks/s. W15 = `--workers 15` (default), W0 = serial.

| Scenario (flags) | Ticks | W15 mean / p50 / p99 / max (ms) | W0 mean / p50 / p99 / max (ms) |
|---|---|---|---|
| FoC Heart of the Maw, every faction an AI (`--economy --galactic-ai --ticks 18000`) | 18000 | 0.21 / 0.19 / 0.52 / 3.1 | 0.19 / 0.16 / 0.49 / 3.0 |
| 400-planet synthetic, 4 AIs (`--synthetic 400 --ais 4`) | 900 | 0.48 / 0.39 / 2.99 / 11.9 | 1.17 / 0.82 / 15.0 / 60.0 |
| FoC land battle (`--skirmish-ai foc-land-campaign --ticks 20000`, ends at 12374; since t50 `foc-land` is the land skirmish, ~7 ms a tick) | 12374 | 3.42 / 3.06 / 6.39 / 104.7 | 3.08 / 2.67 / 6.25 / 108.7 |
| FoC space battle with station (`--skirmish-ai foc-station --seed 3`, ends at 9072) | 9072 | 1.02 / 0.95 / 2.17 / 66.9 | 0.94 / 0.86 / 2.24 / 64.9 |

What the numbers say:
- Every scenario's p99 is far below half a tick (16.7 ms); the sim has plenty of headroom on this PC. These battles are two-AI skirmishes, not
  the largest scenes the game can reach, so the budget below is for these scenes and gets re-checked when bigger ones exist.
- The few ticks over budget are one-off setup ticks: the first battle tick (land 104 ms, space 67 ms; zone `T5.battles`, followed by a 22.8 ms
  `tac.pre_move` tick in space) and frame 0 of the synthetic run (full all-pairs recompute, 11.9 ms at W15, 57 ms at W0).
- Land and space battles are serial: the job slots are busy 4 % of tick time on W15, so more workers do not help there (they cost 10 % on land).
  The synthetic galactic run does use them (W15 is 2.4 x faster on mean, 5 x on p99).
- `T13.end_tick` (end-of-tick work including the CRC) is 46-54 % of every battle tick (1.5-1.7 ms land, 0.55 ms space): the first thing to look at
  for a constant per-tick cost. `T5.battles` outside the `tac.*` steps (1.2 ms p50 on land) is not split yet.
- Space has 11 ms `tac.movement` spikes (frames 2169, 9035) that land does not.

Budget (confirmed by Gabriel, 2026-10-30): per tick, p99 below 16.7 ms (50 % of 33.3 ms) and no tick above 33.3 ms, measured with
`--profile --profile-budget 33.33` on the four scenarios above at the default worker count. Current status: p99 passes in all four
(max 6.4 ms); the no-tick-above-33.3 ms limit fails on exactly the one-off setup ticks listed above, so those are the first inputs for OPT2
(spread battle setup over several ticks or prepare it ahead; start the synthetic all-pairs recompute in the background, as `--latency` already does).
Not covered yet: story / Lua zones (they run inside `T7` / `T5` laps today), and bigger fleets.

## alamo_game frame profile (OPT1 t07, OPT2 t05, 2026-11-02)

**Overlay (OPT1 t07).** `alamo_game --frame-overlay` (F11 toggles it in live play; the key is not passed on) draws a panel at the
top right, on the top layer of the GUI dialog list: the last 120 sim ticks as bars against the 16.7 ms (p99 budget) and 33.3 ms (one
tick) lines, their last / p50 / p99 / max, the worst tick with its biggest zone, and the last 120 host frames as stacked bars (sim
step, render, its GPU / swapchain wait, rest of the frame). It reads the profiler ring through `profile::snapshot(max_ticks)` (new
optional cap, newest N ticks, so a 65536-tick ring is not copied each frame) only between ticks, and starts the profiler with a
1024-tick ring when nothing else did (stopped again on F11 unless `--profile`). `alamo_game --profile [--profile-out FILE]
[--profile-budget MS]` prints the per-phase frame table (frame, sim, prepare, render, wait: mean / p50 / p99 / max) and the
usual tick report at exit; the profiler stops before the smoke CRC check's replays. Files: `engine/apps/game/app/frame_overlay.*`,
`app_frame.cpp`, `game_main.cpp`. Checks: the land smoke with `--frame-overlay --profile-out` matches its CRC / command golden (4605
lines) and its presentation on/off replay; overlay off, the smoke goldens and `tools\visual\visual_check.cmd` are unchanged.
Screenshot (land smoke, battle shot, cropped): `docs/images/opt1_frame_overlay.png`; smoke frames run 4 ticks each with blocking
model loads, so their frame bars sit at the 40 ms top of the graph.

**Measured loop (attract-demo land battle, `--attract --no-intro --audio null --frames 6000 --immediate --profile`, 1280 x 720,
same PC as above, three runs).** The loop is serial on the main thread: input / routing / UI, `step_sim` (paced tick + post-tick
snapshot captures), presentation steps, `render_frame` (world prepare + record, HUD build + record, submit, present).

| Phase (ms) | mean | p50 | p99 | max |
|---|---|---|---|---|
| frame | 4.5-5.3 | 4.0-4.8 | 9.2-19.0 | 350-430 |
| sim (`step_sim`, all frames; a tick every ~7 frames) | 0.31-0.38 | 0.001 | 3.1-3.6 | 258-279 |
| prepare (battle view: interpolation, instances, particles) | 0.19-0.22 | 0.16-0.18 | 0.67-0.76 | 21-31 |
| render (incl. wait) | 4.1-4.8 | 3.8-4.6 | 6.4-17.4 | 56-116 |
| wait (fence + acquire + present) | 0.8-1.2 | 0.8-0.9 | 1.2-13.1 | 15-104 |
| sim tick (profiler) | 2.1-2.3 | 1.70-1.76 | 6.3-7.0 | 198-217 |

The one tick over 33.3 ms is the battle's first (T5.battles about 150 ms + T5.anim about 49 ms), the same setup spike as the
headless runs; the frame max is that tick plus the battle load.

**Sim / render overlap (OPT2 t05): not shipped.** The loop did run tick and render back to back. Only the battle view's prepare can
overlap the tick without a redesign: the HUD build, the GUI providers and the post-tick captures read the live sim, and the GPU
recording cannot start before `post_tick` because a tick can end the battle and free the scene it draws. Tried and measured:
1. Tick on a second thread, prepare on the main thread: aborts at once, the sim's Lua VMs are bound to the thread that made them
   (`lua_vm.hpp` `check_owner`); moving the tick needs a hand-off of every story / AI VM (engine/apps/sim, engine/ai).
2. Tick on the main thread, prepare on a helper thread, only when a tick is due (the CRC of the run equals its own presentation-off
   replay): frame mean 5.96 / 5.03 ms vs 5.02 / 5.09 / 4.51 ms serial, so no gain beyond run-to-run noise (the most it can save is
   the 0.2 ms prepare on tick frames). One of the two runs had 19 ticks over 33.3 ms, all in `T13.end_tick` (37-118 ms): job slot
   0 is shared by every non-worker thread, so the sim's parallel waits ran the helper's render jobs. The scripted land smoke with the
   overlap also left its golden at tick 304 (4836 vs 4605 CRC lines): input reads the prepared frame (`bview_.frame()`: world picks,
   camera focus, audio positions), so preparing one tick earlier changes the commands the script issues.
An overlap would need a snapshot-fed HUD and input, a Lua VM hand-off or a render thread, and a job priority for presentation work
in slot 0. Worth it only once a frame profile shows tick + render over 16.7 ms on target hardware; today the tick frames take
about 2 ms + 5 ms. The experiment's patch is kept outside the repo (session notes of agent-task-group-assignment, 2026-11-02).

## OPT3 t05 / t06: spike-list fixes before threading (2026-11-02)

Profiled with `alamo_headless --profile` plus temporary zones (removed again, except the per-system CRC zones below). All four fixes
keep results identical: the per-tick CRC logs before and after are byte-identical on the FoC land battle (`--skirmish-ai foc-land`,
3000 ticks) and the FoC space battle (`--skirmish-ai foc-station --seed 3`, 12000 ticks to the end). The land battle is also identical at
0 / 1 / 4 / 15 workers (worker-count check above), and every golden is unchanged.
- **`T13.end_tick` (t05).** The CRC capture is the per-system hashers on the job system. One system, `battle.ai` (the tactical AI: threat
  grid per cell and player, plans, task forces), took 2.1 ms p50 per land tick on its own. It feeds about 65k fields one at a time, and each
  went through the out-of-line `StateHasher::bytes()` with a byte-by-byte copy. The typed feeds (`u8`..`u64`, `f32`, `f64`) are now inline
  and OR the value into a pending 64-bit word. That gives the same byte stream and the same digests (the pinned digest test, a new mixed-feed
  equivalence test, and every golden). Engine/core public header change (`sim_crc.hpp`). Each CRC system's hasher is now a profiler zone
  `crc.<name>`, so `--profile` splits T13.
- **First battle tick (t05).** On land the setup was 129 ms. 73 ms of it was the map objects, almost all `TacticalMode::bounds_of` →
  `BattleCatalog::avoid_model` loading each type's ALO the first time it appears, one type after another under the model-cache mutex. Then
  41 ms of land grid (31 ms the region build, 9.6 ms the stamps, the same model loads) and 14 ms of S4 placements. `BattleCatalog::preload_models`
  now loads the battle's types (map placements and participants) on the job system before any spawn, holding the lock only for the cache
  lookups and inserts. Each entry depends only on the files, so the results don't change. `RegionMap::build` runs one job per movement mask
  (13 on the FoC land maps, a component BFS plus a chamfer distance transform over 311 × 311 cells each) with per-mask outputs. That is also
  used by the mid-battle rebuilds after a stamp change (`tick_land`).
- **Space `tac.movement` spikes (t06).** The kinematic space planner calls `SpaceOccupancy::query_segment` for each candidate move. The query
  walked two `std::map`s and re-formed every track piece (frame bounds, velocity with a divide) for each query. Statics and tracks are now
  id-sorted contiguous vectors (same order, so hash and save are unchanged). Track pieces are formed once in `set_track` with the same float
  expressions, and a per-track box cull with a 16-unit margin over the float error skips far tracks. The result is an OR of bits, so it is
  unchanged.
- **Synthetic galactic frame 0 (all-pairs).** No algorithmic change. `compute_all_pairs` already uses the proven-exact parallel per-target
  Dijkstra above 150 planets. What remains is scheduling (start the frame-0 recompute in the background, as `--latency` does): OPT2.

Numbers. This machine was at 100 % CPU throughout (other sessions' gates and tests), so before / after runs were alternated back to back
and the robust statistics are given. MSVC `/O2`, default 15 workers.

| Measure | Before | After |
|---|---|---|
| Land battle, 3000 ticks: `crc.battle.ai` p50 | 2.06 ms | 0.73 ms |
| Land battle: `T13.end_tick` p50 | 3.14 ms | 0.87 ms |
| Land battle: tick p50 / p99 | 6.83 / 23.3 ms | 3.85 / 18.0 ms |
| Space battle, 12000 ticks: `T13.end_tick` p50 | 0.88 ms | 0.35 ms |
| Space battle: `crc.battle.ai` p50 | 0.73 ms | 0.29 ms |
| Space battle: tick p50 | 2.41 ms | 1.59 ms |
| Space battle: `tac.movement` mean / p99 | 0.54 / 6.43 ms | 0.43 / 3.68 ms |
| Land first tick: map objects (`bt_map_load`) | 73 ms | 2.4 ms |
| Land first tick: obstacle stamps / S4 placements | 9.6 / 14 ms | 1.0 / 0.5 ms |
| Land first tick: region build | 42 ms | 20 ms |
| Land first tick: model preload (new, parallel) | — | 30-45 ms |

The first-tick totals were not comparable under this load (one exe gave 206-1853 ms for the same tick), so only the same-run component
times are listed. The model loads now run in parallel instead of inside the spawns. Re-run `bench_msvc.cmd` on a quiet machine to replace
the OPT1 baseline table.

Leftovers (files other sessions are editing, or work for OPT2):
- ~~`CampaignBattles::ai_begin` (tactical AI start: Lua VMs and plans, `campaign_battles_ai.cpp`, A1) is now the largest part of the first
  battle tick, 120-130 ms here.~~ Done in OPT3 t09 (below).
- ~~The `battle.ai` hasher still walks the threat grid cell by cell with vector-of-vector memory lists (`engine/ai/tactical`, being edited
  by the a1i sessions). Flattening it is a t06 follow-up.~~ Done in OPT3 t09.
- ~~`bt_map_load` itself and the space setup (`tactical_battle_bt.cpp`, skai-skirmish branch).~~ OPT3 t09: the table rebuilds and model
  loads of the setup; what is left is listed there.
- The frame-0 all-pairs recompute in the background (OPT2).

## OPT3 t09: battle-setup spikes; OPT2 t08: battle-AI measurement (2026-11-02)

The first battle tick was the only tick over 33.3 ms (OPT1: land 104 ms, space 67 ms). Profiled with temporary zones (removed again).
Before the fix, on this loaded machine: the tactical AI's plan load took 313 ms of the land campaign battle's first tick and 310 ms of the
space one. Building the battle tables took about 150 ms of the land skirmish's, half of it in model-file parsing. Three fixes, all with
identical results:
- **Plan load in parallel** (`PlanScriptRuntime::load`, `engine/ai`). Every plan file gets its own fresh Lua VM (54 land / 27 space
  plans, each running the PG library scripts and `Definitions()`). The VMs are now built through a `ParallelFor`, one job per file, and
  committed in path order (`LuaVm::claim_ownership`, then the duplicate check, the VM count and the diagnostics in the old order). The
  shipped `Definitions()` only set globals and call `DebugMessage`, and the bindings only attach registry pointers, so the jobs share
  nothing. The tactical AI passes its `lua_gc_pf` (the SimHost job system); the galactic AI still loads serially (a follow-up).
  `alamo_ai_galactic_tests` `test_plan_load_parallel`: a 2 / 4 / 8-thread load equals the serial load (library, pools, pristine
  digests, VM count, diagnostics including a script error, a missing file and a duplicate plan).
- **Battle tables built incrementally** (`BattleCatalog::begin_battle` / `extend_battle`, `TacticalBattle::tables_extend`). The setup
  appends types to its list several times (prepass, pads, abilities, hangars). Each time, all tables were rebuilt from the start. The
  build is a left fold over the type list, so each step now builds only the new types. A new type's model data (bones, avoid
  model, land stamp extents) is preloaded in parallel first, parsing each ALO once for all three cache entries (`BattleCatalog::preload`).
  The setup's own model preload (OPT3 t05) moved after the prepass, so a land skirmish's pads are in it.
- **`battle.ai` hasher flattened** (`TacticalPerceptionGrid::hash`): the grid is serialised into one buffer and hashed with one
  `bytes()` call (same byte stream, same digests).

Checks: per-tick CRC logs byte-identical to the unchanged build, full length, on the land campaign battle
(`--skirmish-ai foc-land-campaign --ticks 20000`), the space battle (`foc-station --seed 3 --ticks 20000`) and the land skirmish
(`foc-land --ticks 3000`). The land skirmish and the space battle are also identical at 0 / 1 / 4 / 16 workers. After merging main
(A1j), `build_gate.cmd`: clang 163/163 and sanitizer 54/54 passed with 0 warnings; MSVC built with 0 warnings and 158/163 passed, the other 5 (`alamo_commands_tests`, `alamo_ai_tests`, `alamo_ai_tactical_tests`, `alamo_ai_story_save_size_late`, `alamo_render_galactic_headless_check`) timed out under load and passed on a `-j 2` re-run. Every golden passed, including the smoke and land-outcome worker-count entries. After merging main again (Q2F t09 goldens), the gate passed clean: MSVC and clang 163/163, sanitizer 54/54, 0 warnings.

First battle tick, before / after (ms, MSVC `/O2`, 15 workers). The machine was at 100 % CPU from other sessions the whole time, so these
are minima of alternating runs (before and after back to back), several times the quiet-machine values; the OPT1 table's quiet first
ticks were land 104 ms and space 67 ms. Batch 1 ran under lighter load than batch 2.

| Batch (runs per exe) | Land campaign battle (frame 22) | Land skirmish (frame 1) | Space battle (frame 33) |
|---|---|---|---|
| 1 (3): before | 403 | 884 | 347 |
| 1 (3): plan load in parallel | 127 | 605 | 92 |
| 1 (3): + incremental tables, preload after the prepass | 124 | 436 | 79 |
| 1b (4): the same / + one parse per ALO (final) | 282 / 232 | 583 / 400 | 220 / 157 |
| 2 (5): before / final (heaviest load) | 514 / 546 | 1248 / 685 | 464 / 424 |

On the first two batches the first tick is 3-4.5 x shorter in space and on the land campaign battle and about 2 x on the land skirmish.
Batch 2 was too noisy to show more than the skirmish gain (medians 2140 / 1405 ms, space 836 / 553 ms). Whether the quiet first tick is now
under 33.3 ms needs a quiet re-run (`bench_msvc.cmd`); scaling batch 1 by its before / quiet ratio (3.9 land, 5.2 space) gives roughly 32 ms on the land campaign battle and 15 ms in
space (an estimate, not a measurement). `bench_msvc.cmd` was not re-run: the machine was still at 82 % CPU (5 other `alamo_headless` runs) at the end, so the OPT1 table above is unchanged.

What is left of the setup spike (land skirmish, same profile): the land obstacle stamps (`mv::stamp_map_obstacles`, Fine_Tune ray
tests, a quarter to a third of the setup; `engine/tactical/movement`, the per-object-pass session's area), placing the start forces and the
pre-built base, and the region build. If the first tick still exceeds 33.3 ms on a quiet machine, the next step is OPT2 work:
spreading setup over ticks or preparing it before the battle starts (that moves goldens).

**OPT2 t08 measurement (battle AI, Lua).** New profiler zones: `tac.ai` (the tactical AI's update), `tac.ai.perception` (snapshot,
threat grid, signals), `tac.ai.p0`..`p8+` (each AI player's service: goals, plans, free store) and `lua.exec` (outermost Lua
execution on a thread: `run`, `require`, `call`, `service_threads` and the AI binding calls; nested calls are not counted twice).
The galactic AI already had `T7.p<n>`. Runs on the merged branch at the default worker count. The machine was at 100 % CPU (about 10
other `alamo_headless`, 12 ctest and 17 compiler processes), so absolute times are several times too high; the shares are what
count:

| Scenario (ticks run) | AI share of tick time | AI per tick mean / p50 (ms) | perception share | per player (p0 / p1) share | `lua.exec` share | `lua.exec` / AI |
|---|---|---|---|---|---|---|
| FoC land skirmish (`foc-land --ticks 7000`) | 20.1 % (`tac.ai`) | 18.0 / 6.8 | 3.7 % | 9.1 / 7.2 % | 0.1 % | 0.5 % |
| FoC land campaign battle (`foc-land-campaign`, ends at 2074 since A1j) | 13.8 % | 19.3 / 4.6 | 2.5 % | 6.2 / 4.8 % | 0.2 % | 1.5 % |
| FoC space battle (`foc-station --seed 3`, 14433 ticks) | 5.1 % | 1.7 / 0.78 | 1.3 % | 2.0 / 1.9 % | 0.3 % | 5.5 % |
| Galactic HotM, every faction an AI (`--economy --galactic-ai --ticks 18000`) | 47 % (`T7.p*`) | 0.41 / 0.19 | — | 21.3 / 18.8 % | 3.1 % | 6.6 % (upper bound: includes story scripts) |

What it says: the Lua scripts are a small part of the AI's cost (0.5-1.5 % of the battle AI's time on land, 5.5 % in space, under 7 % in the galactic AI). Almost all AI time is the C++ work behind each player's
service (goal evaluation, plan selection, task forces) and the shared perception. Running the AI players' Lua in parallel
(`docs/OPT_RESEARCH.md` section 5) would therefore save at most the `lua.exec` share, and it needs per-player id spaces, command
queues, RNG streams and evaluator VMs. **Not worth it.** If the battle AI needs to be faster, profile inside `tac.ai.p<n>`
(per-player services in parallel need the same infrastructure; the perception refresh is a per-cell pass that could use per-index
outputs).

## Lua GC schedule (OPT2 t04, 2026-11-02)

Lua 5.0.2's collector is unchanged (whole-heap mark and sweep, no incremental GC). What changed is when it runs:
- **Staggered scheduled collections** (`engine/lua` `gc_schedule.hpp`): each host collects its VMs at one fixed point of its
  tick, before any of its scripts run: the galactic AI once per frame (free stores by player, plans by id, `Script_` evaluators
  by name), the tactical AI at the start of `update`, the story session at the start of `service`. VM v is due on tick t when
  `(t + slot(v)) % 32 == 0` and its byte count is at least 67 % of its automatic threshold. `slot(v)` is a hash of the VM's
  threshold, so it is fixed between two collections, differs from VM to VM and is saved with the VM (no new save data). After a
  scheduled collection the threshold is 150 % of Lua's own (3 x live instead of 2 x), so a VM is collected about as often as
  before but at the scheduled point, with 1 x live of headroom per period before the automatic backstop fires inside a script.
- **Parallel collection**: the due VMs of one host are collected through its `ParallelFor` (the SimHost job system: tactical AI
  via `BattleAi`, story via `SimHost::start_story`, galactic AI via `GalacticAiConfig::lua_gc_pf` in `alamo_headless`). Per-VM
  work only: Lua 5.0.2 keeps every collector structure in the state's own `global_State`, and the build patches already make
  collections address-independent. `__gc` finalizers are split off (`alamo_luaC_collect`, a build patch of `lgc.c`) and run
  afterwards on the sim thread in list order. `alamo_game`'s galactic AI (`apps/game/game_session.cpp`) passes the host's
  `ParallelFor` too since OPT2 t09 (2026-11-02; the smoke CRC goldens are unchanged).
- **Profiler zones**: `lua.gc` (every collection's mark / sweep, on any thread), `lua.gc.auto` (the automatic ones inside a
  script call: the spikes this is about), `lua.gc.batch` (wall time of a host's scheduled batch).
- **Determinism**: no shipped FoC / EaW script (584 Lua files) uses weak tables, `__gc`, `newproxy` or `gcinfo`, so collection
  timing is invisible to scripts and no CRC golden moved. `alamo_lua_gc_tests` checks identical traces, heap accounting and
  save images serially and on 0 / 1 / 4 / 16 workers (8 VMs, ticks with 2 or more VMs collected at once), save / load
  mid-schedule (the loaded run continues exactly like the uninterrupted one) and that a VM is never collected during a call.

Numbers (`alamo_headless --profile`, the OPT1 scenarios at the default worker count; the synthetic 400-planet run has no Lua VMs).
Ticks with a collection are exact (a function of the run). The "before" milliseconds were measured while other sessions ran about
14 `alamo_headless` processes and two games on this PC, so they are upper bounds; the "after" ones come from a re-run with 3 other
headless processes (the first after run, under the full load, had one 31.5 ms space batch: preemption).

| Scenario | Before: ticks with an automatic collection, max ms | After: automatic (inside a script call), max ms | After: scheduled batches, max ms |
|---|---|---|---|
| Galactic HotM, every faction an AI (18000 ticks) | 110, 1.9 | 34, 0.51 | 69, 0.53 |
| FoC land battle | 88, 4.2 (frame 22, battle setup) | 13, 2.8 | 77, 1.1 |
| FoC space battle with station | 45, 1.4 | 9, 1.3 | 41, 0.91 |

What it says: Lua GC was never a spike source here (at most a few ms for one collection, under 0.5 % of tick time). The schedule
moves 70-85 % of the collections out of script calls to a known point. The remaining automatic ones come from VMs that allocate
more than their headroom within one period (not traced per VM; a fresh plan VM loading its whole script is the likely case). Several VMs rarely fall due on one tick (the slots are staggered),
so the parallel batch matters only for bursts. Running the AI players' scripts themselves in parallel was investigated and left
as OPT2 t08 (`docs/OPT_RESEARCH.md` section 5: the AI bindings write shared AI state and draw from the one synced RNG stream).

## AI scheduling (OPT2 t01 / t02, OPT3 t04, 2026-11-02)

Two changes to *when* the AI thinks; what it computes is unchanged. Both are scheduled by tick number only (rule 3), and
`alamo_headless --ai-latency 0 --no-ai-stagger` (galactic and skirmish AI runs) restores the old timing: with those two knobs every
golden of the suite passed unchanged before the defaults were switched (MSVC, 2026-11-02), so the goldens that moved below moved
for timing only.
- **Stagger (t01).** `AiContext::set_stagger(slot, slots)`: each AI player's Perception / Goal / Planning / Execution / Learning
  clocks start `(slot * period / slots) % period` frames after initialisation, `slot` = the player's ordinal among the world's AI
  players (by player index), `slots` = their number. Players on the same period no longer all plan on the same frame. Galactic
  (`GalacticAiConfig::stagger`) and tactical (`TacticalAiConfig::stagger`, `BattleAi` via `TacticalAiSetup::stagger`); default on.
- **Fixed-latency background jobs (t02).** `alamo/ai/latency_job.hpp`: `LatencyJob<T>` kicks a computation on tick N from a copy
  of its inputs and applies the result on tick N + k, never earlier, blocking on N + k when the job is late. The host hook
  `BackgroundStart` (like `ParallelFor`; `jobs::make_background_start` runs it as one Background-priority job, a serial host runs it
  inline at the kick with the same result) keeps engine/ai free of engine/jobs. Converted:
  - galactic AI all-pairs path recomputes (`AiPathFinder`, Q11 cadence every 90 frames): k = `galactic::kDefaultAllPairsLatency`
    = 15 frames by default (`GalacticAiConfig::path_latency`). The creation recompute stays synchronous (the AI needs paths on
    its first frame). `alamo_headless --synthetic`'s `--latency` defaults to the same 15 (was a flag, default 0).
  - tactical zone reachability (land path finder, `ZoneReachability::compute` / `adopt`): the execution-service recompute is
    kicked with the threat costs of that frame and adopted 15 frames later (`TacticalAiConfig::path_latency`).
  The CRC hashes a job's ready frame only while it is in flight, so the latency-0 path hashes exactly what it did before.
- **Save rule (OPT3 t04).** Stagger needs no save data: the clocks' next-due frames are already saved, and the phases are a
  function of the player list. An in-flight job is saved as its inputs plus kick / ready frame (`AiPathFinder` save version 2,
  tactical AI save version 9); a load re-runs it inline from those inputs and applies it on the saved ready frame, so the loaded
  run applies the same matrices on the same tick. Tests: `alamo_ai_tests` (`test_path_finder_background`: save at three points of a
  job's life, 0 / 1 / 4 / 16 / default workers), `alamo_headless_save_hotm_scheduled` and
  `alamo_headless_save_skirmish_foc_land_scheduled` (`--save-when scheduled`: save on the first tick at or after `--save-at` with
  a job in flight, `--save-check` compares the loaded run's CRC log with the uninterrupted one).
- **Not converted (follow-ups).** Galactic AI perception (`GalacticPerception::update`) runs once per frame and the same frame's
  readers use its results; investigated in OPT2 t11 below and left serial. Task-force planning (the PP solve) drew from the
  synced RNG in proposal order and its accept was synchronous; OPT2 t10 gave it per-solve RNG streams, a pending-goal state and
  a background job ("Slot 10 accept (OPT2 t10)" below).
- **`alamo_game`** passes the same background hook to its galactic AI (`game_session.cpp`; results unchanged, the smoke goldens
  did not move).

Moved goldens (regenerated with MSVC; each run again with one knob against the old golden to name the cause):

| Golden | Moved by | First difference |
|---|---|---|
| `ai/tests/golden/ai_galactic_synthetic_crc.txt` | stagger (p1 plans 1 frame later) | the 15-min synthetic campaign cascades: end state E=0 R=8 became E=4 R=4 |
| `ai/tactical/tests/golden/ai_tactical_world_crc.txt` | stagger + latency | p1 decisions 1 frame later; move targets from paths applied 15 frames after the kick |
| `apps/headless/testdata/galactic_ai_hotm_golden.txt`, `galactic_ai_hotm_human_golden.txt` | stagger only (latency 15 alone matches) | AI decision log |
| `apps/headless/testdata/skirmish_ai_synthetic_golden.crc`, `skirmish_ai_foc_golden.crc` (space) | stagger only (latency alone identical) | tick 114 / 34, system `rng` (AI draws on other frames) |
| `apps/headless/testdata/skirmish_ai_foc_land_golden.crc`, `skirmish_ai_foc_land_campaign_golden.crc` | stagger and latency (each alone moves it) | tick 2 / 23, system `battle.ai` (the hash holds an in-flight job's ready frame) |

`replay_golden.crc` did not move: the recorded stream stores its latency (0) in its metadata and replays with it.
The `alamo_game` smoke goldens `game_smoke_campaign.crc` and `game_smoke_land.crc` moved too (MSVC = clang; first difference
campaign tick 1 `ai.goals` / `rng`, land tick 51 `battle.ai`; `alamo_game` has no timing knobs, so these were not re-run at the old
timing). Space skirmish outcome seeds: 6 and 9 now end, 2 reaches the 14400-frame cap (it ends at 5724 with `--no-ai-stagger`).

Worker-count check (OPT3 t07): per-tick CRC logs identical at 0 / 1 / 4 / 16 / 15 workers and MSVC = clang for HotM with every faction
an AI (18000 ticks), the 400-planet synthetic (`--record-stream --synthetic 400 --ais 4 --frames 900`, latency 15, then `--replay`),
the FoC space battle with station (`--skirmish-ai foc-station --seed 3`, 9598 ticks), and in the suite `alamo_headless_galactic_ai_human*`
and `alamo_headless_skirmish_ai_foc_land_outcome*` (land). `alamo_ai_tests` runs the path finder's background job on job systems with
0 / 1 / 4 / 16 / default workers, saving mid-job. `build_asan.cmd` passes with UBSan and with `ALAMO_SANITIZE=address`.
Re-run after merging OPT3 t09 (#42) and OPT2 t03 (#43): every golden above still matches unchanged (MSVC and clang 166/166),
`--ai-latency 0 --no-ai-stagger` still matches main's six headless goldens, and the check above (plus land, `foc-land`, 7000 ticks)
is again identical at 0 / 1 / 4 / 16 / 15 workers with MSVC = clang.

Numbers (`--profile --profile-budget 33.33`, the OPT1 scenarios at the default worker count, one exe; "old" = `--ai-latency 0
--no-ai-stagger` (`--latency 0` for the synthetic), run back to back with "new" while other sessions loaded the PC, so compare
the pairs, not with the OPT1 table):

| Scenario | Old: tick mean / p99 / max (ms) | New: tick mean / p99 / max (ms) | What moved |
|---|---|---|---|
| 400-planet synthetic, 4 AIs (900 ticks) | 0.46 / 2.96 / 12.5 | 0.48 / 1.64 / 5.4 | the frame-0 all-pairs (11.9 ms) runs in the background; worst tick is frame 15 waiting for it (the run is not paced: 15 ticks are ~6 ms here, 500 ms in the game) |
| HotM, every faction an AI (18000 ticks) | 0.21 / 0.55 / 3.0 | 0.20 / 0.50 / 4.8 | `ai.planning` / `ai.execution` active on 17893 / 18000 ticks instead of ~6000, max 0.42 -> 0.37 / 0.30 -> 0.18 ms; the max tick is `ai.goals` (every frame, not staggered) |
| FoC land battle (`foc-land-campaign`) | 4.35 / 14.5 / 309 | 4.38 / 14.0 / 297 | `ai.execution` p99 0.64 -> 0.06 ms, max 0.88 -> 0.23 ms (zone-graph recompute off the sim thread); the max is the first battle tick (setup) |
| FoC space battle with station | 1.35 / 2.93 / 233 | 1.22 / 2.90 / 248 | `ai.execution` max 0.47 -> 0.25 ms; the max is the first battle tick (setup) |

The AI's own per-tick peaks were already small next to battle setup and `ai.goals` (the goal evaluation runs every frame for every
player: the next candidate for spreading, OPT2 t03 / follow-ups).

## Galactic perception latency (OPT2 t11, 2026-11-03)

Question: should the galactic AI's per-frame perception update become a fixed-latency background job (`LatencyJob`, as the
all-pairs in t02)? Answer: **no, leave it serial; t11 can be closed.** It costs too little to matter, and a latency changes
what the AI decides. A prototype was built and measured; it is not merged (branch `claude/opt2-gal-perception-prototype`,
commit 08cda18). The profiler zones it needed are kept.

**Cost.** New zones: `ai.gal.snapshot` (`snapshot_from_sim`), `ai.gal.perception` (`GalacticPerception::update`, split into
`ai.perc.observe` / `ai.perc.evaluate`), `ai.gal.memcopy`, `ai.gal.refresh`, `ai.gal.danger` (the danger mask,
`ai_dangerous`) and `ai.perc.fill_view`. `alamo_headless --profile`, MSVC, default worker count. The galactic runs were on a
nearly idle PC (CPU 0-12 %), the synthetic run at about 70 %:

| Scenario | Tick mean / p99 (ms) | Perception mean / p99 / max (ms) | Share of `ai.*` |
|---|---|---|---|
| HotM, every faction an AI (13 planets, 4 AIs, 18000 ticks) | 0.21 / 0.55 | 0.008 / 0.014 / 0.07 | 4.8 % (8.9 % with snapshot and danger mask) |
| HotM, scripted human Rebel (36000 ticks) | 0.20 / 0.76 | 0.007 / 0.012 / 1.5 (one preempted tick) | 4.5 % (11.6 %) |
| FoC `Sandbox_Equal_Footing_Underworld`, the largest FoC galaxy (56 planets, 4 AIs, 18000 ticks) | 0.55 / 1.69 | 0.036 / 0.061 / 0.11 | 7.9 % (11.2 %) |
| 400-planet synthetic, 4 AIs (`--synthetic 400 --ais 4`, 900 ticks; its own perception in `main.cpp`) | 0.49 / 1.51 | 0.27 / 0.44 / 0.70 (system `sys.ai.perception`, incl. 4 x `fill_view`) | 56 % of the tick (no other AI here) |

On real galaxies, perception's p99 is 0.06 ms at most, 0.4 % of the 16.7 ms p99 budget; `ai.goals` (63 % of the tick) is
what costs. Only the synthetic shows a visible share, and it is still 0.44 ms at p99. 82-92 % of perception time is `evaluate`: the four
consumer paths go through the general equation evaluator for each (AI, planet) pair, although each is a single token.

**Who reads perception in the same frame.** The concern in t02 was that the token resolver reads perception memory. It does
not: `GalacticSimTokenResolver` receives the perception resolver (`set_s2`) but never calls it. Every `EnemyForce.*` token
in goal, planning and Lua (`EvaluatePerception`, `FindTarget`) evaluations is computed live from the sim
(`galactic_ai_planet_force`, with the resolver's own leaf cache). So the per-frame `w.mem` copy and `w.s2` are dead work
(0.001 ms). The only readers of the published results are:
- the danger mask (`ai_dangerous` -> `fill_view`: `HasSpaceForce` plus the snapshot's starbase / held-by-ally rules). It feeds
  the reachability floods: goal `blocked()` on every proposal pass, `source_planets` (cached 10 s) for planning / PP, and
  market delivery reachability (production, during T5, so already one frame old). `TargetPlayerID` also feeds the
  `ai_eval_failed` / `ai_perceived_non_neutral` PlayerView fields;
- the path finder inputs (`all_pairs_input`: Node_Weight and the fog gate), read when the 90-frame recompute starts and applied
  15 frames later (t02).
With a latency of k frames, the danger mask and passability would lag k frames. The node values would then be k + 15 frames
old when applied. A split variant (observe and the danger bits inline, Node_Weight deferred) would remove the lag but save
almost nothing: the danger bits come from the same `evaluate` calls. The cheaper fix, if this ever matters, needs no latency
and changes no result: read the four bare-token paths directly from memory instead of through the evaluator, and compute
Node_Weight only for an AI whose path recompute starts this frame.

**Prototype** (`GalacticAiConfig::perception_latency`, `alamo_headless --ai-perception-latency F`, default 0). The first update
runs inline. After that, a job kicked on host frame N from a copy of N's snapshot is published on N + F, and the next job is
kicked then. Only one job runs at a time, so the AI sees perception refreshed every F frames and F to 2F - 1 frames old. The
job's buffer carries the memory from one kick to the next. Measured:
- Knob off: final CRCs and AI logs equal to the goldens.
- Knob on, worker count: CRC logs identical at 0 / 1 / 4 / 16 / 15 workers. Checked on HotM and on Equal Footing with an idle
  human (seed 2), 18000 ticks each, latency 15.
- Tick time: the sim thread's perception time drops to the publish copy (mean < 0.001 ms). Tick mean and p99 do not move
  measurably: HotM 0.209 / 0.546 -> 0.217 / 0.621 ms, Equal Footing 0.552 / 1.692 -> 0.570 / 1.858 ms. The run-to-run noise
  is larger than the 0.03 ms saved.
- Drift: compared the decision log, per-system CRC logs and end state against the knob-off run for the same seed. Campaigns:
  HotM + scripted human, Equal Footing all-AI, and Equal Footing + idle human Empire; seeds 1-4, 36000 ticks each, so 12 runs
  per latency. Plus HotM all-AI and Equal Footing all-AI, 18000 ticks, latency 15.
  - Latency 15: the decision log was identical in every run and the end state (owners, credits) in 12 / 12. CRC logs
    differ in 6 of the 14 runs (final CRC in 5), and only in `ai.goals` (first tick 1654 on HotM, 3106-29452 elsewhere): proposal-pass counters
    and the proposed list change when a reachability-blocked candidate is skipped in one run and evaluated in the other. In one run
    (Equal Footing + human, seed 2) the synced RNG also differs on 20 ticks from 7854, then matches again.
  - Latency 30: 11 / 12 identical decisions and end state. In Equal Footing + human, seed 2, the first difference is a plan
    ending one frame earlier (frame 7937), after which about 2450 decision lines differ on each side. At 36000 ticks, Rebel holds 19
    planets and Underworld 18, instead of 18 and 19 (3 ownership changes instead of 4).
- Save / load with a job in flight: not implemented (the prototype is not kept).

**Recommendation.** Do not ship perception latency in either the full or the split form. On the real galaxies there is no
gain to measure (≤ 0.06 ms p99 against 16.7 ms), and it adds a behaviour change (rare at 15 frames, outcome-changing at 30),
save state for the in-flight job, and a second copy of perception state. If bigger galaxies or the synthetic ever make
`ai.perc.evaluate` matter, use the direct-read and on-demand Node_Weight changes above first; neither changes a golden.
Separately, removing the dead `w.mem` / `w.s2` copy is a clean-up for whoever next edits the token resolver. Whether the
goal resolver should read fogged perception memory instead of the live sim (the `set_s2` hook) is a parity question for
S2 / CP3, not a scheduling one.

## Serial per-object passes on the job system (OPT2 t03, 2026-11-02)

Ranking (`alamo_headless --profile` with temporary zones, 15 workers): in the FoC land battle (`--skirmish-ai foc-land`) nearly all
of `T5.battles` is the tactical AI, not the battle passes. Per tick, p50 / p99: goal system `ai.c.goals` 3.5 / 177 ms (inside it,
candidate perception values and slot 10 accept (the PotentialPlan solve) about equal), `BattleAi::fill` (the AI's per-object view of
the battle) 1.6 / 19 ms; the battle's own passes are small (`tac.fog` 0.08, `tac.movement` 0.03, `tac.combat` 0.03, `tac.cleanup`
0.02, `tac.trees` 0.02, `tac.anim` 0.000 ms p50). In the FoC space battle (`foc-station --seed 3`) `T5.battles` is 0.6 ms p50:
movement and combat 0.2-0.3 each (both already parallel), accept 0.32, fill 0.14, values 0.11. The galactic run's time is the
galactic AI (CP3, not here); `gal.sim` is 0.01 ms.

Changed (results identical for any fan-out):
- **`BattleAi::fill`** (`apps/sim/battle_ai.cpp`): the listed objects are collected in store order, their entries built through
  the SimHost `ParallelFor` (grain 64, one output slot per object, const reads only), then the UNIT_AI re-projection stamps are
  drawn serially in store order (the only synced draws).
- **Goal-candidate values** (`ai/ai_goals.cpp`, "BATCHED VALUES" in `ai_goals.hpp`): a mode with `candidate_pf()` walks a proposal
  slice first, evaluates the candidates whose perception function has no `Script_` reference (Lua and synced draws) on the job
  system (`TacticalAiConfig::pf`, set by `BattleAi`), then runs learning terms, accept and insert for every candidate in visit
  order. Slices with fewer than 4 such values run them in place (space: 2-3 values of about 20 us; land: about 5 of 120 us).
  The galactic AI passes no `candidate_pf` (unchanged; its goal scoring is CP3). The tactical leaf cache's hit counter (saved, not
  hashed) now counts only the serial evaluations, so it stays independent of timing (`LeafCacheUncounted`).

Left serial: slot 10 accept (the PotentialPlan solve drew synced random numbers a data-dependent number of times, so a candidate's
solve could not start before the previous ones finished; OPT2 t10 below found most of its cost in the host's build-pad query and
moved the solves onto their own streams); fog, anim, trees and
cleanup (0.1 ms or less per tick, below the cost of a dispatch, and fog writes one shared grid); movement and combat were already
parallel (combat's phase D stays serial for its draws). `crc.battle.ai` (about 0.9 ms per land tick) is OPT3's.

Before / after (`origin/main` 15cba66 against this branch, 15 workers, both at AboveNormal priority and run alternately because
other sessions kept the PC at about 100 % CPU; two rounds each, so treat single numbers as ±20 %):

| Scenario | Tick p50 ms | Tick p99 ms | Tick mean ms | `T5.battles` p50 / p99 ms |
|---|---|---|---|---|
| FoC land, 2000 ticks, before | 3.2 / 4.7 | 73 / 95 | 11.1 / 13.5 | 2.2-2.8 / 72-93 |
| FoC land, after | 2.3 / 3.8 | 28 / 39 | 7.2 / 9.9 | 1.2-1.9 / 27-37 |
| FoC space, 4000 ticks, before | 1.25 / 1.16 | 3.39 / 3.40 | 1.45 / 1.35 | 0.47-0.52 / 1.8-2.0 |
| FoC space, after | 1.28 / 1.31 | 3.47 / 3.51 | 1.48 / 1.50 | 0.51-0.53 / 2.0-2.1 |

Max tick (frame 1, battle setup, 0.9-1.6 s) is unchanged. Per pass (land, earlier zoned runs): fill p50 1.6 → 0.70 ms, p99 19 →
1.3-3.5 ms; job slots busy 1 % → 5 %; worst non-setup ticks 170-190 → 50-84 ms. Space is unchanged within noise (fill gains about
70 us, the small value batches run in place).

Determinism: per-tick CRC logs byte-identical to `origin/main` for land (7000 ticks) and space (to the end, 14433 ticks) at 0, 1,
4, 16 and 15 workers and for galactic HotM (18000 ticks) at 15; every golden unchanged. `alamo_ai_galactic_tests`
`test_goal_batched_values` compares no batching, serial, reverse-order and 4-thread fan-outs (CRC per tick, draws, evaluation
count and the serial call trace).

## Slot 10 accept (OPT2 t10, 2026-11-03)

Slot 10 (`PlayerAi::accept`: the plan pick, then the first PotentialPlan solve) was half of the land battle's tick time on
`origin/main` (temporary zone, 7000 land ticks: 2.6 ms of a 5.2 ms mean tick, p99 16.5 ms). Two changes:

- **Host answers once per slice [port, same results].** Most of that time was not the solver but the host's build-pad query
  (`BattleAi::build_pads` re-listed every capture-point blocker for every pad, for every candidate). `BattleAi` now lists the
  blockers once per query and remembers per object type whether it can block a capture point; the tactical AI asks the host
  for pads, reinforcement pool and population once per proposal slice (`PpSliceHost`, opened by the new `slice_begin()` /
  `slice_end()` hooks of the goal system and cleared by any executed order). CRC logs stayed byte-identical to main (land,
  land campaign, space, HotM at 0 / 1 / 4 / 16 / 15 workers).
- **Batched accept on own RNG streams [port, moves results]** (`ai_goals.hpp` "BATCHED ACCEPT", `TacticalAiConfig::pp_streams`
  / `pp_latency`). Each candidate's plan pick and solve draw from a stream seeded by (session seed, frame, player, goal source,
  target) instead of the synced stream, so no solve waits for the previous one. In visit order `accept_prepare()` picks the plan
  and copies the solve's input, replacing the two world callbacks by their answers (`can_build_now` for the plan's team types,
  `enemy_strength` for the Contrast masks, `enemy_strengths_at` sums all masks in one pass, same object order); the goal is
  pending. After the slice the solves run as one `LatencyJob` (kick on frame N, applied at the start of N + `pp_latency`,
  default 1, before that frame's slice; latency 0 runs them on the `ParallelFor` inside the slice). The apply
  (`accept_finish()`: veto, PotentialPlan record, then the budget test and insert) is serial in visit order. No new slice starts
  while solves are pending; a pass whose last slice left solves pending commits after the apply; `block_proposals()` drops
  pending goals. The plan's re-solve at Commit (`resolve_pp`) still reads the live world, so a picked unit that died between
  kick and apply is handled as before.
- **Knobs.** `alamo_headless --no-pp-streams` (`TacticalAiSetup::pp_streams = false`) restores the synced draws in proposal
  order: with it every golden from before this change passes unchanged (full MSVC ctest, 166 / 166). `--pp-latency F` sets the
  apply delay (0 = in the slice).
- **Saves.** Goal system save version 2: the pending goals, their ready frame and whether the pass waits for them. Tactical AI
  save version 10: when solves are pending the save waits for the job and writes its results (plan, ok, picks), not its inputs
  as the OPT3 t04 path job does: the inputs hold copies of the pool, pads and enemy strengths and are much larger than the
  picks, and the job is pure, so waiting for it at a save costs one batch of solves (about 1 ms). The loaded run applies the
  saved results on the saved ready frame. Tests: `alamo_ai_galactic_tests` `test_goal_batched_accept` (latency 0 / 1, serial /
  reversed / threaded fan-out give the same CRCs, a save while pending continues identically, a block while pending),
  `alamo_ai_tactical_world_tests` `test_pp_pending_save` (save on the first frame with solves pending, 900 frames after the load
  identical, every truncation / corruption refused), `alamo_headless_save_skirmish_foc_land_scheduled` (`--save-when
  scheduled` counts pending solves as a job in flight, `TacticalAiWorld::background_jobs_in_flight`).

Moved goldens (MSVC; clang matches), first differing tick and cause: plans now start one frame later (the apply delay) and the
plan picks / solve draws come from the per-candidate streams, so the AI's command stream changes first:

| Golden | First diff | Run length before → after | Winner |
|---|---|---|---|
| `skirmish_ai_synthetic_golden.crc` | tick 114 | 730 → 697 ticks | Empire, both |
| `skirmish_ai_foc_golden.crc` (space, Kuat) | tick 34 | 5868 → 6627 ticks | Empire, both |
| `skirmish_ai_foc_land_golden.crc` | tick 14 | 3600 ticks (cap) | no end in the window, both |
| `skirmish_ai_foc_land_campaign_golden.crc` | tick 27 | 3008 → 2819 ticks | Empire (Rebel retreat), both |
| `ai_tactical_world_crc.txt` (synthetic mock) | frame 3 (plans at 4) | 176 lines | - |
| `game_smoke_skirmish.crc` / `.rec` | tick 23 | 3122 → 3128 recorded ticks | - |
| `game_smoke_campaign.crc` | tick 1019 (its battle) | same | - |
| `game_smoke_land.crc` / `.rec` | tick 57 | same length | - |

The smoke recordings move because the smoke scripts wait on game state before their next input. HotM (galactic AI only) is
unchanged. Outcome seeds, `--no-pp-streams` against the new default (same winner on every one): FoC space seeds 1-16 all Empire
(seed 2, which reached the 14400 cap before, now ends at 8158; it is still not in the test list since the stall it hit is not
fixed); `foc-station` seed 2 and default seed Rebel both (victory all-retreated → all-destroyed); land campaign seeds 6 and 1
Empire both; the 7000-tick land outcome keeps its skirmish base (`--expect-skirmish-base` ok, 6 structures started instead of 0).

Worker counts (new defaults, 0 / 1 / 4 / 16 / 15 workers, `--crc-out` compared byte for byte): land 7000 ticks, land campaign,
space `foc-station --seed 3`, HotM 18000 ticks all identical across counts; `--pp-latency 0` (solves on the `ParallelFor`) land
and space at 0 / 4 / 15 identical; synthetic `--check-workers` at latency 0, 1 and 3 identical. HotM is byte-identical to main.

Before / after (`alamo_headless --profile`, 15 workers, two rounds each, run alternately; machine otherwise idle: no other
alamo / compiler / ctest process, CPU 0-63 % sampled around the runs; the land and space runs after the change play different
games, so compare distributions). Main = `origin/main` 6d63bcf with temporary zones, cache = the first change only:

| Scenario | Tick mean ms | p50 | p99 | Accept mean / p99 ms |
|---|---|---|---|---|
| FoC land 7000 ticks, main | 5.19 / 5.17 | 2.76 / 2.72 | 20.3 / 20.2 | 2.62 / 16.5 |
| land, cache | 2.71 / 2.69 | 1.81 / 1.81 | 13.5 / 13.4 | 0.171 / 1.03 |
| land, streams (default) | 2.53 / 2.53 | 1.67 / 1.67 | 12.5 / 12.6 | 0.081 / 0.38 |
| FoC space `foc-station --seed 3`, main | 1.17 / 1.17 | 1.07 / 1.07 | 2.80 / 2.80 | 0.240 / 1.05 |
| space, cache | 0.98 / 0.98 | 0.87 / 0.86 | 2.39 / 2.40 | 0.065 / 0.29 |
| space, streams | 1.00 / 1.01 | 0.90 / 0.91 | 2.30 / 2.30 | 0.033 / 0.11 |

`tac.ai.p<n>` (one player's AI service) p99, land: main 10.2 / 10.4 ms, cache 9.4 / 9.5, streams 8.8 / 8.9. The solves themselves
(`tac.ai.pp`, now on a worker during the next frame's other passes) are 0.12 ms mean, 1.0 ms p99 per solve on land and 0.05 /
0.26 ms on space. Max tick is battle setup (frame 1, 190-206 ms) in every run. An earlier pair of rounds under heavy load
(CPU 43-96 %, 3-10 other heavy processes) gave the same picture for the cache change: land mean 8.35 / 7.15 → 4.13 / 2.92 ms,
ticks over 33 ms 92 / 67 → 17 / 1.

Remaining land spikes: `ai.goals` p99 11 ms, max 25-27 ms, now almost all candidate values: OPT2 t03's
batched `evaluate_prepared` (one heavy candidate per slice takes up to 33 ms under load; a slice's values wait for its slowest
one) and the `Script_` evaluators run in place (up to 9 ms; Lua, serial). Fixed in OPT2 t13 (next section).

## Goal-scoring spikes (OPT2 t13, 2026-11-03)

**Where the time went.** Profiled with temporary zones inside the goal system (slice, batched values, in-place values, Commit) and
temporary per-equation and per-leaf timers (removed again), on the FoC land skirmish (`--skirmish-ai foc-land --ticks 7000`), the land
campaign battle (`foc-land-campaign`) and the space battle (`foc-station --seed 3`). On land, one equation made the spikes:
`Need_To_Sell_Tactical_Structure` took 10.8 ms per evaluation on average (max 19.6 ms; 987 evaluations, 10.6 s of the run). It calls
five `Need_*` equations twice each, and they share more sub-equations, so one candidate evaluates a few thousand leaves. Most of those
leaves are cheap or come from the leaf cache. One is not: **`Variable_Self.OpenBuildPadCount {Parameter_Type = ...}`** (cache lifetime 0, so
it is computed every time) ran `type_index()` for every open pad and every listed name. Each call is a case-insensitive scan of every
object type name, and the result fed only the host's `pad_accepts` hook, which no host installs. That leaf was 25.5 s of the 28.6 s spent
in all perception leaves on land: 66,749 calls at about 380 us each. The other heavy equations (`Need_*_Turret`,
`Tactical_Multiplayer_Should_Build_*`) and the in-place `Script_` ones (`Need_To_Secure_Contestable_Land`: Lua plus the same leaf) were slow for
the same reason. The Lua evaluator calls themselves are about 3.5 us each (`TargetHasProperty`). Space has no such leaf: its goal values are
20-35 us per candidate and `ai.goals` p99 is under 1 ms.

**Fix [port, same results]** (`tactical_perception.cpp`, `OpenBuildPadCount`): the listed names' type indices are looked up once per call,
and only when `pad_accepts` is installed. The hook is called with the same arguments in the same order as before, and `type_index()` is a pure
lookup. `alamo_ai_tactical_perception_tests` `test_open_pad_types` covers the hook path: own-type match, unknown names, call order, and stopping
at the first yes. After the fix, the same instrumented land run spends 2.6 s in perception leaves instead of 28.6 s, and
`Need_To_Sell_Tactical_Structure` takes 0.63 ms (max 0.91 ms).

Checks: per-tick CRC logs are byte-identical to `origin/main` (1f09584) for land (7000 ticks), the land campaign battle (to its end, 2819),
space (to its end, 9579) and HotM (18000 ticks). Worker-count check: land and space at 0 / 1 / 4 / 16 / 15 workers all equal main's
15-worker log. Clang's logs equal main's MSVC logs for all four. No golden changed. No parallel code was added.

Before / after (`alamo_headless --profile --profile-budget 33.33`, MSVC `/O2`, 15 workers, main and this branch run alternately, two rounds
of one run each). Other sessions loaded the PC. CPU load and the count of other alamo / compiler / ctest processes were sampled around every run:
round 1 at 74-99 % CPU with 8-10 processes, round 2 for land and the land campaign at 56-84 % with 0-6 processes. Round 2 is the cleaner
comparison. The tick max is battle setup in every run (frame 1 / 22 / 33).

| Scenario, round (load) | Tick mean / p50 / p99 ms | Ticks > 33.3 ms | `ai.goals` mean / p99 / max ms | `tac.ai.p0` / `p1` p99 ms |
|---|---|---|---|---|
| Land 7000 ticks, r1 (heavy): main | 4.07 / 2.36 / 23.9 | 23 | 2.01 / 21.3 / 58.4 | 17.6 / 17.4 |
| Land, r1: t13 | 3.06 / 2.75 / 7.32 | 1 | 0.62 / 3.05 / 11.0 | 1.94 / 1.89 |
| Land, r2 (~60 %): main | 3.07 / 1.93 / 19.7 | 7 | 1.49 / 18.1 / 34.2 | 13.7 / 14.0 |
| Land, r2: t13 | 1.73 / 1.65 / 2.56 | 1 | 0.28 / 1.01 / 4.27 | 0.65 / 0.69 |
| Land campaign (2819 ticks), r1 (heavy): main | 2.95 / 2.46 / 7.88 | 2 | 0.97 / 5.07 / 17.2 | 3.82 / 3.95 |
| Land campaign, r1: t13 | 2.40 / 2.17 / 6.22 | 2 | 0.49 / 2.22 / 16.4 | 1.54 / 1.56 |
| Land campaign, r2 (~60 %): main | 1.63 / 1.37 / 3.68 | 1 | 0.47 / 2.32 / 5.15 | 1.88 / 2.00 |
| Land campaign, r2: t13 | 1.39 / 1.32 / 2.39 | 1 | 0.24 / 0.76 / 4.16 | 0.52 / 0.55 |
| Space (9579 ticks), r2: main | 1.18 / 0.99 / 3.31 | 4 | 0.14 / 0.76 / 5.49 | 0.42 / 0.32 |
| Space, r2: t13 | 1.11 / 1.02 / 2.65 | 2 | 0.14 / 0.75 / 3.13 | 0.42 / 0.35 |

Space is unchanged within noise, as expected: the fix does not touch its path. Its round-2 runs straddled a load jump from 4 % to 100 % CPU
(12 processes). The land campaign's `ai.goals` max of 16 ms in round 1 is one preempted tick (`tac.ai.p1` 16.1 ms, its round-2 max 4.0 ms).

What is left over budget or near it (follow-ups, not goal scoring):
- `ai.goals.commit` 4-7 ms on about 10-13 ticks per land battle: the PotentialPlan re-solve at activation (`plan_activatable`, up to 3.3 ms)
  and plan instantiation (up to 4.3 ms). Re-evaluating the incumbents is at most 0.66 ms.
- `tac.movement`: land 10-12 ms at frames 62-66 (both builds), space 41 ms at frame 4651.
- Battle teardown: the land campaign's last tick (2819) is 26 ms and space's (9579) is 24-39 ms in `T5.battles` outside the `tac.*` zones.
- `tac.ai.perception` on the first AI tick of a battle: 7.6 ms on land, 10.5 ms on the land campaign.
- In the goal values, half of the remaining leaf time is building the leaf-cache key (`canonical_params` text) and the mutex-guarded
  `std::map` lookup (about 1 us per cached leaf). A shared lock and precomputed parameter text would cut it, but goal values are
  no longer a spike source (land `ai.goals.values` p99 about 1 ms).

## Space outcome seed 2 and A1k t01 on the OPT2 timing (OPT2 t12, 2026-11-03)

Kuat (`--skirmish-ai foc`) seed 2 was out of the space outcome list: it reached the cap with OPT2 t01's stagger and, after t10,
still with `--no-pp-streams`. A1k t01 (branch `claude/a1k-space-replan-6392f0`, A1_READINGS §39) is merged here and is the fix:
the Empire's BURNUNITS plan collected every free unit (reserved ones and other plans' members too) and attack-moved on one point
from frame 4906, so every other plan failed its PotentialPlan and the Rebels' last frigate was never attacked. Ablation on seed 2
with `--no-pp-streams`: the collect rule alone ends it (6696), the priority reset alone does not (A1_READINGS §39.7). Seed 2 is
back in `alamo_headless_skirmish_ai_foc_outcome_s*` (all 16; 6 and 9 were already back). The re-plan test checks the §39.1
cadence at the original planning timing (`TacticalAiConfig::stagger` / `pp_streams` off) and the leave / clear pairing at the
default timing too (there a unit can still be a member at frame 3600 and Easy may not re-plan within the run).

| Seeds (`--workers 0 --expect-battle-end`) | origin/main 1f09584 | this merge |
|---|---|---|
| Kuat 1-16, defaults | 16/16 (4885-11590) | 16/16 (5818-9894), Empire all destroyed every seed |
| Kuat 1-16, `--no-pp-streams` | 15/16, seed 2 at the cap | 16/16 (6122-11047) |
| Kuat 2, `--no-ai-stagger --ai-latency 0 --no-pp-streams` | 5724 | 6629 |
| foc-station 1-8 | 5/8 (5, 6, 8 at the cap) | 8/8, Rebel (9480-13985); seed 6 has 1 Lua error (DefendSpaceStation's `Find_Nearest` on its dead Target) |
| foc-land-campaign 1-12 | 12/12, Empire (Rebels retreat) | 12/12, the same winners (2239-3550) |
| foc-land 7000 ticks | skirmish base ok | identical CRC log |

Winners: the same on every seed that ended on main; the three station seeds that reached the cap now end with the Rebels winning.
Victory kinds that changed: station 2 and 7 all destroyed → all retreated, station 3 all retreated → all destroyed.

Goldens regenerated (MSVC; first difference against main; cause from main vs merged under each knob set and an ablation of the two
halves): `skirmish_ai_foc_golden.crc` tick 322 `battle.combat` (priority reset; collect rule alone from 3460; `--no-pp-streams`
1188, original timing 652 as A1k reported), `skirmish_ai_foc_land_campaign_golden.crc` tick 471 `battle.ai` (priority reset only;
original timing 575 as A1k reported), `game_smoke_skirmish.crc` / `.rec` tick 1050 `battle.combat` (3128 → 3649 lines),
`game_smoke_land.crc` tick 1074 `battle.combat`. With both halves switched off the merged build is byte-identical to main on the
two headless goldens. Worker counts 0 / 1 / 4 / 16 / 15 identical (Kuat seeds 1 and 2, foc-station 2, foc-land-campaign 1,
foc-land 7000 ticks).

## Profile after OPT2 (OPT2 t07, OPT3 t10, 2026-11-03)

The OPT1 scenarios re-run on main 1b8c5f5 (after #42-#48), same PC and build type as the OPT1 table, nothing else running (CPU 6-11 %
before and after). `bench_msvc.cmd` plus two more rounds; median of 3 runs each, the runs agree within about 2 %.

| Scenario | Ticks | W15 mean / p50 / p99 / max (ms) | W0 mean / p50 / p99 / max (ms) | OPT1 W15 (2026-10-30) |
|---|---|---|---|---|
| FoC HotM, every faction an AI | 18000 | 0.208 / 0.182 / 0.513 / 4.35 | 0.178 / 0.153 / 0.458 / 4.51 | 0.21 / 0.19 / 0.52 / 3.1 |
| 400-planet synthetic, 4 AIs | 900 | 0.479 / 0.376 / 1.53 / 4.71 | 1.50 / 0.81 / 14.2 / 56.5 | 0.48 / 0.39 / 2.99 / 11.9 |
| FoC land battle (`foc-land-campaign`) | 3014 | 1.38 / 1.32 / 2.21 / 69.7 | 2.02 / 1.82 / 3.47 / 297 | 3.42 / 3.06 / 6.39 / 104.7 |
| FoC space battle with station (`foc-station --seed 3`) | 12173 | 0.890 / 0.799 / 2.15 / 58.5 | 0.922 / 0.812 / 2.40 / 236 | 1.02 / 0.95 / 2.17 / 66.9 |

The land and space battles are not the same games as in OPT1: AI changes since then end the land battle at 3014 ticks (was 12374)
and the space battle at 12173 (was 9072), so compare per-tick numbers, not totals.

Budget check (`--profile-budget 33.33`, default workers):
- **p99 below 16.7 ms: met** in all four, max 2.2 ms (land p99 6.39 → 2.21 ms; synthetic 2.99 → 1.53 ms, its frame-0 all-pairs now in
  the background).
- **No tick above 33.3 ms: met** on the galactic runs; **not met on one tick per battle**, the first battle tick (land 69.7 ms, space
  58.5 ms; OPT1: 104.7 / 66.9 ms). It is battle setup: `T5.battles` with the parallel plan load (`lua.exec` about 320-335 ms of thread
  time over the workers, about 7 ms of it `lua.gc.auto`). At W0 the same tick takes 236-297 ms. Every other battle tick is under
  12 ms; the next worst are the battle-end tick (land 27 ms, space 23.5 ms), the second battle tick (first perception, 6.8-11 ms) and
  space `tac.movement` / `tac.events` ticks (7-8.6 ms).
- The galactic runs' max: HotM 4.3-4.5 ms is `ai.goals` on a goal-proposal frame (frame 132, mostly Lua; CP3 territory); synthetic
  4.7 ms is `sys.ai.all_pairs` at frame 15, where the frame-0 background recompute is applied (the zone includes
  any wait for the job and the adopt).

What is left for the 33.3 ms limit is battle setup and teardown, which are one-off ticks: spread setup over several ticks or prepare it
before the battle's first tick (moves goldens), and the remaining items listed under OPT3 t09 (land obstacle stamps, galactic plan load).

## Worst ticks after OPT2 (OPT3 t11, 2026-11-03)

Profiled with temporary zones (removed again) on the three battle scenarios at W15 on a quiet PC: `foc-land-campaign` (3014 ticks),
`foc-station --seed 3` (12173) and the land skirmish `foc-land` (3000). Four changes, all golden-neutral: the per-tick CRC logs of all
three runs are byte-identical to main ad9216b.

| Worst tick (ms, W15) | main ad9216b | after |
|---|---|---|
| Land campaign, first battle tick | 75.3 | 55.1 |
| Space station, first battle tick | 56.5 | 42.4 |
| Land skirmish, first tick | 192.7 | 138.8 |
| Land battle-end tick | 27.0 | 5.3 |
| Space battle-end tick | 23.5 | below 8.4 (out of the worst 3) |

- **Battle end: Lua VMs closed off the tick.** About 23 ms of the end tick was `TacticalAiWorld`'s destructor closing its plan, evaluator
  and free-store VMs one by one. `TacticalAiWorld::release_scripts` (called from `CampaignBattles::ai_end`) now runs every VM's
  finalizers on the sim thread (`LuaVm::finalize_for_close`: `lua_close`'s own steps; userdata a finalizer creates are marked finalized,
  as `lua_close` never finalizes them), so destroying a VM afterwards only frees its own heap and runs no Lua code. The VMs are then freed
  as one job on the AI's `background` hook (`lua::prepare_detached_close`; without a background hook through `lua_gc_pf`,
  `lua::destroy_detached`). `CampaignBattles` waits for the job before the next battle's AI ends and in its destructor. Nothing reads a
  closed VM, so results do not depend on it. `alamo_lua_gc_tests` `test_destroy_detached`: every finalizer runs once, on the calling
  thread, also with a finalizer that re-arms a new finalizable proxy, serially and on 1 / 4 workers.
- **`require` read cache for plan VMs.** Every plan VM requires the same PG library files, each through up to 22 script-root lookups.
  `PlanScriptRuntime` now gives its VMs a per-runtime `FileProvider` that remembers every read (found or not; thread-safe for the
  parallel plan load). Bytes only: each VM still parses its own chunk, so the VMs, their serials and pristine digests are unchanged
  (a shared bytecode cache was rejected for that reason). Plan-load file reads: about 98 → 25 ms of thread time per battle.
- **Map obstacle stamps in parallel.** The land skirmish's `stamp_map_obstacles` took about 60 ms, nearly all of it
  Fine_Tune_Occupied_Passability rays against collision meshes. The footprints are still built serially (they call the model hooks), then
  `PassabilityGrid::stamp_all` samples every footprint's cells through the battle's `ParallelFor` (sampling reads only the base grid and
  the footprint) and stamps them in map order: same grid and stamps. Skirmish: 60 → 17 ms. `alamo_movement_tests` `test_grid`:
  `stamp_all` with a back-to-front `ParallelFor`, a fine-tune ray, a repeated id and an already stamped id equals the `stamp()` loop.
- **Faster ALO decode** (`tools/formats/src/alo.cpp`). Vertex records and index lists were read value by value through the bounds-checked
  reader; after the existing size check they are now copied field by field. Same arrays. `alo::parse` thread time: land 51 → 29 ms,
  skirmish 333 → 223 ms.

What is left in the first battle tick (all one-off setup, already spread over the workers): the parallel plan load (land about 20 ms
wall, 250 ms of Lua thread time, mostly parsing the PG libraries in every plan VM), model loading (skirmish about 570 ms of thread time:
VFS read, parse, render-vertex packing the sim does not need), and serial steps such as the skirmish's `bt_place_forces` (about 25 ms,
per-object stamps in RNG order). Getting under 33.3 ms needs setup spread over several ticks or prepared before the battle (moves goldens),
or a sim-only model load (positions, indices and skeleton only). The second battle tick (first perception, 8-11 ms) is unchanged.
