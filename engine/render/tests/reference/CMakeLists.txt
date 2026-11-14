# C4b reference tests (owned by the materials agent): FF alpha/additive effects vs a CPU D3D9 reference
# and skinned draws vs the Phase 1 glTF exports. Uses only alamo_render's public API.
add_executable(alamo_render_reference_tests
  test_reference.cpp
  ff_reference.cpp
  png_decode.cpp
  gltf.cpp
)
target_link_libraries(alamo_render_reference_tests PRIVATE alamo_render alamo_render_test_support)
target_compile_definitions(alamo_render_reference_tests PRIVATE "ALAMO_RENDER_SOURCE_DIR=\"${PROJECT_SOURCE_DIR}\"")
# CPU checks always; GPU checks skip without a Vulkan 1.3 device (validation required when the
# project-local layer is present); real-data checks only with ALAMO_FOC_DATA.
alamo_shaderxlat_copy_dxc(alamo_render_reference_tests)
add_test(NAME alamo_render_reference_tests COMMAND alamo_render_reference_tests)
set_tests_properties(alamo_render_reference_tests PROPERTIES TIMEOUT 900)
