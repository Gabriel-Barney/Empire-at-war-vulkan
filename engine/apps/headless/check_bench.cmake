# BM1 t07: the benchmark end to end on a short synthetic scenario (no game data needed).
#   cmake -DEXE=<alamo_headless> -DWORK=<dir> -P check_bench.cmake
# 1. --bench-make: the synthetic save session (save at tick 150, 150 ticks, orders recorded, fingerprint per tick).
# 2. --bench: 1 warm-up + 2 timed runs, every run matches the fingerprint, JSON written with the required fields.
# 3. --bench-regen: remaking it gives identical files, so the version stays 1 (the make is deterministic).
# 4. --bench-baseline: a second result compares with the first (a speedup / slowdown is reported).
# 5. t04: a scenario whose fingerprint belongs to another run (seed 8) is "scenario changed, not comparable" (exit 3; 0 with
#    --bench-allow-changed), and its result does not compare with the baseline; a sidecar file edited after the manifest
#    was written is refused the same way.
cmake_minimum_required(VERSION 3.20)
foreach(v EXE WORK)
  if(NOT DEFINED ${v})
    message(FATAL_ERROR "check_bench.cmake: -D${v}= is required")
  endif()
endforeach()
file(REMOVE_RECURSE ${WORK})
file(MAKE_DIRECTORY ${WORK}/a ${WORK}/b ${WORK}/c)

function(run expect_rc out_var)
  execute_process(COMMAND ${EXE} ${ARGN} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  message(STATUS "alamo_headless ${ARGN}: exit ${rc}")
  if(NOT "${rc}" STREQUAL "${expect_rc}")
    message(FATAL_ERROR "expected exit ${expect_rc}, got ${rc}\n${out}\n${err}")
  endif()
  set(${out_var} "${out}" PARENT_SCOPE)
endfunction()
function(expect_in text what)
  string(FIND "${text}" "${what}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "expected '${what}' in:\n${text}")
  endif()
endfunction()

set(make_args --save-session synthetic --bench-save-at 150 --ticks 150 --workers 2 --quiet)
run(0 out --bench-make ${WORK}/a/syn.abench ${make_args} --bench-desc "synthetic check")
expect_in("${out}" "version 1")
file(READ ${WORK}/a/syn.abench manifest)
expect_in("${manifest}" "ALAMO_BENCH 1\nname syn\nversion 1\ndescription synthetic check\n")
expect_in("${manifest}" "fingerprint syn.crc ")
expect_in("${manifest}" "start_tick 150\nticks 150\nworkers 2\n")

run(0 out --bench ${WORK}/a/syn.abench --runs 2 --warmup 1 --bench-json ${WORK}/a.json)
expect_in("${out}" "bench: all timed runs, 300 ticks: median")
expect_in("${out}" "bench: OK")
file(READ ${WORK}/a.json json)
foreach(field "\"format\": \"alamo-bench-result\"" "\"comparable\": true" "\"name\": \"syn\"" "\"version\": 1" "\"cpu\":" "\"gpu\":"
              "\"gpu_driver\":" "\"os\":" "\"compiler\":" "\"git\":" "\"warmup_runs\": 1" "\"timed_runs\": 2" "\"median_ms\":" "\"p99_ms\":"
              "\"worst_ms\":" "\"noisy\":" "\"other_cpu_pct\":" "\"zones\": [")
  expect_in("${json}" "${field}")
endforeach()

run(0 out --bench-regen ${WORK}/a/syn.abench)
expect_in("${out}" "version 1 (unchanged)")
file(READ ${WORK}/a/syn.abench manifest2)
if(NOT manifest2 STREQUAL manifest)
  message(FATAL_ERROR "--bench-regen changed the manifest:\n${manifest}\n---\n${manifest2}")
endif()

run(0 out --bench ${WORK}/a/syn.abench --runs 1 --warmup 0 --bench-baseline ${WORK}/a.json --workers 0)
expect_in("${out}" "vs baseline a.json: median")
expect_in("${out}" "note, compared with the baseline: ")
expect_in("${out}" "other worker count")

# t04: scenario a's save and orders with the fingerprint of the same session made with seed 8.
run(0 out --bench-make ${WORK}/b/syn.abench ${make_args} --seed 8)
file(STRINGS ${WORK}/b/syn.abench b_fp REGEX "^fingerprint ")
file(STRINGS ${WORK}/a/syn.abench a_fp REGEX "^fingerprint ")
string(REPLACE "${a_fp}" "${b_fp}" mixed "${manifest}")
file(WRITE ${WORK}/c/syn.abench "${mixed}")
file(COPY ${WORK}/a/syn.alsv ${WORK}/a/syn.acmd ${WORK}/b/syn.crc DESTINATION ${WORK}/c)
run(3 out --bench ${WORK}/c/syn.abench --runs 1 --warmup 0 --bench-json ${WORK}/c.json --bench-baseline ${WORK}/a.json)
expect_in("${out}" "scenario changed, not comparable")
expect_in("${out}" "timings above are NOT comparable")
expect_in("${out}" "vs baseline a.json: scenario changed, not comparable: this run did not match")
file(READ ${WORK}/c.json cjson)
expect_in("${cjson}" "\"comparable\": false")
run(0 out --bench ${WORK}/c/syn.abench --runs 1 --warmup 0 --bench-allow-changed)
expect_in("${out}" "scenario changed, not comparable")

# A sidecar edited after the manifest was written.
file(APPEND ${WORK}/a/syn.crc "# edited\n")
run(3 out --bench ${WORK}/a/syn.abench --runs 1 --warmup 0)
expect_in("${out}" "scenario changed, not comparable: syn.crc differs from the manifest")
message(STATUS "check_bench: OK")
