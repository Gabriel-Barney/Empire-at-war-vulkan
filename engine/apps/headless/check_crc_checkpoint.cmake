# Golden checkpoints (ALAMO_CRC 2) end to end (no game data needed). The full log comes from a fresh replay run (not from a
# checked-in golden, which may be a checkpoint log by now):
#   cmake -DEXE=<alamo_headless> -DSTREAM=<replay_stream.acmd> -DCOMPARE_SMOKE=<compare_smoke.cmake>
#         -DWORK=<scratch dir> [-DPY=<python> -DCONVERT=<tools/goldens/convert_goldens.py>] -P check_crc_checkpoint.cmake
# 0. --crc-out writes the full v1 log by default.
# 1. alamo_headless --crc-convert (the C++ writer) makes the checkpoint log; tools/goldens/convert_goldens.py (the Python
#    writer, with its own --check proof) makes it again; both files must be byte-identical (with and without a keep line).
# 2. --compare says MATCH for the checkpoint log against the full log, both ways round.
# 3. A change between two checkpoints is not seen; one at a checkpoint is, with the window of ticks and the way to the exact tick.
# 4. compare_smoke.cmake (the game smoke golden check) accepts a checkpoint golden and still takes a full one, and fails on a
#    change at a checkpoint.
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}/py64" "${WORK}/pykeep")
set(GOLDEN "${WORK}/full.crc")  # the full v1 log every other step starts from

function(_run out_rc out_text)
  execute_process(COMMAND ${ARGN} RESULT_VARIABLE rc OUTPUT_VARIABLE so ERROR_VARIABLE se)
  set(${out_rc} ${rc} PARENT_SCOPE)
  set(${out_text} "${so}${se}" PARENT_SCOPE)
endfunction()
function(_expect_ok what rc text)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "crc checkpoint: ${what} failed (exit ${rc}):\n${text}")
  endif()
endfunction()
# _expect(<what> <text shown on failure> <if() condition...>): fails unless the condition holds.
function(_expect what text)
  if(NOT (${ARGN}))
    message(FATAL_ERROR "crc checkpoint: ${what}\n${text}")
  endif()
endfunction()

# 0. The full log: what --crc-out writes by default is ALAMO_CRC 1, one line per tick.
_run(rc t "${EXE}" --replay "${STREAM}" --workers 0 --crc-out "${GOLDEN}")
_expect_ok("the replay run" ${rc} "${t}")
file(STRINGS "${GOLDEN}" first_line LIMIT_COUNT 1)
_expect("--crc-out must write ALAMO_CRC 1 by default, got '${first_line}'" "" first_line STREQUAL "ALAMO_CRC 1")

# 1. The two writers.
_run(rc t "${EXE}" --crc-convert "${GOLDEN}" "${WORK}/cpp64.crc" --crc-checkpoint 64)
_expect_ok("--crc-convert (64)" ${rc} "${t}")
_run(rc t "${EXE}" --crc-convert "${GOLDEN}" "${WORK}/cppkeep.crc" --crc-checkpoint 64 --crc-keep 100,333)
_expect_ok("--crc-convert (64, keep)" ${rc} "${t}")
file(READ "${WORK}/cpp64.crc" cpp64)
string(FIND "${cpp64}" "ALAMO_CRC 2\nsystems " pos)
_expect("the checkpoint file does not start with the v2 header" "${cpp64}" pos EQUAL 0)
string(REGEX MATCHALL "\n[0-9]+ [0-9a-f]+ " rows "${cpp64}")
list(LENGTH rows nrows)
_expect("expected a handful of stored records (every 64th tick, the first and the last), got ${nrows}" "" nrows GREATER 4 AND nrows LESS 40)
file(READ "${WORK}/cppkeep.crc" cppkeep)
string(FIND "${cppkeep}" "\nkeep 100 333\n" pos)
_expect("no keep line" "${cppkeep}" NOT pos EQUAL -1)
if(PY AND CONVERT)
  _run(rc t "${PY}" "${CONVERT}" --out-dir "${WORK}/py64" --check --checkpoint 64 "${GOLDEN}")
  _expect_ok("convert_goldens.py (64)" ${rc} "${t}")
  _run(rc t "${PY}" "${CONVERT}" --out-dir "${WORK}/pykeep" --check --checkpoint 64 --keep 100,333 "${GOLDEN}")
  _expect_ok("convert_goldens.py (64, keep)" ${rc} "${t}")
  get_filename_component(_name "${GOLDEN}" NAME)
  execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files "${WORK}/cpp64.crc" "${WORK}/py64/${_name}" RESULT_VARIABLE rc)
  _expect("the C++ and the Python writer differ (64)" "" rc EQUAL 0)
  execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files "${WORK}/cppkeep.crc" "${WORK}/pykeep/${_name}" RESULT_VARIABLE rc)
  _expect("the C++ and the Python writer differ (64, keep)" "" rc EQUAL 0)
  _run(rc t "${PY}" "${CONVERT}" --verify "${WORK}/cpp64.crc" "${GOLDEN}")
  _expect_ok("convert_goldens.py --verify" ${rc} "${t}")
