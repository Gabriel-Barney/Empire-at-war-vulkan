# Top-level build for the Alamo rewrite: Phase 1 tools + Phase 2 engine (engine/).
cmake_minimum_required(VERSION 3.20)
project(alamo_rewrite LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Compiler cache: when toolchain/sccache/sccache.exe exists (toolchain/10_sccache.cmd) every compile goes through it, so build
# trees and git worktrees share objects. The cache never changes the output (same bytes as an uncached compile), only
# whether the compiler runs. -DALAMO_COMPILER_CACHE=OFF compiles without it; tools/cache_env.cmd sets its cache folder.
option(ALAMO_COMPILER_CACHE "Compile through toolchain/sccache when it is installed" ON)
if(ALAMO_COMPILER_CACHE AND NOT "$ENV{ALAMO_COMPILER_CACHE}" STREQUAL "OFF" AND NOT CMAKE_CXX_COMPILER_LAUNCHER AND EXISTS "${CMAKE_SOURCE_DIR}/toolchain/sccache/sccache.exe")
  set(CMAKE_CXX_COMPILER_LAUNCHER "${CMAKE_SOURCE_DIR}/toolchain/sccache/sccache.exe")
  message(STATUS "Compiler cache: ${CMAKE_CXX_COMPILER_LAUNCHER}")
endif()

# Static CRT so the tools run on any Windows box without the VC++ redistributable. Applies to every
# compiler targeting the MSVC ABI (cl, and clang++ in build_clang.cmd); ignored by MinGW GCC.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(WIN32)
  # Same Windows header/CRT settings for cl, clang++ (MSVC ABI) and MinGW GCC.
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
if(MSVC)
  add_compile_options(/utf-8 /permissive- /Zc:__cplusplus /EHsc)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

enable_testing()

# ctest -j starts the longest tests first, from the costs of the previous run in the same build tree. A fresh tree (new worktree,
# build_agent / build_module tree) has none, so seed it from tests/ctest_cost_seed.txt (tools/lint/ctest_cost_seed.py refreshes it).
set(_alamo_cost_file "${CMAKE_BINARY_DIR}/Testing/Temporary/CTestCostData.txt")
if(NOT EXISTS "${_alamo_cost_file}" AND EXISTS "${CMAKE_SOURCE_DIR}/tests/ctest_cost_seed.txt")
  configure_file("${CMAKE_SOURCE_DIR}/tests/ctest_cost_seed.txt" "${_alamo_cost_file}" COPYONLY)
endif()
add_subdirectory(tools/meg)
add_subdirectory(tools/formats)
add_subdirectory(tools/shaders/cpp)

# Phase 2: headless engine core.
add_subdirectory(engine)
