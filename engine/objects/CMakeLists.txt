# S1 - game object model: GameObjectType database (every XML element kept, schema from
# docs/re/xml_field_tables.md, field coverage report), typed/merged type view (OBJECT_TYPES.md),
# entity ids + SoA object store (OBJECT_RUNTIME.md).
add_library(alamo_objects STATIC
  src/field_schema.cpp
  src/object_types.cpp
  src/coverage.cpp
  src/behaviors.cpp
  src/type_fields.cpp
  src/enums.cpp
  src/game_object_types.cpp
  src/object_store.cpp
)
add_library(alamo::objects ALIAS alamo_objects)
target_include_directories(alamo_objects PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(alamo_objects PUBLIC alamo_data alamo_core alamo_engine_flags)

add_executable(alamo_objects_tests tests/test_objects.cpp)
target_link_libraries(alamo_objects_tests PRIVATE alamo_objects Threads::Threads)
# Coverage reports for real data go to <build dir>/logs/objects_coverage_<game>.txt (per build tree, so
# two builds' ctests running at once never share the file; the test prints the path).
target_compile_definitions(alamo_objects_tests PRIVATE "ALAMO_OBJECTS_LOG_DIR=\"${CMAKE_BINARY_DIR}/logs\"")
# Real installs keep their XML in MEGs: mount them through the VFS, as alamo_headless does.
if(TARGET alamo_vfs)
  target_link_libraries(alamo_objects_tests PRIVATE alamo_vfs)
  target_compile_definitions(alamo_objects_tests PRIVATE ALAMO_OBJECTS_HAVE_VFS=1)
endif()
# The ModelAnimType converter is injected (GameObjectTypes::build's AnimTypeLookup): alamo_anim links
# alamo_objects, so only the TEST links alamo_anim, to check the real table. alamo_anim is configured
# after this folder, hence the generator expressions.
target_link_libraries(alamo_objects_tests PRIVATE $<TARGET_NAME_IF_EXISTS:alamo_anim>)
target_compile_definitions(alamo_objects_tests PRIVATE $<$<TARGET_EXISTS:alamo_anim>:ALAMO_OBJECTS_HAVE_ANIM=1>)
add_test(NAME alamo_objects_tests COMMAND alamo_objects_tests)
