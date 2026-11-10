# P host (owner: game host agent): alamo_game, the playable game exe - window + input, menus / load screens, the
# galactic HUD over the galactic sim, the tactical HUD over the battles, the Vulkan renderer and audio, per frame.
#   alamo_game_core   render-free session library (game data, GameSession = SimHost + campaign + CampaignBattles +
#                     SimAnimation + human order routing, audio host); used by the exe, its tests and replays.
#   alamo_game        the exe (created by a deferred call: it needs alamo_render_sim / alamo_render_galactic / alamo_ui_draw,
#                     which engine/render creates at the end of the top-level configure).
if(NOT TARGET alamo_sim OR NOT TARGET alamo_tactical_movement OR NOT TARGET alamo_tactical_combat OR NOT TARGET alamo_tactical_abilities)
  message(STATUS "alamo_game: alamo_sim with battles not configured; game host skipped")
  return()
endif()

add_library(alamo_game_core STATIC
  src/game_data.cpp
  src/game_session.cpp
  src/battle_orders.cpp
  src/game_audio_host.cpp
  src/battle_fx.cpp
  src/story_script.cpp
  src/game_controls.cpp
  src/attract_mode.cpp
  src/hud_feeds.cpp
  src/battle_summary.cpp
  src/story_ui.cpp
  src/save_games.cpp
  src/mod_check.cpp
  src/mod_picker.cpp
  src/benchmark.cpp
  src/benchmark_scenario.cpp
  src/benchmark_save.cpp
)
target_include_directories(alamo_game_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(alamo_game_core PUBLIC alamo_sim alamo_vfs alamo_data alamo_core alamo_engine_flags)
target_link_libraries(alamo_game_core PUBLIC alamo_game_rules)  # q6 t03: skirmish rules shared with alamo_headless (rules/)
if(TARGET alamo_ai_galactic)  # A2 galactic AI for the AI players (needs engine/ai + engine/lua)
  target_link_libraries(alamo_game_core PUBLIC alamo_ai_galactic)
  target_compile_definitions(alamo_game_core PUBLIC ALAMO_GAME_HAVE_GALACTIC_AI=1)
endif()
if(TARGET alamo_ui)  # pause_event (orders.hpp)
  target_link_libraries(alamo_game_core PUBLIC alamo_ui)
  target_compile_definitions(alamo_game_core PUBLIC ALAMO_GAME_HAVE_UI=1)
  target_sources(alamo_game_core PRIVATE src/world_pick.cpp)  # refixf t08: tactical world pick tests (world_pick.hpp)
endif()
if(TARGET alamo_ui_hud)  # OI host: hotkey table + menus (game_controls.hpp)
  target_link_libraries(alamo_game_core PUBLIC alamo_ui_hud)
  target_compile_definitions(alamo_game_core PUBLIC ALAMO_GAME_HAVE_CONTROLS=1)
  target_sources(alamo_game_core PRIVATE src/radar_feed.cpp)  # radar feeds (radar_feed.hpp)
endif()
if(TARGET alamo_lua_vm)  # Q5 mod check: compiles the data's Lua scripts (mod_check.cpp)
  target_link_libraries(alamo_game_core PRIVATE alamo_lua_vm)
  target_compile_definitions(alamo_game_core PRIVATE ALAMO_GAME_HAVE_LUA_VM=1)
endif()
if(TARGET alamo_materials AND TARGET alamo_assets)  # modc t21 mod check: effect audit + model materials (mod_check.cpp)
  target_link_libraries(alamo_game_core PRIVATE alamo_materials alamo_assets)
  target_compile_definitions(alamo_game_core PRIVATE ALAMO_GAME_HAVE_MATERIALS=1)
endif()
if(TARGET alamo_audio)
  target_link_libraries(alamo_game_core PUBLIC alamo_audio)
  target_compile_definitions(alamo_game_core PUBLIC ALAMO_GAME_HAVE_AUDIO=1)
endif()

# Data-free: synthetic skirmish through GameSession (human orders routed into the battle, battle end, record/replay CRC).
add_executable(alamo_game_session_tests tests/test_game_session.cpp)
target_link_libraries(alamo_game_session_tests PRIVATE alamo_core alamo_game_core alamo_test_support)
add_test(NAME alamo_game_session_tests COMMAND alamo_game_session_tests)
set_tests_properties(alamo_game_session_tests PROPERTIES TIMEOUT 600)

# N1 / t09: save / load on the FoC data (ALAMO_FOC_DATA, skipped without): a campaign saved through the menus' save path
# at tick T, loaded by a fresh process (this exe --child) through the menus' load path: per-tick CRC == the uninterrupted
# run from T on; load + replay in process; another seed refused; animation-on saves load or are refused at the check.
add_executable(alamo_game_save_tests tests/test_game_save.cpp)
target_link_libraries(alamo_game_save_tests PRIVATE alamo_core alamo_game_core alamo_test_support)
add_test(NAME alamo_game_save_tests COMMAND alamo_game_save_tests)
set_tests_properties(alamo_game_save_tests PROPERTIES TIMEOUT 1200)
alamo_label_slow(alamo_game_save_tests)

# p4ui t06 (UI.md section 9): the skirmish map catalog (official / custom / mod-folder maps) on a fake install in its own temp
# folder; with ALAMO_FOC_DATA also the shipped data (24 land + 24 space official rows, no custom).
add_executable(alamo_game_skirmish_maps_tests tests/test_skirmish_maps.cpp)
target_link_libraries(alamo_game_skirmish_maps_tests PRIVATE alamo_core alamo_game_core alamo_test_support)
add_test(NAME alamo_game_skirmish_maps_tests COMMAND alamo_game_skirmish_maps_tests)
set_tests_properties(alamo_game_skirmish_maps_tests PROPERTIES TIMEOUT 120)

# Playtest 2 t13: auto-resolve results screen figures (battle time, bounty, efficiency): pure rules data-free, bounty data
# on the FoC data (ALAMO_FOC_DATA, skipped without).
add_executable(alamo_game_battle_summary_tests tests/test_battle_summary.cpp)
target_link_libraries(alamo_game_battle_summary_tests PRIVATE alamo_core alamo_game_core alamo_test_support)
add_test(NAME alamo_game_battle_summary_tests COMMAND alamo_game_battle_summary_tests)
set_tests_properties(alamo_game_battle_summary_tests PROPERTIES TIMEOUT 600)

# BM2a data-free: the benchmark mode's options / flags, presets, fixed-frames clock, frame-time statistics and JSON report
# (benchmark.hpp; the exe side is app/app_benchmark.cpp, checked by alamo_game_bench_* below).
add_executable(alamo_game_benchmark_tests tests/test_benchmark.cpp)
target_link_libraries(alamo_game_benchmark_tests PRIVATE alamo_core alamo_game_core alamo_test_support)
add_test(NAME alamo_game_benchmark_tests COMMAND alamo_game_benchmark_tests)
set_tests_properties(alamo_game_benchmark_tests PROPERTIES TIMEOUT 60)

# Data-free: GameAudioHost movie music (modes 14 / 17, Halt_Game_Music) on a synthetic audio tree and the null backend.
add_executable(alamo_game_audio_host_tests tests/test_game_audio_host.cpp)
target_link_libraries(alamo_game_audio_host_tests PRIVATE alamo_core alamo_game_core alamo_test_support)
add_test(NAME alamo_game_audio_host_tests COMMAND alamo_game_audio_host_tests)
set_tests_properties(alamo_game_audio_host_tests PROPERTIES TIMEOUT 120)

# Data-free (refixf t08): the tactical world pick (HUD_GAPS §2.2; world_pick.hpp + engine/ui pick_world) on a SpaceMode
# with hand-built model collision data: mesh hit, box-but-not-mesh miss, Mouse_Collide_Override_Sphere_Radius, nearest hit.
if(TARGET alamo_ui)
  add_executable(alamo_game_world_pick_tests tests/test_world_pick.cpp)
  target_link_libraries(alamo_game_world_pick_tests PRIVATE alamo_core alamo_game_core alamo_test_support)
  add_test(NAME alamo_game_world_pick_tests COMMAND alamo_game_world_pick_tests)
  set_tests_properties(alamo_game_world_pick_tests PROPERTIES TIMEOUT 120)
endif()

# Data-free (refix t06, refixf t05): the tactical super weapon's presentation cues (battle_fx.hpp), the cinematic camera
# (TSW firing shot, explosion shake, letterbox; TSW_PLANET_DESTROY.md §11) and GameAudioHost::tsw_audio on a synthetic type set / audio tree with the null backend.
add_executable(alamo_game_battle_fx_tests tests/test_battle_fx.cpp)
target_link_libraries(alamo_game_battle_fx_tests PRIVATE alamo_core alamo_game_core alamo_test_support)
add_test(NAME alamo_game_battle_fx_tests COMMAND alamo_game_battle_fx_tests)
set_tests_properties(alamo_game_battle_fx_tests PROPERTIES TIMEOUT 120)

# Q5 mod diagnostics (mod_check.hpp): alamo_mod_check --game DIR [MODPATH=..] reports what the port makes of a mod's data.
# The test runs it on a fake install with a mod folder (new / replaced files, a broken script, a campaign without planets)
# in its own temp folder; with ALAMO_FOC_DATA also the shipped data (no errors).
add_executable(alamo_mod_check tools/mod_check_main.cpp)
target_link_libraries(alamo_mod_check PRIVATE alamo_core alamo_game_core)
if(COMMAND alamo_shaderxlat_copy_dxc)  # modc t21: the runtime DXC compiles the audited shader passes
  alamo_shaderxlat_copy_dxc(alamo_mod_check)
endif()
# q6 t02: alamo_session_run starts a GameSession (campaign or skirmish) on any data set (MODPATH=..) and ticks it, no window.
add_executable(alamo_session_run tools/session_run_main.cpp)
target_link_libraries(alamo_session_run PRIVATE alamo_core alamo_game_core)
add_executable(alamo_game_mod_check_tests tests/test_mod_check.cpp)
target_link_libraries(alamo_game_mod_check_tests PRIVATE alamo_core alamo_game_core alamo_test_support)
add_test(NAME alamo_game_mod_check_tests COMMAND alamo_game_mod_check_tests)
set_tests_properties(alamo_game_mod_check_tests PROPERTIES TIMEOUT 300)
# modc t07: the synthetic mod of testdata/mod_fixture (a new faction, unit, planet, campaign and a Lua override) through
# the real MODPATH= route, no game data; read only (no fixture path shared with any other test). The second entry runs the
# exe itself and checks its JSON summary (docs/MODS.md section 5.1).
add_executable(alamo_game_mod_fixture_tests tests/test_mod_fixture.cpp)
target_link_libraries(alamo_game_mod_fixture_tests PRIVATE alamo_core alamo_game_core alamo_test_support)
add_test(NAME alamo_game_mod_fixture_tests COMMAND alamo_game_mod_fixture_tests ${CMAKE_CURRENT_SOURCE_DIR}/testdata/mod_fixture)
set_tests_properties(alamo_game_mod_fixture_tests PROPERTIES TIMEOUT 120)
add_test(NAME alamo_mod_check_fixture
         COMMAND alamo_mod_check --game ${CMAKE_CURRENT_SOURCE_DIR}/testdata/mod_fixture/corruption MODPATH=Mods/FixtureMod --json -)
set_tests_properties(alamo_mod_check_fixture PROPERTIES TIMEOUT 120
                     PASS_REGULAR_EXPRESSION "\"result\": \"ok\",[^{]*\"errors\": 0,"
                     FAIL_REGULAR_EXPRESSION "\"run_errors\": \\[[^]]")

# modc t08: the mod picker's data side (mod_picker.hpp) on the same read-only fixture; its modinfo.json decoding cases write
# to their own temp folder (%TEMP%lamo_mod_picker_tests, no other test uses it).
add_executable(alamo_game_mod_picker_tests tests/test_mod_picker.cpp)
target_link_libraries(alamo_game_mod_picker_tests PRIVATE alamo_core alamo_game_core alamo_test_support)
add_test(NAME alamo_game_mod_picker_tests COMMAND alamo_game_mod_picker_tests ${CMAKE_CURRENT_SOURCE_DIR}/testdata/mod_fixture)
set_tests_properties(alamo_game_mod_picker_tests PROPERTIES TIMEOUT 120)

# q6 t08: AI player types (Basic_AI / AI_Player_Control) and default skirmish forces for mod factions on synthetic data in its
# own temp folder; with ALAMO_FOC_DATA also the shipped factions (they resolve as the port's hard-coded tables).
add_executable(alamo_game_mod_faction_tests tests/test_mod_factions.cpp)
target_link_libraries(alamo_game_mod_faction_tests PRIVATE alamo_core alamo_game_core alamo_test_support)
add_test(NAME alamo_game_mod_faction_tests COMMAND alamo_game_mod_faction_tests)
set_tests_properties(alamo_game_mod_faction_tests PROPERTIES TIMEOUT 300)

function(alamo_game_add_exe)
  foreach(_dep alamo_render_sim alamo_render_galactic alamo_ui_draw alamo_ui_hud alamo_ui_rhi)
    if(NOT TARGET ${_dep})
      message(STATUS "alamo_game: ${_dep} not configured; alamo_game exe skipped")
      return()
    endif()
  endforeach()
  set(_dir ${CMAKE_CURRENT_FUNCTION_LIST_DIR})
  add_executable(alamo_game ${_dir}/app/game_main.cpp ${_dir}/app/app_session.cpp ${_dir}/app/app_input.cpp ${_dir}/app/app_screens.cpp
                           ${_dir}/app/app_hud.cpp ${_dir}/app/app_frame.cpp ${_dir}/app/app_cinematic.cpp ${_dir}/app/app_script.cpp ${_dir}/app/app_script_saveload.cpp ${_dir}/app/app_benchmark.cpp ${_dir}/app/app_bench_scenario.cpp ${_dir}/app/game_render.cpp ${_dir}/app/game_ui.cpp ${_dir}/app/game_movies.cpp ${_dir}/app/frame_overlay.cpp
                           ${_dir}/app/game_info.cpp ${_dir}/app/game_display.cpp ${_dir}/app/app_mods.cpp)
  # alamo_core first: tools/formats' inline ByteReader vs core's (LNK2005, see alamo_render_app).
  target_link_libraries(alamo_game PRIVATE alamo_core alamo_game_core alamo_render_galactic alamo_render_sim alamo_ui_draw alamo_ui_hud
                                           alamo_ui_rhi alamo_engine_flags)
  target_compile_definitions(alamo_game PRIVATE "ALAMO_RENDER_SOURCE_DIR=\"${PROJECT_SOURCE_DIR}\"")
  if(TARGET alamo_render_particles_sim)
    target_link_libraries(alamo_game PRIVATE alamo_render_particles_sim)
    target_compile_definitions(alamo_game PRIVATE ALAMO_PARTICLES_HAVE_SIM=1)
  endif()
  if(TARGET alamo_ui_lua)  # ui::hud_objectives (the story's objectives for the tactical objectives box)
    target_link_libraries(alamo_game PRIVATE alamo_ui_lua)
    target_compile_definitions(alamo_game PRIVATE ALAMO_UI_HAVE_LUA=1)
  endif()
  if(TARGET alamo_render_anim)
    target_link_libraries(alamo_game PRIVATE alamo_render_anim)
    target_compile_definitions(alamo_game PRIVATE ALAMO_RENDER_HAVE_ANIM_DRIVER=1)
  endif()
  if(TARGET alamo_video AND TARGET alamo_audio)  # Bink movies (--movie); FFmpeg DLLs loaded at run time, skipped cleanly when missing
    target_link_libraries(alamo_game PRIVATE alamo_video)
    target_compile_definitions(alamo_game PRIVATE ALAMO_GAME_HAVE_VIDEO=1)
    if(COMMAND alamo_video_copy_ffmpeg_dlls)
      alamo_video_copy_ffmpeg_dlls(alamo_game)
    endif()
  endif()
  if(WIN32)
    target_link_libraries(alamo_game PRIVATE user32)
  endif()
  alamo_shaderxlat_copy_dxc(alamo_game)
  # bm2d t01: a scenario whose save does not load is "scenario changed, not comparable", exit 3 (needs a window and a GPU, data-free).
  add_test(NAME alamo_game_bench_stale_scene COMMAND alamo_game_benchmark_tests --stale-exe $<TARGET_FILE:alamo_game>)
  set_tests_properties(alamo_game_bench_stale_scene PROPERTIES TIMEOUT 120)
  # [port] video options helpers (game_display.cpp): mode list text, selection, options file, detail options mapping.
  add_executable(alamo_game_display_tests ${_dir}/tests/test_game_display.cpp ${_dir}/app/game_display.cpp)
  target_link_libraries(alamo_game_display_tests PRIVATE alamo_core alamo_render_sim alamo_ui_hud alamo_engine_flags alamo_test_support)
  if(WIN32)
    target_link_libraries(alamo_game_display_tests PRIVATE user32)
  endif()
  add_test(NAME alamo_game_display_tests COMMAND alamo_game_display_tests)
  # Smoke tests (need ALAMO_FOC_DATA; exit 0 "skipped" without it): scripted input through the real menus, PNGs to
  # <build>/smoke_out (Int-final: one folder per build, so the three compilers' runs never overwrite each other), per-tick
  # CRC of the run == a fresh presentation-off replay of its command stream, 0 validation messages.
  # Int-final: alamo_game_smoke_<kind>_golden then compares the run's dumped CRC log (comments ignored) and command
  # recording with engine/apps/game/testdata/game_smoke_<kind>.crc / .rec (MSVC-generated; clang must match).
  # Regenerate after an intended sim change: copy build-msvc/smoke_out/game_smoke_<kind>.crc / .rec over the goldens, or run
  # tools\regen_goldens.cmd (it converts the .crc to the checkpoint form). The .crc golden may be a full log (ALAMO_CRC 1, exact
  # line compare) or a checkpoint log (ALAMO_CRC 2, docs/status/golden-checkpoints.md: compared on its stored ticks by
  # alamo_headless --compare, hence -DHEADLESS below).
  set(_smoke_headless "")
  if(TARGET alamo_headless)
    set(_smoke_headless -DHEADLESS=$<TARGET_FILE:alamo_headless>)
  endif()
  set(_smoke_out ${CMAKE_BINARY_DIR}/smoke_out)
  file(MAKE_DIRECTORY ${_smoke_out})
  # cu8g t02 (2026-11-08): the land smoke runs seed 2. With the map-load environment pick (one synced draw on its 2-environment
  # map, docs/re/CU8G_T02_ENV_PICK_RE.md) seed 1's AI retreats before the forces meet; seeds 2-5 all reach combat.
  # cu8i t02 (2026-11-08): seed 3. With the FormationClass ctor draws (one per land formation and the Load_Map-tail pass)
  # seeds 1, 2 and 6 retreat before the forces meet; seeds 3-5 reach combat.
  # s78n t20 batch A (2026-11-10): seed 2. With the destination check, Path Around detours and the walked StoppedVsStopped
  # resolution (MOVEMENT 4.3.1 / 1.5.1 / 5.4.3) the enemy squads' moves differ: on seeds 1, 3, 5 and 6 the scripted attackers
  # chase them and the first death comes after the "land forces engage" step has timed out (the battle ends by retreat
  # first); seeds 2 and 4 reach combat in time (first deaths before tick 1400).
  foreach(_k skirmish campaign land)
    set(_seed_args "")
    if(_k STREQUAL "land")
      set(_seed_args --seed 2)
    endif()
    add_test(NAME alamo_game_smoke_${_k} COMMAND alamo_game --smoke ${_k} --hidden --audio null ${_seed_args} --png-dir ${_smoke_out})
    set_tests_properties(alamo_game_smoke_${_k} PROPERTIES TIMEOUT 1800 FIXTURES_SETUP alamo_game_smoke_${_k}_run)
    add_test(NAME alamo_game_smoke_${_k}_golden
             COMMAND ${CMAKE_COMMAND} -DACTUAL=${_smoke_out}/game_smoke_${_k}
                     -DGOLDEN=${PROJECT_SOURCE_DIR}/engine/apps/game/testdata/game_smoke_${_k} ${_smoke_headless}
                     -P ${PROJECT_SOURCE_DIR}/engine/apps/game/compare_smoke.cmake)
    set_tests_properties(alamo_game_smoke_${_k}_golden PROPERTIES TIMEOUT 120 FIXTURES_REQUIRED alamo_game_smoke_${_k}_run)
  endforeach()
  # QA t12: the campaign smoke with the per-screen shots (victory, unit tooltip, video dialog: game_campaign_screen_*.png in
  # smoke_out/screens) and pseudo-localized text. Nothing reads the PNGs: look at them for overflow. Its CRC log and command
  # recording must still equal the normal campaign golden (the text and the screens never reach the sim).
  set(_screens_out ${_smoke_out}/screens)
  file(MAKE_DIRECTORY ${_screens_out})
  add_test(NAME alamo_game_smoke_screens COMMAND alamo_game --smoke campaign --hidden --audio null --png-dir ${_screens_out})
  set_tests_properties(alamo_game_smoke_screens PROPERTIES TIMEOUT 1800 FIXTURES_SETUP alamo_game_smoke_screens_run
                       ENVIRONMENT "ALAMO_SMOKE_SCREENS=1;ALAMO_PSEUDO_LOC=1")
  add_test(NAME alamo_game_smoke_screens_golden
           COMMAND ${CMAKE_COMMAND} -DACTUAL=${_screens_out}/game_smoke_campaign
                   -DGOLDEN=${PROJECT_SOURCE_DIR}/engine/apps/game/testdata/game_smoke_campaign ${_smoke_headless}
                   -P ${PROJECT_SOURCE_DIR}/engine/apps/game/compare_smoke.cmake)
  set_tests_properties(alamo_game_smoke_screens_golden PROPERTIES TIMEOUT 120 FIXTURES_REQUIRED alamo_game_smoke_screens_run)
  # N1 follow-up t01: the save / load menus driven like a player (app_script_saveload.cpp): Save with a typed name, the
  # overwrite and delete questions (No / Yes), Load from the in-game menu and from the main menu (tick + state CRC == the
  # save's), a save during a battle loaded back into it. PNGs game_smoke_saveload_*.png; its own saves folder (the run
  # replaces only its own three save names there). No golden: the .rec holds the loaded save's digest, which covers the
  # save's wall-clock time (the .crc matched across MSVC / clang); the checks are the smoke's own plus presentation on/off.
  add_test(NAME alamo_game_smoke_saveload COMMAND alamo_game --smoke saveload --hidden --audio null --png-dir ${_smoke_out}
                                                 --saves-dir ${_smoke_out}/saveload_saves)
  set_tests_properties(alamo_game_smoke_saveload PROPERTIES TIMEOUT 1800 FIXTURES_SETUP alamo_game_smoke_saveload_run)
  # BM2a: the benchmark mode on the saveload smoke's saves (bench_check.cmake; skipped without them): fixed frames from the
  # battle save and from the galactic save, each twice (same CRC log, identical PNGs); real time from the galactic save
  # twice (frame times reported, same CRC log). BM2b: fixed frames with TAA (bm-high at 640x360) from the battle save, the
  # second run forcing every 3rd settled frame to be redrawn: still identical PNGs (the redraws leave the TAA history).
  set(_bench_out ${_smoke_out}/bench)
  file(MAKE_DIRECTORY ${_bench_out})
  foreach(_case fixed:smoke_battle:fixed:0 fixed_galactic:smoke_main:fixed:0 realtime:smoke_main:realtime:0
                fixed_taa:smoke_battle:fixed:1)
    string(REPLACE ":" ";" _parts ${_case})
    list(GET _parts 0 _name)
    list(GET _parts 1 _save)
    list(GET _parts 2 _m)
    list(GET _parts 3 _taa)
    add_test(NAME alamo_game_bench_${_name}
             COMMAND ${CMAKE_COMMAND} -DGAME=$<TARGET_FILE:alamo_game> -DSAVE=${_smoke_out}/saveload_saves/${_save}.alsv
                     -DOUT=${_bench_out}/${_name} -DMODE=${_m} -DTAA=${_taa} -P ${PROJECT_SOURCE_DIR}/engine/apps/game/bench_check.cmake)
    set_tests_properties(alamo_game_bench_${_name} PROPERTIES TIMEOUT 1200 FIXTURES_REQUIRED alamo_game_smoke_saveload_run)
  endforeach()
  alamo_label_slow(alamo_game_smoke_skirmish alamo_game_smoke_campaign alamo_game_smoke_land alamo_game_smoke_screens
                   alamo_game_smoke_skirmish_golden alamo_game_smoke_campaign_golden alamo_game_smoke_land_golden
                   alamo_game_smoke_screens_golden alamo_game_smoke_saveload alamo_game_bench_fixed alamo_game_bench_fixed_galactic
                   alamo_game_bench_realtime alamo_game_bench_fixed_taa)
  # bm2: a scenario made with --bench-make from the battle save and the test recipe (testdata/bench_orbit.abench: fixed
  # frames, a camera path anchored on the start view), then run twice with --bench-scenario: the fingerprint matches, GPU
  # timer scopes are reported, the two runs' PNGs are identical and the camera moved; a changed pinned file is refused
  # (bench_scenario_check.cmake; skipped without the save).
  add_test(NAME alamo_game_bench_scenario
           COMMAND ${CMAKE_COMMAND} -DGAME=$<TARGET_FILE:alamo_game> -DSAVE=${_smoke_out}/saveload_saves/smoke_battle.alsv
                   -DRECIPE=${_dir}/testdata/bench_orbit.abench -DOUT=${_bench_out}/scenario
                   -P ${PROJECT_SOURCE_DIR}/engine/apps/game/bench_scenario_check.cmake)
  set_tests_properties(alamo_game_bench_scenario PROPERTIES TIMEOUT 1800 FIXTURES_REQUIRED alamo_game_smoke_saveload_run)
  alamo_label_slow(alamo_game_bench_scenario)
  # bm2c t03: the same on a TAA recipe (testdata/bench_orbit_taa.abench: bm-high, run at 640x360; run b redraws every 3rd
  # settled frame): a fixed-frames scenario with TAA gives identical PNGs in both runs (bench_scenario_check.cmake -DTAA=1).
  add_test(NAME alamo_game_bench_scenario_taa
           COMMAND ${CMAKE_COMMAND} -DGAME=$<TARGET_FILE:alamo_game> -DSAVE=${_smoke_out}/saveload_saves/smoke_battle.alsv
                   -DRECIPE=${_dir}/testdata/bench_orbit_taa.abench -DOUT=${_bench_out}/scenario_taa -DTAA=1
                   -P ${PROJECT_SOURCE_DIR}/engine/apps/game/bench_scenario_check.cmake)
  set_tests_properties(alamo_game_bench_scenario_taa PROPERTIES TIMEOUT 1800 FIXTURES_REQUIRED alamo_game_smoke_saveload_run)
  alamo_label_slow(alamo_game_bench_scenario_taa)
  # bm3: the committed rendered scenes (tests/benchmark/*.abench with `host game`): each runs 120 ticks hidden and its fingerprint
  # matches (bench_scene_check.cmake; skipped without ALAMO_FOC_DATA). One entry per scene, each with its own output folder.
  foreach(_scene space_battle_game nebula_battle water_battle)
    if(EXISTS ${PROJECT_SOURCE_DIR}/tests/benchmark/${_scene}.abench)
      add_test(NAME alamo_game_bench_scene_${_scene}
               COMMAND ${CMAKE_COMMAND} -DGAME=$<TARGET_FILE:alamo_game> -DSCENE=${PROJECT_SOURCE_DIR}/tests/benchmark/${_scene}.abench
                       -DOUT=${CMAKE_BINARY_DIR}/smoke_out/bench/scene_${_scene} -P ${PROJECT_SOURCE_DIR}/engine/apps/game/bench_scene_check.cmake)
      set_tests_properties(alamo_game_bench_scene_${_scene} PROPERTIES TIMEOUT 600)
      alamo_label_slow(alamo_game_bench_scene_${_scene})
    endif()
  endforeach()
  # bm2 (no data, no window): BM1's headless scenario is refused with the reason (its save is an alamo_headless harness
  # session, not an alamo_game session) before anything loads.
  add_test(NAME alamo_game_bench_scenario_headless
           COMMAND alamo_game --hidden --bench-scenario ${PROJECT_SOURCE_DIR}/tests/benchmark/space_battle.abench)
  set_tests_properties(alamo_game_bench_scenario_headless PROPERTIES TIMEOUT 60
                       PASS_REGULAR_EXPRESSION "space_battle.abench is an alamo_headless scenario \\(host headless\\)")
endfunction()
cmake_language(DEFER DIRECTORY ${PROJECT_SOURCE_DIR} CALL alamo_game_add_exe)
