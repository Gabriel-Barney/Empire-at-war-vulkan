# Q4 parity harness (tools/parity/README.md, roadmap q4h).
#   parity_harness_selftest           synthetic fixtures, no game data (fast tier)
#   parity_harness_<scenario>         the Q2 reference runs against an alamo_headless run of the same scenario, checked
#                                     against baseline.json: fails only when a metric that passed in the baseline fails
#                                     now. Skips (exit 77) without ALAMO_FOC_DATA or the reference runs. Label slow.
#   parity_harness_<scenario>_seeds   distribution mode (parity tier 2, docs/DECISIONS.md "Parity tiers"): the port over
#                                     10 seeds against every retail run, per-metric means and spreads, checked against
#                                     baseline_seeds.json the same way. Own output folder (out_seeds). Label slow.
find_program(ALAMO_PARITY_PYTHON NAMES python PATHS ${CMAKE_SOURCE_DIR}/toolchain/venv/Scripts NO_DEFAULT_PATH)
find_program(ALAMO_PARITY_PYTHON NAMES python python3)
if(NOT ALAMO_PARITY_PYTHON)
  return()
endif()
set(_parity ${CMAKE_CURRENT_SOURCE_DIR}/parity_harness.py)
add_test(NAME parity_harness_selftest COMMAND ${ALAMO_PARITY_PYTHON} -I ${_parity} selftest)
set_tests_properties(parity_harness_selftest PROPERTIES TIMEOUT 60)
# bkc1 t08 (backlog b0027): the galactic auto-resolve against the retail battles of autoresolve_refs.json, over 200 seeds
# (autoresolve_check.py; its selftest needs no data). Skips (77) without ALAMO_FOC_DATA.
add_test(NAME parity_autoresolve_selftest COMMAND ${ALAMO_PARITY_PYTHON} -I ${CMAKE_CURRENT_SOURCE_DIR}/autoresolve_check.py selftest)
set_tests_properties(parity_autoresolve_selftest PROPERTIES TIMEOUT 60)
if(TARGET alamo_galactic_autoresolve_parity)
  add_test(NAME parity_autoresolve
           COMMAND ${ALAMO_PARITY_PYTHON} -I ${CMAKE_CURRENT_SOURCE_DIR}/autoresolve_check.py run
                   --exe $<TARGET_FILE:alamo_galactic_autoresolve_parity>
                   --refs ${CMAKE_CURRENT_SOURCE_DIR}/autoresolve_refs.json --out ${CMAKE_CURRENT_BINARY_DIR}/out_autoresolve)
  set_tests_properties(parity_autoresolve PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77)
endif()
if(TARGET alamo_headless)
  foreach(_s hotm_galactic geonosis_skirmish)
    add_test(NAME parity_harness_${_s}
             COMMAND ${ALAMO_PARITY_PYTHON} -I ${_parity} run --scenario ${_s} --port-exe $<TARGET_FILE:alamo_headless>
                     --out ${CMAKE_CURRENT_BINARY_DIR}/out --check-baseline ${CMAKE_CURRENT_SOURCE_DIR}/baseline.json)
    set_tests_properties(parity_harness_${_s} PROPERTIES TIMEOUT 600 SKIP_RETURN_CODE 77 LABELS slow)
    add_test(NAME parity_harness_${_s}_seeds
             COMMAND ${ALAMO_PARITY_PYTHON} -I ${_parity} run --scenario ${_s} --port-exe $<TARGET_FILE:alamo_headless>
                     --seeds 10 --jobs 4 --out ${CMAKE_CURRENT_BINARY_DIR}/out_seeds
                     --check-baseline ${CMAKE_CURRENT_SOURCE_DIR}/baseline_seeds.json)
    set_tests_properties(parity_harness_${_s}_seeds PROPERTIES TIMEOUT 1200 SKIP_RETURN_CODE 77 LABELS slow)
  endforeach()
endif()
