# bm2: alamo_game scenario runs (ctest alamo_game_bench_scenario; bm2c t03: alamo_game_bench_scenario_taa with -DTAA=1).
#   cmake -DGAME=<alamo_game> -DSAVE=<.alsv> -DRECIPE=<recipe .abench> -DOUT=<dir> [-DTAA=1] -P bench_scenario_check.cmake
# 1. make:  --bench-make on a copy of the recipe from the battle save: the run passes and writes a `host game` scenario
#           (save, orders, fingerprint pinned; the recipe's camera, mode, preset and frames per tick kept).
# 2. a, b:  two --bench-scenario runs of it (PNG every 10th counted frame): both pass, the fingerprint matches, the GPU
#           timer scopes are reported (when the queue has timestamps), the PNGs of the two runs are identical, and the
#           camera moved (the first and last PNG differ).
# 3. pins:  a scenario whose fingerprint file changed is refused before anything loads.
# -DTAA=1 (bm2c t03): the recipe is on bm-high (TAA Medium); make and runs at 640x360, run b with --bench-redraw-every 3 (a
#           third of its settled frames drawn again and taken back out of the TAA history, as alamo_game_bench_fixed_taa):
#           both reports say "taa": true, run b reports the forced redraws and TAA discards, and the PNGs still match run a's.
# The scenario files are named after the recipe's `name` (battle_orbit, battle_orbit_taa).
# Without the save (no ALAMO_FOC_DATA: the saveload smoke skipped) the check is skipped.
if(NOT EXISTS "${SAVE}")
  message(STATUS "alamo_game bench scenario: ${SAVE} missing (saveload smoke skipped?): skipped")
  return()
endif()
include("${CMAKE_CURRENT_LIST_DIR}/bench_common.cmake")  # json_field, compare_png_sets

file(STRINGS "${RECIPE}" name_line REGEX "^name ")
string(REPLACE "name " "" name "${name_line}")
set(scen "${OUT}/scenario")
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${scen}")
file(COPY_FILE "${RECIPE}" "${scen}/${name}.abench")
set(size_args "")
set(preset bm-smoke)
if(TAA)
  set(size_args --width 640 --height 360)
  set(preset bm-high)
endif()

function(game_run tag out_var)
  set(dir "${OUT}/${tag}")
  file(MAKE_DIRECTORY "${dir}")
  execute_process(COMMAND "${GAME}" --hidden ${ARGN} --bench-report "${dir}/report.json" RESULT_VARIABLE rc OUTPUT_VARIABLE out
                  ERROR_VARIABLE err)
  file(WRITE "${dir}/stdout.txt" "${out}")
  file(WRITE "${dir}/stderr.txt" "${err}")
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "alamo_game bench scenario ${tag}: exit ${rc}\n${out}\n${err}")
  endif()
  file(READ "${dir}/report.json" report)
  set(${out_var} "${report}" PARENT_SCOPE)
endfunction()

# 1. make
game_run(make rm --bench-make "${scen}/${name}.abench" --bench-save "${SAVE}" ${size_args})
json_field("${rm}" result res_m)
json_field("${rm}" scenario_made made)
json_field("${rm}" presentation_check check_m)
if(NOT res_m STREQUAL "ok" OR NOT check_m STREQUAL "match" OR made STREQUAL "")
  message(FATAL_ERROR "alamo_game bench scenario make: result ${res_m}, presentation check ${check_m}, made '${made}'\n${rm}")
endif()
file(READ "${scen}/${name}.abench" manifest)
foreach(line "ALAMO_BENCH 2" "name ${name}" "version 1" "save ${name}.alsv " "orders ${name}.acmd "
             "fingerprint ${name}.crc " "ticks 60" "host game" "mode fixed" "preset ${preset}" "frames_per_tick 2"
             "camera_anchor start" "camera 0 0 0 0 0 35 900" "camera 60 0 0 0 180 20 600 60")
  string(FIND "${manifest}" "${line}" at)
  if(at LESS 0)
    message(FATAL_ERROR "alamo_game bench scenario make: the manifest has no '${line}'\n${manifest}")
  endif()
endforeach()

