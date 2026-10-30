# Top-level build for the Alamo rewrite: Phase 1 tools + Phase 2 engine (engine/).
cmake_minimum_required(VERSION 3.20)
project(alamo_rewrite LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

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
