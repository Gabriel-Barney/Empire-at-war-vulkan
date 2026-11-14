# CPU performance beyond "parallel first" (research, 2026-11-14)

Question (Gabriel, 2026-11-14): what else would make the CPU side faster, even where it gives up the goal of doing everything in
parallel? This report answers it from a fresh worker-count sweep (measurements in [`PERFORMANCE.md`](PERFORMANCE.md), "Worker-count
sweep") and a re-read of the open findings in [`PERF_AUDIT_2026-11-07.md`](PERF_AUDIT_2026-11-07.md) (J*, S*, L* ids below are
that audit's). Nothing in `engine/` was changed.

**Status on the roadmap.** All of this is parity tier 3 (performance), which is paused under the current focus
([`DECISIONS.md`](DECISIONS.md), "Current focus"). It is input for when Gabriel lifts the pause or promotes items, not work to start.

## What the sweep says

- **Parallelism buys little in battles: 5-25 % on the median tick, and 3 workers get all of it.** W3, W7 and W15 are within noise of
  each other in every battle scene; W0 against W15 is 1.20 vs 1.00 ms (space), 3.46 vs 2.90 ms (land), 3.00 vs 2.39 ms (water).
- **Galactic play is fastest serial.** `galactic_late` median 0.63 ms at W0 against 0.71-0.83 ms at W15 (up to 30 % worse with
  workers), the same direction as the OPT1 HotM table (0.19 vs 0.21 ms). Fork, wake and wait cost more than the forked work.
- **The tick is dominated by three serial costs** (W0, share of tick time, `--profile` zones):

  | Zone | land_battle | water_battle | space_battle_game | galactic_late |
  |---|---|---|---|---|
  | `tac.ai.perception` (AI snapshot, threat grid, signals) | 40 % (1.43 ms p50) | 46 % (1.38 ms) | 22 % (0.63 ms) | — |
  | `crc.battle.ai` (hashing the tactical AI's state) | 19 % (0.70 ms) | 16 % (0.49 ms) | inside 12 % `T13.end_tick` | — |
  | `ai.goals` (goal scoring) | 9 % | 5 % | — | 69 % (0.33 ms p50, 4.4 ms p99) |

  Together these are 60-70 % of a land or water battle tick, and none of it runs on the job system.
- **Spikes are serial too.** `nebula_battle`: `tac.movement` p99 24 ms (69 % of tick time), and one `ai.planning` / `lua.exec` tick of
  190 ms; `galactic_late`: `ai.goals` ticks up to 22 ms. Workers do not move these.

So the remaining CPU wins are in **doing less serial work**, not in spreading it wider.

## Options, ranked by expected gain

Each gives: what it costs in the parallel / determinism design, the expected gain, golden impact. "Golden-neutral" means per-tick
CRC logs stay byte-identical.

### 1. Stop hashing the whole sim every tick in `alamo_game` (16-24 % of a battle tick)
- Today `SimHost::tick_once` calls `recorder_->capture(frame_)` every tick (`engine/apps/sim/src/sim_host.cpp:628`), hashing every
  system including the tactical AI's full state, and keeps every record (S5). Goldens already store only every 64th tick (`ALAMO_CRC 2`).
- In the game (no multiplayer, no live desync check) capture only on checkpoint ticks (`tick % 64 == 0`), on save and on demand;
  keep every-tick capture in headless, tests and `--crc-out`. Ring-buffer the log. Saves ~0.5-0.7 ms per land / water tick, ~15/16
  of the hash cost.
- Cost: an in-game divergence is located to a 64-tick window instead of a tick (bisect with headless replay, which still captures
  every tick). Golden-neutral (headless unchanged). Effort S.
- Further, golden-changing: cached sub-digests for big blocks that change rarely (threat grid, paths; S5).

### 2. Incremental tactical perception (up to ~40 % of a land battle tick)
- `TacticalAiWorld::Impl::refresh` (`engine/ai/tactical/src/tactical_ai.cpp:1438`) rebuilds the AI's view of every object each tick
  through `BattleAi::fill`, which does about 12 string-keyed type lookups per object (S6: heap `std::string` per lookup), then sorts
  and diffs the whole snapshot to find added / removed objects.
- Serial-friendly fixes, golden-neutral: a per-battle `TypeFacts[type]` table built once (S6); refresh only the per-object fields
  that change per tick (position, health, owner, order) and keep static fields; take added / removed from the object store's own
  create / destroy events instead of sort-and-diff. Also: `type_index` name scans and per-evaluation parameter parsing (S7).
- Expected: the OPT2 t13 precedent (one string-scan fix took land p99 19.7 -> 2.6 ms) suggests the lookup part alone is large;
  measure `fill` vs `index` vs the signal pass first with temporary zones.
- Golden-changing variant (needs an RE check of retail's cadence, tier 1 if retail differs): refresh the threat grid on a slower
  cadence than every tick.

### 3. Fewer workers by default, and no fork for small work (galactic up to 30 %, battles noise to small gain)
- This is the direct "give up on everything in parallel" lever. Results never depend on worker count (rule 3), so the pool size is
  free to choose.
- (a) Default to `min(hardware threads - 1, 4)` instead of 15 (W3 matched W15 in every scene; the audit's J3 suggested 8).
  (b) Serial inside galactic mode: forks that go through `ParallelFor` run inline when the mode is galactic, except the all-pairs and
  other background jobs. (c) A minimum-work guard in `parallel_for_ref` (`engine/jobs/src/job_system.cpp:356`): fork only when
  `count / grain` and an estimated cost per item clear a threshold; the CRC capture (`grain 1`, ~18 systems, every tick) is the
  first candidate to run inline. The usual rule of thumb is that a loop needs on the order of a million cycles (~0.5 ms) of work
  before a fork pays (Intel TBB guidance, quoted second-hand).
- (d) The wait fixes J1 / J2 (wait on finished chunks, `_mm_pause` spin then `atomic::wait`, wake only as many workers as copies,
  no `std::this_thread::yield()` on the sim thread) matter most on a busy PC, where BM1 measured W15 losing badly to W0.
- Cost: a 400-planet synthetic galaxy loses its 3x from workers if (b) is applied blindly; gate (b) on measured scene size or keep
  all-pairs on workers. Golden-neutral. Effort S (a, c), M (d).

### 4. Fix the serial spikes where they happen
- `nebula_battle` `tac.movement` p99 24 ms and max 76 ms; one 190 ms `ai.planning` tick (a plan VM load or solve inside the tick,
  see S1 / S2: plan VMs re-parse the PG libraries; a mid-battle `fresh()` builds a VM on the sim thread). `galactic_late` `ai.goals`
  ticks up to 22 ms (S7: equation names upper-cased, hashed and stats keys built by string concatenation per planet per call).
- Each needs a temporary-zone profile of the worst tick (`--profile` worst-20 table) before a fix. All golden-neutral if the
  expressions are unchanged.

### 5. Build: PGO + LTO (5-30 % on everything serial, by vendor numbers)
- Microsoft's Xbox GDK documentation gives 5-30 % from PGO depending on title and training match; JetBrains measured 18-23 % for
  LTO and PGO + LTO on an MSVC-built clangd; Microsoft's own Unreal CitySample comparison (compiler versions, not PGO) moved render
  thread time ~3 %. The headless bench scenes are a ready training set.
- Cost: a two-stage release build, and goldens must be re-checked on both compilers (`/fp:precise` keeps FP semantics, inlining
  changes do not change results under it). The deferred LP4 group. Effort M.

### 6. Allocator (only if a profile shows heap time)
- Lua uses the CRT heap (L5, S9); `BattleAi::fill` allocates strings on every worker (S6). A per-VM size-class pool for Lua (no
  approval needed, via the existing `alamo_lua_patch`) or mimalloc (a new dependency: needs Gabriel). Fixing S6 first removes most
  of the hot allocations anyway. Golden-neutral while no pointer-keyed iteration exists (rule 3).

### 7. OS scheduling for the sim thread
- Opt the process out of power throttling (`SetProcessInformation` with `PROCESS_POWER_THROTTLING_EXECUTION_SPEED` and
  `..._IGNORE_TIMER_RESOLUTION` cleared, as emulators such as yuzu do), raise the sim thread's priority one step, and on dual-CCD X3D
  or hybrid CPUs keep the sim thread and the (smaller) pool on the V-cache CCD / P-cores (the deferred CP1). Gains are unmeasured
  here; on the single-CCD 7800X3D expect little. Golden-neutral (wall-clock only).

### Not recommended
- **Letting results depend on thread count or timing** (time-budgeted AI, first-come ordering). It would buy little (the work is
  serial anyway, see above) and breaks rule 3: saves, replays and parity checks rely on it.
- **Running AI players' Lua in parallel** (already investigated, OPT_RESEARCH.md): scripts are a small share of AI time.
- **Sim / render overlap**: measured no gain at today's ~2 + 5 ms tick frames (PERFORMANCE.md, "Tried and not shipped").

## Suggested order
1. Item 1 (game-only checkpoint CRC) and item 3 (a)+(c): small, golden-neutral, measurable on the bench suite.
2. Item 2 with S6 first, after a temporary-zone split of `tac.ai.perception`.
3. Item 3 (d) and item 4 spikes.
4. Item 5 (PGO + LTO) last, once the code-level wins are in, so PGO trains on the final hot paths.

## Sources
- Intel TBB loop-size guidance, quoted in [Intel TBB parallelization overhead (Stack Overflow archive)](https://kiwix.lema.org/stackoverflow.com_en_all_2023-11/questions/6784523/intel-tbb-parallelization-overhead)
- [The Tiny-Tasks Granularity Trade-Off (arXiv 2202.11464)](https://arxiv.org/pdf/2202.11464)
- [Xbox GDK: PGO](https://devdocs.xbox.com/tools/tools-console/pgo/pgo_full) (5-30 %)
- [JetBrains: LTO, PGO and unity builds](https://blog.jetbrains.com/clion/2022/05/testing-3-approaches-performance-cpp_apps/)
- [Microsoft C++ team blog, March 2026](https://devblogs.microsoft.com/cppblog/?p=36345) (MSVC 14.51 on CitySample)
- [Unreal forum: MSVC + PGO vs clang / ICX + PGO](https://forums.unrealengine.com/t/performance-impact-msvc-pgo-vs-clang-intel-icx-pgo/2721619) (anecdotal)
- [THREAD_POWER_THROTTLING_STATE](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/ns-processthreadsapi-thread_power_throttling_state),
  [Quality of service](https://github.com/MicrosoftDocs/win32/blob/docs/desktop-src/ProcThread/quality-of-service.md),
  [yuzu: set process to high QoS](https://git.openpunk.com/OpenPunk/yuzu/commit/026eaddbeef4d0a4992fb19b40af9242f09c3c6a)
