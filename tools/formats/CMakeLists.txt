# Clean-room parsers for Alamo engine assets (ALO/ALA/MTD/DAT/TEM/TEE). No external dependencies.
# Linked by engine/anim, assets and render/particles, so it builds with the engine's flags (alamo_engine_flags: warnings,
# /fp:precise, -ffp-contract=off, C++20; the target is defined in engine/CMakeLists.txt and resolved at generate time).

add_library(alamo_formats
  src/common.cpp
  src/chunk.cpp
  src/alo.cpp
  src/particle.cpp
  src/ala.cpp
  src/text.cpp
  src/minichunk_file.cpp
)
target_include_directories(alamo_formats PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(alamo_formats PRIVATE alamo_engine_flags)

foreach(t alodump alo2gltf formats_validate)
  add_executable(${t} tools/${t}.cpp)
  target_link_libraries(${t} PRIVATE alamo_formats alamo_engine_flags)
endforeach()

add_executable(alamo_formats_tests tests/test_formats.cpp)
target_link_libraries(alamo_formats_tests PRIVATE alamo_formats alamo_engine_flags)
add_test(NAME alamo_formats_tests COMMAND alamo_formats_tests)
