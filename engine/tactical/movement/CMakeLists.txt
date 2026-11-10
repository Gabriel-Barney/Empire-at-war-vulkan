# S7 - movement & pathing (docs/re/MOVEMENT.md): land passability grid from .ted maps, region A* +
# smoothing + clearance re-plan, walk locomotor, land collision with the dynamic local pathfinder,
# space kinematic A* with time-sliced occupancy and Hermite playback, formations, MovementSystem
# (the per-tick entry point for the sim host).
#
# Library deps: core (dmath, SimRng, CRC), tactical (SpatialTree for collision candidates), objects
# (type fields -> unit parameters, ObjectStore commit), data (MovementClassTypeDefs.xml, GameConstants).
# The .ted adapter (ted_grid.hpp) is compiled when the terrain module is configured.
if(NOT TARGET alamo_objects)
  message(STATUS "alamo_tactical_movement: objects module not configured, skipped")
  return()
endif()

add_library(alamo_tactical_movement STATIC
  src/common.cpp
  src/passability.cpp
  src/land_path.cpp
  src/locomotor.cpp
  src/collision.cpp
  src/space_path.cpp
  src/formation.cpp
  src/unit_params.cpp
  src/movement_system.cpp
  src/hyperspace.cpp  # L1 systems
  src/corridor.cpp            # REMAINING_GAPS 4.1
  src/special_locomotors.cpp  # REMAINING_GAPS 4.3-4.7 (pure rules)
  src/special_system.cpp      # REMAINING_GAPS 4.3-4.7 (MovementSystem integration)
  src/deploy_system.cpp       # S8 t11: Walk locomotor deploy states (MOVEMENT 3.2)
  src/fighter.cpp             # FIGHTER_FLYING 1-5 (fighter primitives and cell grids)
  src/fighter_formation.cpp   # s78f t02: fighter formation block stacks (FIGHTER_FLYING 4.11)
  src/space_avoid.cpp         # SPACE_AVOIDANCE: avoidance tree members + precise model test (FighterAvoidFn rules)
  src/movement_save.cpp       # N1 stage 2: save / load of the battle movement state
  src/fleet_placement.cpp     # cu8l t03: campaign space fleet placement layout (CU8K_T03_T08_T09_RE 1)
  src/land_team.cpp           # s78n t03: land squad containers walk their own path, members steer to slots (MOVEMENT 3.7)
)
add_library(alamo::tactical_movement ALIAS alamo_tactical_movement)
target_include_directories(alamo_tactical_movement PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(alamo_tactical_movement PUBLIC alamo_tactical alamo_objects alamo_data alamo_core alamo_engine_flags)
# terrain is configured after tactical (engine/CMakeLists.txt order), so test the module list, not TARGET.
if("terrain" IN_LIST ALAMO_ENGINE_MODULES AND EXISTS ${PROJECT_SOURCE_DIR}/engine/terrain/CMakeLists.txt)
  target_sources(alamo_tactical_movement PRIVATE src/ted_grid.cpp)
  target_link_libraries(alamo_tactical_movement PUBLIC alamo_terrain)
  target_compile_definitions(alamo_tactical_movement PUBLIC ALAMO_MOVEMENT_HAVE_TERRAIN=1)
endif()

add_executable(alamo_tactical_movement_tests tests/test_movement.cpp)
target_link_libraries(alamo_tactical_movement_tests PRIVATE alamo_tactical_movement Threads::Threads alamo_test_support)
# Real-data tests (ALAMO_FOC_DATA): every shipped land / space map through the VFS.
if("vfs" IN_LIST ALAMO_ENGINE_MODULES AND "terrain" IN_LIST ALAMO_ENGINE_MODULES)
  target_link_libraries(alamo_tactical_movement_tests PRIVATE alamo_vfs)
  target_compile_definitions(alamo_tactical_movement_tests PRIVATE ALAMO_MOVEMENT_HAVE_DATA=1)
endif()
add_test(NAME alamo_tactical_movement_tests COMMAND alamo_tactical_movement_tests)
set_tests_properties(alamo_tactical_movement_tests PROPERTIES TIMEOUT 900)

# REMAINING_GAPS 4.1-4.7: corridor squeeze, threat mode, special locomotors.
add_executable(alamo_tactical_movement_gaps_tests tests/test_movement_gaps.cpp)
target_link_libraries(alamo_tactical_movement_gaps_tests PRIVATE alamo_tactical_movement Threads::Threads alamo_test_support)
add_test(NAME alamo_tactical_movement_gaps_tests COMMAND alamo_tactical_movement_gaps_tests)
set_tests_properties(alamo_tactical_movement_gaps_tests PROPERTIES TIMEOUT 300)

# L1 systems: hyperspace departures, engine control, teleport (hyperspace.hpp, MovementSystem::teleport).
add_executable(alamo_tactical_hyperspace_tests tests/test_hyperspace.cpp)
target_link_libraries(alamo_tactical_hyperspace_tests PRIVATE alamo_tactical_movement alamo_test_support)
add_test(NAME alamo_tactical_hyperspace_tests COMMAND alamo_tactical_hyperspace_tests)
set_tests_properties(alamo_tactical_hyperspace_tests PROPERTIES TIMEOUT 120)

# cu8l t03: the campaign space fleet placement layout (fleet_placement.hpp): lists, grid / ring / row offsets, members.
add_executable(alamo_tactical_fleet_placement_tests tests/test_fleet_placement.cpp)
target_link_libraries(alamo_tactical_fleet_placement_tests PRIVATE alamo_tactical_movement alamo_test_support)
add_test(NAME alamo_tactical_fleet_placement_tests COMMAND alamo_tactical_fleet_placement_tests)
set_tests_properties(alamo_tactical_fleet_placement_tests PROPERTIES TIMEOUT 60)

# N1 stage 2: save / load mid-battle (MovementSystem, grid stamps, occupancy, collision layer, fighter grids,
# hyperspace departures, engine control): resumed runs must match the uninterrupted run tick for tick.
add_executable(alamo_tactical_movement_save_tests tests/test_movement_save.cpp)
target_link_libraries(alamo_tactical_movement_save_tests PRIVATE alamo_tactical_movement Threads::Threads alamo_test_support)
add_test(NAME alamo_tactical_movement_save_tests COMMAND alamo_tactical_movement_save_tests)
set_tests_properties(alamo_tactical_movement_save_tests PROPERTIES TIMEOUT 600)
