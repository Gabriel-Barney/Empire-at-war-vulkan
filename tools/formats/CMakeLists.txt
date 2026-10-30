cmake_minimum_required(VERSION 3.20)
project(alamo_formats LANGUAGES CXX VERSION 0.1.0)

# Clean-room parsers for Alamo engine assets (ALO/ALA/MTD/DAT/TEM/TEE). No external dependencies.

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 20)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ALAMO_FORMATS_BUILD_TOOLS "Build alodump / alo2gltf / formats_validate" ON)
option(ALAMO_FORMATS_BUILD_TESTS "Build unit tests" ON)

add_library(alamo_formats
  src/common.cpp
  src/chunk.cpp
  src/alo.cpp
  src/particle.cpp
  src/ala.cpp
  src/text.cpp
  src/minichunk_file.cpp
)
add_library(alamo::formats ALIAS alamo_formats)
target_include_directories(alamo_formats PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_compile_features(alamo_formats PUBLIC cxx_std_20)
if(MSVC)
  set(ALAMO_FORMATS_WARNINGS /W4 /permissive- /utf-8)
else()
  set(ALAMO_FORMATS_WARNINGS -Wall -Wextra -Wpedantic)
endif()
target_compile_options(alamo_formats PRIVATE ${ALAMO_FORMATS_WARNINGS})

if(ALAMO_FORMATS_BUILD_TOOLS)
  foreach(t alodump alo2gltf formats_validate)
    add_executable(${t} tools/${t}.cpp)
    target_link_libraries(${t} PRIVATE alamo_formats)
    target_compile_options(${t} PRIVATE ${ALAMO_FORMATS_WARNINGS})
  endforeach()
endif()

if(ALAMO_FORMATS_BUILD_TESTS)
  enable_testing()
  add_executable(alamo_formats_tests tests/test_formats.cpp)
  target_link_libraries(alamo_formats_tests PRIVATE alamo_formats)
  target_compile_options(alamo_formats_tests PRIVATE ${ALAMO_FORMATS_WARNINGS})
  add_test(NAME alamo_formats_tests COMMAND alamo_formats_tests)
endif()
