# N1 save size guard for the FoC story campaign (Full_Story_Campaign_Underworld with its story plots): unlike the HotM
# guard in test_ai_galactic.cpp this checks the whole save file, because the story's Lua VM ("story.lua") is not pristine
# and only the file's LZ packing shrinks it. Run by ctest: cmake -DHEADLESS=... -DOUT=... -DMAX_FILE=... -DMAX_RAW=...
# [-DSAVE_AT=TICK] -P (SAVE_AT default 9000 = 5 min of game time; N1f t08: a late-game entry saves at 2 h).
# Needs ALAMO_FOC_DATA; prints SKIP without it (SKIP_REGULAR_EXPRESSION).
if(NOT DEFINED ENV{ALAMO_FOC_DATA} OR "$ENV{ALAMO_FOC_DATA}" STREQUAL "")
  message(STATUS "SKIP: ALAMO_FOC_DATA is not set")
  return()
endif()
if(NOT DEFINED SAVE_AT)
  set(SAVE_AT 9000)
endif()
math(EXPR minutes "${SAVE_AT} / 1800")
file(REMOVE "${OUT}")
execute_process(COMMAND "${HEADLESS}" --save-session galactic --campaign Full_Story_Campaign_Underworld --story
                        --save-at ${SAVE_AT} "${OUT}" --ticks 0
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT EXISTS "${OUT}")
  message(FATAL_ERROR "story save failed (${rc}):\n${out}\n${err}")
endif()
if(NOT out MATCHES "chunks, ([0-9]+) bytes")
  message(FATAL_ERROR "no chunk size in the output:\n${out}")
endif()
set(raw "${CMAKE_MATCH_1}")
file(SIZE "${OUT}" size)
message(STATUS "story save at ${minutes} min: ${size} bytes in the file (guard ${MAX_FILE}), ${raw} bytes of chunks (guard ${MAX_RAW})")
if(size GREATER MAX_FILE)
  message(FATAL_ERROR "story save file larger than its guard")
endif()
if(raw GREATER MAX_RAW)
  message(FATAL_ERROR "story save chunks larger than their guard")
endif()
execute_process(COMMAND "${HEADLESS}" --load "${OUT}" --ticks 30 RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "story save did not load (${rc}):\n${out}\n${err}")
endif()
