# R3 render side - animated sim units: SimAnimDriver (interim visual driver over alamo_anim, fed from R1 snapshots)
# whose poses reach the GPU through Instance::bone_world (the renderer's skinned palettes). No Vulkan code here.
# Hooked into alamo_render --sim (app/sim_main.cpp, ALAMO_RENDER_HAVE_ANIM_DRIVER) and SimInstanceBuilder's pose
# source. Needs engine/anim built with engine/objects (anim::type_anim_params).
if(NOT TARGET alamo_anim OR NOT TARGET alamo_objects OR NOT TARGET alamo_assets)
  message(STATUS "alamo_render_anim skipped: needs the anim, objects and assets modules")
  return()
endif()

add_library(alamo_render_anim STATIC src/sim_anim_driver.cpp src/anim_pose_cache.cpp)
add_library(alamo::render_anim ALIAS alamo_render_anim)
target_include_directories(alamo_render_anim PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(alamo_render_anim PUBLIC alamo_anim alamo_assets alamo_objects PRIVATE alamo_engine_flags)

if(TARGET alamo_render_app)
  target_link_libraries(alamo_render_app PRIVATE alamo_render_anim)
  target_compile_definitions(alamo_render_app PRIVATE ALAMO_RENDER_HAVE_ANIM_DRIVER=1)
endif()

add_executable(alamo_render_anim_tests tests/test_render_anim.cpp)
target_link_libraries(alamo_render_anim_tests PRIVATE alamo_core alamo_render_anim alamo_engine_flags)
target_compile_definitions(alamo_render_anim_tests PRIVATE "ALAMO_RENDER_ANIM_LOG_DIR=\"${PROJECT_SOURCE_DIR}/logs\"")
if(TARGET alamo_jobs)
  target_link_libraries(alamo_render_anim_tests PRIVATE alamo_jobs)
  target_compile_definitions(alamo_render_anim_tests PRIVATE ALAMO_RENDER_ANIM_HAVE_JOBS=1)
endif()
if(TARGET alamo_vfs)
  target_link_libraries(alamo_render_anim_tests PRIVATE alamo_vfs)
  target_compile_definitions(alamo_render_anim_tests PRIVATE ALAMO_RENDER_ANIM_HAVE_VFS=1)
endif()
# The GPU part (with ALAMO_FOC_DATA): alamo_render --sim-headless-check on a land map with unit animation on -> PNG in
# logs/, per-tick CRC rendering on == off, 0 validation messages. The app is only complete once the deferred R1
# targets exist, so its path is passed as a generator expression.
if(TARGET alamo_render_app)
  add_dependencies(alamo_render_anim_tests alamo_render_app)
  target_compile_definitions(alamo_render_anim_tests PRIVATE "ALAMO_RENDER_APP=\"$<TARGET_FILE:alamo_render_app>\""
                             "$<$<TARGET_EXISTS:alamo_render_sim>:ALAMO_RENDER_APP_HAS_SIM=1>")
endif()
add_test(NAME alamo_render_anim_tests COMMAND alamo_render_anim_tests)
set_tests_properties(alamo_render_anim_tests PROPERTIES TIMEOUT 1200)
