# P3 - video: Bink movie playback (docs/PHASE4.md P3).
#
# FFmpeg 8.1.3 Bink-only LGPL v2.1+ shared build (approved by Gabriel 2026-10-27) from toolchain\9_ffmpeg_bink.cmd
# into toolchain\ffmpeg-8.1.3: avcodec-62 / avformat-62 / avutil-60 DLLs + headers. Nothing links against it:
# src/ffmpeg_api.cpp loads the DLLs at run time (LoadLibrary) and takes the function pointers, so the
# MSVC and clang (MSVC ABI) builds both use the same MSVC-built DLLs, and swapping the DLLs for a
# user-built LGPL FFmpeg needs no relink. Only the headers are used at build time (SYSTEM include dir).
# Skipped when the FFmpeg tree is absent so the rest of the engine still builds.
set(ALAMO_FFMPEG_DIR "${PROJECT_SOURCE_DIR}/toolchain/ffmpeg-8.1.3" CACHE PATH "FFmpeg Bink build (toolchain\\9_ffmpeg_bink.cmd)")
if(NOT EXISTS "${ALAMO_FFMPEG_DIR}/include/libavcodec/avcodec.h")
  message(WARNING "engine/video skipped: FFmpeg not found in ${ALAMO_FFMPEG_DIR} (run toolchain\\9_ffmpeg_bink.cmd)")
  return()
endif()
if(WIN32)
  set(_alamo_ffmpeg_bin "${ALAMO_FFMPEG_DIR}/bin")
else()
  set(_alamo_ffmpeg_bin "${ALAMO_FFMPEG_DIR}/lib")
endif()
set(ALAMO_FFMPEG_RUNTIME_DIR "${_alamo_ffmpeg_bin}" CACHE INTERNAL "Folder with the FFmpeg shared libraries")

add_library(alamo_ffmpeg_headers INTERFACE)
target_include_directories(alamo_ffmpeg_headers SYSTEM INTERFACE "${ALAMO_FFMPEG_DIR}/include")
target_compile_definitions(alamo_ffmpeg_headers INTERFACE __STDC_CONSTANT_MACROS)

add_library(alamo_video STATIC
  src/ffmpeg_api.cpp
  src/bink_decoder.cpp
  src/yuv.cpp
  src/movie_player.cpp
)
add_library(alamo::video ALIAS alamo_video)
target_include_directories(alamo_video PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(alamo_video PUBLIC alamo_core alamo_engine_flags PRIVATE alamo_ffmpeg_headers ${CMAKE_DL_LIBS})
if(TARGET alamo_data)  # Movies.xml (movie_db.hpp: Alpha / Cannot_Skip / Loop / Commandbar_Offset per movie)
  target_sources(alamo_video PRIVATE src/movie_db.cpp)
  target_link_libraries(alamo_video PRIVATE alamo_data)
  target_compile_definitions(alamo_video PUBLIC ALAMO_VIDEO_HAVE_MOVIE_DB=1)
endif()
# Fallback load folder when the DLLs are not next to the executable.
target_compile_definitions(alamo_video PRIVATE "ALAMO_VIDEO_FFMPEG_DEFAULT_DIR=\"${ALAMO_FFMPEG_RUNTIME_DIR}\"")

# Copies the FFmpeg runtime (and its licence) next to `target`. The game host calls this for its exe.
function(alamo_video_copy_ffmpeg_dlls target)
  if(WIN32)
    file(GLOB _ff_dlls "${ALAMO_FFMPEG_RUNTIME_DIR}/*.dll")
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_ff_dlls} "${ALAMO_FFMPEG_DIR}/COPYING.LGPLv2.1"
              "$<TARGET_FILE_DIR:${target}>"
      VERBATIM)
  endif()
endfunction()

add_executable(alamo_video_tests tests/test_video.cpp)
target_link_libraries(alamo_video_tests PRIVATE alamo_video)
if(TARGET alamo_data)
  target_link_libraries(alamo_video_tests PRIVATE alamo_data)
endif()
if(TARGET alamo_vfs)
  target_link_libraries(alamo_video_tests PRIVATE alamo_vfs)
  target_compile_definitions(alamo_video_tests PRIVATE ALAMO_VIDEO_HAVE_VFS=1)
endif()
# The decoded sample frame (PNG) goes into the build dir.
target_compile_definitions(alamo_video_tests PRIVATE "ALAMO_VIDEO_OUT_DIR=\"${CMAKE_CURRENT_BINARY_DIR}\"")
alamo_video_copy_ffmpeg_dlls(alamo_video_tests)
add_test(NAME alamo_video_tests COMMAND alamo_video_tests)
set_tests_properties(alamo_video_tests PROPERTIES TIMEOUT 300)
