# R3 - sim-side animation (docs/re/ANIM_PARTICLES.md §A): ModelAnimType table, clip discovery + binding,
# per-object three-slot playback state (SoA by object id), deterministic advance and bone poses read by
# gameplay. Clean-room: written from docs/ only. No Vulkan, no render dependency.
#   alamo_anim         core: needs alamo_core + alamo_formats (ALA/ALO parsing, private)
#   type_params.cpp    only when engine/objects is configured (GameObjectType anim fields)
add_library(alamo_anim STATIC
  src/anim_types.cpp
  src/anim_math.cpp
  src/clip.cpp
  src/anim_state.cpp
  src/anim_behaviors.cpp
  src/anim_events.cpp
  src/anim_save.cpp
)
add_library(alamo::anim ALIAS alamo_anim)
target_include_directories(alamo_anim PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(alamo_anim PUBLIC alamo_core alamo_engine_flags PRIVATE alamo_formats)
if(TARGET alamo_objects)
  target_sources(alamo_anim PRIVATE src/type_params.cpp)
  target_link_libraries(alamo_anim PUBLIC alamo_objects)
  target_compile_definitions(alamo_anim PUBLIC ALAMO_ANIM_HAVE_OBJECTS=1)
endif()

add_executable(alamo_anim_tests tests/test_anim.cpp)
target_link_libraries(alamo_anim_tests PRIVATE alamo_anim alamo_formats Threads::Threads)
target_compile_definitions(alamo_anim_tests PRIVATE "ALAMO_ANIM_LOG_DIR=\"${PROJECT_SOURCE_DIR}/logs\"")
if(TARGET alamo_jobs)
  target_link_libraries(alamo_anim_tests PRIVATE alamo_jobs)
  target_compile_definitions(alamo_anim_tests PRIVATE ALAMO_ANIM_HAVE_JOBS=1)
endif()
# Real data (ALAMO_FOC_DATA): models and clips are read from the MEGs through the VFS.
if(TARGET alamo_vfs)
  target_link_libraries(alamo_anim_tests PRIVATE alamo_vfs)
  target_compile_definitions(alamo_anim_tests PRIVATE ALAMO_ANIM_HAVE_VFS=1)
endif()
add_test(NAME alamo_anim_tests COMMAND alamo_anim_tests)
set_tests_properties(alamo_anim_tests PROPERTIES TIMEOUT 900)
