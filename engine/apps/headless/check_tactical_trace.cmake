# ctest driver of alamo_headless_skirmish_trace (q4hf t02, tactical_trace.hpp). -D EXE, TRACE, GOLDEN, PY (may be empty), Q2TRACE.
# 1. The synthetic skirmish (no game data) with --check-workers, --golden (its checked-in CRC golden) and --trace-out: only the
#    first run writes the trace, so the golden and equal CRCs across worker counts show the trace changes nothing in the sim.
# 2. The trace holds the battle: the slots, a first-shot record, a keyframe, destroyed objects and the end reason.
# 3. q2trace.py validate re-reads the trace, rebuilds the objects frame by frame and recomputes every digest (exit 0 = consistent).
file(REMOVE "${TRACE}")
get_filename_component(_dir "${TRACE}" DIRECTORY)
file(MAKE_DIRECTORY "${_dir}")
execute_process(COMMAND "${EXE}" --skirmish-ai synthetic --check-workers --quiet --golden "${GOLDEN}" --trace-out "${TRACE}"
                RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "alamo_headless --skirmish-ai synthetic --trace-out run failed (exit ${_rc})")
endif()
if(NOT EXISTS "${TRACE}")
  message(FATAL_ERROR "no trace written: ${TRACE}")
endif()
file(READ "${TRACE}" _t)
# The synthetic battle (1169 frames on 2026-11-08) fires, loses units (X lines) and ends (mode_changed). Hull drops show as
# O lines with a lower health; the parity harness selftest checks how they are reduced to combat.damage_taken.
foreach(_need "Q2TRACE 1\n" "\nmeta scenario skirmish\n" "\nmeta skirmish space 2 players, AI Empire vs AI Rebel\n" "\nstart "
              "\nM first_shot_frame " "\nK " "\nX " "\nT " "\nE mode_changed\nend\n")
  string(FIND "${_t}" "${_need}" _at)
  if(_at LESS 0)
    message(FATAL_ERROR "trace ${TRACE} has no '${_need}'")
  endif()
endforeach()
if(NOT PY)
  message(STATUS "no python found for q2trace.py validate: skipped")
  return()
endif()
execute_process(COMMAND "${PY}" -I "${Q2TRACE}" validate "${TRACE}" RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "q2trace.py validate failed on ${TRACE} (exit ${_rc})")
endif()