else()
  message(STATUS "crc checkpoint: no python: the Python writer is not compared")
endif()

# 2. v2 against the full log, both ways round.
_run(rc t "${EXE}" --compare "${WORK}/cpp64.crc" "${GOLDEN}")
_expect_ok("--compare v2 full" ${rc} "${t}")
_expect("--compare v2 full: no MATCH" "${t}" t MATCHES "MATCH: identical on ${nrows} checkpoints")
_run(rc t "${EXE}" --compare "${GOLDEN}" "${WORK}/cpp64.crc")
_expect_ok("--compare full v2" ${rc} "${t}")

# 3. A tampered full log: tick 130 is between checkpoints (not seen), tick 128 is a checkpoint (seen).
file(READ "${GOLDEN}" full)
string(REGEX REPLACE "\n130 [0-9a-f]+ " "\n130 ffffffffffffffff " off_cp "${full}")
_expect("tampering tick 130 changed nothing" "" NOT off_cp STREQUAL full)
file(WRITE "${WORK}/off_checkpoint.crc" "${off_cp}")
_run(rc t "${EXE}" --compare "${WORK}/cpp64.crc" "${WORK}/off_checkpoint.crc")
_expect_ok("a change between checkpoints must not be seen" ${rc} "${t}")
string(REGEX REPLACE "\n128 [0-9a-f]+ " "\n128 ffffffffffffffff " on_cp "${full}")
_expect("tampering tick 128 changed nothing" "" NOT on_cp STREQUAL full)
file(WRITE "${WORK}/on_checkpoint.crc" "${on_cp}")
_run(rc t "${EXE}" --compare "${WORK}/cpp64.crc" "${WORK}/on_checkpoint.crc")
_expect("a change at checkpoint 128 was not reported (exit ${rc})" "${t}" NOT rc EQUAL 0)
foreach(needle "DIVERGED" "checkpoint tick 128" "65..128" "checkpoint 64 matched" "combined" "--crc-out full.crc")
  string(FIND "${t}" "${needle}" pos)
  _expect("the mismatch message lacks '${needle}'" "${t}" NOT pos EQUAL -1)
endforeach()
_run(rc t "${EXE}" --compare "${WORK}/cppkeep.crc" "${WORK}/on_checkpoint.crc")  # the kept tick 100 narrows the window
string(FIND "${t}" "101..128" pos)
_expect("the keep line did not narrow the window" "${t}" NOT pos EQUAL -1)

# 4. The game smoke golden check.
file(COPY_FILE "${GOLDEN}" "${WORK}/actual.crc")
file(COPY_FILE "${STREAM}" "${WORK}/actual.rec")
file(COPY_FILE "${WORK}/cpp64.crc" "${WORK}/golden_v2.crc")
file(COPY_FILE "${STREAM}" "${WORK}/golden_v2.rec")
file(COPY_FILE "${GOLDEN}" "${WORK}/golden_v1.crc")
file(COPY_FILE "${STREAM}" "${WORK}/golden_v1.rec")
_run(rc t ${CMAKE_COMMAND} -DACTUAL=${WORK}/actual -DGOLDEN=${WORK}/golden_v2 -DHEADLESS=${EXE} -P "${COMPARE_SMOKE}")
_expect_ok("compare_smoke.cmake with a checkpoint golden" ${rc} "${t}")
_run(rc t ${CMAKE_COMMAND} -DACTUAL=${WORK}/actual -DGOLDEN=${WORK}/golden_v1 -P "${COMPARE_SMOKE}")
_expect_ok("compare_smoke.cmake with a full golden" ${rc} "${t}")
_run(rc t ${CMAKE_COMMAND} -DACTUAL=${WORK}/actual -DGOLDEN=${WORK}/golden_v2 -P "${COMPARE_SMOKE}")
_expect("a checkpoint golden without -DHEADLESS must fail" "${t}" NOT rc EQUAL 0)
file(COPY_FILE "${WORK}/on_checkpoint.crc" "${WORK}/tampered.crc")
file(COPY_FILE "${STREAM}" "${WORK}/tampered.rec")
_run(rc t ${CMAKE_COMMAND} -DACTUAL=${WORK}/tampered -DGOLDEN=${WORK}/golden_v2 -DHEADLESS=${EXE} -P "${COMPARE_SMOKE}")
_expect("compare_smoke.cmake accepted a changed checkpoint" "${t}" NOT rc EQUAL 0)
_run(rc t ${CMAKE_COMMAND} -DACTUAL=${WORK}/tampered -DGOLDEN=${WORK}/golden_v1 -P "${COMPARE_SMOKE}")
_expect("compare_smoke.cmake accepted a changed full golden line" "${t}" NOT rc EQUAL 0)
message(STATUS "crc checkpoint: OK")
