# Build, test and gate

The full detail of building, testing and the gate. `CLAUDE.md` keeps the short rules agents must follow; everything else is here (moved verbatim on 2026-11-06 from `CLAUDE.md` "Build and test" and the build rules of "Platform and compilers", and from `docs/PHASE4.md` "Worker-count check" and "Sanitizer limits").

## Which checks a change needs (from "Platform and compilers")
- Two compilers: MSVC ships, clang checks.
- Every change: MSVC build + tests. Changes to sim code (everything in `engine/` except render, audio,
  video and UI): also the clang build + tests, including the CRC selfchecks. Changes to `engine/jobs` or
  other threading code: also `build_asan.cmd` (clang UBSan by default; `set ALAMO_SANITIZE=address` for ASan, which then runs with `-asan-stack=0` (heap and globals only) because stack instrumentation crashes catch handlers on this toolchain, see "Sanitizer limits" below; the default module list is the 11 sim modules; no TSan runtime on Windows, so races are covered by the per-worker-count CRC tests).
- Shaders are compiled by DXC (`toolchain\dxc`) to SPIR-V; the C++ compiler choice doesn't affect them.

## Build and test (moved from CLAUDE.md)
- Line endings: `.gitattributes` keeps text files LF in the working tree too (golden files and test data are compared byte for byte, so a CRLF checkout fails `alamo_ai_tests` and `alamo_shaderxlat_tests`); `.cmd` / `.bat` / `.ps1` are CRLF.
- Screenshot diffs (2026-10-30): the game smoke tests' PNGs (`build-msvc\smoke_out`, `smoke_out\screens`) are compared with 320-px-wide baselines in `tests/visual_baselines/` (about 7 MB in git). `tools\visual\visual_check.cmd` compares the last run; `run` runs the smoke tests first (needs game data and a GPU); `update` stores new baselines after an intended visual change (commit them). Mismatches get diff images in `logs\visual_diff\`. The engine is `tools/visual/png_diff.py` (`--selftest`). Not in the gate: it needs the GPU and game data.
- Version control (2026-10-30): the project is a git repository (`main`); commit before and after each agent task. Build trees, logs, `toolchain/`, game data (`Data/`, `extracted/`), `re_reference/` and probe captures are ignored; `roadmap-store/` is its own repository. Remotes (both private, 2026-10-30): engine `https://github.com/Gabriel-Barney/Empire-at-war-vulkan.git` (`origin`, branch `main`; push after each task); roadmap store `https://github.com/Gabriel-Barney/Empire-Vulkan-Brenach.git` (`roadmap_store.py` pushes by itself after every write and build record; `python roadmap-store/roadmap_store.py sync` checks it). On a new account or machine: `git clone` the engine repo, `git clone` the store into `roadmap-store/`, then rebuild `toolchain/` with `toolchain\*.cmd` and unzip game data (these are not in git).
- Main build: run `build_msvc.cmd` from the project root. It configures `build-msvc\`
  with the local toolchain, builds everything and runs ctest. Read `logs\msvc_configure.txt` and
  `logs\msvc_build.txt`; the run is finished when `msvc_configure.txt` ends with `BUILD_DONE`.
- Every build script records its result on the roadmap (`docs/ROADMAP_PROTOCOL.md`, "Build records").
- Clang check: `build_clang.cmd` (project-local LLVM 20, MSVC ABI and headers, adds `-Wconversion -Wshadow`; logs `logs\clang_*.txt`, finished when `clang_configure.txt` ends with `BUILD_DONE`).
- One module only: add `-DALAMO_ENGINE_MODULES="core;<module>"`.
- Data-driven tests read `ALAMO_FOC_DATA` / `ALAMO_EAW_DATA` (folders containing `Data\XML`; unzip `extracted/*.zip`). They skip when unset, so a pass without them proves less. `build_msvc.cmd`, `build_clang.cmd`, `build_asan.cmd`, `build_agent.cmd` and `build_module.cmd` (the last two since 2026-11-02) call `tools\test_env.cmd`, which defaults them to `F:\SteamLibrary\steamapps\common\Star Wars Empire at War\corruption` and `...\GameData` when unset and the folder exists (2026-10-30); tests print a skip message and exit 0, so each script ends its log with `skips: data=N gpu=N other=N` from `tools\lint\ctest_skips.py` (reads ctest's JUnit file; `--list` names them). Last full run: 1 data skip (`alamo_lua_players_tests`, no PerceptualEquations folder), 0 gpu.
- Tick profiler (2026-10-30, OPT1): `alamo_headless ... --profile [--profile-out FILE] [--profile-budget MS]` prints per-tick p50/p99/max, per-zone time and the 20 worst ticks; baselines and the budget are in `docs/PERFORMANCE.md`; `bench_msvc.cmd` re-runs them. Wall-clock only: never let sim code read a profiler value.
- Scheduled gate (Gabriel, 2026-10-30: this PC runs it): `tools\gate_scheduled.cmd` runs the gate on this checkout and appends a line to `logs\gate_history.txt`; it skips with exit 3 when tracked files have uncommitted edits. `tools\install_gate_task.cmd [HH:MM]` registers it as the per-user nightly task `AlamoGateNightly` (default 03:00, no admin, runs while logged on; remove with `schtasks /Delete /TN AlamoGateNightly /F`). Run the installer from the main checkout once this is merged.
- Pre-merge gate: `build_gate.cmd` runs the MSVC build, the clang build and `build_asan.cmd`, records each on the roadmap, prints `logs\gate_summary.txt` and exits 0 only when all three pass with 0 warnings (about 4 minutes when little has changed).
- Tests run in parallel (2026-10-30): `build_msvc.cmd` / `build_clang.cmd` use `ctest -j 8`, `build_agent.cmd` / `build_module.cmd` use `-j 4`. A full run takes about 110 s on MSVC and 100 s on clang (was about 640 s and 590 s). If you run ctest by hand, pass `-j` too and set `TMP` / `TEMP` to `<build tree>\tmp`.
- Writing tests for parallel runs: two ctest entries must not share a fixed `%TEMP%` fixture or output path, including entries that run the same exe with different arguments. Give them separate paths, or a `RESOURCE_LOCK` when they cannot be separated. Treat a flaky failure in one tree as a possible clash first.
- Long determinism tests: don't put a long run under `--check-workers`, which runs each worker count in turn inside one test. Use one ctest entry per worker count: the 0-worker entry writes the CRC log (`--crc-out`, `FIXTURES_SETUP`), the others compare against it (`--golden`, `FIXTURES_REQUIRED`). `alamo_add_worker_count_check` in `engine/CMakeLists.txt` does this; example: `alamo_headless_skirmish_ai_foc_land_outcome` in `engine/apps/CMakeLists.txt`.
- Test tiers (2026-10-31): tests that take more than a few seconds carry the ctest label `slow` (`alamo_label_slow()` in `engine/CMakeLists.txt`; today the long headless skirmish / save / galactic-AI runs, the land-outcome worker-count runs, the game smoke tests and `alamo_commands_tests` (thread-heavy scheduler test, 2026-11-01): 29 of 134). `build_agent.cmd` and `build_module.cmd` run only the fast tier (`ctest -LE slow`) by default: pass a regex to `build_agent.cmd` to run matching tests, `all` (or `set ALAMO_TESTS=all` for `build_module.cmd`) to run everything. `build_msvc.cmd`, `build_clang.cmd`, `build_asan.cmd` and the gate always run everything. Label any new test that takes more than about 5 s `slow`, together with the tests that need its fixture.
- Short build output (2026-10-31): every build script ends with `tools\lint\build_summary.py` (exits, unique warnings / errors, ctest result, failed tests with the tail of their output, skips). Read that, not the whole log; `set ALAMO_FULL_LOG=1` prints the log too for `build_agent` / `build_module`. Run it by hand on any log: `python tools\lint\build_summary.py logs\msvc_configure.txt logs\msvc_build.txt`.
- Compiler cache (2026-10-31): when `toolchain\sccache\sccache.exe` exists (`toolchain\10_sccache.cmd`, pinned v0.19.0) the top-level CMake uses it as the compiler launcher and `tools\cache_env.cmd` points it at `toolchain\sccache\cache`, shared by every build tree and worktree. `-DALAMO_COMPILER_CACHE=OFF` or `set ALAMO_COMPILER_CACHE=OFF` builds without it. `build_summary.py` prints the hit / miss counts. Measured on 16 cores (full MSVC build): cold cache 165 s, a second tree in the same checkout 28 s, a worktree at another path 114 s (about half the files hit: the test `CHECK` macros bake `__FILE__` into the object; `SCCACHE_BASEDIRS` did not help); the uncached build is about 150 s, so the gain is for fresh trees and branch switches, not edits inside one tree.
- One engine module: `build_module.cmd <module> ["module;list"]` (its own `build-<module>\`, log `logs\module_<module>.txt`; the list defaults to `core;<module>`, so pass the dependencies for modules like render).
- Validation layers live in `toolchain\vvl` (pinned tag in `vvl\VERSION.txt`); their source/build trees are deleted after install. To rebuild, delete `toolchain\vvl` and run `toolchain\4_validation_layers.cmd` (re-clones; needs network).

## Worker-count check (OPT3 t07, 2026-11-02)

Rule 3 says per-tick CRCs are identical for every worker count. Every OPT2 / OPT3 / CP / BM change cites this check: name the
scenario(s) it ran and the result in the PR. Two forms, both comparing every count's CRC log with the 0-worker log
(`first_divergence`, which names the first diverging tick and system):
- **Per-process (long runs), CMake** `alamo_add_worker_count_check(<name> COMMAND <exe> [args] [SETUP_ARGS ...] [CHECK_ARGS ...]
  [WORKERS ...] [TIMEOUT s] [SLOW] [PROPERTIES ...])` in `engine/CMakeLists.txt`. One ctest entry per count, so the counts run side by
  side under `ctest -j`: `<name>` runs `--workers 0 --crc-out <bin>/<name>_w0.crc` (`FIXTURES_SETUP <name>_w0`), `<name>_w1`, `_w4`, `_w16`
  and `_w<hardware threads - 1>` (`_w15` on the 16-thread reference PC) run `--workers N --golden` against it (`FIXTURES_REQUIRED`). 16 is
  more workers than logical cores here, so preemption inside a tick is covered too. `SETUP_ARGS` go to the 0-worker entry only (a
  checked-in golden, an `--expect-*` check), `CHECK_ARGS` to the others; `SLOW` labels all entries `slow`. Further entries can compare
  against the same log with `FIXTURES_REQUIRED <name>_w0` and `--golden ${<name>_CRC}`. The command needs `--workers`, `--crc-out`
  and `--golden` (every `alamo_headless` mode has them).
- **In-process (unit tests, short runs), C++** `alamo::jobs::check_worker_counts(run, counts = determinism_worker_counts())` in
  `engine/jobs/include/alamo/jobs/crc_recorder.hpp`: calls `run(workers)` (returns the run's `CrcLog`) for 0 / 1 / 4 / default and
  returns a `WorkerCountReport` (`ok()`, `first`, per-count `vs_first`). Used by `alamo_jobs_crc_tests` (incl. an injected divergence)
  and by `alamo_headless --replay --check-workers`.

Converted: `alamo_headless_replay_workers*` (per-process, the 0-worker entry also against `replay_golden.crc`; `alamo_headless_replay_golden`
now runs the in-process form) and `alamo_headless_galactic_ai_human*` (36000 ticks, was one `--check-workers` entry running the counts in
turn; each count's AI log is compared with the golden) and `alamo_headless_skirmish_ai_foc_land_outcome*` (OPT3 t08: 7000-tick land
skirmish, the 0-worker entry also checks `--expect-skirmish-base`; `_profiled` compares a `--profile` run with the same 0-worker log).
BM1 t05 (2026-11-06): `alamo_headless_bench_space_battle*` runs the check on the benchmark scenario from its save (`--bench
tests/benchmark/space_battle.abench --runs 1 --warmup 0`, slow, FoC data; docs/PHASE4.md "BM - repeatable benchmark").
Not converted: `alamo_headless_skirmish_ai_synthetic` and the in-process loops in `engine/apps/sim/tests` stay as they are (short).

## Sanitizer limits (PAF t02 / t03, 2026-10-30)

`build_asan.cmd` runs clang 20 (MSVC ABI) with UBSan by default; `ALAMO_SANITIZE=address` adds ASan.
- **ASan and exceptions.** A minimal `try { throw } catch` program built with `-fsanitize=address` crashes in the catch handler
  (static runtime: access violation inside `main` right after `CxxCallCatchBlock`; `-fms-runtime-lib=dll` does not link at all
  with this toolchain). Cause: the stack instrumentation touches the catch funclet's frame. `-mllvm -asan-stack=0` fixes it (the
  program prints and exits 0); `-asan-globals=0`, `-fsanitize-address-use-after-return=never` and `-asan-use-after-scope=0` do not.
  `build_asan.cmd` now adds `-mllvm -asan-stack=0` whenever `address` is in `ALAMO_SANITIZE`, so ASan covers heap, globals and
  use-after-free but **not stack buffer overflows or use-after-return**. The false "member call on null" at
  `engine/jobs/tests/test_jobs.cpp:196` came from the same instrumentation.
- **TSan:** no Windows runtime in LLVM 20; races are covered by the per-worker-count CRC tests.
- **Modules covered** (2026-10-30): the default is now the sim set `core;vfs;data;jobs;galactic;objects;anim;commands;tactical;lua;ai` (52 tests; UBSan 17 s, ASan about 3 min,
  both 52/52 and 0 warnings). Not covered: assets, rhi, materials, shaderxlat, terrain, render, ui, audio, video, apps (GPU / window / big-data
  tests; sim code is what the sanitizer rule protects). The first run found three real bugs, all fixed: `alamo_commands_tests` shifted a `uint32_t` by 32
  (`engine/commands/tests/test_commands.cpp`, UBSan); `alamo_tactical_abilities_tests` replaced its `CombatSystem` before its `AbilitySystem`, whose
  destructor then wrote to the freed system (test fixture order, `World::make`); `luaG_checkcode` indexed `luaP_opmodes` with an opcode
  from a corrupt save (patch in `engine/lua/CMakeLists.txt`, the save is now rejected).

## Test thread pools under a loaded machine (2026-11-03)

`alamo_ai_tactical_tests` (2 s) ran past ctest's 600 s timeout at 100% CPU. Not a deadlock: it finished once the load went.
Its `make_thread_pf` spawned and joined fresh `std::thread`s on every call, and the zone all-pairs (Floyd-Warshall) makes one
call per k, about 9300 over the shipped maps; each join waited for the new threads to get a time slice. Reproduced with 48
busy-loop processes on the 16 cores: still running at 150 s (`alamo_ai_tactical_perception_tests`, same helper: still
running at 120 s). Fix: `engine/ai/tactical/tests/thread_parallel_for.hpp`, a persistent pool whose caller claims chunks too
and waits only for chunks a thread has claimed, never for a worker to wake. Same load: 15 s and 7 s. The pools in
`engine/tactical/tests` and `engine/galactic/tests/thread_parallel_for.hpp` still wait for every worker to report back on each
call, and the `engine/ai/tests` / `engine/tactical/movement/tests` helpers still spawn per call.
