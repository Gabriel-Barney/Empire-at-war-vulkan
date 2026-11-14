add_library(alamo_data STATIC
  src/xml.cpp
  src/xml_list.cpp
  src/values.cpp
  src/game_database.cpp
)
add_library(alamo::data ALIAS alamo_data)
target_include_directories(alamo_data PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(alamo_data PUBLIC alamo_core alamo_engine_flags)

add_executable(alamo_data_tests tests/test_data.cpp)
target_link_libraries(alamo_data_tests PRIVATE alamo_data alamo_test_support)
# Real-install integration (ALAMO_FOC_DATA = game folder, XML inside MEGs) reads through the VFS,
# as alamo_headless does; only the test links it. Without vfs such a folder is skipped.
if(TARGET alamo_vfs)
  target_link_libraries(alamo_data_tests PRIVATE alamo_vfs)
  target_compile_definitions(alamo_data_tests PRIVATE ALAMO_DATA_HAVE_VFS=1)
endif()
add_test(NAME alamo_data_tests COMMAND alamo_data_tests)
