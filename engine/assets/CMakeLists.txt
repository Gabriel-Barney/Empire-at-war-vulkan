# C1 - assets: ALO/ALA/DDS/TGA -> GPU-ready CPU data (no Vulkan). Interleaved vertex buffers in two
# fixed layouts, index buffers, bounds, material refs, bone palettes, skeletons, sampled animation,
# texture mip chains, and a thread-safe name-keyed AssetCache.
add_library(alamo_assets STATIC
  src/math.cpp
  src/model.cpp
  src/skeleton.cpp
  src/texture.cpp
  src/asset_cache.cpp
)
add_library(alamo::assets ALIAS alamo_assets)
target_include_directories(alamo_assets PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# alamo_formats stays private: public headers expose only alamo::assets types.
target_link_libraries(alamo_assets PUBLIC alamo_core PRIVATE alamo_formats)

add_executable(alamo_assets_tests tests/test_assets.cpp tests/mini_json.hpp)
# alamo_formats directly too: the real-data test counts the ALOs it parses without assets.
target_link_libraries(alamo_assets_tests PRIVATE alamo_assets alamo_formats Threads::Threads alamo_test_support)
target_compile_definitions(alamo_assets_tests PRIVATE
  "ALAMO_ASSETS_EXAMPLES_DIR=\"${CMAKE_SOURCE_DIR}/docs/formats/examples\""
  "ALAMO_ASSETS_SHADER_CATALOG=\"${CMAKE_SOURCE_DIR}/docs/shaders/catalog.json\"")
# Real-data tests (ALAMO_FOC_DATA) read models from MEGs through the VFS; only the test links it.
if(TARGET alamo_vfs)
  target_link_libraries(alamo_assets_tests PRIVATE alamo_vfs)
  target_compile_definitions(alamo_assets_tests PRIVATE ALAMO_ASSETS_HAVE_VFS=1)
endif()
add_test(NAME alamo_assets_tests COMMAND alamo_assets_tests)
