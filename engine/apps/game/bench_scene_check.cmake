# bm3: a committed rendered benchmark scene (tests/benchmark/<name>.abench, host game) runs and its fingerprint holds.
#   cmake -DGAME=<alamo_game> -DSCENE=<.abench> -DOUT=<dir> [-DTICKS=120] -P bench_scene_check.cmake
# One short hidden alamo_game --bench-scenario run (the scenario's own mode / preset / camera, TICKS ticks instead of its full length; the
# fingerprint covers those ticks): exit 0, the report says result ok, fingerprint match, the scenario's file name, load_ms and a hitch
# summary, and the GPU timer scopes when the queue has timestamps. A scene whose save/fingerprint no longer matches the sim exits 3
# ("scenario changed, not comparable"): remake it with tools\goldens\regen_goldens.py / alamo_game --bench-regen (docs/PHASE4.md, BM3).
# Skipped (exit 0) without the game data (ALAMO_FOC_DATA) or the scene file.
if(NOT EXISTS "${SCENE}")
  message(STATUS "bench scene: ${SCENE} missing: skipped")
  return()
endif()
if(NOT DEFINED ENV{ALAMO_FOC_DATA} OR "$ENV{ALAMO_FOC_DATA}" STREQUAL "")
  message(STATUS "bench scene: ALAMO_FOC_DATA not set: skipped")
  return()
endif()
if(NOT DEFINED TICKS)
  set(TICKS 120)
endif()
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
execute_process(COMMAND "${GAME}" --hidden --bench-scenario "${SCENE}" --bench-ticks ${TICKS} --bench-report "${OUT}/report.json"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
file(WRITE "${OUT}/stdout.txt" "${out}")
file(WRITE "${OUT}/stderr.txt" "${err}")
if(rc EQUAL 3)
  message(FATAL_ERROR "bench scene ${SCENE}: scenario changed, not comparable (exit 3): remake it (alamo_game --hidden --bench-regen)\n${out}")
elseif(NOT rc EQUAL 0)
  message(FATAL_ERROR "bench scene ${SCENE}: exit ${rc}\n${out}\n${err}")
endif()
file(READ "${OUT}/report.json" report)
get_filename_component(scene_file "${SCENE}" NAME)
foreach(want "\"result\": \"ok\"" "\"fingerprint\": \"match\"" "${scene_file}" "\"load_ms\":" "\"hitches\":" "\"ticks\": ${TICKS}," "\"presentation_check\": \"match\"")
  string(FIND "${report}" "${want}" at)
  if(at EQUAL -1)
    message(FATAL_ERROR "bench scene ${SCENE}: the report lacks ${want}:\n${report}")
  endif()
endforeach()
string(FIND "${out}" "validation: on, 0 errors" at)
if(at EQUAL -1)
  message(STATUS "bench scene ${scene_file}: no validation summary (a window-less run?); the report checks passed")
endif()
message(STATUS "bench scene ${scene_file}: ${TICKS} ticks, fingerprint match")
