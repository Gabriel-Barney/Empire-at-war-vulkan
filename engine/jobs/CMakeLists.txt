# B4 - job system, deterministic helpers, task graph, fixed-step tick. No third-party deps.
add_library(alamo_jobs STATIC
  src/job_system.cpp
  src/scratch.cpp
  src/task_graph.cpp
  src/tick.cpp
)
add_library(alamo::jobs ALIAS alamo_jobs)
target_include_directories(alamo_jobs PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(alamo_jobs PUBLIC alamo_core)

add_executable(alamo_jobs_tests tests/test_jobs.cpp)
target_link_libraries(alamo_jobs_tests PRIVATE alamo_jobs)
add_test(NAME alamo_jobs_tests COMMAND alamo_jobs_tests)
set_tests_properties(alamo_jobs_tests PROPERTIES TIMEOUT 900)

# Benchmark (not a test): alamo_jobs_bench [max_workers]
add_executable(alamo_jobs_bench bench/bench_jobs.cpp)
target_link_libraries(alamo_jobs_bench PRIVATE alamo_jobs)

# Q1: per-tick CRC harness on the job system - identical CRC sequences for 0/1/4/N workers.
add_executable(alamo_jobs_crc_tests tests/test_crc_harness.cpp)
target_link_libraries(alamo_jobs_crc_tests PRIVATE alamo_jobs)
add_test(NAME alamo_jobs_crc_tests COMMAND alamo_jobs_crc_tests)
set_tests_properties(alamo_jobs_crc_tests PROPERTIES TIMEOUT 300)
