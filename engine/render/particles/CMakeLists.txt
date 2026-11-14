# R4 particle systems (Phase 4): particle ALO -> CPU data, CPU emitter bookkeeping (counts and timing of the original),
# GPU simulation (compute) + drawing in buckets 7 / 8 / 11 through the Renderer's scene hook, attachment to model
# proxies (sim view objects, map placements). Clean-room: docs/re/ANIM_PARTICLES.md B, docs/formats/SPEC.md 3.
# Render particles are visual only (B.6): nothing here is read by the simulation.

# ---------------------------------------------------------------- shaders (DXC -> SPIR-V headers)
set(_pt_dxc "${ALAMO_DXC}")
if(NOT EXISTS "${_pt_dxc}")
  find_program(_pt_dxc_found NAMES dxc)
  set(_pt_dxc "${_pt_dxc_found}")
endif()
set(_pt_src ${CMAKE_CURRENT_SOURCE_DIR}/shaders/particles.hlsl)
set(_pt_gen ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${_pt_gen})
set(_pt_headers)
# name : entry : profile : define (CS = compute entries with read-write particle buffers)
foreach(spec "particles_cs_update:CSUpdate:cs_6_0:CS" "particles_cs_spawn:CSSpawn:cs_6_0:CS" "particles_cs_sort:CSSort:cs_6_0:CS"
             "particles_cs_clear:CSClear:cs_6_0:CS"
             "particles_vs:VSMain:vs_6_0:GFX" "particles_ps:PSMain:ps_6_0:GFX")
  string(REPLACE ":" ";" parts "${spec}")
  list(GET parts 0 _name)
  list(GET parts 1 _entry)
  list(GET parts 2 _profile)
  list(GET parts 3 _def)
  set(_hdr ${_pt_gen}/${_name}.h)
  add_custom_command(
    OUTPUT ${_hdr}
    COMMAND "${_pt_dxc}" -nologo -spirv -fspv-target-env=vulkan1.3 -WX -O3 -D ${_def}
            -T ${_profile} -E ${_entry} -Fh ${_hdr} -Vn g_${_name} "${_pt_src}"
    DEPENDS ${_pt_src}
    COMMENT "DXC ${_name} (particles.hlsl ${_entry} ${_profile})"
    VERBATIM)
  list(APPEND _pt_headers ${_hdr})
endforeach()
add_custom_target(alamo_render_particles_shaders DEPENDS ${_pt_headers})

# ---------------------------------------------------------------- library
add_library(alamo_render_particles STATIC
  src/particle_data.cpp
  src/particle_sim.cpp
  src/particle_gpu.cpp
  src/particle_attach.cpp
  src/particle_synthetic.cpp
  ${_pt_headers}
)
add_dependencies(alamo_render_particles alamo_render_particles_shaders)
target_include_directories(alamo_render_particles PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include PRIVATE ${_pt_gen})
target_link_libraries(alamo_render_particles PUBLIC alamo_render PRIVATE alamo_formats alamo_engine_flags)

# ---------------------------------------------------------------- tests
add_executable(alamo_render_particles_tests tests/test_particles.cpp)
# alamo_core first: tools/formats' inline ByteReader vs core's (see alamo_render_app).
target_link_libraries(alamo_render_particles_tests PRIVATE alamo_core alamo_render_particles alamo_formats alamo_vfs alamo_engine_flags alamo_render_test_support)
target_compile_definitions(alamo_render_particles_tests PRIVATE "ALAMO_RENDER_SOURCE_DIR=\"${PROJECT_SOURCE_DIR}\"")
add_test(NAME alamo_render_particles_tests COMMAND alamo_render_particles_tests)
set_tests_properties(alamo_render_particles_tests PROPERTIES TIMEOUT 900)

# ---------------------------------------------------------------- alamo_render --particles (+ sim attachment)
target_sources(alamo_render_app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/app/particles_main.cpp)
target_link_libraries(alamo_render_app PRIVATE alamo_render_particles)
target_compile_definitions(alamo_render_app PRIVATE ALAMO_RENDER_HAVE_PARTICLES=1)
add_test(NAME alamo_render_particles_showcase COMMAND alamo_render_app --particles --synthetic --hidden)
set_tests_properties(alamo_render_particles_showcase PROPERTIES TIMEOUT 300)

# Sim view attachment needs alamo_render_sim (created by a deferred call of engine/render; this one is queued after it).
function(alamo_render_particles_add_sim)
  if(NOT TARGET alamo_render_sim OR NOT TARGET alamo_render_particles)
    message(STATUS "alamo_render_particles: alamo_render_sim not configured; sim attachment skipped")
    return()
  endif()
  set(_dir ${CMAKE_CURRENT_FUNCTION_LIST_DIR})
  add_library(alamo_render_particles_sim STATIC ${_dir}/src/particle_sim_attach.cpp)
  target_include_directories(alamo_render_particles_sim PUBLIC ${_dir}/include)
  target_link_libraries(alamo_render_particles_sim PUBLIC alamo_render_particles alamo_render_sim PRIVATE alamo_engine_flags)
  target_link_libraries(alamo_render_particles_tests PRIVATE alamo_render_particles_sim)
  target_compile_definitions(alamo_render_particles_tests PRIVATE ALAMO_PARTICLES_HAVE_SIM=1)
  target_link_libraries(alamo_render_app PRIVATE alamo_render_particles_sim)
  target_compile_definitions(alamo_render_app PRIVATE ALAMO_PARTICLES_HAVE_SIM=1)
  # Particles on vs off over the synthetic sim map: identical per-tick CRC logs, 0 validation messages.
  add_test(NAME alamo_render_particles_sim_check COMMAND alamo_render_app --particles --sim-check --ticks 300 --hidden)
  set_tests_properties(alamo_render_particles_sim_check PROPERTIES TIMEOUT 300)
  # Integration: alamo_render --sim-headless-check with particles (+ R3 animation on real maps) on vs rendering off:
  # identical per-tick CRC logs, particles actually spawned, 0 validation messages.
  add_test(NAME alamo_render_sim_particles_check COMMAND alamo_render_app --sim-headless-check --ticks 300 --expect-particles)
  set_tests_properties(alamo_render_sim_particles_check PROPERTIES TIMEOUT 300)
endfunction()
cmake_language(DEFER DIRECTORY ${PROJECT_SOURCE_DIR} CALL alamo_render_particles_add_sim)
