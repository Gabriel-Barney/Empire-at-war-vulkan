# S8 - combat & modifiers (docs/re/TACTICAL_COMBAT.md sections 2-4): targeting, weapons, projectiles,
# damage, ModifierSet + Mod_Multiplier with a per-tick cache. Built on the S6 tree (alamo_tactical);
# the table loaders read engine/data (GameConstants, XML) and engine/objects (enum converters).
if(NOT TARGET alamo_objects OR NOT TARGET alamo_data)
  message(STATUS "alamo_tactical_combat skipped: needs the objects and data modules")
  return()
endif()

add_library(alamo_tactical_combat STATIC
  src/modifiers.cpp
  src/formulas.cpp
  src/tables.cpp
  src/combat_system.cpp
  src/weather.cpp  # L1 systems
  src/combat_save.cpp  # N1 stage 2: CombatSystem / ModifierSet / WeatherSystem write + read
  src/rocket.cpp  # refixf t01: ROCKET / MPTL_ROCKET spline flight
  src/hero_clash.cpp  # s78n t06: HEROCLASH.XML + the land hero-clash manager
)
add_library(alamo::tactical_combat ALIAS alamo_tactical_combat)
target_include_directories(alamo_tactical_combat PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(alamo_tactical_combat PUBLIC alamo_tactical alamo_core alamo_engine_flags PRIVATE alamo_data alamo_objects)

# Tests: hand-computed formulas, modifier stacking, priority sets, a scripted 2-sided space battle
# (CRC log identical for 1/4/N threads; golden file shared by MSVC and clang), per-tick
# allocation count, modifier-aggregation micro-benchmark (printed). Data checks use ALAMO_FOC_DATA.
# Regenerate the golden: alamo_tactical_combat_tests --write-golden <file>.
add_executable(alamo_tactical_combat_tests tests/test_combat.cpp tests/test_combat_part1.cpp tests/test_combat_part2.cpp tests/test_combat_part3.cpp)
target_link_libraries(alamo_tactical_combat_tests PRIVATE alamo_tactical_combat alamo_data alamo_objects alamo_test_support)
# FoC data through the VFS (Data\XML lives in Config.meg in the shipped game).
if(TARGET alamo_vfs)
  target_link_libraries(alamo_tactical_combat_tests PRIVATE alamo_vfs)
  target_compile_definitions(alamo_tactical_combat_tests PRIVATE ALAMO_COMBAT_TESTS_HAVE_VFS=1)
endif()
# S8 t01: the LAND_OBSTACLE aim walk against a real PassabilityGrid stamp (movement/ is configured before combat/).
if(TARGET alamo_tactical_movement)
  target_link_libraries(alamo_tactical_combat_tests PRIVATE alamo_tactical_movement)
  target_compile_definitions(alamo_tactical_combat_tests PRIVATE ALAMO_COMBAT_TESTS_HAVE_MOVEMENT=1)
endif()
add_test(NAME alamo_tactical_combat_tests
         COMMAND alamo_tactical_combat_tests --golden ${CMAKE_CURRENT_SOURCE_DIR}/tests/golden/combat_space_crc.txt)
set_tests_properties(alamo_tactical_combat_tests PROPERTIES TIMEOUT 300)
