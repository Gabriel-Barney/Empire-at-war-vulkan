# C2 rhi - Vulkan 1.3 backend: runtime vulkan-1 loader, instance/device/queues, Win32 surface +
# swapchain, frames in flight, sync2 helpers, VMA-backed memory, staging uploads, pipelines,
# descriptors, sampler cache and a minimal Win32 window. Clean-room: written from docs/ only.
if(NOT TARGET alamo_vulkan_headers OR NOT TARGET alamo_vma)
  message(WARNING "alamo_rhi skipped: Vulkan-Headers or VMA not found (run toolchain\\3_renderer_deps.cmd)")
  return()
endif()

add_library(alamo_rhi STATIC
  src/loader.cpp
  src/context.cpp
  src/format.cpp
  src/memory.cpp
  src/vma_impl.cpp
  src/sync.cpp
  src/upload.cpp
  src/descriptors.cpp
  src/sampler.cpp
  src/pipeline.cpp
  src/surface.cpp
  src/swapchain.cpp
  src/frame.cpp
  src/timer.cpp
  src/rendertarget.cpp
  src/window.cpp
)
add_library(alamo::rhi ALIAS alamo_rhi)
target_include_directories(alamo_rhi PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(alamo_rhi PUBLIC alamo_core alamo_vulkan_headers PRIVATE alamo_vma)
target_link_libraries(alamo_rhi PRIVATE user32)

# --- Test shaders: HLSL -> SPIR-V C headers with the project-local DXC, same flags as engine/materials.
set(_rhi_shader_dir ${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders)
set(_rhi_shader_out ${CMAKE_CURRENT_BINARY_DIR}/shaders)
set(_rhi_shader_headers)
set(_rhi_have_dxc OFF)
if(EXISTS "${ALAMO_DXC}")
  set(_rhi_have_dxc ON)
else()
  find_program(_rhi_dxc_found NAMES dxc)
  if(_rhi_dxc_found)
    set(ALAMO_DXC "${_rhi_dxc_found}")
    set(_rhi_have_dxc ON)
  endif()
endif()
if(_rhi_have_dxc)
  foreach(spec "tri_vs;vs_6_0;test_shaders" "tri_ps;ps_6_0;test_shaders" "fs_vs;vs_6_0;test_shaders"
               "fs_ps;ps_6_0;test_shaders" "discard_ps;ps_6_0;test_shaders" "bl_ps;ps_6_0;bindless" "cube_ps;ps_6_0;cube" "vol_ps;ps_6_0;volume" "cs_main;cs_6_0;compute")
    list(GET spec 0 _entry)
    list(GET spec 1 _profile)
    list(GET spec 2 _file)
    set(_src ${_rhi_shader_dir}/${_file}.hlsl)
    set(_hdr ${_rhi_shader_out}/${_entry}.h)
    add_custom_command(
      OUTPUT ${_hdr}
      COMMAND ${CMAKE_COMMAND} -E make_directory ${_rhi_shader_out}
      COMMAND "${ALAMO_DXC}" -nologo -spirv -fvk-use-dx-layout -fspv-target-env=vulkan1.3 -WX -O3 -T ${_profile} -E ${_entry}
              -Fh ${_hdr} -Vn g_${_entry} ${_src}
      DEPENDS ${_src}
      COMMENT "DXC ${_entry} -> SPIR-V"
      VERBATIM)
    list(APPEND _rhi_shader_headers ${_hdr})
  endforeach()
  add_custom_target(alamo_rhi_test_shaders DEPENDS ${_rhi_shader_headers})
else()
  message(WARNING "alamo_rhi: DXC not found; GPU render tests will skip")
endif()

add_executable(alamo_rhi_tests tests/test_rhi.cpp)
target_link_libraries(alamo_rhi_tests PRIVATE alamo_rhi)
if(_rhi_have_dxc)
  add_dependencies(alamo_rhi_tests alamo_rhi_test_shaders)
  target_include_directories(alamo_rhi_tests PRIVATE ${_rhi_shader_out})
  target_compile_definitions(alamo_rhi_tests PRIVATE ALAMO_RHI_HAVE_SHADERS=1)
endif()
# (a) CPU-only checks: always run.
add_test(NAME alamo_rhi_tests COMMAND alamo_rhi_tests cpu)
# (b) GPU checks: exit 0 with "SKIP" when there is no Vulkan 1.3 device.
add_test(NAME alamo_rhi_gpu_tests COMMAND alamo_rhi_tests gpu)
set(_rhi_gpu_env "VK_LOADER_LAYERS_DISABLE=~implicit~")  # OBS/Steam/RTSS/EOS overlays stay out of tests
if(EXISTS "${PROJECT_SOURCE_DIR}/toolchain/vvl/bin/VkLayer_khronos_validation.json")
  # Project-local Khronos validation layer: the test then requires it and fails on any message.
  list(APPEND _rhi_gpu_env "VK_LAYER_PATH=${PROJECT_SOURCE_DIR}/toolchain/vvl/bin")
endif()
set_tests_properties(alamo_rhi_gpu_tests PROPERTIES ENVIRONMENT "${_rhi_gpu_env}")

# Manual check (not a test): window cleared to a cycling colour, Esc quits. --frames N exits after N frames.
if(WIN32)
  add_executable(alamo_rhi_window_demo demo/window_demo.cpp)
  target_link_libraries(alamo_rhi_window_demo PRIVATE alamo_rhi)
endif()
