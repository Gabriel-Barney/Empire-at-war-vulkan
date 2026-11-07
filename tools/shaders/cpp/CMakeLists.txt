# FX2 (compiled .fxo effect) reader, used by engine/materials, plus the fx2dump CLI. Built with the engine's flags
# (alamo_engine_flags: C++20, warnings; defined in engine/CMakeLists.txt and resolved at generate time).
add_library(alamo_fx2 STATIC fx2_reader.cpp)
target_include_directories(alamo_fx2 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(alamo_fx2 PRIVATE alamo_engine_flags)
add_executable(fx2dump fx2dump.cpp)
target_link_libraries(fx2dump PRIVATE alamo_fx2 alamo_engine_flags)
