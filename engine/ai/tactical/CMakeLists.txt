# A1 stage 1 - tactical AI pieces that do not depend on the shared AI framework (A2) or the tactical
# perception evaluators (docs/re/AI_TACTICAL.md sections 1.3, 3, 5, 6.1-6.3, 10):
#   target lists (unit targets, land hint-zone regions, space CELL_x_y grid), the Applies filter,
#   land zone reachability (zone graph all-pairs + threat tolerances), FindTarget / Reachable_Target
#   selection, DAMAGE_TRACKING store + FindDeadlyEnemy, free-store admission + service scheduling,
#   the land campaign credit transfer.
# Library deps: core (SimRng, CRC), data (GameConstants), galactic (all-pairs reference FW) and
# galactic_sim (econ::add_credits). Optional: terrain (.ted hint-zone adapter), tactical_movement
# (passability-component prefilter), tactical_combat (damage-event adapter).
if(NOT TARGET alamo_galactic_sim)
  message(STATUS "alamo_ai_tactical: galactic_sim (data/objects/commands) not configured, skipped")
  return()
endif()

add_library(alamo_ai_tactical STATIC
  src/constants.cpp
  src/target_list.cpp
  src/applies.cpp
  src/zone_reachability.cpp
  src/find_target.cpp
  src/damage_tracking.cpp
  src/free_store.cpp
  src/tactical_credits.cpp
  # stage 1b: perception (evaluators, token leaves, leaf cache, threat grid, Lua-facing functions)
  src/perception_world.cpp
  src/perception_cache.cpp
  src/threat_grid.cpp
  src/tactical_perception.cpp
  src/perception_lua.cpp
  # A2 tactical: the tactical PotentialPlan (land / space marshaller lists on ai_plans' run_pp_solver)
  src/tactical_pp.cpp
  # N1 stage 2: save / load of the components above (TacticalAiWorld's chunk drives them)
  src/tactical_save.cpp
)
add_library(alamo::ai_tactical ALIAS alamo_ai_tactical)
target_include_directories(alamo_ai_tactical PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# alamo_ai: the S2 perception-equation language (parser, linker, evaluator, token ids), used unchanged.
target_link_libraries(alamo_ai_tactical PUBLIC alamo_ai alamo_galactic_sim alamo_galactic alamo_data alamo_core alamo_engine_flags)
# terrain is configured after ai (engine/CMakeLists.txt order), so test the module list, not TARGET.
if("terrain" IN_LIST ALAMO_ENGINE_MODULES AND EXISTS ${PROJECT_SOURCE_DIR}/engine/terrain/CMakeLists.txt)
  target_sources(alamo_ai_tactical PRIVATE src/hint_zones_ted.cpp)
  target_link_libraries(alamo_ai_tactical PUBLIC alamo_terrain)
  target_compile_definitions(alamo_ai_tactical PUBLIC ALAMO_AI_TACTICAL_HAVE_TERRAIN=1)
endif()
if(TARGET alamo_tactical_movement)
  target_sources(alamo_ai_tactical PRIVATE src/land_connectivity.cpp)
  target_link_libraries(alamo_ai_tactical PUBLIC alamo_tactical_movement)
  target_compile_definitions(alamo_ai_tactical PUBLIC ALAMO_AI_TACTICAL_HAVE_MOVEMENT=1)
endif()
if(TARGET alamo_tactical_combat)
  target_link_libraries(alamo_ai_tactical PUBLIC alamo_tactical_combat)
  target_compile_definitions(alamo_ai_tactical PUBLIC ALAMO_AI_TACTICAL_HAVE_COMBAT=1)
endif()

# Stage 2: the tactical AI mode on the A2 framework (contexts, goal hooks, free stores, plans / task forces on
# the Lua bridge): alamo_ai_tactical_world (needs A2's alamo_ai_lua and engine/lua).
if(TARGET alamo_ai_lua)
  add_library(alamo_ai_tactical_world STATIC src/tactical_ai.cpp)
  add_library(alamo::ai_tactical_world ALIAS alamo_ai_tactical_world)
  target_link_libraries(alamo_ai_tactical_world PUBLIC alamo_ai_tactical alamo_ai_lua alamo_ai alamo_lua alamo_objects
                                                       alamo_commands alamo_data alamo_engine_flags PRIVATE alamo_lua_vm)
  add_executable(alamo_ai_tactical_world_tests tests/test_ai_tactical_world.cpp)
  target_link_libraries(alamo_ai_tactical_world_tests PRIVATE alamo_ai_tactical_world Threads::Threads)
  target_compile_definitions(alamo_ai_tactical_world_tests PRIVATE
                             "ALAMO_AI_TACTICAL_GOLDEN_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/golden\"")
  add_test(NAME alamo_ai_tactical_world_tests COMMAND alamo_ai_tactical_world_tests)
  set_tests_properties(alamo_ai_tactical_world_tests PROPERTIES TIMEOUT 600)
endif()

# A1 t01 settled readings (docs/re/A1_READINGS.md 6, 9, 13-15): nFow rounding, UnitAI data-pack stamps and the first
# re-projection check, destroyed objects' memory entries, free-store land / space admission and service timing.
add_executable(alamo_ai_tactical_readings_tests tests/test_ai_tactical_readings.cpp)
target_link_libraries(alamo_ai_tactical_readings_tests PRIVATE alamo_ai_tactical)
add_test(NAME alamo_ai_tactical_readings_tests COMMAND alamo_ai_tactical_readings_tests)

add_executable(alamo_ai_tactical_tests tests/test_ai_tactical.cpp)
target_link_libraries(alamo_ai_tactical_tests PRIVATE alamo_ai_tactical Threads::Threads)
# Real data (ALAMO_FOC_DATA): every shipped map's hint zones / space grid through the VFS.
if("vfs" IN_LIST ALAMO_ENGINE_MODULES AND "terrain" IN_LIST ALAMO_ENGINE_MODULES)
  target_link_libraries(alamo_ai_tactical_tests PRIVATE alamo_vfs)
  target_compile_definitions(alamo_ai_tactical_tests PRIVATE ALAMO_AI_TACTICAL_HAVE_DATA=1)
endif()
# Scenario CRC golden (written by MSVC with --write-golden <file>; clang must match).
add_test(NAME alamo_ai_tactical_tests
         COMMAND alamo_ai_tactical_tests --golden ${CMAKE_CURRENT_SOURCE_DIR}/tests/golden/ai_tactical_scenario_crc.txt)
set_tests_properties(alamo_ai_tactical_tests PROPERTIES TIMEOUT 600)

# Stage 1b perception tests: leaf cache rules, every token, threat-grid projection / re-projection /
# fog memory, Lua-facing functions, scenario CRC golden (1 / 4 / N threads; MSVC writes it, clang
# must match) and, with ALAMO_FOC_DATA, the shipped tactical perceptual equations.
add_executable(alamo_ai_tactical_perception_tests tests/test_ai_tactical_perception.cpp)
target_link_libraries(alamo_ai_tactical_perception_tests PRIVATE alamo_ai_tactical Threads::Threads)
if(TARGET alamo_vfs)
  target_link_libraries(alamo_ai_tactical_perception_tests PRIVATE alamo_vfs)
  target_compile_definitions(alamo_ai_tactical_perception_tests PRIVATE ALAMO_AI_TACTICAL_HAVE_VFS=1)
endif()
add_test(NAME alamo_ai_tactical_perception_tests
         COMMAND alamo_ai_tactical_perception_tests --golden ${CMAKE_CURRENT_SOURCE_DIR}/tests/golden/ai_tactical_perception_crc.txt)
set_tests_properties(alamo_ai_tactical_perception_tests PROPERTIES TIMEOUT 600)