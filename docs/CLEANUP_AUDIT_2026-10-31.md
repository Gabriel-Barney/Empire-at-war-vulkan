# Cleanup and simplification audit — 2026-10-31

Scope: whole tree (21 engine modules, about 320 K lines incl. tests, `tools/`, build scripts, docs). Read-only: nothing
was built, changed or run; this file is the only addition. Four module slices were read by separate reviewers; the
items marked **verified** were checked by grepping all callers, **suspected** were judged from reading. Two claims were
re-checked by hand while merging (see "Corrections"). Every code change below needs the normal gate (`build_gate.cmd`)
and, for sim code, identical per-tick CRCs.

Overall: the code is in good shape. No `#if 0`, no commented-out code, 2 real TODO-type markers. The payoff is in
(1) copy-pasted helpers and test boilerplate, (2) subsystems nothing calls, (3) a few oversized files, (4) repo clutter.

## Corrections to reviewer advice
- **Do not swap local `iequal` copies for `alamo::iequals`.** `engine/core/src/hash.cpp:43` folds `/` to `\` as well
  (`up()` at :17), so it is a path comparison, not a plain case-insensitive one. Add a new `core/text.hpp`
  (`ascii_iequals`, `ascii_lower`, `ascii_upper`, `ascii_trim`) and migrate to that. Check the trim whitespace set
  (`data::trim` also strips `\v\f`) and locale-dependent `toupper` copies before each swap.
- Do not merge `parse_int` / `parse_float` copies (hangar, ai_plans, combat/tables, objects `c_atof`): edge-case caps and
  `strtod` hex/inf/nan behaviour differ. `allied()` (7 variants), `wrap_deg` (3), `dist3` (2), `space_category` (2) and
  `put_handle` (4 wire layouts) also differ on purpose.

## A. Mechanical, low risk (about 2,600 lines)
1. **Shared text helpers** (`core/text.hpp`): about 50 `iequals`-style, 27 `upper`, 6 `lower`, 19 char-case and 16 `trim`
   copies across ai, galactic, tactical, lua, ui, render, apps, anim, materials, terrain, objects, assets. About
   550–650 lines. Verified. Examples: `ai/src/galactic_seed.cpp:15`, `galactic/src/economy.cpp:13`,
   `lua/src/story_model.cpp:33`, `ui/src/gui_resource.cpp:15`, `render/src/convert.cpp:42`.
2. **Shared test header** (`engine/testing/` or `tests/support/`): `CHECK` macro in 82 test files (about 650 lines),
   `FileProvider` memory copy x14–17 (about 250), `Gpu` struct x14 and `Offscreen` x14 in render/ui tests (about 600),
   `env_or` / `env_or_empty` x14 with a dead non-MSVC branch, `write_file` x17, `near()` x3, allocation counter x3 with
   GCC-only pragmas, two different `thread_parallel_for.hpp` (galactic/tests 108 lines vs tactical/tests 37; both can use
   `jobs::make_parallel_for`), three stored-block PNG writers, `FakeLuaRequest`/`FakeModels` copies. About 1,900 lines.
   Touches top-level CMake: goes through the integrating agent. Verified.
3. **Lua bind helpers**: `push_nil/bool/num` pasted in 8 `lua/src/bind_*.cpp` (about 280 call sites), `push_str`,
   `is_bool`, `to_bool`, `store()`, `types()`, `bridge()`, `self_object` in 2–3 files each. Move to `vm_internal.hpp`.
   About 130 lines. Verified.
4. **Save I/O primitives**: `get_count` / `put_vec3` / `get_vec3` in 6 files (`combat_save.cpp:46`, `garrison_save.cpp:25`,
   `hangar_save.cpp:30`, `ability_save.cpp:40`, `galactic_save.cpp:48`, `movement_save.cpp:53` with swapped args) plus
   `object_store.cpp`; three archive headers re-implement the same `put/get` chains (`apps/sim/src/save_io.hpp` 208,
   `ai/src/ai_save_io.hpp` 138, `ai/tactical/src/tactical_save_io.hpp` 158). Promote `save_io.hpp` to core, add vec3
   helpers to `core/bytes.hpp`. Bytes unchanged. Keep the differing failure semantics (`ai::save::count` fails the
   reader, `get_count` only returns false). About 400 lines. Verified.
5. **Hash / IO primitives**: FNV-1a x3 (`rhi/src/pipeline.cpp:63`, `particle_attach.cpp:18`, `materials/src/programs.cpp:19`,
   the last constexpr so core needs a constexpr overload), CRC-32 table built 4 times (`core/hash.cpp:11`,
   `sim_crc.cpp:182`, `render/png.cpp:17`, `video/tests`), `bool fail(std::string*, std::string)` x5–6, 10 hand-rolled
   whole-file binary reads (add `read_file_bytes` beside core's `read_text_file`). About 150 lines. Verified.
6. **Dead accessors / functions with zero callers** (about 250 lines), all verified by grep, including tests:
   - tactical/galactic/ai: `CombatSystem::unit_fire_range` (declared, never defined), `AbilitySystem::get_shield`,
     `interdicted`, `retreat_blocked`, `weather_affected`, `find_record` (`ability_data.cpp:424`),
     `SpaceOccupancy::has_track/remove_static`, `TreeSet::rebuild_all`, `CombatSystem::max_energy/set_energy`,
     `MovementSystem::fjoin/build_fighter_grids`, `FogOfWar::set_fow_active`, `PassabilityGrid::secondary_stamped`,
     `ConflictManager::planet_maps/last_auto_resolve`, `EngineLoops::land_transform`, `ai_mode_name`, and others;
     unused `(void)x` params at `campaign.cpp:536,608`, `special_systems.cpp:1124`, `ai_goals.cpp:307`, `land_path.cpp:199`,
     `ability_classes.cpp:112`. `PerceptionObject::valid_bombing_target` is saved but never read: leave it (save format).
   - apps: `corruption_effects`, `move_acks_spawned`, `map_started`, `ai_services`, `income_events`, `set_income_interval`,
     `set_idle_type_hook`, `find_secondary_world`, `hangar_launches`, `retreat_attrition_kills`, `is_saveable`,
     `bombing_run_frames_until_available` (`tactical_battle_l1_strikes.cpp:317`), `fog_end_setup_phase`
     (`tactical_battle_fog.cpp:329`).
   - core/commands/objects/anim: `file_count`, `SimRng::set_state`, `log::tracef`, `AnimState::set_tunables`, `field_count`,
     `wstr`, `default_value`, `is_variant`, `sync_rng`, `interval_config`, `get_ss`.
   - rhi: `RenderTarget::depth_state`, `Swapchain::supports_transfer_src`, `Uploader::ring_capacity`,
     `Window::request_close/take_wheel_horizontal/clear_text`.
7. **DXC CMake loops** copied 8 times (`render/CMakeLists.txt` x4, `ui`, `render/particles`, `rhi`, `materials`):
   one `alamo_add_dxc_shaders()` function. About 100 lines. Verified.
8. **Repo clutter** (no code risk):
   - `tools/probe_mod/fixpass_capture/` is 38 MB of PNGs tracked in git (the `.gitignore` says probe captures are never
     committed); docs only mention the folder name. `git rm` the PNGs and keep `README.md` / `run_plain.py`.
   - 5 tracked `.exe` (`g1_capture/selftest/bin`, `q2_traces/bin` x2, `s10_map_owners/bin` x2) are build outputs of tracked
     sources (`build_reader.cmd`, `build_fake_game.cmd`): untrack and ignore.
   - `fixpass_capture/run_plain.log`, `docs/shaders/shaders_decompiled.zip` and `docs/archive/*.zip` are derived or history.
   - `probe_mod` scripts share small helpers (`running`, `write_png` x3, `f32` x4, `cstr` x3, `chunks`): a `probe_common.py`.
   - Rename `render/src/taa.hpp` to `taa_pass.hpp` (it shadows the public `render/taa.hpp`). The other duplicate
     basenames are genuinely different files.

## B. Needs a decision from Gabriel (about 3,000 lines)
1. **Out-of-scope multiplayer code** (CLAUDE.md: no networking / lobby / MP):
   - `ui/src/gui_lobby.cpp` (713) + `gui_lobby.hpp` (213) + lobby tests (about 150): `LobbyScreens` is only constructed in
     tests; `gui_menus.cpp` does not reference it. Verified.
   - `commands/wire_codec.*` + `src/wire.hpp` (about 315) and their tests, and lockstep scheduling in
     `commands/scheduler` (`ScheduleMode::Lockstep`, `lan()`, `internet*()`, `lockstep_execute_frame`, `receive`,
     `set_max_ahead`, `FrameSync` at `pipeline.cpp:122`). Verified for the codec; the scheduler's mode/turn/max_ahead are
     written into saves, so removing them is a save-version bump (or keep them inert). `.acmd` goldens must stay byte-identical.
2. **Unused jobs subsystems** (verified, no caller outside `engine/jobs`): `TaskGraph` (246 lines + test), `ScratchArena` /
   `ScratchScope` / `thread_scratch` (199 + test; only `job_system.cpp` wraps each job in a scope), `exec_for`,
   `parallel_reduce`, `ResultCell` (test-only), `TickScheduler::kick_frame/system_name`. About 650 lines. Check they are
   not roadmap deliverables for the performance lane (CP/OPT items) before deleting.
3. **Headless CLI flags with no use anywhere, docs included**: `--record-stream` (about 65 lines, `main.cpp:616-680`),
   `--realtime`, `--no-ai`, `--story-out`, `--story-golden`, `--aggressor`, `--battle-retreat`. May be run by hand.
4. **Optional-module `#if ALAMO_*_HAVE_*` guards**: 531 `#if` lines (357 outside tests), 80 CMake defines, kept only for
   the `-DALAMO_ENGINE_MODULES` subset build (`build_module.cmd`, `build_agent.cmd`). `story_session_none.cpp`
   (78 lines of stubs) and the fog / garrison no-op stubs are part of this. Making lua / fog / garrison / anim
   hard dependencies of apps removes about 400+ lines and many untested configurations, but drops partial builds.
5. **`binding_table.inc`**: 97 of 466 Lua bindings have script-use count 0. Recommend keeping (mod compatibility).
6. **Golden `.crc` format**: about 11.4 MB tracked (skirmish_ai_foc 3.6 MB, synthetic 1.8 MB, game_smoke x3, ...), one
   64-bit hash per system per tick. Store the combined digest every tick and per-system columns every N ticks: about
   10 MB less and smaller diffs on every regeneration. Changes `CrcLog` and every compare tool; regenerate all goldens on
   MSVC and re-check clang. Its own task.
7. **Misplaced production code**: `objects/coverage.*` (393 lines, used only by `test_objects.cpp`); `shaderxlat/src/interpreter.cpp`
   (811 lines, used only by tests and `gpu_diff.cpp`) compiled into the shipped lib; `setup_validation_env`
   (`renderer_graph.cpp:1134`, called from 20 test/app files) lives in the renderer.

## C. Structural simplification (no line savings to speak of, readability and build time)
1. `tactical_ai.cpp` (3,398): move the N1 save/load (lines 2521–3365, `read` is 397 lines) to `tactical_ai_save.cpp`
   behind a `tactical_ai_impl.hpp`, like `galactic_ai_save.cpp`.
2. `movement_system.cpp` (2,205): fighter section (line 1275 to end, about 930 lines) to `fighter_system.cpp`.
   `combat_system.cpp` (3,306): split targeting / projectiles / take_damage along its banners.
3. `AbilitySystem::class_apply` (429 lines) and `class_update` (435 lines) in `ability_classes.cpp`: one method per class.
   `parse_ability` (`special_rules.cpp:95-137`): 36-branch `ieq` chain to a table like `kSpyFlags`.
4. Plan service duplicated between `GalacticAiWorld::Impl` and `TacticalAiWorld::Impl` (`service_plans`, `end_plan`,
   `tf_event`, `drain_world_requests`; galactic_ai.cpp:772–842 vs tactical_ai.cpp:1509–1601): shared helper with lambdas.
   About 120–180 lines. Keep the ascending-id iteration order exactly; needs the AI CRC runs at 0/1/4/N workers.
5. `app_script.cpp` + `app_script_saveload.cpp` (about 1,950 lines of smoke-test driver compiled into the shipped game
   exe; `App::make_script` is 420 lines): move to a `smoke/` object library, split by smoke kind.
6. `StorySession::write/read` (`story_session.cpp:1865/1936`), `StoryModel::write/read`, `SessionDescriptor`,
   `sim_host_save.cpp`: hand-mirrored; convert to the symmetric `save_io` field lists used in `tactical_battle_save.cpp`.
   About 250 lines. Medium risk: bytes must stay identical (test_game_save + save checks).
7. Headless run tail pasted 5 times (`ai_run.cpp:397`, `skirmish_run.cpp:580`, `battle_run.cpp:167`, `map_run.cpp` x3):
   one `finish_crc_run()`; `headless_main` (286 lines, 48-flag if/else chain) to a flag table. About 220 lines.
8. `TacticalBattle` (1,245-line header, 120+ members over 11 `.cpp` files): extract fog / garrison / hangar / pads as
   member objects. High effort and CRC risk: last, goldens as the safety net.
9. `GalacticHud` / `TacticalHud` duplicate base plumbing (`resize`, `hit_test`, `component_size`, `font_of`): `HudLayer`
   base, about 100 lines. Fullscreen-pass setup (`gamma`, `ssao`, `taa`, `hud_renderer`, `particle_gpu`): an RHI
   `create_sampler_set_layout` helper plus a shared `fullscreen.hlsli` vertex shader, about 150 lines. Lasers / beams
   fixed-function template and texture-fallback code repeated 5x, about 100 lines.
10. `terrain/src/decorations.cpp` (976) = four independent builders: split. `vm_persist.cpp` (1,649) = Saver / Loader /
    Validator / Builder: split. `bind_script_gaps.cpp`, `bind_script_remaining.cpp`, `bind_ai_tactical.cpp` are named after
    development waves, not domains: fold into objects / players / presentation families.
11. Giant functions (mostly test code): `run_gpu` (`test_render_gpu.cpp:6`, 3,438 lines), `test_real` (941), render app
    `main()` (853), `FrontEnd::on_click` (304), `ParticleRenderer::prepare` (263), `StorySession::apply_reward` (221) and
    `StoryModel::evaluate` (200) as `type == "..."` chains.
12. Build scripts: `build_module.cmd` is a subset of `build_agent.cmd`; `build_msvc.cmd` and `build_clang.cmd` differ by
    compiler setup only. One `build.cmd <msvc|clang>` core plus thin wrappers would remove about 80 lines, but scripts are
    named in CLAUDE.md, docs and agent definitions, so low priority.

## D. Not worth changing
- The 466-row generated `binding_table.inc`, `field_schema_table.inc` (generated, committed), `Interpreter::execute` (a
  plain opcode switch), `anim::Float3` (deliberate, `static_assert` against `assets::Mat3x4`), the five `Vec3` types.
- `render/ted.hpp` (30-line shim) and the duplicate `camera`/`decorations`/`pipeline`/`constants` basenames: distinct files.
- Docs: `docs/PHASE4.md` (172 KB) and the two catalog JSONs (6.4 MB) are large but are reference data; the big win would
  be pruning finished items from `PHASE4.md`, which the earlier audit (finding 13) already covers.

## Suggested order
1. A1, A3, A7, A8 (text helpers, Lua helpers, CMake DXC function, untrack PNGs/exes). One PR each, full gate.
2. A2 (shared test header) in one change, since it touches top-level CMake.
3. A4–A6 (save primitives, hash/IO primitives, dead code) per module, clang + ASan gate for anything touching `jobs`.
4. B decisions, then C in the order 1, 2, 5, 7, 4, 6, 8.
Expected total: about 5,000–6,000 lines removed or relocated, and about 40 MB less tracked in git, without behaviour change.
