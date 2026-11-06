# Code audit: optimization and refactoring (2026-11-06)

**Scope.** This audit reads the code. It covers all 21 engine modules, the CMake files, the build scripts, `tools/` and the docs, as they stand at `ddfe132` on `main`.

**Method.** Five reviewers each read part of the tree, and I re-checked the main claims in the code. I also ran a scan for duplicated code and counted how many files include each header.

**What was changed.** Nothing was edited or built, except for this file. `re_reference/` was not opened.

**How this relates to earlier work.** The [2026-10-30 production audit](PRODUCTION_AUDIT_2026-10-30.md) covered process (git, CI, sanitizers, probes). Of its findings, only #13 (docs size) comes up again here. It is now worse.

Effort scale: S is under half a day, M is one or two days, L is several days. "Owner" is the module folder that would make the change, following the CLAUDE.md parallel-work rule. Changes to `engine/core` headers or the top-level CMake go through the integrator.

## Summary

The codebase is in better shape than its size suggests: 365 k lines, 832 source files.

**Determinism.** The determinism contract holds everywhere the reviewers looked:
- Sim containers are ordered.
- `unordered_map`s are sorted before they are saved.
- Spatial queries return results in ascending handle order.

**Duplication.** There is little copy-paste:
- The scan found 916 duplicated 10-line windows across 832 files.
- Most of them sit in test files and a few known pairs.

**Renderer.** The main path follows its own rules:
- The draw sort is a radix sort with reused scratch buffers.
- Barriers go through rhi.
- No wait-idle happens per frame.
- The main pipelines are cached on disk.

**Where the debt is.** It sits in five places:

1. **Test harness.** The harness is re-implemented in every test file. 83 files define their own `CHECK` macro. 71 files read `ALAMO_FOC_DATA` by hand, with 101 different skip messages. That is why skips are counted with a regex.
2. **Per-tick string lookups and allocations.** These are in the parts of the sim the profiler says are expensive:
   - tactical AI fill: `tac.ai` is 14–20 % of a land tick
   - galactic AI goals: `ai.goals` is 63 % of a galactic tick
   - ability area scans
   - movement scratch buffers
3. **Overloaded headers.**
   - `combat_system.hpp` has 1,838 lines and reaches 62 TUs.
   - `tactical_battle.hpp` has 1,698 lines.
   - `campaign.hpp` has 1,135 lines and 49 includers.
   - `renderer.hpp` has 863 lines and 59 includers.

   A private-field edit in any of them rebuilds most of `apps`.
4. **`engine/apps` holds code that belongs lower down.** `TacticalBattle` alone is 11.8 k of apps/sim's 24.7 k lines, and render has to link `alamo_sim` backwards to reach it. This is the root of the "apps is a choke point" problem from the 10-30 audit.
5. **Build scripts and probe kits are copy-paste.** The scripts have already drifted in ways that hide failures (findings 1–2 below).

## A. Correctness risks found along the way (do first)

