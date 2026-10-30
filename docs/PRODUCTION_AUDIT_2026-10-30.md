# Production audit — 2026-10-30

Scope: workflow, build, test, asset/art-integration and handoff process of the EaW:FoC rewrite, read from the tree,
logs and roadmap store as they stood at 18:20 on 2026-10-30. Read-only; nothing was changed except this file.
Not covered: test source quality, render output correctness, runtime performance (no benchmark exists yet), and anything
that needs the game launched. The "art" and "design" pipelines are asset *integration* (the port ships no new art);
there are no dedicated art or design teams in this tree.

## What is healthy
- Build/test: MSVC and clang both 127/127 tests, 0 warnings. Full ctest dropped from 641 s to 109 s (MSVC) / 99 s (clang) after `-j 8`.
- Build records land on the roadmap automatically (6 records, all pass); module boundaries are clean (21 modules, each with a test exe).
- The two-compiler rule and the determinism contract are enforced by tests (CRC selfchecks, one ctest entry per worker count).

## Findings, by priority

### P0 — Safety net
1. **No version control.** *(Fixed 2026-10-30: `git init`, first commit `fdc6b9c`, 1,251 files, 52 MB pack; no remote yet.)* The folder is not a git repository. History is a set of zips and folders in `backups/`
   (`goldens-before-*`, `logs-through-*`, `pre-phase*`), made by hand. Nothing can be bisected, diffed or rolled back, and
   parallel agents edit one live tree. This is the biggest single risk.
2. **No CI.** No `.github`, no scheduled build. Every gate depends on a person or agent running `build_msvc.cmd`/`build_clang.cmd`.
3. **Sanitizer gate is not wired.** *(Partly fixed 2026-10-30: `build_asan.cmd` runs UBSan on core+jobs, 10/10 pass. ASan is opt-in because clang 20 + MSVC ABI crashes on any caught exception, and ASan+UBSan reports a false null-member-call at `test_jobs.cpp:196`; so no ASan result for jobs yet.)* `CLAUDE.md` requires an ASan/UBSan (TSan where possible) run for `engine/jobs` and threading
   changes. Neither the CMake files nor the build scripts outside `toolchain/` and the build trees mention a sanitizer flag.
   There is no `build_asan.cmd`. The rule cannot currently be followed as written.
4. **Silent skips in data-driven tests.** 62 test sources read `ALAMO_FOC_DATA` / `ALAMO_EAW_DATA` and skip when unset. The
   build scripts do not set them, so a green run on a fresh shell proves less than the 127/127 suggests. The report does not
   say how many tests skipped.

### P1 — Throughput
5. **Performance lane is entirely unstarted.** Roadmap lanes for benchmark, CPU, GPU, loading and OPT (BM1–4, CP1–6, GP1–3,
   LP1/4/5, OPT1–3) are all `big`/`next`. There is no tick profiler, no budget, and no regression gate (LP5), so velocity work
   on the sim cannot be measured. OPT1 (tick profiler and budgets) is the prerequisite for the rest.
6. **Game-launch probes block design decisions.** The "Probe list for when in-game runs resume" item (`gab`) gates A1 items
   (FreeStore timing, landing-point ownership, provisional [M]/[L] header readings). Each is a Gabriel-only handoff with no
   scheduled slot; batch them into one session.
7. **Single-file hotspots.** `engine/apps/game/app/game_main.cpp` 354 KB, `render/tests/test_render.cpp` 306 KB,
   `render/src/renderer.cpp` 203 KB, `ui/tests/test_gui_screens.cpp` 192 KB, `apps/sim/tests/test_battles.cpp` 168 KB.
   These dominate incremental rebuild time and are merge-conflict magnets when several agents own one module.
8. **`engine/apps` is the integration choke point.** 33 of 111 `add_test` entries and 43 K lines sit there, and CLAUDE.md
   makes it single-owner. That serialises every cross-module feature.

### P1 — Art / asset-integration loop
9. **Visual QA is manual.** `logs/` holds ~150 PNGs (`render_*`, `ui_gui_*`, `*_before/after`), mostly 3.7–8.3 MB each, produced by
   hand-run tools. I found no automated image diff against a baseline in the scripts; regressions are caught by eye. (Test
   internals not read, so an in-test image check may exist.)
10. **Asset preload / shader precompile** are roadmap side goals (LP1, shader precompile lane), not yet shipped; first-load
    cost of map assets is therefore unmeasured.

### P2 — Debt and hygiene
11. **Stray project inside the tree:** `.claude/worktrees/modest-wiles-29b26c/` is an unrelated C# "Empire Mod Manager"
    (with its own `.github/workflows`). It is not part of the engine and may confuse tools that glob the repo.
12. **Disk and log sprawl:** `logs/` 402 MB (70+ `agent_*` text logs plus PNGs, never pruned except by manual zip), `build-msvc` 1.0 GB,
    `build-clang` 0.7 GB, `toolchain` 8 GB, `backups` 223 MB. `build-core` is a stale 25 MB tree.
13. **Docs are very large and duplicated.** `PHASE4_HISTORY.md` 387 KB, `PHASE4.md` 155 KB, `PHASE3.md` 82 KB, `roadmap-store/ROADMAP.md`
    93 KB. `ROADMAP.html` exists byte-identical in the root and `roadmap-store/`, with the shell source in `docs/` and
    `roadmap-store/page_shell.html`. Four places to update by hand in CLAUDE.md-mandated step 6 (roadmap + `claude/roadmap.md`
    + phase status + PHASE docs) invites drift.
14. **Roadmap store has no remote and a hook error.** `roadmap-store/hook_errors.log` records `record-build FileNotFoundError`
    at 21:06 UTC (first build record). It has no backup beyond the local folder, and the "shared by every account" claim in
    its README holds only on this machine.
15. **Roadmap data quality:** of 74 items, 18 are done and 27 are `big`; items have no title in the field I read (`title` is
    present, `id` is not, so listings show blanks). Several items carry ETAs but most `big` items carry none.
16. **Debt markers (approximate, not triaged):** TODO 4, XXX 4, "stub" 78, "placeholder" 30, "not implemented" 7 in `engine/`.
    (A "hack" search returned 261 hits, almost certainly the game's hacking mechanic, not debt.)

## Recommended order
1. `git init` the tree now (ignore `build-*`, `toolchain`, `logs`, `dist`, `re_reference` as appropriate), commit, and add a
   private remote. Replaces `backups/` zips and makes agent merges reviewable. *Needs Gabriel's go-ahead; not done here.*
2. Add `build_asan.cmd` (clang ASan/UBSan, `jobs` + `core` + sim tests) and make the data env vars default inside the build
   scripts (fall back to the Steam paths in CLAUDE.md), and have ctest print the skip count.
3. Add a scheduled/pre-merge script (or CI runner on this machine) that runs both builds and records them, so the gate stops
   depending on who remembers.
4. Start OPT1 (tick profiler + budgets), then LP5 (regression gate), before any further performance items.
5. Batch the open probes into one in-game session.
6. Split `game_main.cpp`, `renderer.cpp` and the two 300 KB test files along existing seams; automate screenshot diffs for
   the `render_*`/`ui_gui_*` set against stored baselines.
7. Housekeeping: delete `.claude/worktrees/modest-wiles-29b26c`, `build-core`; prune `logs/` into a dated zip per day;
   collapse the four roadmap/doc copies to one generated source; fix the `record-build` hook error and give the store a remote.
