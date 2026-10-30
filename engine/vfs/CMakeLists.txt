# alamo_vfs: verified Data\ resolution order (mods -> FoC loose -> MEGs -> fallback chain).
add_library(alamo_vfs STATIC
  src/vfs.cpp
  include/alamo/vfs/vfs.hpp
)
add_library(alamo::vfs ALIAS alamo_vfs)
target_include_directories(alamo_vfs PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(alamo_vfs PUBLIC alamo_core alamo_meg)

add_executable(alamo_vfs_tests tests/test_vfs.cpp)
target_link_libraries(alamo_vfs_tests PRIVATE alamo_vfs)
add_test(NAME alamo_vfs_tests COMMAND alamo_vfs_tests)
