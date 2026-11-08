# R2 galactic map render (Phase 4): snapshot of the running GalacticSim after each tick -> planets, trade routes,
# fleets, ownership markers and the galactic background as render::Instance lists; alamo_render --galactic /
# --galactic-headless-check. Owner: the R2 agent (this folder). Clean-room: written from docs/ only.
# Needs alamo_render_sim (R1, engine/render) and alamo_sim (engine/apps), which exist only at the end of the
# top-level configure: the targets are created by a call deferred after R1's (engine/render/CMakeLists.txt adds this
# directory after registering its own deferred call).
function(alamo_render_add_galactic_targets)
  foreach(_dep alamo_render_sim alamo_sim alamo_ui)
    if(NOT TARGET ${_dep})
      message(STATUS "alamo_render_galactic: ${_dep} not configured; R2 galactic view skipped")
      return()
    endif()
  endforeach()
  set(_dir ${CMAKE_CURRENT_FUNCTION_LIST_DIR})
  add_library(alamo_render_galactic STATIC
    ${_dir}/src/galactic_snapshot.cpp
    ${_dir}/src/galactic_view.cpp
    ${_dir}/src/galactic_session.cpp
    ${_dir}/src/planet_portrait.cpp)
  target_include_directories(alamo_render_galactic PUBLIC ${_dir}/include)
  target_link_libraries(alamo_render_galactic PUBLIC alamo_render_sim alamo_sim alamo_data PRIVATE alamo_engine_flags)

  # alamo_render --galactic / --galactic-headless-check (dispatch hook in engine/render/app/main.cpp).
  target_sources(alamo_render_app PRIVATE ${_dir}/app/galactic_main.cpp)
  target_link_libraries(alamo_render_app PRIVATE alamo_render_galactic alamo_ui)
  if(TARGET alamo_ui_rhi)
    target_link_libraries(alamo_render_app PRIVATE alamo_ui_rhi)
    target_compile_definitions(alamo_render_app PRIVATE ALAMO_RENDER_GALACTIC_UI_INPUT=1)
  endif()
  target_compile_definitions(alamo_render_app PRIVATE ALAMO_RENDER_HAVE_GALACTIC=1)

  add_executable(alamo_render_galactic_tests ${_dir}/tests/test_render_galactic.cpp)
  # alamo_core first: see alamo_render_app (formats' inline ByteReader vs core's, LNK2005).
  target_link_libraries(alamo_render_galactic_tests PRIVATE alamo_core alamo_render_galactic alamo_vfs alamo_engine_flags alamo_render_test_support)
  target_compile_definitions(alamo_render_galactic_tests PRIVATE "ALAMO_RENDER_SOURCE_DIR=\"${PROJECT_SOURCE_DIR}\"")
  alamo_shaderxlat_copy_dxc(alamo_render_galactic_tests)
  add_test(NAME alamo_render_galactic_tests COMMAND alamo_render_galactic_tests)
  set_tests_properties(alamo_render_galactic_tests PROPERTIES TIMEOUT 900)
  # Rendering on vs off over the synthetic galaxy: identical per-tick CRC logs and galactic state hash, 0 validation
  # messages.
  add_test(NAME alamo_render_galactic_headless_check COMMAND alamo_render_app --galactic-headless-check --ticks 600)
  set_tests_properties(alamo_render_galactic_headless_check PROPERTIES TIMEOUT 300)
endfunction()
cmake_language(DEFER DIRECTORY ${PROJECT_SOURCE_DIR} CALL alamo_render_add_galactic_targets)
