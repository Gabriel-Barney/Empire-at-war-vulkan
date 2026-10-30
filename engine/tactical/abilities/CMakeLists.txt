# S9 - hardpoints & abilities (docs/re/ABILITIES_HARDPOINTS.md): unit-ability info table, records and
# XML parsers, countdowns, command 43/40/57/58 paths, special abilities (tractor, ion, lucky shot,
# concentrate fire, energy weapon, stealth, jamming, laser defense, blaster listeners). Built on
# the S8 CombatSystem (hooks); the XML parsers read engine/data.
if(NOT TARGET alamo_tactical_combat OR NOT TARGET alamo_data)
  message(STATUS "alamo_tactical_abilities skipped: needs alamo_tactical_combat and alamo_data")
  return()
endif()

add_library(alamo_tactical_abilities STATIC
  src/ability_types.cpp
  src/ability_data.cpp
  src/countdown.cpp
  src/ability_system.cpp
  src/ability_behaviors.cpp  # OI abilities: SAP init, autofire, auras, handler bodies, HUNT / CONFUSE, mines
  src/ability_classes.cpp    # OI abilities: special-ability class bodies (TACTICAL_GAPS §2.3)
  src/special_weapons.cpp  # L1 systems
  src/space_fields.cpp     # L1 systems
  src/ability_save.cpp     # N1 stage 2: AbilitySystem / SpecialWeaponSystem / SpaceFieldIndex write + read
)
add_library(alamo::tactical_abilities ALIAS alamo_tactical_abilities)
target_include_directories(alamo_tactical_abilities PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(alamo_tactical_abilities PUBLIC alamo_tactical_combat alamo_tactical alamo_core alamo_engine_flags
                      PRIVATE alamo_data)

# Tests: hand-computed rules (info table, countdown arithmetic, gates, Is_Active squad aggregation,
# Mod_Multiplier, hardpoint draws / Get_Hull / Die order / smoke events, tractor, ion stun, missile
# shield, energy pool, ignition, death projectile, converted decay, lucky shot, stealth, jamming,
# laser defense, opportunity fire), a scripted space battle with abilities and hardpoint kills whose
# CRC log is identical for 1/4/N threads and matches the golden on every compiler, a per-tick
# allocation count, FoC data checks (ALAMO_FOC_DATA). Regenerate: --write-golden <file>.
add_executable(alamo_tactical_abilities_tests tests/test_abilities.cpp)
target_link_libraries(alamo_tactical_abilities_tests PRIVATE alamo_tactical_abilities alamo_data Threads::Threads)
target_include_directories(alamo_tactical_abilities_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../tests)
# FoC data through the VFS (Data\XML lives in Config.meg in the shipped game).
if(TARGET alamo_vfs)
  target_link_libraries(alamo_tactical_abilities_tests PRIVATE alamo_vfs)
  target_compile_definitions(alamo_tactical_abilities_tests PRIVATE ALAMO_ABILITIES_TESTS_HAVE_VFS=1)
endif()
add_test(NAME alamo_tactical_abilities_tests
         COMMAND alamo_tactical_abilities_tests --golden ${CMAKE_CURRENT_SOURCE_DIR}/tests/golden/abilities_space_crc.txt)
set_tests_properties(alamo_tactical_abilities_tests PROPERTIES TIMEOUT 300)

# L1 systems: special weapons / lobbing superweapons / tactical superweapon (special_weapons.hpp) and the combat
# pieces they use (launch_projectile, the WEAPON_SPECIAL force-1 fog gate). Hand-computed frames.
add_executable(alamo_tactical_special_weapons_tests tests/test_special_weapons.cpp)
target_link_libraries(alamo_tactical_special_weapons_tests PRIVATE alamo_tactical_abilities Threads::Threads)
target_include_directories(alamo_tactical_special_weapons_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../tests)
add_test(NAME alamo_tactical_special_weapons_tests COMMAND alamo_tactical_special_weapons_tests)
set_tests_properties(alamo_tactical_special_weapons_tests PROPERTIES TIMEOUT 120)