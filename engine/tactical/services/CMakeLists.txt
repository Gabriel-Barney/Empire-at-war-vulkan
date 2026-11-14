# SP5 (docs/SIM_PARALLEL_DESIGN.md §4.3, §4.4, §5): the services phase graph (declared read / write component sets,
# phases of disjoint services, debug write check, reverse-order check), deferred-write buffers committed by canonical key,
# and counter-based random streams. Depends on core only; the host (TacticalBattle) declares its services.
add_library(alamo_tactical_services STATIC
  src/service_graph.cpp
)
add_library(alamo::tactical_services ALIAS alamo_tactical_services)
target_include_directories(alamo_tactical_services PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(alamo_tactical_services PUBLIC alamo_core alamo_engine_flags)

# Tests: phase placement (conflicts, edges, barriers), Serial / Parallel (1 / 4 / 8 threads) / Reverse runs identical,
# the write check and the reverse-order read check, deferred commits in key order for any split, fixed stream seeds.
add_executable(alamo_tactical_services_tests tests/test_services.cpp)
target_link_libraries(alamo_tactical_services_tests PRIVATE alamo_tactical_services alamo_test_support)
add_test(NAME alamo_tactical_services_tests COMMAND alamo_tactical_services_tests)
set_tests_properties(alamo_tactical_services_tests PROPERTIES TIMEOUT 300)
