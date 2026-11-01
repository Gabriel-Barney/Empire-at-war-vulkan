# EaW:FoC engine rewrite — guide for Claude agents

Clean-room C++20 rewrite of the Star Wars: Empire at War: Forces of Corruption (Alamo) engine:
headless multithreaded simulation core first, then a Vulkan 1.3 renderer. Any Claude agent
(Claude Code CLI, desktop, cloud session or subagent) working in this folder follows this file.

## Where things are
| Path | What |
|---|---|
| `engine/` | Engine. One static lib per module: Phase 2 `core`, `vfs`, `data`, `jobs`, `galactic`, `apps`; Phase 3 `assets`, `rhi`, `materials`, `shaderxlat`, `terrain`, `render` (Vulkan 1.3 renderer, `alamo_render.exe`); Phase 4 `objects` (game object model) |
| `docs/PHASE4.md` | Phase 4 status board (the game); roadmap steps are on `ROADMAP.html` |
| **Project roadmap** | **The main roadmap and flowchart: `ROADMAP.html` in this folder, the project's local roadmap on this device (Gabriel, 2026-10-30 / 2026-10-31). It opens with no sign-in and is generated, so never edit it by hand.** Its page shell is `docs/ROADMAP.html`, which also opens on its own because it carries the same data between the `roadmap-data` markers (the only hand-edited page file: edit outside the markers). Update it with `python roadmap-store/roadmap_store.py write <file>`; ordered stages, boxes and arrows, task groups, tasks, build records and the activity log live in `roadmap-store/data/`. Rules and field list: `docs/ROADMAP_PROTOCOL.md`. When `ROADMAP.html` changes in the GitHub store repo, run `python roadmap-store/roadmap_store.py sync` (pulls and regenerates the local `ROADMAP.html` and the `docs/ROADMAP.html` data block; `write` and the build scripts do the same), see "Keeping the local page current" in the protocol |
| `roadmap-store/` | Own git repository holding the roadmap data, so every account shares one copy. Rules in its `README.md` |
| `docs/PHASE3.md` | Phase 3 status board + flowchart (renderer). Long details of finished work: `docs/archive/*.zip` (2026-10-30) |
| `tools/` | Phase 1 tools: `meg` (MEG archives, used by the VFS), `formats` (ALO/ALA/…), `shaders`, `re`, `probe_mod` |
| `docs/PHASE2.md` | Phase 2 status board + flowchart + design notes (determinism contract, file resolution order) |
| `docs/re/` | Behavioural specs recovered from the binary (GALACTIC, HOTSPOTS, STRUCTS, SUBSYSTEMS, LUA_API, xml tags) |
| `docs/assets/`, `docs/formats/`, `docs/shaders/` | Asset catalog and format specs |
| `extracted/` | Game text (XML/Lua) zipped: `foc_data_text.zip`, `eaw_data_text.zip` |
| `re_reference/` | Decompiled output. **Never read it while writing engine code** (clean-room). Only RE/doc tasks use it, and they write behaviour into `docs/re/`, never code |
| `toolchain/` | Project-local MSVC 14.51, Windows SDK, CMake, Ninja. Nothing is installed system-wide |
| `logs/` | Build/test logs written by `build_msvc.cmd` |

## Platform and compilers (Gabriel, 2026-10-27)
- **Windows x64 is the only supported platform.** No other platforms, no non-Windows code paths
  (Gabriel, 2026-10-29). Multiplayer is out of scope (Gabriel, 2026-10-29): no networking, lobby or MP-only work.
  Loading the original game's save files is out of scope too (Gabriel, 2026-10-29): the port uses only its own save format (N1).
  Parity with the original is checked by behaviour, not by matching its CRC / hashes bit for bit (Gabriel, 2026-10-29);
  the port's own determinism rule 3 is unchanged. RenderDoc is out of scope: captures use apitrace.
