# EaW:FoC engine rewrite — guide for Claude agents

Clean-room C++20 rewrite of the Star Wars: Empire at War: Forces of Corruption (Alamo) engine:
headless multithreaded simulation core first, then a Vulkan 1.3 renderer. Any Claude agent
(Claude Code CLI, desktop, cloud session or subagent) working in this folder follows this file.

## Where things are
| Path | What |
|---|---|
| `engine/` | Engine. One static lib per module: Phase 2 `core`, `vfs`, `data`, `jobs`, `galactic`, `apps`; Phase 3 `assets`, `rhi`, `materials`, `shaderxlat`, `terrain`, `render` (Vulkan 1.3 renderer, `alamo_render.exe`); Phase 4 `objects` (game object model) |
| `docs/README.md` | **Index of all documentation (start here).** One place per kind: `DECISIONS.md` (binding decisions), `BUILD.md` (build / test / gate), `PERFORMANCE.md` (measurements), `PROBES.md` (in-game probe index), `ROADMAP_PROTOCOL.md` (roadmap rules); status lives only on the roadmap |
| `docs/PHASE4.md` | Phase 4 step map (what each step is, where its code is) and the open per-step port choices. Finished-work notes: `docs/PHASE4_HISTORY.md` |
| **Project roadmap** | **The one main roadmap: `roadmap-store/` in this folder (Gabriel, 2026-11-06: one location; before that the page was also tracked in this repo as `ROADMAP.html` and `docs/ROADMAP.html`, both removed). Open `roadmap-store/ROADMAP.html` in any browser; it needs no sign-in and is generated, so never edit it by hand.** Update it with `python roadmap-store/roadmap_store.py write <file>`; ordered stages, boxes and arrows, task groups, tasks, build records and the activity log live in `roadmap-store/data/`, the page layout in `roadmap-store/page_shell.html` (edit outside the `roadmap-data` markers, then `render`). Rules and field list: `docs/ROADMAP_PROTOCOL.md`. `roadmap-store/` is its own git repository (remote `Empire-Vulkan-Brenach`) and is not in worktrees; use the main checkout's copy. When the remote changed, run `python roadmap-store/roadmap_store.py sync` |
| `roadmap-store/` | Own git repository holding the roadmap data, so every account shares one copy. Rules in its `README.md` |
| `docs/PHASE3.md` | Phase 3 (renderer) results, assumptions and reference; no status board. Long details of finished work: `docs/archive/*.zip` (2026-10-30) |
| `tools/` | Phase 1 tools: `meg` (MEG archives, used by the VFS), `formats` (ALO/ALA/…), `shaders`, `re`, `probe_mod` |
| `docs/PHASE2.md` | Phase 2 results and design notes (determinism contract, file resolution order); no status board |
| `docs/re/` | Behavioural specs recovered from the binary (GALACTIC, HOTSPOTS, STRUCTS, SUBSYSTEMS, LUA_API, xml tags) |
| `docs/assets/`, `docs/formats/`, `docs/shaders/` | Asset catalog and format specs |
| `extracted/` | Game text (XML/Lua) zipped: `foc_data_text.zip`, `eaw_data_text.zip` |
| `re_reference/` | Decompiled output. **Never read it while writing engine code** (clean-room). Only RE/doc tasks use it, and they write behaviour into `docs/re/`, never code |
| `toolchain/` | Project-local MSVC 14.51, Windows SDK, CMake, Ninja. Nothing is installed system-wide |
| `logs/` | Build/test logs written by `build_msvc.cmd` |

## Platform and compilers (Gabriel, 2026-10-27; full record in `docs/DECISIONS.md`)
- **Windows x64 only.** Multiplayer, loading the original game's save files and RenderDoc are out of scope. Parity with the original is checked by behaviour, not by matching its CRC / hashes bit for bit; the port's own determinism rule 3 is unchanged. Same outcome, own method (Gabriel, 2026-11-09, "Parity tiers" in `docs/DECISIONS.md`): rules, data / Lua handling and feel match retail exactly; random-driven outcomes match retail's distribution over many seeds; internals (including random-number draw order) are the port's choice, recorded as `[port]` entries.
- **Two compilers: MSVC (ships) and clang (checks).** Every change: MSVC build + tests. Sim code (everything in `engine/` except render, audio, video and UI): also the clang build + tests, including the CRC selfchecks. `engine/jobs` or other threading code: also `build_asan.cmd`. Details and sanitizer limits: `docs/BUILD.md`.
- Shaders are compiled by DXC (`toolchain\dxc`) to SPIR-V; the C++ compiler choice doesn't affect them.

