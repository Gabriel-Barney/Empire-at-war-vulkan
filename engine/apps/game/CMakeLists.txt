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
  src/benchmark.cpp
)
target_include_directories(alamo_game_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(alamo_game_core PUBLIC alamo_sim alamo_vfs alamo_data alamo_core alamo_engine_flags)
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
if(TARGET alamo_audio)
  target_link_libraries(alamo_game_core PUBLIC alamo_audio)
  target_compile_definitions(alamo_game_core PUBLIC ALAMO_GAME_HAVE_AUDIO=1)
endif()

# Data-free: synthetic skirmish through GameSession (human orders routed into the battle, battle end, record/replay CRC).
add_executable(alamo_game_session_tests tests/test_game_session.cpp)
target_link_libraries(alamo_game_session_tests PRIVATE alamo_core alamo_game_core)
add_test(NAME alamo_game_session_tests COMMAND alamo_game_session_tests)
set_tests_properties(alamo_game_session_tests PROPERTIES TIMEOUT 600)

# N1 / t09: save / load on the FoC data (ALAMO_FOC_DATA, skipped without): a campaign saved through the menus' save path
# at tick T, loaded by a fresh process (this exe --child) through the menus' load path: per-tick CRC == the uninterrupted
# run from T on; load + replay in process; another seed refused; animation-on saves load or are refused at the check.
add_executable(alamo_game_save_tests tests/test_game_save.cpp)
target_link_libraries(alamo_game_save_tests PRIVATE alamo_core alamo_game_core)
add_test(NAME alamo_game_save_tests COMMAND alamo_game_save_tests)
set_tests_properties(alamo_game_save_tests PROPERTIES TIMEOUT 1200)
alamo_label_slow(alamo_game_save_tests)

# p4ui t06 (UI.md section 9): the skirmish map catalog (official / custom / mod-folder maps) on a fake install in its own temp
# folder; with ALAMO_FOC_DATA also the shipped data (24 land + 24 space official rows, no custom).
add_executable(alamo_game_skirmish_maps_tests tests/test_skirmish_maps.cpp)
target_link_libraries(alamo_game_skirmish_maps_tests PRIVATE alamo_core alamo_game_core)
add_test(NAME alamo_game_skirmish_maps_tests COMMAND alamo_game_skirmish_maps_tests)
set_tests_properties(alamo_game_skirmish_maps_tests PROPERTIES TIMEOUT 120)

# Playtest 2 t13: auto-resolve results screen figures (battle time, bounty, efficiency): pure rules data-free, bounty data
# on the FoC data (ALAMO_FOC_DATA, skipped without).
add_executable(alamo_game_battle_summary_tests tests/test_battle_summary.cpp)
target_link_libraries(alamo_game_battle_summary_tests PRIVATE alamo_core alamo_game_core)
add_test(NAME alamo_game_battle_summary_tests COMMAND alamo_game_battle_summary_tests)
set_tests_properties(alamo_game_battle_summary_tests PROPERTIES TIMEOUT 600)

# BM2a data-free: the benchmark mode's options / flags, presets, fixed-frames clock, frame-time statistics and JSON report
# (benchmark.hpp; the exe side is app/app_benchmark.cpp, checked by alamo_game_bench_* below).
add_executable(alamo_game_benchmark_tests tests/test_benchmark.cpp)
target_link_libraries(alamo_game_benchmark_tests PRIVATE alamo_core alamo_game_core)
add_test(NAME alamo_game_benchmark_tests COMMAND alamo_game_benchmark_tests)
set_tests_properties(alamo_game_benchmark_tests PROPERTIES TIMEOUT 60)

# Data-free: GameAudioHost movie music (modes 14 / 17, Halt_Game_Music) on a synthetic audio tree and the null backend.
add_executable(alamo_game_audio_host_tests tests/test_game_audio_host.cpp)
target_link_libraries(alamo_game_audio_host_tests PRIVATE alamo_core alamo_game_core)
add_test(NAME alamo_game_audio_host_tests COMMAND alamo_game_audio_host_tests)
set_tests_properties(alamo_game_audio_host_tests PROPERTIES TIMEOUT 120)

# Data-free (refixf t08): the tactical world pick (HUD_GAPS §2.2; world_pick.hpp + engine/ui pick_world) on a SpaceMode
# with hand-built model collision data: mesh hit, box-but-not-mesh miss, Mouse_Collide_Override_Sphere_Radius, nearest hit.
if(TARGET alamo_ui)
  add_executable(alamo_game_world_pick_tests tests/test_world_pick.cpp)
  target_link_libraries(alamo_game_world_pick_tests PRIVATE alamo_core alamo_game_core)
  add_test(NAME alamo_game_world_pick_tests COMMAND alamo_game_world_pick_tests)
  set_tests_properties(alamo_game_world_pick_tests PROPERTIES TIMEOUT 120)
endif()

# Data-free (refix t06, refixf t05): the tactical super weapon's presentation cues (battle_fx.hpp), the cinematic camera
# (TSW firing shot, explosion shake, letterbox; TSW_PLANET_DESTROY.md §11) and GameAudioHost::tsw_audio on a synthetic type set / audio tree with the null backend.
add_executable(alamo_game_battle_fx_tests tests/test_battle_fx.cpp)
target_link_libraries(alamo_game_battle_fx_tests PRIVATE alamo_core alamo_game_core)
add_test(NAME alamo_game_battle_fx_tests COMMAND alamo_game_battle_fx_tests)
set_tests_properties(alamo_game_battle_fx_tests PROPERTIES TIMEOUT 120)

function(alamo_game_add_exe)
  foreach(_dep alamo_render_sim alamo_render_galactic alamo_ui_draw alamo_ui_hud alamo_ui_rhi)
    if(NOT TARGET ${_dep})
      message(STATUS "alamo_game: ${_dep} not configured; alamo_game exe skipped")
      return()
    endif()
  endforeach()
  set(_dir ${CMAKE_CURRENT_FUNCTION_LIST_DIR})
  if(NOT EXISTS ${_dir}/app/game_main.cpp)
    return()
  endif()
  add_executable(alamo_game ${_dir}/app/game_main.cpp ${_dir}/app/app_session.cpp ${_dir}/app/app_input.cpp ${_dir}/app/app_screens.cpp
                           ${_dir}/app/app_hud.cpp ${_dir}/app/app_frame.cpp ${_dir}/app/app_cinematic.cpp ${_dir}/app/app_script.cpp ${_dir}/app/app_script_saveload.cpp ${_dir}/app/app_benchmark.cpp ${_dir}/app/game_render.cpp ${_dir}/app/game_ui.cpp ${_dir}/app/game_movies.cpp ${_dir}/app/frame_overlay.cpp
                           ${_dir}/app/game_info.cpp ${_dir}/app/game_display.cpp)
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
  # [port] video options helpers (game_display.cpp): mode list text, selection, options file, detail options mapping.
  add_executable(alamo_game_display_tests ${_dir}/tests/test_game_display.cpp ${_dir}/app/game_display.cpp)
  target_link_libraries(alamo_game_display_tests PRIVATE alamo_core alamo_render_sim alamo_ui_hud alamo_engine_flags)
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
  foreach(_k skirmish campaign land)
    add_test(NAME alamo_game_smoke_${_k} COMMAND alamo_game --smoke ${_k} --hidden --audio null --png-dir ${_smoke_out})
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
  # twice (frame times reported, same CRC log).
  set(_bench_out ${_smoke_out}/bench)
  file(MAKE_DIRECTORY ${_bench_out})
  foreach(_case fixed:smoke_battle:fixed fixed_galactic:smoke_main:fixed realtime:smoke_main:realtime)
    string(REPLACE ":" ";" _parts ${_case})
    list(GET _parts 0 _name)
    list(GET _parts 1 _save)
    list(GET _parts 2 _m)
    add_test(NAME alamo_game_bench_${_name}
             COMMAND ${CMAKE_COMMAND} -DGAME=$<TARGET_FILE:alamo_game> -DSAVE=${_smoke_out}/saveload_saves/${_save}.alsv
                     -DOUT=${_bench_out}/${_name} -DMODE=${_m} -P ${PROJECT_SOURCE_DIR}/engine/apps/game/bench_check.cmake)
    set_tests_properties(alamo_game_bench_${_name} PROPERTIES TIMEOUT 1200 FIXTURES_REQUIRED alamo_game_smoke_saveload_run)
  endforeach()
  alamo_label_slow(alamo_game_smoke_skirmish alamo_game_smoke_campaign alamo_game_smoke_land alamo_game_smoke_screens
                   alamo_game_smoke_skirmish_golden alamo_game_smoke_campaign_golden alamo_game_smoke_land_golden
                   alamo_game_smoke_screens_golden alamo_game_smoke_saveload alamo_game_bench_fixed alamo_game_bench_fixed_galactic
                   alamo_game_bench_realtime)
endfunction()
cmake_language(DEFER DIRECTORY ${PROJECT_SOURCE_DIR} CALL alamo_game_add_exe)