| # | Finding | Evidence | Fix | Effort | Owner |
|---|---|---|---|---|---|
| A1 | **The gate and the build summary disagree on what a warning is.** The gate can print PASS on a log that `build_summary.py` marks FAIL. | `tools/lint/gate_summary.py:33` matches only `\bwarning( [A-Z]+\d+)?:` and does not count errors. `build_summary.py:19` also matches `: warning\b` and counts errors. | `gate_summary` calls `build_summary`'s parser. Add a fixture-log selftest for both. | S | tools |
| A2 | **"No tests ran" counts as a pass.** `build_module.cmd apps` runs zero tests, because no apps test name contains "apps". When `build_summary.py` finds no ctest result, it prints that but does not fail. | `build_module.cmd:31` (`-R "%~1"`). `build_summary.py:111` | Use `--no-tests=error`. Treat "no ctest result" as FAIL. Filter by a `module:<m>` label instead of a name regex. | S | tools / CMake |
| A3 | **Seven tests write fixed file names into `<source>/logs`**, which every build tree and worktree shares. This breaks the parallel-test rule in CLAUDE.md. | `PROJECT_SOURCE_DIR}/logs` in `ai/CMakeLists.txt:28,57`, `anim:26`, `audio:64,71`, `lua:287` and `render/anim:22`. `objects` already uses `CMAKE_BINARY_DIR`. | Point them at `${CMAKE_BINARY_DIR}/logs`. | S | per module |
| A4 | **`Resume_Hyperspace_In` does nothing.** `hyperspace_hold` is set, saved and hashed, but nothing reads it to hold reinforcements, and `CampaignBattles::resume_hyperspace_in()` has no caller. Lua records the op and the battle ignores it. | `campaign_battles.hpp:208,234`; `campaign_battles.cpp:997,1047,1081`; `lua/src/bind_presentation.cpp:588` | Either wire both the hold and the resume, or drop the field. This is a parity gap, not just dead code. | S–M | apps |
| A5 | **`skirmish_save_start` has drifted from `skirmish_ai_run`.** The save path drops `skirmish_map`, `skip_human_units`, `max_frames=0` for map hosts, and the OPT2 tuning settings. | `apps/headless/skirmish_run.cpp:645-697` vs `:928-976` | One `make_skirmish_battle_options()` used by both. | S | apps |
| A6 | **`ALAMO_EAW_DATA` is honoured by only 2 of the 7 data-load copies.** | `ai_run.cpp:126` and `save_run.cpp:495` read it. The others (`battle_run`, `map_run` ×2, `skirmish_run` ×2, `game_data`) do not. | See C4. | — | apps |
| A7 | **The meg and formats tools build without `/fp:precise` and `-ffp-contract=off`**, yet `alamo_formats` is linked into sim `alamo_anim` and does float math. The risk is latent, because no FMA instruction set is enabled today. Also, `-Wconversion -Wshadow` exist only on the script command lines, so an IDE configure misses them. | `tools/meg`, `tools/formats` CMake files; `ala.cpp` `quat_from_i16` | Link `alamo_engine_flags` into both. Move the clang warning flags into that target. | S | CMake |

## B. Hot-path performance (sim)

These are ranked by how much tick time the profiler attributes to each area (`docs/PHASE4.md` profile baselines). Every fix keeps ascending-id or index order, so determinism is unaffected. Re-run `bench_msvc.cmd` before and after each one.

| # | Finding | Evidence | Fix | Effort |
|---|---|---|---|---|
| B1 | **Galactic AI builds strings on every equation evaluation**: an upper-cased name copy, a `std::map<std::string>` stats increment, two `snprintf("%.17g")` cache keys per leaf, and a global mutex. `ai.goals` is 63 % of a galactic tick. | `ai/src/galactic_ai.cpp:95,115-116,542-554`; `galactic_ai_tokens.cpp:224-245,532-545`; `galactic_ai_tokens.hpp:226` | Resolve equation indices at link time. Use integer stats counters and format them only in `stats()`. Intern leaf and parameter text to ids when the equations are compiled. | M |
| B2 | **`BattleAi::fill` does 10 string-tag type lookups per object per tick**, each one building a `normalize_name` string and doing a hash lookup inside `parallel_for`. It also rebuilds two `std::map`s and scans `containers_` linearly per TEAM object. `tac.ai` is 14–20 % of a land tick. | `apps/sim/src/battle_ai.cpp:587,659,691,709-722,782`; `objects/src/type_fields.cpp:94` | Compute per-type flags once, as `avoid_snapshot` already does. Reuse scratch buffers. Use `Unit::container`. | S |
| B3 | **Ability area scans cost 6–8 binary searches per unit.** Each loop goes `unit_id_at(i)` → `alive_unit(id)`, `owner(id)`, `position(id)` and so on, and each of those calls is a `lower_bound`. | 14 loops in `tactical/abilities/src/ability_classes.cpp` (741, 776, 799, 999, 1187, …); `combat_system.cpp:154` | Add index accessors or a `for_each_unit(UnitView)` in id order. Use `SpatialTree::query_circle_xy` for radius queries. | M |
| B4 | **Movement allocates on every tick**, despite the "no per-tick allocation" rule that combat follows. | `movement_system.cpp:578-579,643-648,770,821,957-988,1085,1183,1271-1275,1325`. The per-fighter cell copy at `:2133` is the worst. Combat: `:1775,1915`. | Member scratch vectors with `clear()`. | S–M |
| B5 | **Team membership is found by scanning every unit**, once per team per tick, and twice per damage request for redirect and share. | `combat_system.cpp:830,846,1658,1777,3143-3147` | Per-team member lists in id order, rebuilt next to `team_heads_`. | M |
| B6 | **Tactical perception matches type names as strings per object.** | `ai/tactical/src/tactical_perception.cpp:327-331,944-1004` | Resolve `Parameter_Type` names to sorted type-index lists once per equation node. | S–M |
| B7 | **"Which planet or fleet holds this unit" is an O(units in galaxy) scan**, written 7 times. The galactic AI calls it per unit inside task-force loops. | `ai_hooks.cpp:301,497`; `story_ops.cpp:14,32,158`; `campaign.cpp:957`; `galactic_ai.cpp:157,1170-1180` | Step 1: one helper. Step 2: a derived slot→location vector that is rebuilt rather than saved. | S, then M |
| B8 | **Smaller allocation and copy costs.** <ul><li>`EventBus::deliver` copies every `std::function` subscriber on each call.</li><li>`StoryModel::dispatch` allocates an upper-cased copy of each event, copies `walk` vectors at 6 sites and compares event types as strings.</li><li>The Lua `Find_*` bindings call `get_type_refs("Company_Units")` per object.</li><li>`GalacticSim::player_view` allocates about 8 bitsets per call. It is unmeasured, so profile it first.</li></ul> | `commands/src/events.cpp:112`; `galactic/src/story_model.cpp:705-1093`; `lua/src/bind_objects.cpp:705,739`; `campaign.cpp:152` | Reuse buffers. Use an enum for event types. Cache per `TypeIndex`. | S each |

