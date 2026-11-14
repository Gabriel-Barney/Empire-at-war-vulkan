# alamo_vfs: verified Data\ resolution order (mods -> FoC loose -> MEGs -> fallback chain).
add_library(alamo_vfs STATIC
  src/vfs.cpp
  src/steam_workshop.cpp
  include/alamo/vfs/vfs.hpp
  include/alamo/vfs/steam_workshop.hpp
)
add_library(alamo::vfs ALIAS alamo_vfs)
target_include_directories(alamo_vfs PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(alamo_vfs PUBLIC alamo_core alamo_meg)
if(WIN32)
  target_link_libraries(alamo_vfs PRIVATE advapi32)  # steam_workshop.cpp: RegGetValueW (read only)
endif()

add_executable(alamo_vfs_tests tests/test_vfs.cpp)
target_link_libraries(alamo_vfs_tests PRIVATE alamo_vfs alamo_test_support)
add_test(NAME alamo_vfs_tests COMMAND alamo_vfs_tests)

# modc t10: the STEAMMOD= resolver (steam_workshop.hpp) on a synthetic Steam layout in its own temp folder.
add_executable(alamo_vfs_steam_tests tests/test_steam_workshop.cpp)
target_link_libraries(alamo_vfs_steam_tests PRIVATE alamo_vfs alamo_test_support)
add_test(NAME alamo_vfs_steam_tests COMMAND alamo_vfs_steam_tests)
