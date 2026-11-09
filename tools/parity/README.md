# Q4 behavioural parity harness (`tools\parity\`)

Roadmap group `q4h`. It compares the original game with the port **by behaviour**, not by CRC or hash equality
(Gabriel, 2026-10-29): travel and build timings, income, AI choices, battle outcomes and damage, each against a
tolerance that is written down with its reason. It replaces the `q2trace.py compare` step that Q2F t07 used. The trace,
production-queue and AI-log parsers come from `tools\probe_mod\q2_traces\analysis\q2_parity.py` and are imported, not
copied. Standard library only.

| file | what |
|---|---|
| `parity_harness.py` | `run`, `retail`, `list`, `selftest` (below) |
| `scenarios.json` | the scenarios (reference runs, port command, kit) and every tolerance with its reason |
| `baseline.json` | per-metric verdicts of the last recorded run (dev commit inside); the regression check of the slow ctests |
| `CMakeLists.txt` | ctest `parity_harness_selftest` (fast tier) and `parity_harness_<scenario>` (label `slow`) |
| `out\` | reports and port runs (ignored by git) |

## Use

```
toolchain\venv\Scripts\python.exe -I tools\parity\parity_harness.py run [--scenario hotm_galactic] [--port-exe build-msvc\bin\alamo_headless.exe]
toolchain\venv\Scripts\python.exe -I tools\parity\parity_harness.py run --check-baseline tools\parity\baseline.json
toolchain\venv\Scripts\python.exe -I tools\parity\parity_harness.py retail      (the retail runs against each other)
toolchain\venv\Scripts\python.exe -I tools\parity\parity_harness.py list        (reference runs present on this machine)
toolchain\venv\Scripts\python.exe -I tools\parity\parity_harness.py selftest
```

`run` needs `ALAMO_FOC_DATA` / `ALAMO_EAW_DATA` (the build scripts set them through `tools\test_env.cmd`) and an
`alamo_headless.exe` (default: the first of `build-wt-msvc`, `build-msvc`, `build-wt-clang`, `build-clang`). It replays
each scenario in the port with the reference runs' campaign or map, seed and tick count, writes
`out\<scenario>\report.txt` and `report.json` and prints the report. Exit codes: 0 no metric out of tolerance (with
`--check-baseline`: no metric that passed in the baseline fails now), 1 out of tolerance / regression, 2 error, 77
skipped (no game data, no reference run or no `alamo_headless`). `--reuse-port` grades an earlier port run again (for
tolerance work); `--write-baseline FILE` records the verdicts.

The two slow ctests run `run --check-baseline baseline.json` against the tree's own `alamo_headless`. When an intended
behaviour change moves a metric that passed in the baseline, re-record it, like a golden, and say so in the commit:

```
toolchain\venv\Scripts\python.exe -I tools\parity\parity_harness.py run --write-baseline tools\parity\baseline.json --commit <dev commit>
```

## Reference runs and where they come from

The reference runs are the Q2 traces of the original game (Steam FoC 1.121), made by the Q2 kit
(`tools\probe_mod\q2_traces\README.md`). They live in the kit's ignored `out\` folder. In a git worktree that folder is
empty, so the harness reads the main checkout's copy (found with `git rev-parse --git-common-dir`); a run missing from a
worktree's own `out\` (a worktree whose kit recorded new runs) is looked up in the main checkout's next. `--refs DIR` or
`ALAMO_Q2_TRACES` points elsewhere (that folder only). A scenario's `max_tick` cuts every galactic reference run at that
tick, so a run that went on longer (or with human input later) still grades the same window. Travel timings come from these traces as well: the G1 hop probe
(`tools\probe_mod\g1_hops\`) measured the retail hops from the same galactic trace.

| scenario | reference runs | kit that makes one |
|---|---|---|
| `hotm_galactic` | `20261108-170824-galactic-vanilla-lua`, `20261030-061228-galactic-vanilla-lua` (reader 2: credits, production queues), `20261027-101057-galactic-vanilla` (reader 1: no credits or queues, so travel, fleets and outcomes only), `20261108-204711-galactic-vanilla-lua` (q4hf t03: 40 cycles, two auto-resolved battles at tick 21094; graded up to `max_tick` 13501). `20261103-152619-galactic-vanilla-lua` was dropped on 2026-11-08 (q2f t08): its Lua plan hooks broke retail's own AI plans (`excluded_references` in `scenarios.json`) | `run_lua_probe.cmd` (or `run_galactic.cmd`), Heart of the Maw, Empire, defaults; `-NonInteractive` drives the menus |
| `geonosis_skirmish` | `20261030-062751-skirmish-vanilla-lua`, `20261108-204213-skirmish-vanilla-lua` (q4hf t03), `20261108-203639-skirmish-vanilla` (q4hf t03, same scenario without the Lua probe) | `run_skirmish.cmd -LuaProbe`, (2) Battle Over Geonosis, Empire vs Medium AI Rebel, defaults |

The G1 render captures (`tools\probe_mod\g1_capture\`) are image captures for the renderer, not behaviour, so the harness
does not read them.

## Metrics

Both sides are reduced to the same metric ids. Galactic: the retail trace and production queues on one side, the port's
`--trace-out` state trace (q2f t05) and its AI log on the other. Skirmish: the retail trace on one side, the skirmish host's
AI log (state blocks every 600 battle frames: credits, units by type, pad contents) and, for `combat.*`, its tactical
`--trace-out` trace (q4hf t02, `engine/apps/headless/tactical_trace.hpp`: the same Q2TRACE 1 format, per-object owner / type /
combat hull every tick, reduced by the same reader as the retail trace) on the other.

| category | metric | galactic | skirmish |
|---|---|---|---|
| timing | `travel.hop_ticks[Faction:FROM->TO]` | every leg's duration, from fleet positions (same rule both sides, q4hf t04): a fleet stops when its position holds for 3 samples (one repeated retail sample mid-flight is not a stop); a trip that passes a planet's centre is split there; a leg runs from the last sample at its start planet (retail: the centre sample after the orbit slot; port: the last parked sample) to the first sample at its end planet | - |
| timing | `build.ticks[TYPE@PLANET]` | shortest completed build: retail queue start to leaving the queue (uninterrupted items only), port build order to the new object | - |
| timing | `econ.cycle_tick[k]` | fiscal cycle k | - |
| income | `econ.income_cycle1[F]`, `econ.income_mean[F]` | payment on the first cycle frame, mean payment | - |
| income | `econ.credits_at_T[F]` | balance at ticks 4500 / 9000 / 13500 | balance at frames 1800 / 3600 / 5400 |
| AI choices | `ai.build_count[F]`, `ai.build_mix[F]`, `ai.first_build_tick[F]` | queued items (retail queue starts, port build orders incl. starbases) | units and structures gained between snapshots (`ai.first_build_frame`) |
| AI choices | `ai.fleet_departures[F]`, `ai.fleet_targets[F]`, `ai.first_departure_tick[F]` | fleet trips (stop to stop, not split at planets passed) by faction and target planet | - |
| battle outcomes | `outcome.planets_final[F]`, `outcome.captures[F]`, `outcome.owner_changes` | planet owners at the end, captures | - |
| battle outcomes | `outcome.units_final[F]`, `outcome.units_lost[F]`, `outcome.battle_ended` | - | units alive at the end, units lost, battle over before the limit |
| damage | `combat.damage_taken[F]`, `combat.first_shot_frame` | - | hull lost per faction (health drops of live units between samples, same rule both sides), first projectile (retail: first `PROJ_` object; port: `M first_shot_frame`, the battle frame of the first shot, since its projectiles are not store objects) |
| diagnostic | `diag.rng_draws_total` | synced-RNG draws over the run, never graded | - |

## Grading and tolerances

The retail runs of one scenario differ from each other although the galaxy gets no input (the original AI reacts to
wall-clock timing): the three HotM runs build different units from tick 140 on, end with 22 to 899 Rebel credits, and
two of them see Shola captured. So:

- a scalar metric passes when the port value lies in `[min(retail) - tol, max(retail) + tol]`,
  `tol = max(abs, rel * max(|min|, |max|))`;
- `travel.hop_ticks` is multi-modal (one route, several fleet speeds): each port hop must lie within tolerance of some
  retail hop of that route (`nearest`);
- a type mix (`ai.build_mix`, `ai.fleet_targets`) passes when its multiset overlap with the closest retail run,
  `sum(min(a, b)) / max(sum(a), sum(b))`, is at least the threshold; with `retail_floor` the threshold is capped at
  the retail runs' overlap among themselves ("as close to a retail run as the retail runs are to each other");
- a route or a build type seen on one side only is not graded on its own (the mixes grade that choice);
- INFO is reported, never graded; N/A says which side has no value and why.

The tolerance of each metric family and its reason are in `scenarios.json` (`tolerances`); the most specific key wins
(`econ.credits_at[Empire]` over `econ.credits_at`). `retail` prints the spread and a leave-one-out check: every retail
run graded like the port against the others. Its FAILs are the retail spread the tolerances still flag; each `run`
report ends with the same leave-one-out counts, so a port FAIL count can be read against the original's own.

## Gaps (what the port cannot be checked on yet)

- **Galactic battles**: graded by planet owners only. `autoresolve_refs.json` holds the retail auto-resolve outcomes of
  `20261108-204711-galactic-vanilla-lua` (Tatooine, tick 21094: a space and a land battle the human Empire answered with
  Auto-resolve; forces, losses by company, owner change), beyond `max_tick`; no check reads it yet.
- **Object counts by type** (galactic) are not compared: the retail store lists company and fleet members, the port's
  galactic store the companies (`BOBA_FETT` vs `BOBA_FETT_TEAM`).
- **Story battles**: `20261108-234224-story-vanilla` (story t04, Kessel UM01 linked land battle, 18000 frames) is not a
  scenario yet. The port's `--galactic-ai --trace-out` writes the galactic store only, not the battle's objects, so only
  synced-RNG draws and the battle end would compare; `docs/re/STORY_T04_KESSEL_RUN.md` §2-§4 (comparison by hand with
  `tools/probe_mod/q2_traces/analysis/story_battle.py`, and what a `story_kessel` scenario needs).