- **Two compilers: MSVC (ships) and clang (checks).**
- Every change: MSVC build + tests. Changes to sim code (everything in `engine/` except render, audio,
  video and UI): also the clang build + tests, including the CRC selfchecks. Changes to `engine/jobs` or
  other threading code: also `build_asan.cmd` (clang UBSan by default; `set ALAMO_SANITIZE=address` for ASan, which then runs with `-asan-stack=0` (heap and globals only) because stack instrumentation crashes catch handlers on this toolchain, see `docs/PHASE4.md` "Sanitizer limits"; the default module list is the 11 sim modules; no TSan runtime on Windows, so races are covered by the per-worker-count CRC tests).
- Shaders are compiled by DXC (`toolchain\dxc`) to SPIR-V; the C++ compiler choice doesn't affect them.

## Build and test
- Line endings: `.gitattributes` keeps text files LF in the working tree too (golden files and test data are compared byte for byte, so a CRLF checkout fails `alamo_ai_tests` and `alamo_shaderxlat_tests`); `.cmd` / `.bat` / `.ps1` are CRLF.
- Screenshot diffs (2026-10-30): the game smoke tests' PNGs (`build-msvc\smoke_out`, `smoke_out\screens`) are compared with 320-px-wide baselines in `tests/visual_baselines/` (about 7 MB in git). `tools\visual\visual_check.cmd` compares the last run; `run` runs the smoke tests first (needs game data and a GPU); `update` stores new baselines after an intended visual change (commit them). Mismatches get diff images in `logs\visual_diff\`. The engine is `tools/visual/png_diff.py` (`--selftest`). Not in the gate: it needs the GPU and game data.
- Version control (2026-10-30): the project is a git repository (`main`); commit before and after each agent task. Build trees, logs, `toolchain/`, game data (`Data/`, `extracted/`), `re_reference/` and probe captures are ignored; `roadmap-store/` is its own repository. Remotes (both private, 2026-10-30): engine `https://github.com/Gabriel-Barney/Empire-at-war-vulkan.git` (`origin`, branch `main`; push after each task); roadmap store `https://github.com/Gabriel-Barney/Empire-Vulkan-Brenach.git` (`roadmap_store.py` pushes by itself after every write and build record; `python roadmap-store/roadmap_store.py sync` checks it). On a new account or machine: `git clone` the engine repo, `git clone` the store into `roadmap-store/`, then rebuild `toolchain/` with `toolchain\*.cmd` and unzip game data (these are not in git).
- Main build: run `build_msvc.cmd` from the project root. It configures `build-msvc\`
  with the local toolchain, builds everything and runs ctest. Read `logs\msvc_configure.txt` and
  `logs\msvc_build.txt`; the run is finished when `msvc_configure.txt` ends with `BUILD_DONE`.
- Every build script records its result on the roadmap (`docs/ROADMAP_PROTOCOL.md`, "Build records").
- Clang check: `build_clang.cmd` (project-local LLVM 20, MSVC ABI and headers, adds `-Wconversion -Wshadow`; logs `logs\clang_*.txt`, finished when `clang_configure.txt` ends with `BUILD_DONE`).
- One module only: add `-DALAMO_ENGINE_MODULES="core;<module>"`.
- Data-driven tests read `ALAMO_FOC_DATA` / `ALAMO_EAW_DATA` (folders containing `Data\XML`; unzip `extracted/*.zip`). They skip when unset, so a pass without them proves less. `build_msvc.cmd`, `build_clang.cmd` and `build_asan.cmd` call `tools\test_env.cmd`, which defaults them to `F:\SteamLibrary\steamapps\common\Star Wars Empire at War\corruption` and `...\GameData` when unset and the folder exists (2026-10-30); tests print a skip message and exit 0, so each script ends its log with `skips: data=N gpu=N other=N` from `tools\lint\ctest_skips.py` (reads ctest's JUnit file; `--list` names them). Last full run: 1 data skip (`alamo_lua_players_tests`, no PerceptualEquations folder), 0 gpu.
- Tick profiler (2026-10-30, OPT1): `alamo_headless ... --profile [--profile-out FILE] [--profile-budget MS]` prints per-tick p50/p99/max, per-zone time and the 20 worst ticks; baselines and the budget are in `docs/PHASE4.md`; `bench_msvc.cmd` re-runs them. Wall-clock only: never let sim code read a profiler value.
- Scheduled gate (Gabriel, 2026-10-30: this PC runs it): `tools\gate_scheduled.cmd` runs the gate on this checkout and appends a line to `logs\gate_history.txt`; it skips with exit 3 when tracked files have uncommitted edits. `tools\install_gate_task.cmd [HH:MM]` registers it as the per-user nightly task `AlamoGateNightly` (default 03:00, no admin, runs while logged on; remove with `schtasks /Delete /TN AlamoGateNightly /F`). Run the installer from the main checkout once this is merged.
- Pre-merge gate: `build_gate.cmd` runs the MSVC build, the clang build and `build_asan.cmd`, records each on the roadmap, prints `logs\gate_summary.txt` and exits 0 only when all three pass with 0 warnings (about 4 minutes when little has changed).
- Tests run in parallel (2026-10-30): `build_msvc.cmd` / `build_clang.cmd` use `ctest -j 8`, `build_agent.cmd` / `build_module.cmd` use `-j 4`. A full run takes about 110 s on MSVC and 100 s on clang (was about 640 s and 590 s). If you run ctest by hand, pass `-j` too and set `TMP` / `TEMP` to `<build tree>\tmp`.
- Writing tests for parallel runs: two ctest entries must not share a fixed `%TEMP%` fixture or output path, including entries that run the same exe with different arguments. Give them separate paths, or a `RESOURCE_LOCK` when they cannot be separated. Treat a flaky failure in one tree as a possible clash first.
- Long determinism tests: don't put a long run under `--check-workers`, which runs each worker count in turn inside one test. Use one ctest entry per worker count: the 0-worker entry writes the CRC log (`--crc-out`, `FIXTURES_SETUP`), the others compare against it (`--golden`, `FIXTURES_REQUIRED`). Example: `alamo_headless_skirmish_ai_foc_land_outcome` in `engine/apps/CMakeLists.txt`.
- Test tiers (2026-10-31): tests that take more than a few seconds carry the ctest label `slow` (`alamo_label_slow()` in `engine/CMakeLists.txt`; today the long headless skirmish / save / galactic-AI runs, the land-outcome worker-count runs and the game smoke tests: 27 of 130). `build_agent.cmd` and `build_module.cmd` run only the fast tier (`ctest -LE slow`) by default: pass a regex to `build_agent.cmd` to run matching tests, `all` (or `set ALAMO_TESTS=all` for `build_module.cmd`) to run everything. `build_msvc.cmd`, `build_clang.cmd`, `build_asan.cmd` and the gate always run everything. Label any new test that takes more than about 5 s `slow`, together with the tests that need its fixture.
- Short build output (2026-10-31): every build script ends with `tools\lint\build_summary.py` (exits, unique warnings / errors, ctest result, failed tests with the tail of their output, skips). Read that, not the whole log; `set ALAMO_FULL_LOG=1` prints the log too for `build_agent` / `build_module`. Run it by hand on any log: `python tools\lint\build_summary.py logs\msvc_configure.txt logs\msvc_build.txt`.
- Compiler cache (2026-10-31): when `toolchain\sccache\sccache.exe` exists (`toolchain\10_sccache.cmd`, pinned v0.19.0) the top-level CMake uses it as the compiler launcher and `tools\cache_env.cmd` points it at `toolchain\sccache\cache`, shared by every build tree and worktree. `-DALAMO_COMPILER_CACHE=OFF` or `set ALAMO_COMPILER_CACHE=OFF` builds without it. `build_summary.py` prints the hit / miss counts. Measured on 16 cores (full MSVC build): cold cache 165 s, a second tree in the same checkout 28 s, a worktree at another path 114 s (about half the files hit: the test `CHECK` macros bake `__FILE__` into the object; `SCCACHE_BASEDIRS` did not help); the uncached build is about 150 s, so the gain is for fresh trees and branch switches, not edits inside one tree.
- One engine module: `build_module.cmd <module> ["module;list"]` (its own `build-<module>\`, log `logs\module_<module>.txt`; the list defaults to `core;<module>`, so pass the dependencies for modules like render).
- Validation layers live in `toolchain\vvl` (pinned tag in `vvl\VERSION.txt`); their source/build trees are deleted after install. To rebuild, delete `toolchain\vvl` and run `toolchain\4_validation_layers.cmd` (re-clones; needs network).

## Rules
1. Zero warnings at MSVC `/W4 /permissive-` and clang `-Wall -Wextra -Wpedantic -Wconversion -Wshadow`. Explicit narrowing casts; no shadowing.
2. No third-party dependencies in `engine/` without asking Gabriel. Approved so far: Vulkan-Headers, DXC, VMA (Phase 3); Lua 5.0.2 (vendored, original semantics), miniaudio, FFmpeg's Bink decoder (2026-10-27; see `docs/PHASE4.md` Decisions).
3. Determinism (saves, replays and parity checks rely on the per-tick CRC): results read by game logic
   must be bit-identical across thread counts, runs, and the MSVC and clang builds. Parallel jobs write per-index outputs; reductions
   run in index order; no FMA contraction (`/fp:precise`, `-ffp-contract=off`); no iteration over
   pointer-keyed or seeded-hash containers in sim code.
4. Every module has a test exe registered with `add_test`. Keep them passing.
5. Software a task needs is installed project-locally under `toolchain\` with a pinned version and a
   setup step in `toolchain\2_setup.cmd` — never system-wide, no admin, no PATH/registry changes.
6. When a task finishes, update the roadmap (`roadmap_store.py write`, see `docs/ROADMAP_PROTOCOL.md`) and the current phase status (project docs
   `claude/roadmap.md`, `claude/phase2-status.md`, and `docs/PHASE2.md` here): tick the item, update the
   Mermaid flowchart node, record results and open items. Keep edits minimal and factual. When all tasks in a roadmap group
   are finished, mark the group `done` on the roadmap in the same update (Gabriel, 2026-10-30, 2026-10-31): after closing tasks, re-read
   each group you touched, and if it has no `todo` task left set `status: done` (and drop its `preset`) in that same write; a group
   left open with every task closed is a mistake to fix before ending the task. A flowchart box's status is not derived from its
   groups: when the last group in a box is done, also set the box `done` (`nodes/<ID>`, same write). When a finished roadmap
   group gets a follow-up group, give the follow-up a `preset` (recommended model + effort level; see `docs/ROADMAP_PROTOCOL.md`).
8. Shared helpers (2026-11-01): ASCII text helpers live in `engine/core/include/alamo/core/text.hpp` (`ascii_iequals`, `ascii_trim`, ...; `alamo::iequals` in `hash.hpp` is a path compare that also folds `/` and `\`), test executables use `core/test_support.hpp` (`CHECK`, `g_fail`, `MemoryProvider`, `env_or_empty`, `write_file`) and `rhi/test_fixture.hpp` (`Gpu`, `Offscreen`), save code uses `read_count` / `write_vec3` / `read_vec3` from `core/bytes.hpp`, and HLSL is compiled with `alamo_dxc_headers()` (`engine/CMakeLists.txt`). Use them instead of adding another local copy.
7. In-game probes need Gabriel to launch the game; prepare them under `tools/probe_mod/` with run and cleanup scripts.

## Parallel work

Agents split work by module folder: one agent owns one `engine/<module>/` folder and does not edit
others. A change to `engine/core` public headers or the top-level CMake goes through the integrating
agent. Subagent definitions for this split live in `.claude/agents/`.
