# Hangars: SPAWN_SQUADRON fighter launches for stations and carriers, and the end of the skirmish station upgrade
# (Starbase_Upgrade_Ability; the ability apply itself is in abilities/). Specs: docs/re/HARDPOINT_DISABLED.md §3 / §9.4,
# docs/re/STATION_HANGARS_SOUND.md. The battle host (engine/apps/sim) creates the requested squadrons.
if(NOT TARGET alamo_tactical_abilities)
  message(STATUS "alamo_tactical_hangar skipped: needs alamo_tactical_abilities")
  return()
endif()

add_library(alamo_tactical_hangar STATIC src/hangar_system.cpp src/contents.cpp
  src/hangar_save.cpp)  # N1 stage 2: HangarSystem / ContentsSystem / TacticalProduction / EngineLoops write + read
add_library(alamo::tactical_hangar ALIAS alamo_tactical_hangar)
target_include_directories(alamo_tactical_hangar PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(alamo_tactical_hangar PUBLIC alamo_tactical_abilities alamo_tactical_combat alamo_tactical alamo_core alamo_engine_flags)

# Tests: hand-computed launches (first launch, respawn delay and reserve, cap, bay / bone placement and flyout order,
# destroyed vs disabled bays, carrier death / retreat), a station upgrade driven through the abilities module, and FoC
# data (ALAMO_FOC_DATA: an Empire station upgraded mid-battle, a Star Destroyer launching and replacing squadrons).
add_executable(alamo_tactical_hangar_tests tests/test_hangar.cpp)
target_link_libraries(alamo_tactical_hangar_tests PRIVATE alamo_tactical_hangar alamo_tactical_abilities alamo_data Threads::Threads)
if(TARGET alamo_vfs AND TARGET alamo_objects)
  target_link_libraries(alamo_tactical_hangar_tests PRIVATE alamo_vfs alamo_objects)
  target_compile_definitions(alamo_tactical_hangar_tests PRIVATE ALAMO_HANGAR_TESTS_HAVE_DATA=1)
endif()
add_test(NAME alamo_tactical_hangar_tests COMMAND alamo_tactical_hangar_tests)
set_tests_properties(alamo_tactical_hangar_tests PROPERTIES TIMEOUT 300)