## C. Structure and build time

| # | Finding | Evidence | Fix | Effort |
|---|---|---|---|---|
| C1 | **Move the battle layer out of `engine/apps`.** `TacticalBattle` depends only on lower modules, apart from two upward includes. Render links `alamo_sim` through a deferred CMake function just to reach it. | `render/CMakeLists.txt:221-232`; `render/src/sim_snapshot.cpp:18`; `tactical_battle_l1.cpp:12` | Create a new `engine/battle` library (TacticalBattle, BattleCatalog, game_modes, sim_anim). Move the pure rule functions (`land_drop_layout`, `fighter_chase_test`, …) to `tactical/movement`. This removes the main reason apps is single-owner. | L |
| C2 | **Split `combat_system.hpp`** (1,838 lines, 62 TUs): about 330 lines of descriptors, about 270 lines of events and views, and the private `Unit`. | `combat_system.hpp:157-494,709-974,1394+` | `combat_data.hpp`, `combat_events.hpp` and the system header. Forward-declare in `ai/tactical/damage_tracking.hpp`. | M |
| C3 | **God classes.** <ul><li>`TacticalBattle`: about 193 public methods; `friend BattleAi` makes 205 `b_->` accesses.</li><li>`GalacticSim`: about 350 members, declared in `campaign.hpp:355-1133`.</li><li>`Renderer`: 113 members across 11 .cpp files, friends with `WorldRenderer` both ways.</li></ul> | as listed | <ul><li>Give `BattleAi` a narrow view instead of friendship.</li><li>Split `tactical_battle.cpp` at its section markers.</li><li>Move lasers and beams into their own passes.</li><li>Pimpl the renderer's private state.</li></ul> | M–L |
| C4 | **Seven copies of the data-load sequence** (`Vfs::create` → `db.load` → `types.build` → rules), plus three copies of the AI setup. `_dupenv_s` env helpers appear in 32 files. | `game_data.cpp:27-85`; `ai_run.cpp:131`; `battle_run.cpp:75`; `map_run.cpp:67,162`; `save_run.cpp:499-552`; `skirmish_run.cpp:307,414-428` | Move `load_game_data` down into the sim lib and add a core `env` helper. This also fixes A6. | M |
| C5 | **Save, load and hash field lists are kept by hand in three places per system.** CombatSystem `write`, `read` and `hash_extended` are 136, 390 and 148 lines. The same holds for AbilitySystem, ObjectStore and both AI worlds (`read` 471 and 455 lines). A field missed in one list is a silent save or CRC bug. | `combat_save.cpp:107,244`; `combat_system.cpp:4942` | A `template<class Ar> visit(Ar&, T&)` shared by the writer, reader and hasher. The byte order must stay identical, and the save round-trip and CRC golden tests guard it. | L |
| C6 | **Oversized functions** (lines): <ul><li>`run_gpu` 3,494</li><li>`render/app main()` 853</li><li>`Renderer::build` 820</li><li>`AbilitySystem::class_update` 509 and `class_apply` 472</li><li>`GalacticAiWorld::read` 471</li><li>`Renderer::render` 466</li><li>`App::make_script` 425</li><li>`BattleAi::fill` 414</li><li>`FrontEnd::on_click` 306</li><li>`GalacticSim::read` 292</li></ul> | brace-count script | Split along the existing phase comments or the per-class switch. Do one per PR, with the CRC and smoke tests as the guard. | M each |
| C7 | **Two animation stacks**: `assets::sample` looks up bones by name on every sample, alongside `anim::Clip`. There are three sets of math types. `map_scene.cpp:715-729` keeps both paths, and their looping behaviour differs. | `assets/src/skeleton.cpp:73`; `anim/src/clip.cpp:115` | Render uses `anim` clips everywhere. | M–L |
| C8 | **No precompiled headers or unity build**, and 3,334 lines of CMake boilerplate: 75 `add_test(NAME X COMMAND X)`, 74 single `TIMEOUT` properties and 26 identical "link vfs + `HAVE_VFS`" blocks. | all `engine/*/CMakeLists.txt` | `alamo_add_test_exe(name SOURCES … LIBS … TIMEOUT n [SLOW])`. Measure PCH against sccache, because sccache does not cache MSVC `/Yc`. | M |
| C9 | **About 570 optional-module `#if` branches that no build compiles.** FOG and GARRISON are always built with tactical, so their `#else` stubs are dead. | 31 `ALAMO_*_HAVE_*` defines across 116 files; `tactical/CMakeLists.txt:29-30` | Make the always-present sim modules non-optional and delete their stubs. Keep the flags only where a module can really be absent (render, audio, video). | M |

