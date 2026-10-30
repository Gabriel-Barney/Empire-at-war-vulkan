# terrain - .ted maps -> GPU-ready CPU data (no Vulkan): canonical bounds-checked map reader/writer
# (environments, terrain, TEM table, passability, zones, cameras, objects), per-patch terrain meshes
# with per-material A4R4G4B4 blend passes, water plane/mesh/constants, object placements -> models,
# decorations (decals, shadow blobs, map + dynamic tracks) and the fog-of-war texture, space maps.
# Spec: docs/re/TERRAIN_WATER_POST.md 1-3, 5; docs/re/DECORATIONS.md; docs/re/SPACE_AND_OPEN.md A.
add_library(alamo_terrain STATIC
  src/chunk.cpp
  src/ted_read.cpp
  src/ted_write.cpp
  src/mesh.cpp
  src/water.cpp
  src/placement.cpp
  src/decorations.cpp
  src/decoration_xml.cpp
  src/fow.cpp
)
add_library(alamo::terrain ALIAS alamo_terrain)
target_include_directories(alamo_terrain PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(alamo_terrain PUBLIC alamo_core alamo_assets)
# Decoration XML (DynamicTrackFX.xml, ShadowBlobMaterials.xml) uses the alamo_data XML DOM when
# that module is configured; the public headers do not expose it.
if(TARGET alamo_data)
  target_link_libraries(alamo_terrain PRIVATE alamo_data)
  target_compile_definitions(alamo_terrain PRIVATE ALAMO_TERRAIN_HAVE_XML=1)
endif()

add_executable(alamo_terrain_tests tests/test_terrain.cpp)
target_link_libraries(alamo_terrain_tests PRIVATE alamo_terrain)
# Real-data tests (ALAMO_FOC_DATA): maps from maps.meg through the VFS, object types from the game
# database, patches built on the job system. Only the test links these.
if(TARGET alamo_vfs AND TARGET alamo_data AND TARGET alamo_jobs)
  target_link_libraries(alamo_terrain_tests PRIVATE alamo_vfs alamo_data alamo_jobs)
  target_compile_definitions(alamo_terrain_tests PRIVATE ALAMO_TERRAIN_HAVE_DATA=1)
endif()
add_test(NAME alamo_terrain_tests COMMAND alamo_terrain_tests)
set_tests_properties(alamo_terrain_tests PROPERTIES TIMEOUT 900)