## Build and test (everything else is in `docs/BUILD.md`)
- Main build: `build_msvc.cmd` from the project root (local toolchain, builds everything, runs ctest). Finished when `logs\msvc_configure.txt` ends with `BUILD_DONE`. Every build script ends with `tools\lint\build_summary.py`: read that, not the whole log.
- Clang check `build_clang.cmd`; pre-merge gate `build_gate.cmd` (MSVC + clang + ASan, exits 0 only with 0 warnings); one module `build_module.cmd <module> ["module;list"]`; quick iteration `build_agent.cmd` (fast tier: tests labelled `slow` are skipped unless you pass a regex or `all`). Every build script records its result on the roadmap (`docs/ROADMAP_PROTOCOL.md`, "Build records").
- Data-driven tests read `ALAMO_FOC_DATA` / `ALAMO_EAW_DATA` and skip when unset, so a pass without them proves less; the scripts default them (`tools\test_env.cmd`) and end with a `skips:` line.
- Tests run in parallel (`ctest -j`): two ctest entries must not share a fixed `%TEMP%` fixture or output path (separate paths, or a `RESOURCE_LOCK`); a flaky failure in one tree is a possible clash first. Long determinism tests get one ctest entry per worker count (`alamo_add_worker_count_check`), never `--check-workers`. Label any test over about 5 s `slow`.
- Line endings: text files are LF (`.gitattributes`; goldens are compared byte for byte), `.cmd` / `.bat` / `.ps1` are CRLF.
- Version control: the project is a git repository; commit before and after each agent task and push; `roadmap-store/` is its own repository. Remotes and new-machine setup: `docs/BUILD.md`.
- Branches (Gabriel, 2026-11-06; full record in `docs/DECISIONS.md`): **`dev` is the development branch (and GitHub's default branch); `main` is the released branch.** All work lands on `dev`; no PR to `main` from a task, group or session branch. Gabriel merges `dev` into `main` when he decides it is ready: `build_gate.cmd` on `dev`'s tip, then `gh pr create --base main --head dev`. Never push `dev` with a failing MSVC build or test. The app still starts every session on its own `claude/...` branch (git allows one branch per worktree; the app has no setting to work on `dev` directly), so treat that branch as scratch and end each task with `tools\land_on_dev.cmd` (after the builds and tests the change needs): it merges `origin/dev` into your branch (conflicts get resolved in your tree, never on `dev`), pushes to `dev` as a plain fast-forward (never force), and deletes your branch on origin. A rejected push means `dev` moved: rerun it. A session whose branch does not contain `origin/dev` yet (started from `main`) gets it from the same script.
- Tick profiler: `alamo_headless ... --profile`; wall-clock only, never let sim code read a profiler value. Baselines: `docs/PERFORMANCE.md`.

## Rules
1. Zero warnings at MSVC `/W4 /permissive-` and clang `-Wall -Wextra -Wpedantic -Wconversion -Wshadow`. Explicit narrowing casts; no shadowing.
2. No third-party dependencies in `engine/` without asking Gabriel. Approved so far: Vulkan-Headers, DXC, VMA (Phase 3); Lua 5.0.2 (vendored, original semantics), miniaudio, FFmpeg's Bink decoder (2026-10-27; see `docs/DECISIONS.md`).
3. Determinism (saves, replays and parity checks rely on the per-tick CRC): results read by game logic
   must be bit-identical across thread counts, runs, and the MSVC and clang builds. Parallel jobs write per-index outputs; reductions
   run in index order; no FMA contraction (`/fp:precise`, `-ffp-contract=off`); no iteration over
   pointer-keyed or seeded-hash containers in sim code.
4. Every module has a test exe registered with `add_test`. Keep them passing.
5. Software a task needs is installed project-locally under `toolchain\` with a pinned version and a
   setup step in `toolchain\2_setup.cmd` — never system-wide, no admin, no PATH/registry changes.
6. When a task finishes, update the roadmap (`roadmap_store.py write`, see `docs/ROADMAP_PROTOCOL.md`): it is the only place for status, open items and the
   flowchart (Gabriel, 2026-11-06: one main roadmap; the `docs/PHASE*.md` status boards, their Mermaid flowcharts and the `claude/*.md` project docs are not updated any more).
   Tick the item, update the flowchart box, record results and open items there. Results still go to one place each (`docs/README.md`): decisions in `docs/DECISIONS.md`,
   measurements in `docs/PERFORMANCE.md`, build / test rules in `docs/BUILD.md`, probe findings in `docs/re/` (index: `docs/PROBES.md`), per-step port choices in `docs/PHASE4.md`. There is one copy of the roadmap (Gabriel, 2026-11-06, replacing the 2026-11-04 two-copy rule): `roadmap_store.py write` updates `roadmap-store/` and pushes it, and nothing is regenerated or committed in this repository (`docs/ROADMAP_PROTOCOL.md`, "One location"). Keep edits minimal and factual. When all tasks in a roadmap group
   are finished, mark the group `done` on the roadmap in the same update (Gabriel, 2026-10-30, 2026-10-31): after closing tasks, re-read
   each group you touched, and if it has no `todo` task left set `status: done` (and drop its `preset`) in that same write; a group
   left open with every task closed is a mistake to fix before ending the task. A flowchart box's status is not derived from its
   groups: when the last group in a box is done, also set the box `done` (`nodes/<ID>`, same write). When a finished roadmap
   group gets a follow-up group, give the follow-up a `preset` (recommended model + effort level; see `docs/ROADMAP_PROTOCOL.md`).
7. In-game probes need Gabriel to launch the game; prepare them under `tools/probe_mod/` with run and cleanup scripts.

## Parallel work

Real work is done per roadmap group by a `lane` agent (`.claude/agents/lane.md`; `/start-group <id>`, `/finish-group <id>`) in its
own worktree. Lanes own a group, not a folder: two lanes must not edit the same files, and a landing commit names any golden that changed.
A change to `engine/core` public headers or the top-level CMake goes through the integrating agent. Single-module
`engine-module` agents remain for narrow side tasks. Definitions live in `.claude/agents/`.
- Worktrees (2026-11-06): `tools\new_worktree.cmd <name> [--base origin/dev]` creates `.claude\worktrees\<name>` on `claude/<name>`
  from `origin/dev` (default since 2026-11-06), with junctions to `Data`, `extracted`, `toolchain` and `roadmap-store`, and syncs
  the roadmap. Use ONE build tree per worktree (`build_agent.cmd msvc "<modules>"` gives `build-wt-msvc`: the tree name defaults to `wt` since 2026-11-08). After the branch is
  on `dev`, `tools\clean_worktrees.cmd` (dry run; `--apply --only <name>`; `--trees` for idle build trees) removes merged (into
  `origin/dev` or `origin/main`), clean, pushed, idle worktrees, unlinking junctions first. Engine: `tools/worktree/worktree_tool.py` (`--selftest`).
- Claims (2026-11-06): `python roadmap-store/roadmap_store.py claim <group> --by <session>` before starting a group (24 h, `release` when
  done), `claims` lists them, `batch [N]` prints ready groups whose `touches` do not overlap, for handing out a parallel batch.
  Build records are batched: `record-build` no longer pushes by itself; `write`, `sync` and `flush` do. See `docs/ROADMAP_PROTOCOL.md`.
- Dependencies: branch lanes from `origin/dev`. A lane that needs another lane's work waits until that work is on `dev` (or merges
  that lane's branch in, and lands after it).
- Golden CRC files (2026-11-06): `ALAMO_CRC 2` stores a checkpoint every 64 ticks instead of every tick, so a behaviour change is a
  small diff. `--crc-out` still writes full logs; `--crc-checkpoint N` writes v2. Regenerate after an intended behaviour change with
  `tools\regen_goldens.cmd` and say so in the commit message. Format and workflow: `docs/status/golden-checkpoints.md`. All 20 goldens
  were converted on 2026-11-06 (`tools\goldens\convert_goldens.py --all --in-place --check`; MSVC and clang 174/174).
- PR check without hosted CI (now mainly for the `dev` -> `main` PR): `tools\pr_check.cmd [PR] [--compiler msvc|clang|both] [--tests fast|all] [--post]`.