## D. Duplication (shared helpers)

| # | What | Where | Saves |
|---|---|---|---|
| D1 | Test harness: `CHECK`/`g_fail` in 83 files, data-path lookup in 71, `write_file` in 18 | engine-wide | Add `engine/core/testing/alamo_test.hpp` (INTERFACE target) with `CHECK`, `data_root()` and `skip()` → exit 77, plus `SKIP_RETURN_CODE 77`. Skip counts then become exact. |
| D2 | Lua binding helpers: `push_nil`/`push_bool` ×8, `iequal`+`upper` ×5, `cls_position`/`cls_target` ×5. The PositionWrapper bit layout is repeated in 5 files. | `lua/src/bind_*.cpp` | About 280 lines → `bind_common.hpp` |
| D3 | Case-insensitive compare: 45–52 local copies, because `alamo::iequals` also treats `/` and `\` as equal | engine-wide | Add `ascii_iequals` to core |
| D4 | Save helpers: `get_count` ×7 with inconsistent argument order, `get_vec3` ×7, `put_handle` ×3. Galactic and AI encode handles differently. | galactic, ai, objects, movement | Core `save_io.hpp` (ai already has `ai_save_io.hpp`) |
| D5 | HUD helpers: `ascii32` ×7, `num32` ×5, `hit_test` ×3 (identical), `frame_rect` ×2, two thousands formatters | `ui/src/galactic_hud.cpp`, `tactical_hud.cpp`, `hud_panels.cpp` | About 150 lines → `hud_util.hpp` |
| D6 | GPU test setup (`Gpu`/`Offscreen`/`render_frame`) redefined 10 times, although `test_render_common.hpp:109-190` already exists | render, particles and galactic tests | Shared render test library |
| D7 | Post-pass Vulkan setup: TAA, SSAO, gamma and particles each build their own sampled-image layout and write array, and own a pipeline cache that is never saved | `taa.cpp`, `ssao.cpp`, `gamma.cpp`, `particle_gpu.cpp` | Two rhi helpers and one shared, persisted cache |
| D8 | Per-frame growing vertex buffer ×3 (with `% 4` hard-coded); texture upload ×2 (the particle copy skips validation) | `lasers.cpp:311`, `beams_draw.cpp:143`, `decorations.cpp:456`; `renderer.cpp:597` vs `particle_gpu.cpp:333` | A transient allocator on `rhi::Frame`; one upload function |
| D9 | Viewer apps: `sim_main.cpp` and `galactic_main.cpp` share 62 duplicated windows (an identical `Offscreen`, renderer setup and windowed loop), with a third loop in `render/app/main.cpp` | `render/app`, `render/galactic/app` | `app_common.hpp` |
| D10 | Galactic and tactical AI: identical `equation()`, Find_Plan_For_Goal candidate build | `galactic_ai.cpp:94,531`; `tactical_ai.cpp:331,1026` | `ai::EquationIndex`, `ai::plan_candidates()` |
| D11 | Angle constants: `kDegToRad` ×3, two `wrap_deg` with different ranges, `kRadToDeg` set to the retail `57.295776f` in one place and the exact value elsewhere | `combat_system.cpp:34`, `movement/common.hpp:48`, `rocket.cpp:16-17` | Put the constants in `core/dmath.hpp` with the retail value named. **Keep the two wraps separate** under distinct names: merging them would change results in the last bit. |
| D12 | CRC-32 ×4, chunk walker ×2 (terrain vs `tools/formats`), `normalize_name` ×2 | `core/hash.cpp`, `render/png.cpp`, `tools/formats/common.cpp`, `tools/meg/meg.cpp` | Use the core ones |
| D13 | Build scripts: 266 lines across `build_*.cmd` and `bench_msvc.cmd`; msvc and clang share 21 of 30 unique lines. `bench_msvc.cmd` hard-codes the game path that `test_env.cmd` already sets. | repo root | One `tools\build_common.cmd`, with each root script reduced to about 5 lines |
| D14 | Probe kits: about 20 identical run/cleanup wrappers, `build_lua50.cmd` ×3, `build_reader.cmd` ×2, 7 `*_cleanup.py` that differ by about 18 lines, and the game path hard-coded in 10 .py files | `tools/probe_mod/` | `probe_mod/common/` plus one `probe.cmd <kit> <script>` |

## E. Dead code

Each item was grepped across `engine/` and `tools/`, tests included. Delete it, or wire it up if it is meant to be Lua API.

**Combat**
- `unit_fire_range`: declared but never defined
- `max_energy`, `set_energy`
- `rocket_terrain()`

**Abilities**
- `retreat_blocked`, `weather_affected`, `interdicted`
- `set_force_disabled`, `set_player_stealth_bonus`, `get_shield`

**Movement**
- `build_fighter_grids`, `fjoin`, `fighter_formations()`

**Objects**
- `sync_rng()`, `interval_config()`

**Apps**
- `TacticalBattle::death_clones_pending`, `hangar_launches`, `retreat_attrition_kills`
- `game_modes` `ai_services`, `income_events`, `set_income_interval`
- `GameSession::presentation_rate_lock`, `AttractController::map_started`, `is_saveable`
- `set_idle_type_hook`: never called, so the `idle_hook_` branches at `sim_anim.cpp:332,379` are dead

**Render and assets**
- `camera.hpp` `to_mat3x4`
- `set_blob_color`, `blend_texture_r8`, `is_block_compressed`
- `advance_one`, `frame_changed`
- `has_non_heat`, `has_heat`, `set_scales`

**Materials and shaderxlat**
- materials: `draw_pass_count`, `technique_lod_name`, `depends_on`
- shaderxlat: `version_token`

**Audio and video**
- audio: `active_count`, `world_sfx`, `last_error`, `bus_name`
- video: `audio_frames_decoded`

**UI**
- The multiplayer lobby: `ui/src/gui_lobby.cpp` (727 lines) and its header (214 lines). Multiplayer has been out of scope since 2026-10-29, and `MenuAction::OpenMultiplayer` has no consumer. Deleting it needs Gabriel's OK.

**Tools**
- `tools/re/disfn.py`
- `tools/re/ghidra/lua_bind_map.py`
- `tools/probe_mod/fixpass_capture/run_plain.py`

**Stray output**
- Root `INDEX.json` (173 KB), written by `tools/meg/build_reports.py`

## F. Render per-frame costs

- **Particle pipelines are compiled lazily on the frame thread.** `ParticleRenderer::pipeline()` runs inside `draw_range` with a `std::map<tuple>` lookup per draw item (`particle_gpu.cpp:384-431,737`). TAA and gamma do the same in `record`. Pre-create the pipelines at init.
- **`ParticleRenderer::prepare` allocates every frame.** It builds an `unordered_map`, six vectors and nested vectors per visible system (`:442-484`), then runs `stable_sort` over structs that hold vectors.
- **`MapScene::pose_idle_anim` allocates a pose vector per animated placement per frame**, then copies it with memcpy into `a.world`, which has the same layout (`map_scene.cpp:718`). Write the pose into `a.world` directly.
- **`record_range` rebinds the vertex and index buffers on every draw** (`renderer_draw.cpp:1154,1178`).

## G. Repo, docs and tooling hygiene

**Repo weight**
- `tools/probe_mod/fixpass_capture/` holds 29 retail screenshots: 38 MB, 14 % of the pack. Move them to `docs/archive` or keep them out of git. This needs Gabriel's OK, because docs link to them.
- The two generated `ROADMAP.html` copies have 9.6 MB of history. Committing only `docs/ROADMAP.html` would stop that growth.

**Docs**
- `docs/PHASE4.md` has grown from 155 KB to 338 KB. 97 lines are over 400 characters, and one is 32 k characters. Keep it a board of about one line per item and archive the logs.
- CLAUDE.md rule 6, `.claude/agents/integrator.md` and `.claude/commands/phase-status.md` name `claude/roadmap.md` and `claude/phase2-status.md`. Neither file exists.
- `.claude/agents/engine-module.md` tells agents to run a raw `cmake -B build-<module>`. It should name `build_module.cmd`.
- CLAUDE.md says the gate takes about 4 minutes; `build_gate.cmd` says about 10.

**Tooling**
- `build_agent.cmd` doubles the `build-` prefix when it is given `build-X`, which is how this worktree got four `build-build-*` trees (about 1.4 GB). This worktree holds 7.1 GB of build trees in all.
- `.gitattributes` does not declare `*.acmd` or `*.rec` as binary.
- `build_summary.py`, `ctest_skips.py` and `gate_summary.py` have no selftests, and no ctest entry runs any Python selftest.

## Suggested order (work packages)

Each package stays inside one owner's folder where it can. Packages 1 and 2 can run in parallel with everything else.

1. **Gate integrity** (tools + CMake, S): A1, A2, A3, A7, `build-build-` naming, `.gitattributes`, the stale `.claude` agent docs, and Python selftests for the lint scripts.
2. **Test harness** (core via integrator, then mechanical per module, M): D1. Do this before splitting any big test file, so the splits land on the shared header.
3. **AI hot paths** (ai + apps, S–M): B1, B2, B6, D10. These are the biggest measured tick costs. Benchmark before and after.
4. **Tactical hot paths** (tactical, M): B3, B4, B5, then C6 `class_update`/`class_apply`, which pairs with B3's helper.
5. **Parity gap** (apps, S–M): A4 hyperspace hold, plus A5/A6 through C4.
6. **Header splits** (tactical, render, M): C2, then the Renderer pimpl (C3). These cut incremental rebuilds of apps.
7. **Battle library** (integrator, L): C1. This is the biggest structural payoff. It is easiest after 4–6 have shrunk the pieces.
8. **Dead code and duplicates** (each owner, S): E and D2–D9, D11–D14. These are good filler tasks for idle agents.
9. **Save visitor** (L, careful): C5, behind the round-trip and CRC golden tests.

## Checked and found fine

- **Determinism**:
  - `ObjectStore::by_id_` is sorted before it is saved.
  - `parallel_for_ref` splits work into deterministic chunks.
  - The scheduler sort uses a total key.
  - `crush_pass` re-sorts its candidates.
  - Sim-side containers are ordered maps or sorted vectors.
- **Lookups**: `unit_by_id`, `fighter_formation` and `PerceptionWorld::object` use binary search. `avoid_snapshot` and `land_obstacle_attach` already cache per type.
- **Renderer**:
  - The radix draw sort reuses its scratch buffers.
  - Per-frame descriptor pools are used.
  - Device idle waits happen only on resize or a settings change.
  - Main pipelines are created asynchronously and cached on disk.
  - The Vulkan header stays out of the sim modules.
- **Parsers**:
  - There is one ALO/ALA parser: `alamo_formats`, linked privately.
  - FXLC and d3dbc are different formats, so their parsers are not duplicates.
- **UI**: draw lists reuse their capacity. UI headers do not include galactic headers.
- **Lua**: dispatch overhead is real, but the profile puts `lua.exec` at 0.1–3 % of tick time, so it ranks low.
- **CMake**: `alamo_label_slow` and `alamo_add_worker_count_check` are well designed. `test_env.cmd` and `cache_env.cmd` are shared. `test_rhi.cpp` is already split into cases.
- **Debt markers** in `engine/`: TODO 4, FIXME 0, XXX 4, `#if 0` 0. The 289 "hack" hits are the game's hacking mechanic.
