# Fog of war (docs/re/FOG_OF_WAR.md): per-player FOW grids (state / reveal count / fade), filled midpoint-circle
# reveals with the original's top-edge quirk, dense mask, 16-frame staggered decay + refog hook, REVEAL sources
# (ally sharing, one-cell re-reveal), queries with the AI gate, Lua-facing ops, renderer view.
# Depends on core (and the alamo_tactical header category_reduce.hpp) only (the host fills FogObject / FogHideQuery from the store and the type data).
add_library(alamo_tactical_fog STATIC
  src/fog_grid.cpp
  src/fog_of_war.cpp
  src/fog_save.cpp  # N1 stage 2: save / load in full
)
add_library(alamo::tactical_fog ALIAS alamo_tactical_fog)
target_include_directories(alamo_tactical_fog PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(alamo_tactical_fog PUBLIC alamo_core alamo_engine_flags PRIVATE alamo_tactical)  # category_reduce.hpp

# Tests: geometry, circle shapes incl. the top-edge quirk, counts / undo / flash / saturation, decay cadence and
# the 6.4-6.9 s visibility window, refog order, dense mask, ally sharing, re-reveal threshold, owner change,
# queries (AI gate, force, TEAM / HIDE_WHEN_FOGGED, multisample), Lua semantics, setup phase, overrun, order
# independence (shuffled source order -> identical hash). Real FoC data (GameConstants, maps) is exercised by
# alamo_sim_battle_tests (engine/apps/sim) when ALAMO_FOC_DATA is set.
add_executable(alamo_tactical_fog_tests tests/test_fog.cpp)
target_link_libraries(alamo_tactical_fog_tests PRIVATE alamo_tactical_fog)
add_test(NAME alamo_tactical_fog_tests COMMAND alamo_tactical_fog_tests)
set_tests_properties(alamo_tactical_fog_tests PROPERTIES TIMEOUT 300)

# N1 stage 2: save / load mid-battle (reveal sources, decay mid-fade, Lua reveal / undo, Reveal_All, dense mask, setup
# phase): a loaded fog continues identically to the uninterrupted one.
add_executable(alamo_tactical_fog_save_tests tests/test_fog_save.cpp)
target_link_libraries(alamo_tactical_fog_save_tests PRIVATE alamo_tactical_fog)
add_test(NAME alamo_tactical_fog_save_tests COMMAND alamo_tactical_fog_save_tests)
set_tests_properties(alamo_tactical_fog_save_tests PROPERTIES TIMEOUT 300)
