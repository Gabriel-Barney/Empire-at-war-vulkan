# Phase 3 render - scene renderer joining assets + materials + rhi, and the alamo_render test app
# (roadmap C1T clear screen, C2b ship unlit). Clean-room: written from docs/ only.
if(NOT TARGET alamo_rhi OR NOT TARGET alamo_materials OR NOT TARGET alamo_assets OR NOT TARGET alamo_terrain OR NOT TARGET alamo_data)
  message(WARNING "alamo_render skipped: needs the assets, rhi, materials, terrain and data modules")
  return()
endif()

# ---------------------------------------------------------------- unlit shaders (DXC -> SPIR-V headers)
set(_render_dxc "${ALAMO_DXC}")
if(NOT EXISTS "${_render_dxc}")
  find_program(_render_dxc_found NAMES dxc)
  if(_render_dxc_found)
    set(_render_dxc "${_render_dxc_found}")
  else()
    message(FATAL_ERROR "render: DXC not found (ALAMO_DXC='${ALAMO_DXC}'); run toolchain\\3_renderer_deps.cmd")
  endif()
endif()
set(_render_shader_dir ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
set(_render_common_dir ${CMAKE_CURRENT_SOURCE_DIR}/../materials/shaders)  # common.hlsli (hot-set interface)
set(_render_gen_dir ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${_render_gen_dir})
set(_render_shader_headers)
# name : entry : profile : defines
foreach(spec "unlit_vs:VSMain:vs_6_0:" "unlit_vs_skin:VSMain:vs_6_0:SKINNED" "unlit_ps:PSMain:ps_6_0:")
  string(REPLACE ":" ";" parts "${spec}")
  list(GET parts 0 _name)
  list(GET parts 1 _entry)
  list(GET parts 2 _profile)
  list(LENGTH parts _np)
  set(_defs)
  if(_np GREATER 3)
    list(GET parts 3 _d)
    if(NOT _d STREQUAL "")
      set(_defs -D ${_d})
    endif()
  endif()
  set(_hdr ${_render_gen_dir}/${_name}.h)
  add_custom_command(
    OUTPUT ${_hdr}
    COMMAND "${_render_dxc}" -nologo -spirv -fvk-use-dx-layout -fspv-target-env=vulkan1.3 -WX -O3
            -T ${_profile} -E ${_entry} ${_defs} -I "${_render_common_dir}"
            -Fh ${_hdr} -Vn g_${_name} "${_render_shader_dir}/unlit.hlsl"
    DEPENDS ${_render_shader_dir}/unlit.hlsl ${_render_common_dir}/common.hlsli
    COMMENT "DXC ${_name} (unlit.hlsl ${_entry} ${_profile})"
    VERBATIM)
  list(APPEND _render_shader_headers ${_hdr})
endforeach()
add_custom_target(alamo_render_shaders DEPENDS ${_render_shader_headers})

# ---------------------------------------------------------------- [port] SSAO shaders (shaders/ssao.hlsl, src/ssao.cpp)
set(_render_ssao_headers)
# name : entry : profile
foreach(spec "ssao_vs:VSFull:vs_6_0" "ssao_ps_ao:PSAo:ps_6_0" "ssao_ps_blur:PSBlur:ps_6_0" "ssao_ps_apply:PSApply:ps_6_0")
  string(REPLACE ":" ";" parts "${spec}")
  list(GET parts 0 _name)
  list(GET parts 1 _entry)
  list(GET parts 2 _profile)
  set(_hdr ${_render_gen_dir}/${_name}.h)
  add_custom_command(
    OUTPUT ${_hdr}
    COMMAND "${_render_dxc}" -nologo -spirv -fvk-use-dx-layout -fspv-target-env=vulkan1.3 -WX -O3
            -T ${_profile} -E ${_entry} -Fh ${_hdr} -Vn g_${_name} "${_render_shader_dir}/ssao.hlsl"
    DEPENDS ${_render_shader_dir}/ssao.hlsl
    COMMENT "DXC ${_name} (ssao.hlsl ${_entry} ${_profile})"
    VERBATIM)
  list(APPEND _render_ssao_headers ${_hdr})
endforeach()
add_custom_target(alamo_render_ssao_shaders DEPENDS ${_render_ssao_headers})

# ---------------------------------------------------------------- library
add_library(alamo_render STATIC
  src/convert.cpp
  src/camera.cpp
  src/lod.cpp
  src/hardpoints.cpp
  src/png.cpp
  src/lighting.cpp
  src/fleet.cpp
  src/ted.cpp
  src/objects.cpp
  src/world.cpp
  src/decorations.cpp
  src/fog_view.cpp
  src/surface_fx.cpp
  src/scene_anim.cpp
  src/stealth_fx.cpp
  src/map_scene.cpp
  src/pipelines.cpp
  src/renderer.cpp
  src/renderer_xlat.cpp
  src/renderer_draw.cpp
  src/renderer_graph.cpp
  src/lasers.cpp
  src/beams.cpp
  src/beams_draw.cpp
  src/ssao.cpp
  ${_render_shader_headers}
  ${_render_ssao_headers}
)
add_library(alamo::render ALIAS alamo_render)
add_dependencies(alamo_render alamo_render_shaders alamo_render_ssao_shaders)
target_include_directories(alamo_render PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include PRIVATE ${_render_gen_dir})
target_link_libraries(alamo_render PUBLIC alamo_assets alamo_materials alamo_shaderxlat alamo_rhi alamo_jobs alamo_vfs alamo_terrain alamo_data)
# R3: MapScene's background idle clips use alamo_anim's frame rules (loops wrap at count - 1: 239 s planet turns).
if(TARGET alamo_anim)
  target_link_libraries(alamo_render PRIVATE alamo_anim)
  target_compile_definitions(alamo_render PRIVATE ALAMO_RENDER_HAVE_ANIM=1)
endif()

# ---------------------------------------------------------------- [port] TAA (shaders/taa.hlsl, src/taa.cpp)
set(_render_taa_headers)
# name : entry : profile : define
foreach(spec "taa_vs:VSMain:vs_6_0:" "taa_ps_resolve:PSResolve:ps_6_0:RESOLVE" "taa_ps_sharpen:PSSharpen:ps_6_0:SHARPEN" "taa_ps_depth:PSDepth:ps_6_0:DEPTH")
  string(REPLACE ":" ";" parts "${spec}")
  list(GET parts 0 _name)
  list(GET parts 1 _entry)
  list(GET parts 2 _profile)
  list(LENGTH parts _np)
  set(_defs)
  if(_np GREATER 3)
    list(GET parts 3 _d)
    if(NOT _d STREQUAL "")
      set(_defs -D ${_d})
    endif()
  endif()
  set(_hdr ${_render_gen_dir}/${_name}.h)
  add_custom_command(
    OUTPUT ${_hdr}
    COMMAND "${_render_dxc}" -nologo -spirv -fvk-use-dx-layout -fspv-target-env=vulkan1.3 -WX -O3
            -T ${_profile} -E ${_entry} ${_defs} -Fh ${_hdr} -Vn g_${_name} "${_render_shader_dir}/taa.hlsl"
    DEPENDS ${_render_shader_dir}/taa.hlsl
    COMMENT "DXC ${_name} (taa.hlsl ${_entry} ${_profile})"
    VERBATIM)
  list(APPEND _render_taa_headers ${_hdr})
endforeach()
add_custom_target(alamo_render_taa_shaders DEPENDS ${_render_taa_headers})
target_sources(alamo_render PRIVATE src/taa.cpp ${_render_taa_headers})
add_dependencies(alamo_render alamo_render_taa_shaders)

# ---------------------------------------------------------------- [port] output gamma (shaders/gamma.hlsl, src/gamma.cpp)
set(_render_gamma_headers)
foreach(spec "gamma_vs:VSMain:vs_6_0" "gamma_ps:PSMain:ps_6_0")
  string(REPLACE ":" ";" parts "${spec}")
  list(GET parts 0 _name)
  list(GET parts 1 _entry)
  list(GET parts 2 _profile)
  set(_hdr ${_render_gen_dir}/${_name}.h)
  add_custom_command(
    OUTPUT ${_hdr}
    COMMAND "${_render_dxc}" -nologo -spirv -fvk-use-dx-layout -fspv-target-env=vulkan1.3 -WX -O3
            -T ${_profile} -E ${_entry} -Fh ${_hdr} -Vn g_${_name} "${_render_shader_dir}/gamma.hlsl"
    DEPENDS ${_render_shader_dir}/gamma.hlsl
    COMMENT "DXC ${_name} (gamma.hlsl ${_entry} ${_profile})"
    VERBATIM)
  list(APPEND _render_gamma_headers ${_hdr})
endforeach()
add_custom_target(alamo_render_gamma_shaders DEPENDS ${_render_gamma_headers})
target_sources(alamo_render PRIVATE src/gamma.cpp ${_render_gamma_headers})
add_dependencies(alamo_render alamo_render_gamma_shaders)
# Halton / jitter / history rules (CPU) + the resolve on the GPU (empty scene, validation required when available) and,
# with ALAMO_FOC_DATA, a land map under a slowly moving camera, TAA off vs on -> logs\render_taa_*.png.
add_executable(alamo_render_taa_tests tests/test_render_taa.cpp)
target_link_libraries(alamo_render_taa_tests PRIVATE alamo_core alamo_render)
target_compile_definitions(alamo_render_taa_tests PRIVATE "ALAMO_RENDER_SOURCE_DIR=\"${PROJECT_SOURCE_DIR}\"")
alamo_shaderxlat_copy_dxc(alamo_render_taa_tests)
add_test(NAME alamo_render_taa_tests COMMAND alamo_render_taa_tests)
set_tests_properties(alamo_render_taa_tests PROPERTIES TIMEOUT 900 ENVIRONMENT "VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true")

# Soft shadows must not darken the unshadowed frame (playtest 3); real data: _MP_LAND_ENDOR shadows off / hard / soft.
add_executable(alamo_render_shadow_tests tests/test_render_shadow.cpp)
target_link_libraries(alamo_render_shadow_tests PRIVATE alamo_core alamo_render)
target_compile_definitions(alamo_render_shadow_tests PRIVATE "ALAMO_RENDER_SOURCE_DIR=\"${PROJECT_SOURCE_DIR}\"")
alamo_shaderxlat_copy_dxc(alamo_render_shadow_tests)
add_test(NAME alamo_render_shadow_tests COMMAND alamo_render_shadow_tests)
set_tests_properties(alamo_render_shadow_tests PROPERTIES TIMEOUT 900 ENVIRONMENT "VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true")

# ---------------------------------------------------------------- test app
# Target alamo_render_app, file alamo_render.exe (the library owns the target name alamo_render).
add_executable(alamo_render_app app/main.cpp)
set_target_properties(alamo_render_app PROPERTIES OUTPUT_NAME alamo_render)
# alamo_core first: tools/formats defines its own inline alamo::ByteReader that clashes with core's out-of-line one
# when MSVC pulls formats' COMDATs first (LNK2005; same workaround as alamo_headless).
target_link_libraries(alamo_render_app PRIVATE alamo_core alamo_render)
target_compile_definitions(alamo_render_app PRIVATE "ALAMO_RENDER_SOURCE_DIR=\"${PROJECT_SOURCE_DIR}\"")
if(WIN32)
  target_link_libraries(alamo_render_app PRIVATE user32)
endif()
alamo_shaderxlat_copy_dxc(alamo_render_app)  # runtime dxcompiler.dll + dxil.dll for track B
# Smoke run of the frame graph into a real swapchain with synchronization validation (0 hazards required:
# the app exits non-zero on any validation message). 30 frames into a never-shown (hidden) window: the
# swapchain, acquire / present and the sync validation stay covered without a window popping up.
add_test(NAME alamo_render_app_sync COMMAND alamo_render_app --clear --frame-graph --frames 30 --hidden)
set_tests_properties(alamo_render_app_sync PROPERTIES TIMEOUT 120 ENVIRONMENT "VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true")

# ---------------------------------------------------------------- tests
add_executable(alamo_render_tests tests/test_render.cpp tests/test_render_gpu.cpp)
target_link_libraries(alamo_render_tests PRIVATE alamo_render)
target_compile_definitions(alamo_render_tests PRIVATE "ALAMO_RENDER_SOURCE_DIR=\"${PROJECT_SOURCE_DIR}\"")
# CPU checks always; GPU checks skip cleanly without a Vulkan 1.3 device. The test sets
# VK_LAYER_PATH (toolchain\vvl\bin) itself and then requires validation with 0 messages.
alamo_shaderxlat_copy_dxc(alamo_render_tests)
add_test(NAME alamo_render_tests COMMAND alamo_render_tests)
set_tests_properties(alamo_render_tests PROPERTIES TIMEOUT 600)

# Space background scene order on real data (planets vs nebula domes, SPACE_AND_OPEN.md D.7 3); PNGs to logs\.
add_executable(alamo_render_space_tests tests/test_render_space.cpp)
target_link_libraries(alamo_render_space_tests PRIVATE alamo_render)
target_compile_definitions(alamo_render_space_tests PRIVATE "ALAMO_RENDER_SOURCE_DIR=\"${PROJECT_SOURCE_DIR}\"")
alamo_shaderxlat_copy_dxc(alamo_render_space_tests)
add_test(NAME alamo_render_space_tests COMMAND alamo_render_space_tests)
set_tests_properties(alamo_render_space_tests PROPERTIES TIMEOUT 600)

# ---------------------------------------------------------------- R1 sim -> render (Phase 4)
# Snapshot of a finished sim tick -> render::Instance, interpolation, alamo_render --sim / --sim-headless-check.
# Needs alamo_sim (engine/apps), which is configured after this directory: the targets are created by a call
# deferred to the end of the top-level directory (every module's targets exist by then).
function(alamo_render_add_sim_targets)
  if(NOT TARGET alamo_sim OR NOT TARGET alamo_render)
    message(STATUS "alamo_render: alamo_sim not configured (apps/objects/commands/tactical/galactic); R1 sim view skipped")
    return()
  endif()
  set(_dir ${CMAKE_CURRENT_FUNCTION_LIST_DIR})
  add_library(alamo_render_sim STATIC ${_dir}/src/sim_snapshot.cpp ${_dir}/src/sim_view.cpp ${_dir}/src/sim_session.cpp
              ${_dir}/src/sim_decorations.cpp)
  target_include_directories(alamo_render_sim PUBLIC ${_dir}/include)
  target_link_libraries(alamo_render_sim PUBLIC alamo_render alamo_sim alamo_objects PRIVATE alamo_engine_flags)
  target_sources(alamo_render_sim PRIVATE ${_dir}/src/sim_beams.cpp)  # R5 t04 ability beams (sim_beams.hpp)
  target_sources(alamo_render_sim PRIVATE ${_dir}/src/jam_fx.cpp)  # R5 sensor jamming blob (jam_fx.hpp)

  target_sources(alamo_render_app PRIVATE ${_dir}/app/sim_main.cpp)
  target_link_libraries(alamo_render_app PRIVATE alamo_render_sim)
  target_compile_definitions(alamo_render_app PRIVATE ALAMO_RENDER_HAVE_SIM=1)

  add_executable(alamo_render_sim_tests ${_dir}/tests/test_render_sim.cpp)
  target_link_libraries(alamo_render_sim_tests PRIVATE alamo_core alamo_render_sim alamo_vfs alamo_engine_flags)  # core first: see alamo_render_app
  target_compile_definitions(alamo_render_sim_tests PRIVATE "ALAMO_RENDER_SOURCE_DIR=\"${PROJECT_SOURCE_DIR}\"")
  add_test(NAME alamo_render_sim_tests COMMAND alamo_render_sim_tests)
  set_tests_properties(alamo_render_sim_tests PROPERTIES TIMEOUT 900)
  # Rendering on vs off: the synthetic map (no game data), per-tick CRC logs must be identical, 0 validation messages.
  add_test(NAME alamo_render_sim_headless_check COMMAND alamo_render_app --sim-headless-check --ticks 300)
  set_tests_properties(alamo_render_sim_headless_check PROPERTIES TIMEOUT 300)
  # R3 sim-owned animation with the opt-in "anim" CRC system (the default run above has it off; both runs of the check
  # attach the SimAnimation): identical per-tick CRC logs, "anim" registered, 0 validation messages.
  add_test(NAME alamo_render_sim_anim_crc_check COMMAND alamo_render_app --sim-headless-check --ticks 300 --anim-crc)
  set_tests_properties(alamo_render_sim_anim_crc_check PROPERTIES TIMEOUT 300)

  # Phase 4 R3 / R4 / R5 render-side additions: fog of war from the sim's grids, runtime decals / unit tracks, _ALT swap,
  # per-bone clip visibility, HideWhenFogged fade. PNGs (real data) go to the build directory.
  add_executable(alamo_render_r5_tests ${_dir}/tests/test_render_r5.cpp)
  target_link_libraries(alamo_render_r5_tests PRIVATE alamo_core alamo_render_sim alamo_vfs alamo_engine_flags)
  target_compile_definitions(alamo_render_r5_tests PRIVATE "ALAMO_RENDER_SOURCE_DIR=\"${PROJECT_SOURCE_DIR}\""
                             "ALAMO_R5_OUT_DIR=\"${CMAKE_BINARY_DIR}\"")
  if(TARGET alamo_tactical_fog)
    target_link_libraries(alamo_render_r5_tests PRIVATE alamo_tactical_fog)
    target_compile_definitions(alamo_render_r5_tests PRIVATE ALAMO_R5_HAVE_FOG=1)
  endif()
  if(TARGET alamo_render_anim)
    target_link_libraries(alamo_render_r5_tests PRIVATE alamo_render_anim)
    target_compile_definitions(alamo_render_r5_tests PRIVATE ALAMO_R5_HAVE_ANIM=1)
  endif()
  alamo_shaderxlat_copy_dxc(alamo_render_r5_tests)
  add_test(NAME alamo_render_r5_tests COMMAND alamo_render_r5_tests)
  set_tests_properties(alamo_render_r5_tests PROPERTIES TIMEOUT 900)

  # Phase 4 R5 t04 ability beams (beams.hpp / sim_beams.hpp): ribbon / dot / lightning geometry, animation, snapshot records
  # from a real battle's abilities; GPU draw with Engine\PrimAdditive when ALAMO_FOC_DATA is set (validation required).
  add_executable(alamo_render_beams_tests ${_dir}/tests/test_render_beams.cpp)
  target_link_libraries(alamo_render_beams_tests PRIVATE alamo_core alamo_render_sim alamo_vfs alamo_engine_flags)
  target_compile_definitions(alamo_render_beams_tests PRIVATE "ALAMO_RENDER_SOURCE_DIR=\"${PROJECT_SOURCE_DIR}\"")
  alamo_shaderxlat_copy_dxc(alamo_render_beams_tests)
  add_test(NAME alamo_render_beams_tests COMMAND alamo_render_beams_tests)
  set_tests_properties(alamo_render_beams_tests PROPERTIES TIMEOUT 600)

  # Phase 4 R5 t05 sensor jamming visual (jam_fx.hpp): blob visibility rule / model / instance, snapshot jamming digest, interpolation.
  add_executable(alamo_render_jam_tests ${_dir}/tests/test_render_jam.cpp)
  target_link_libraries(alamo_render_jam_tests PRIVATE alamo_core alamo_render_sim alamo_vfs alamo_engine_flags)
  add_test(NAME alamo_render_jam_tests COMMAND alamo_render_jam_tests)
  set_tests_properties(alamo_render_jam_tests PROPERTIES TIMEOUT 300)
endfunction()
cmake_language(DEFER DIRECTORY ${PROJECT_SOURCE_DIR} CALL alamo_render_add_sim_targets)

# R2 hook (owner: engine/render/galactic): galactic map render, alamo_render --galactic. Registers its own deferred
# call after R1's, so alamo_render_sim exists when it runs.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/galactic/CMakeLists.txt)
  add_subdirectory(galactic)
endif()

# R4 particle systems (Phase 4): own folder, lib alamo_render_particles, alamo_render --particles (owned by R4).
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/particles/CMakeLists.txt)
  add_subdirectory(particles)
endif()

# C4b reference tests (FF alpha/additive + skinned draws vs the Phase 1 glTF exports); owned separately
# from the renderer sources so both can be worked on in parallel.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/reference/CMakeLists.txt)
  add_subdirectory(tests/reference)
endif()

# R3 animated sim units (SimAnimDriver -> Instance::bone_world); owned by the R3 agent.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/anim/CMakeLists.txt)
  add_subdirectory(anim)
endif()
