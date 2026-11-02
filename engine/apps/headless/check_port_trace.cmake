# ctest driver of alamo_headless_galactic_trace (q2f t05, port_trace.hpp). -D EXE, TRACE, PY (may be empty), Q2TRACE.
# 1. A short HotM galactic-AI run (with the story plots, --story, q2f t03) with --check-workers and --trace-out: only the first run writes the trace, so equal CRCs
#    and AI logs across worker counts show the trace changes nothing in the sim.
# 2. q2trace.py validate re-reads the trace, rebuilds the objects frame by frame and recomputes every digest (exit 0 = consistent).
file(REMOVE "${TRACE}")
get_filename_component(_dir "${TRACE}" DIRECTORY)
file(MAKE_DIRECTORY "${_dir}")
execute_process(COMMAND "${EXE}" --economy --galactic-ai --story --ticks 300 --check-workers --quiet --trace-out "${TRACE}" RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "alamo_headless --trace-out run failed (exit ${_rc})")
endif()
if(NOT EXISTS "${TRACE}")
  message(STATUS "no trace written (ALAMO_FOC_DATA unset): skipped")
  return()
endif()
if(NOT PY)
  message(STATUS "no python found for q2trace.py validate: skipped")
  return()
endif()
execute_process(COMMAND "${PY}" -I "${Q2TRACE}" validate "${TRACE}" RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "q2trace.py validate failed on ${TRACE} (exit ${_rc})")
endif()
