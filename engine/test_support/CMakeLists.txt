# CU4: header-only support for the engine test exes (no library code; never linked by engine libraries).
#   alamo/test/support.hpp             CHECK / g_fail / section / report, env_or_empty / env_or, the FoC / EaW data
#                                      roots, write_file / read_file, kSkipReturnCode (77)
#   alamo/test/thread_parallel_for.hpp the one std::thread ParallelFor pool the 1 / 4 / N-thread CRC tests use
add_library(alamo_test_support INTERFACE)
add_library(alamo::test_support ALIAS alamo_test_support)
target_include_directories(alamo_test_support INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
# thread_parallel_for.hpp includes alamo/core/parallel.hpp, which every user already gets from its module (core links
# Threads). alamo_core is deliberately not linked here, so adding this target never moves core on a link line (the
# render / headless exes keep core first, see engine/render/CMakeLists.txt).
target_link_libraries(alamo_test_support INTERFACE alamo_engine_flags)