# 2. two runs of the scenario
foreach(tag a b)
  set(redraw "")
  if(TAA AND tag STREQUAL "b")
    set(redraw --bench-redraw-every 3)
  endif()
  game_run(${tag} r${tag} --bench-scenario "${scen}/${name}.abench" --bench-png-dir "${OUT}/${tag}" --bench-png-every 10 ${size_args} ${redraw})
  json_field("${r${tag}}" result res)
  json_field("${r${tag}}" fingerprint fp)
  json_field("${r${tag}}" frames frames)
  json_field("${r${tag}}" camera_keys keys)
  json_field("${r${tag}}" scenario_version ver)
  json_field("${r${tag}}" gpu_timestamps gpu_on)
  json_field("${r${tag}}" gpu_frames gpu_frames)
  if(NOT res STREQUAL "ok" OR NOT fp STREQUAL "match" OR NOT frames EQUAL 120 OR NOT keys EQUAL 2 OR NOT ver EQUAL 1)
    message(FATAL_ERROR "alamo_game bench scenario run ${tag}: result ${res}, fingerprint ${fp}, ${frames} frames, ${keys} keys, version ${ver}\n${r${tag}}")
  endif()
  if(gpu_on STREQUAL "ON")
    string(JSON gpu_count GET "${r${tag}}" gpu_ms frame count)
    string(JSON gpu_median GET "${r${tag}}" gpu_ms frame median)
    if(gpu_frames LESS 100 OR NOT gpu_count EQUAL gpu_frames OR gpu_median LESS_EQUAL 0)
      message(FATAL_ERROR "alamo_game bench scenario run ${tag}: GPU timers ${gpu_frames} frames, frame scope ${gpu_count} / median ${gpu_median}\n${r${tag}}")
    endif()
    set(gpu_note "GPU frame median ${gpu_median} ms over ${gpu_frames} frames")
  else()
    set(gpu_note "no GPU timestamps")
  endif()
  json_field("${r${tag}}" taa taa)
  if((TAA AND NOT taa STREQUAL "ON") OR (NOT TAA AND NOT taa STREQUAL "OFF"))
    message(FATAL_ERROR "alamo_game bench scenario run ${tag}: report taa ${taa} with preset ${preset}\n${r${tag}}")
  endif()
endforeach()
set(taa_note "")
if(TAA)
  # 120 counted frames, every 3rd settled one forced: about 60 redraws, each taken back out of the TAA history.
  json_field("${rb}" forced_redraws forced_b)
  json_field("${rb}" taa_discarded discarded_b)
  if(forced_b LESS 50 OR discarded_b LESS forced_b)
    message(FATAL_ERROR "alamo_game bench scenario TAA: run b forced ${forced_b} redraws, ${discarded_b} TAA discards\n${rb}")
  endif()
  set(taa_note ", TAA on, run b forced ${forced_b} redraws / ${discarded_b} TAA discards")
endif()
compare_png_sets("${OUT}/a" "${OUT}/b" "alamo_game bench scenario" pngs_a)
list(LENGTH pngs_a n)
list(GET pngs_a 0 first)
list(GET pngs_a -1 last)
file(SHA256 "${OUT}/a/${first}" h_first)
file(SHA256 "${OUT}/a/${last}" h_last)
if(h_first STREQUAL h_last)
  message(FATAL_ERROR "alamo_game bench scenario: ${first} and ${last} are the same image: the camera path did not move")
endif()

# 3. a changed pinned file is refused before the run
file(APPEND "${scen}/${name}.crc" "# changed\n")
execute_process(COMMAND "${GAME}" --hidden --bench-scenario "${scen}/${name}.abench" RESULT_VARIABLE rc OUTPUT_VARIABLE out
                ERROR_VARIABLE err)
string(FIND "${err}" "not comparable" at)
if(rc EQUAL 0 OR at LESS 0)
  message(FATAL_ERROR "alamo_game bench scenario: a changed fingerprint file was not refused (exit ${rc})\n${out}\n${err}")
endif()
message(STATUS "alamo_game bench scenario: OK (made version 1, two runs: fingerprint match, ${n} identical PNGs, camera moved, ${gpu_note}${taa_note})")
