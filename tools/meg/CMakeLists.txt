# MEG archive reader (used by engine/vfs) plus the megtool CLI and its selftest. Built with the engine's flags
# (alamo_engine_flags, defined in engine/CMakeLists.txt and resolved at generate time).

add_library(alamo_meg STATIC src/meg.cpp include/alamo/meg/meg.h)
target_include_directories(alamo_meg PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(alamo_meg PRIVATE alamo_engine_flags)

add_executable(megtool src/megtool.cpp)
target_link_libraries(megtool PRIVATE alamo_meg alamo_engine_flags)

add_executable(meg_selftest tests/meg_selftest.cpp)
target_link_libraries(meg_selftest PRIVATE alamo_meg alamo_engine_flags)
add_test(NAME meg_selftest COMMAND meg_selftest)
