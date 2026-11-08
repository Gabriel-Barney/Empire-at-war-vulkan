# shaderxlat - PORTING.md track B: D3D9 SM1-2 shader bytecode -> SM6 HLSL -> SPIR-V.
# The library depends on alamo_core only. DXC is used at run time through dxcompiler.dll /
# libdxcompiler.so loaded dynamically (approved by Gabriel 2026-10-26); nothing links against it.
# Tests additionally use alamo_fx2 (FXO parsing), alamo_vfs (real-install corpus, ALAMO_FOC_DATA /
# ALAMO_EAW_DATA) and alamo_rhi (GPU differential test); the library does not.

# One source list for the library and its bounds-checked test copy (alamo_shaderxlat_checked below).
set(_xlat_sources
  src/bytecode.cpp
  src/translate.cpp
  src/interpreter.cpp
  src/dxc.cpp
  src/cache.cpp
  src/shaderxlat.cpp
)
add_library(alamo_shaderxlat STATIC ${_xlat_sources})
add_library(alamo::shaderxlat ALIAS alamo_shaderxlat)
target_include_directories(alamo_shaderxlat PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(alamo_shaderxlat PUBLIC alamo_core)

# dxcompiler.dll + dxil.dll next to an executable (runtime DXC).
function(alamo_shaderxlat_copy_dxc target)
  if(WIN32 AND DEFINED ALAMO_DXC_RUNTIME_DIR AND EXISTS "${ALAMO_DXC_RUNTIME_DIR}/dxcompiler.dll")
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ALAMO_DXC_RUNTIME_DIR}/dxcompiler.dll"
              "${ALAMO_DXC_RUNTIME_DIR}/dxil.dll" "$<TARGET_FILE_DIR:${target}>"
      VERBATIM)
  endif()
endfunction()

# ---------------------------------------------------------------- offline tool
# alamo_shaderxlat_tool --fxo <file.fxo>... | --game <dir> [--eaw <dir>]  --out <dir> [--dxc <dxc.exe>] [--cache <dir>]
add_executable(alamo_shaderxlat_tool tools/xlat_tool.cpp)
target_link_libraries(alamo_shaderxlat_tool PRIVATE alamo_shaderxlat alamo_fx2)
if(TARGET alamo_vfs)
  target_link_libraries(alamo_shaderxlat_tool PRIVATE alamo_vfs)
  target_compile_definitions(alamo_shaderxlat_tool PRIVATE ALAMO_SHADERXLAT_HAVE_VFS=1)
endif()
target_compile_definitions(alamo_shaderxlat_tool PRIVATE "ALAMO_SHADERXLAT_DXC_EXE=\"${ALAMO_DXC}\"")
alamo_shaderxlat_copy_dxc(alamo_shaderxlat_tool)

# Optional build-time batch translation of the installed game (not part of ALL):
#   cmake --build <dir> --target alamo_shaderxlat_prebuild   (needs ALAMO_FOC_DATA at configure time)
if(DEFINED ENV{ALAMO_FOC_DATA})
  add_custom_target(alamo_shaderxlat_prebuild
    COMMAND alamo_shaderxlat_tool --game "$ENV{ALAMO_FOC_DATA}" --out "${CMAKE_BINARY_DIR}/shaderxlat_prebuilt"
            --dxc "${ALAMO_DXC}"
    DEPENDS alamo_shaderxlat_tool
    COMMENT "shaderxlat: translating the installed game's FXO shaders"
    VERBATIM)
endif()

# ---------------------------------------------------------------- tests
# The tests (fuzz loop included) link a bounds-checked copy of the library: standard-library
# hardening (MSVC STL _MSVC_STL_HARDENING, libstdc++ _GLIBCXX_ASSERTIONS; both ABI-compatible, so it
# links against the unhardened core/fx2/vfs/rhi) turns any out-of-range std::array / vector / span /
# string_view index in the parser, emitter or interpreter into an immediate abort.
set(_xlat_checked_defs _MSVC_STL_HARDENING=1 _GLIBCXX_ASSERTIONS=1)
add_library(alamo_shaderxlat_checked STATIC EXCLUDE_FROM_ALL ${_xlat_sources})
target_include_directories(alamo_shaderxlat_checked PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(alamo_shaderxlat_checked PUBLIC alamo_core)
target_compile_definitions(alamo_shaderxlat_checked PUBLIC ${_xlat_checked_defs})

add_executable(alamo_shaderxlat_tests tests/test_shaderxlat.cpp tests/mini_asm.cpp tests/corpus.cpp tests/gpu_diff.cpp)
target_link_libraries(alamo_shaderxlat_tests PRIVATE alamo_shaderxlat_checked alamo_fx2 alamo_test_support)
target_include_directories(alamo_shaderxlat_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_compile_definitions(alamo_shaderxlat_tests PRIVATE
  "ALAMO_SHADERXLAT_GOLDEN_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/golden\"")
if(TARGET alamo_vfs AND TARGET alamo_meg)
  target_link_libraries(alamo_shaderxlat_tests PRIVATE alamo_vfs alamo_meg)
  target_compile_definitions(alamo_shaderxlat_tests PRIVATE ALAMO_SHADERXLAT_HAVE_VFS=1)
endif()
if(TARGET alamo_rhi)
  target_link_libraries(alamo_shaderxlat_tests PRIVATE alamo_rhi)
  target_compile_definitions(alamo_shaderxlat_tests PRIVATE ALAMO_SHADERXLAT_HAVE_RHI=1)
endif()
alamo_shaderxlat_copy_dxc(alamo_shaderxlat_tests)

# (a) CPU: parser, emitter goldens, fuzz, determinism, cache, runtime DXC, corpus (ALAMO_FOC_DATA).
add_test(NAME alamo_shaderxlat_tests COMMAND alamo_shaderxlat_tests cpu)
# (b) GPU differential test vs the CPU interpreter; prints SKIP without a Vulkan 1.3 device.
add_test(NAME alamo_shaderxlat_gpu_tests COMMAND alamo_shaderxlat_tests gpu)
set(_xlat_gpu_env "VK_LOADER_LAYERS_DISABLE=~implicit~")
if(EXISTS "${PROJECT_SOURCE_DIR}/toolchain/vvl/bin/VkLayer_khronos_validation.json")
  # Project-local Khronos validation layer: the test then requires it and fails on any message.
  list(APPEND _xlat_gpu_env "VK_LAYER_PATH=${PROJECT_SOURCE_DIR}/toolchain/vvl/bin")
endif()
set_tests_properties(alamo_shaderxlat_gpu_tests PROPERTIES ENVIRONMENT "${_xlat_gpu_env}")
