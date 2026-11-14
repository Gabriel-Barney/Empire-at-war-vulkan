# S2 - AI perception: perception-equation language (parser, linker, evaluator, coverage over the
# shipped Data\XML\AI\PerceptualEquations files) and galactic perception (the four galactic token
# paths, Node_Weight inputs for the all-pairs, PlayerView fill, CRC). docs/re/GALACTIC_ECONOMY.md section 6.
add_library(alamo_ai STATIC
  src/perception_equations.cpp
  src/galactic_perception.cpp
  src/galactic_seed.cpp
  src/ai_path_finder.cpp  # per-AI all-pairs path finder (GALACTIC.md §4, Lua Find_Path)
  src/ai_free_store_path.cpp  # free-store Get_Next_Hop, AI request flags, reachability flood (FIXPASS_GALACTIC §1)
  # A2 AI framework (shared with the tactical AI, A1): ai_*.hpp
  src/ai_definitions.cpp
  src/ai_learning.cpp
  src/ai_budget.cpp
  src/ai_plans.cpp
  src/ai_templates.cpp
  src/ai_goals.cpp
  src/ai_context.cpp
  src/ai_save.cpp  # N1 save / load of the framework, path finders and perception
)
add_library(alamo::ai ALIAS alamo_ai)
target_include_directories(alamo_ai PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(alamo_ai PUBLIC alamo_galactic alamo_data alamo_core alamo_engine_flags
                               PRIVATE alamo_objects)
# The AI save codec (src/ai_save_io.hpp, not installed) for the tactical AI's save code (engine/ai/tactical): an internal
# include directory, so its tactical_save_io.hpp reuses it instead of a copy.
add_library(alamo_ai_save_io INTERFACE)
target_include_directories(alamo_ai_save_io INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(alamo_ai_save_io INTERFACE alamo_galactic alamo_core)
# q6: the galactic seed's force roles use galactic::galactic_unit_kind (galactic_rules.cpp, in alamo_galactic_sim, which
# exists when data, objects and commands are configured; without it the seed keeps its own tag rule).
if(TARGET alamo_galactic_sim)
  target_link_libraries(alamo_ai PRIVATE alamo_galactic_sim)
  target_compile_definitions(alamo_ai PRIVATE ALAMO_AI_HAVE_GALACTIC_SIM=1)
endif()

add_executable(alamo_ai_tests tests/test_ai.cpp)
target_link_libraries(alamo_ai_tests PRIVATE alamo_ai alamo_objects Threads::Threads alamo_test_support)
# Coverage reports for the shipped equations go to <project>/logs/ai_perception_coverage_<game>.txt.
target_compile_definitions(alamo_ai_tests PRIVATE "ALAMO_AI_LOG_DIR=\"${PROJECT_SOURCE_DIR}/logs\""
                                                  "ALAMO_AI_GOLDEN_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/golden\"")
# Real data (ALAMO_FOC_DATA / ALAMO_EAW_DATA) is read through the VFS (loose folders or MEG installs).
if(TARGET alamo_vfs)
  target_link_libraries(alamo_ai_tests PRIVATE alamo_vfs)
  target_compile_definitions(alamo_ai_tests PRIVATE ALAMO_AI_HAVE_VFS=1)
endif()
if(TARGET alamo_jobs)
  target_link_libraries(alamo_ai_tests PRIVATE alamo_jobs)
  target_compile_definitions(alamo_ai_tests PRIVATE ALAMO_AI_HAVE_JOBS=1)
endif()
# cu5 t01: the seed options come from galactic::load_factions (alamo_galactic_sim).
if(TARGET alamo_galactic_sim)
  target_link_libraries(alamo_ai_tests PRIVATE alamo_galactic_sim)
  target_compile_definitions(alamo_ai_tests PRIVATE ALAMO_AI_HAVE_GALACTIC_SIM=1)
endif()
add_test(NAME alamo_ai_tests COMMAND alamo_ai_tests)
set_tests_properties(alamo_ai_tests PROPERTIES TIMEOUT 120)
# q6: galactic seed roles over Thrawn's Revenge (skips without the mod next to the Steam FoC install / ALAMO_TR_DATA).
add_test(NAME alamo_ai_tests_tr_seed_roles COMMAND alamo_ai_tests --tr-seed-roles)
set_tests_properties(alamo_ai_tests_tr_seed_roles PROPERTIES TIMEOUT 300)
alamo_label_slow(alamo_ai_tests_tr_seed_roles)

# --- A2 (owner: A2 agent) -------------------------------------------------------------------------------
# alamo_ai_lua: plan-script loading + script pool on the Lua bridge (shared with A1).
# alamo_ai_galactic: the galactic AI (perception tokens on the S3 GalacticSim, contexts, plans, task forces,
# free stores). engine/lua is configured before engine/ai, so TARGET tests work here.
if(TARGET alamo_lua AND TARGET alamo_lua_vm AND TARGET alamo_galactic_sim)
  add_library(alamo_ai_lua STATIC src/ai_lua_runtime.cpp)
  target_link_libraries(alamo_ai_lua PUBLIC alamo_ai alamo_lua alamo_engine_flags PRIVATE alamo_lua_vm)
  add_library(alamo_ai_galactic STATIC src/galactic_ai_tokens.cpp src/galactic_ai.cpp src/galactic_pp.cpp src/galactic_build.cpp
                                       src/galactic_ai_save.cpp)
  target_link_libraries(alamo_ai_galactic PUBLIC alamo_ai_lua alamo_ai alamo_galactic_sim alamo_lua alamo_objects
                                                 alamo_commands alamo_data alamo_engine_flags PRIVATE alamo_lua_vm)
  add_executable(alamo_ai_galactic_tests tests/test_ai_galactic.cpp tests/test_ai_galactic_part1.cpp tests/test_ai_galactic_part2.cpp tests/test_ai_galactic_part3.cpp)
  target_link_libraries(alamo_ai_galactic_tests PRIVATE alamo_ai_galactic Threads::Threads alamo_test_support)
  target_compile_definitions(alamo_ai_galactic_tests PRIVATE
                             "ALAMO_AI_GOLDEN_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/golden\""
                             "ALAMO_AI_LOG_DIR=\"${PROJECT_SOURCE_DIR}/logs\"")
  if(TARGET alamo_vfs)
    target_link_libraries(alamo_ai_galactic_tests PRIVATE alamo_vfs)
    target_compile_definitions(alamo_ai_galactic_tests PRIVATE ALAMO_AI_HAVE_VFS=1)
  endif()
  add_test(NAME alamo_ai_galactic_tests COMMAND alamo_ai_galactic_tests)
  set_tests_properties(alamo_ai_galactic_tests PROPERTIES TIMEOUT 900)
  # N1 save size guard: the whole FoC story campaign save, about twice the 2026-10-30 sizes (50 KB file, 334 KB of chunks).
  # The same guard holds late in the game (N1f t08, below); mid-battle story saves: alamo_game_save_tests "Story battle".
  # alamo_headless is configured after this folder (engine/apps), so the check is on the module list.
  set(_ai_story_guard_needs apps vfs objects commands tactical terrain)
  set(_ai_story_guard ON)
  foreach(_m IN LISTS _ai_story_guard_needs)
    if(NOT _m IN_LIST ALAMO_ENGINE_MODULES)
      set(_ai_story_guard OFF)
    endif()
  endforeach()
  if(_ai_story_guard)
    add_test(NAME alamo_ai_story_save_size
             COMMAND ${CMAKE_COMMAND} -DHEADLESS=$<TARGET_FILE:alamo_headless> -DOUT=${CMAKE_CURRENT_BINARY_DIR}/n1_story_size.alsv
                     -DMAX_FILE=120000 -DMAX_RAW=800000 -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/story_save_size.cmake)
    set_tests_properties(alamo_ai_story_save_size PROPERTIES TIMEOUT 300 SKIP_REGULAR_EXPRESSION "SKIP: ")
    # N1f t08: late game, 2 h of game time (2026-10-31: 53775 bytes in the file, 356692 bytes of chunks; 4 h: 57848 / 384324).
    add_test(NAME alamo_ai_story_save_size_late
             COMMAND ${CMAKE_COMMAND} -DHEADLESS=$<TARGET_FILE:alamo_headless> -DOUT=${CMAKE_CURRENT_BINARY_DIR}/n1_story_size_late.alsv
                     -DSAVE_AT=216000 -DMAX_FILE=120000 -DMAX_RAW=800000 -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/story_save_size.cmake)
    set_tests_properties(alamo_ai_story_save_size_late PROPERTIES TIMEOUT 600 SKIP_REGULAR_EXPRESSION "SKIP: ")
    alamo_label_slow(alamo_ai_story_save_size_late)
  endif()
endif()
# --- end A2 ----------------------------------------------------------------------------------------------

# --- A1 tactical AI (engine/ai/tactical, lib alamo_ai_tactical; owned by the A1 agent) ---
add_subdirectory(tactical)
