# S6 - tactical simulation. First piece: the spatial tree replacing TreeCullClass (culling +
# gameplay queries), docs/re/HOTSPOTS.md section 3. The library depends on core only.
add_library(alamo_tactical STATIC
  src/spatial_tree.cpp
  src/tree_set.cpp
  src/tree_save.cpp  # N1 stage 2: entry-set save, rebuild on load
)
add_library(alamo::tactical ALIAS alamo_tactical)
target_include_directories(alamo_tactical PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(alamo_tactical PUBLIC alamo_core)

add_executable(alamo_tactical_tests tests/test_tactical.cpp)
target_link_libraries(alamo_tactical_tests PRIVATE alamo_tactical Threads::Threads)
add_test(NAME alamo_tactical_tests COMMAND alamo_tactical_tests)
set_tests_properties(alamo_tactical_tests PROPERTIES TIMEOUT 300)

# N1 stage 2: SpatialTree / TreeSet save + load (entry set saved, shape rebuilt) against the saving tree.
add_executable(alamo_tactical_tree_save_tests tests/test_tree_save.cpp)
target_link_libraries(alamo_tactical_tree_save_tests PRIVATE alamo_tactical)
add_test(NAME alamo_tactical_tree_save_tests COMMAND alamo_tactical_tree_save_tests)
set_tests_properties(alamo_tactical_tree_save_tests PROPERTIES TIMEOUT 300)

# Tactical systems built on the tree, each in its own subfolder (own CMakeLists, library and test exe)
# so they can be developed in parallel: movement/ = S7 (alamo_tactical_movement),
# combat/ = S8 (alamo_tactical_combat), abilities/ = S9 (alamo_tactical_abilities),
# fog/ = fog of war (alamo_tactical_fog), garrison/ = S4 garrisons + structure capture (alamo_tactical_garrison),
# hangar/ = SPAWN_SQUADRON launches + station-upgrade completion (alamo_tactical_hangar),
# presence/ = R3 presence-induced idle + explosion flinch victims (alamo_tactical_presence).
foreach(_sub movement combat abilities fog garrison hangar presence)
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${_sub}/CMakeLists.txt)
    add_subdirectory(${_sub})
  endif()
endforeach()

# Benchmark (not a test): alamo_tactical_bench. Uses the job system for the parallel paths.
if(TARGET alamo_jobs)
  add_executable(alamo_tactical_bench bench/bench_tactical.cpp)
  target_link_libraries(alamo_tactical_bench PRIVATE alamo_tactical alamo_jobs)
endif()
