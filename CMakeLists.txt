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
  # Worktree stale-deps fix (bo1 t03, 2026-11-13): sccache replays the cached /showIncludes output of the compile that filled
  # the entry, so a hit from another checkout path wrote that checkout's header paths into this tree's .ninja_deps (missing
  # files: every build relinked ~350 targets; existing files: header edits ignored). sccache keys on the preprocessed text
  # (an unused -D does not show up in it), so a forced-included header declares a type alias named after a hash of this
  # source path: entries then match only inside one checkout, and all its build trees still share them. No object changes.
  string(MD5 _alamo_src_id "${CMAKE_SOURCE_DIR}")
  string(SUBSTRING "${_alamo_src_id}" 0 12 _alamo_src_id)
  file(WRITE "${CMAKE_BINARY_DIR}/alamo_cache_scope.h" "#pragma once
using alamo_cache_scope_${_alamo_src_id} = int;
")
  if(MSVC)
    add_compile_options("/FI${CMAKE_BINARY_DIR}/alamo_cache_scope.h")
  else()
    add_compile_options(-include "${CMAKE_BINARY_DIR}/alamo_cache_scope.h")
  endif()
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
add_subdirectory(tools/meg)
add_subdirectory(tools/formats)
add_subdirectory(tools/shaders/cpp)

# Phase 2: headless engine core.
add_subdirectory(engine)

# Q4 behavioural parity harness (tools/parity, roadmap q4h): its selftest, and the reference-run comparisons when
# alamo_headless is built.
add_subdirectory(tools/parity)
