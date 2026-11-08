# S4 - land garrisons and structure capture (garrison.hpp, capture.hpp): occupancy, enter / exit / eject, garrison
# fire queries, CAPTURE_POINT ownership transitions; descriptors from the type data (garrison_types.hpp). The battle
# host (engine/apps/sim TacticalBattle, marked S4 blocks) applies the effects to movement / combat / the store and
# feeds STORY_GARRISON_UNIT / STORY_CAPTURE_STRUCTURE. Library deps: core, objects (type fields).
if(NOT TARGET alamo_objects)
  message(STATUS "alamo_tactical_garrison: objects module not configured, skipped")
  return()
endif()

add_library(alamo_tactical_garrison STATIC
  src/garrison.cpp
  src/capture.cpp
  src/garrison_types.cpp
  src/garrison_save.cpp  # N1 stage 2: GarrisonSystem / CapturePointSystem write + read
)
add_library(alamo::tactical_garrison ALIAS alamo_tactical_garrison)
target_include_directories(alamo_tactical_garrison PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(alamo_tactical_garrison PUBLIC alamo_objects alamo_core alamo_engine_flags
                                              PRIVATE alamo_tactical)  # save_io.hpp

add_executable(alamo_tactical_garrison_tests tests/test_garrison.cpp)
target_link_libraries(alamo_tactical_garrison_tests PRIVATE alamo_tactical_garrison alamo_test_support)
add_test(NAME alamo_tactical_garrison_tests COMMAND alamo_tactical_garrison_tests)
set_tests_properties(alamo_tactical_garrison_tests PROPERTIES TIMEOUT 300)
