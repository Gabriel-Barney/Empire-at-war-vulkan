# R3 presence-induced idle and explosion flinch (docs/re/ANIM_PARTICLES.md §A.11 / §A.12): the IMPOSING_PRESENCE
# service, the IDLE service's presence-map countdown, the default idle type (0x395920) and the EXPLOSION flinch
# victims. The library depends on core only; the battle host (engine/apps/sim) supplies the world through
# PresenceWorld. alamo_tactical_presence_data builds the definitions from objects / data when they are configured.
add_library(alamo_tactical_presence STATIC
  src/presence_system.cpp
  src/presence_save.cpp
  src/explosion_flinch.cpp
)
add_library(alamo::tactical_presence ALIAS alamo_tactical_presence)
target_include_directories(alamo_tactical_presence PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(alamo_tactical_presence PUBLIC alamo_core alamo_engine_flags)

if(TARGET alamo_objects AND TARGET alamo_data)
  add_library(alamo_tactical_presence_data STATIC src/presence_data.cpp)
  add_library(alamo::tactical_presence_data ALIAS alamo_tactical_presence_data)
  target_link_libraries(alamo_tactical_presence_data PUBLIC alamo_tactical_presence alamo_objects alamo_data alamo_core alamo_engine_flags)
endif()

# Tests: the 30-frame service and its gates, aging and the move / target clears, the Deploys freeze, the winner rule
# (strictly more frames, allied override, list order), SPREAD_OUT priority, the hash-map list order and growth, save
# round trip / rejects, explosion intensity and clamp; with FoC data (ALAMO_FOC_DATA, objects + vfs + anim
# configured) the vanilla Presence_Induced_Animations lists.
add_executable(alamo_tactical_presence_tests tests/test_presence.cpp)
target_link_libraries(alamo_tactical_presence_tests PRIVATE alamo_tactical_presence)
if(TARGET alamo_tactical_presence_data AND TARGET alamo_vfs AND TARGET alamo_anim)
  target_link_libraries(alamo_tactical_presence_tests PRIVATE alamo_tactical_presence_data alamo_vfs alamo_anim)
  target_compile_definitions(alamo_tactical_presence_tests PRIVATE ALAMO_PRESENCE_TESTS_HAVE_DATA=1)
endif()
add_test(NAME alamo_tactical_presence_tests COMMAND alamo_tactical_presence_tests)
set_tests_properties(alamo_tactical_presence_tests PROPERTIES TIMEOUT 300)
