---
name: determinism-check
description: Checklist and test recipe for keeping simulation code deterministic (CLAUDE.md rule 3) - bit-identical results across worker counts, runs and the MSVC / clang builds. Use before committing any change to sim code (everything in engine/ except render, audio, video, UI), when writing parallel or job-system code, when reviewing a diff for lockstep safety, or when a test reports a worker-count, run-to-run or MSVC-vs-clang CRC difference.
---
# Determinism check

Saves, replays and the parity harness all compare the per-tick CRC, so any result game logic reads must be bit-identical for
every worker count, every run and both compilers. A nondeterminism bug rarely fails where it is introduced: it shows up later as
a replay desync or a golden that moves on one machine. Catching it at the diff is far cheaper. Contract: `docs/PHASE2.md`
("Determinism contract"); test rules: `docs/BUILD.md`.

## Read the diff for these

Go through the changed sim code and check each item. For every hit, either fix it or be able to say why it cannot reach a
value game logic reads.

1. **Unordered iteration.** Iterating `std::unordered_map` / `unordered_set`, any pointer-keyed container (`std::map<T*, ...>`,
   `std::set<T*>`), or a container sorted by address. Iteration order then depends on hash seeds or allocation addresses. Use
   index- or id-keyed containers, or sort by a stable id before iterating. Lookup alone (`find`, `count`) is fine.
2. **Parallel writes and reductions.** Parallel jobs write only to their own per-index output slot; combining runs afterwards in
   index order (`parallel_reduce`, `parallel_map_then_ordered_commit` in `engine/jobs`). No shared accumulator, no atomics whose
   final order affects a value, no "first thread to finish wins".
3. **Floating point.** No FMA contraction (`/fp:precise`, `-ffp-contract=off` are set; do not override per file), no `-ffast-math`
   style flags, no `std::pow` / `exp` / trig whose result differs between MSVC and clang CRT where it feeds game logic (`sqrt` is
   correctly rounded and fine). Watch float-to-int conversion and accumulation order: `a + b + c` in a different order is a different
   value.
4. **Uninitialised or padding reads.** Every field that enters the CRC or a save is initialised; no hashing a struct's raw bytes
   when it has padding.
5. **Outside inputs.** Sim code never reads wall-clock time, the profiler, thread ids, `std::random_device`, `rand()`, environment
   variables, or the worker count. Randomness comes from the sim's own seeded generators, drawn in a fixed order on the sim thread
   (or per-index streams in parallel code).
6. **Scheduling by tick only.** Deferred work becomes visible at a fixed tick (`Deferred<T>`, `TickScheduler`), never "when the
   background job finishes".
7. **New state reaches the CRC and the save.** A new piece of sim state that affects later ticks is hashed into its system's CRC
   column and saved; otherwise a load or a replay diverges silently. A `[port]` option that is off must leave every CRC unchanged.

## Prove it with tests

- Build and test both compilers on the changed modules: `& .\build_agent.cmd --% msvc wt "<modules>"` and
  `& .\build_agent.cmd --% clang wt "<modules>"`. The clang build runs the CRC selfchecks; the two must agree with the same goldens.
- Threading code (`engine/jobs`, anything adding parallel work): also `build_asan.cmd` (UBSan; `set ALAMO_SANITIZE=address` for ASan).
- A new long sim scenario gets one ctest entry per worker count with `alamo_add_worker_count_check` (`engine/CMakeLists.txt`;
  example `alamo_sim_fleet_groups_battle` in `engine/apps/CMakeLists.txt`), never one test running `--check-workers` over all counts.
  Label it `slow` when it takes over about 5 s.
- A worker-count or run-to-run failure is always a real bug, never golden drift: do not regenerate goldens for it
  (`regen-goldens` skill).

## Finding a divergence

Run the two sides with full logs (`--crc-out full.crc`, no `--crc-checkpoint`), then
`alamo_headless --compare a_full.crc b_full.crc`: it names the first differing tick and system column. Bisect inside that system's
update for the first value that differs between the two runs (worker count 0 vs N, or MSVC vs clang).

For an independent read of a diff, spawn the `reviewer` agent with the changed files; it checks these same rules and never edits.
