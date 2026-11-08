# Shared by bench_check.cmake and bench_scenario_check.cmake (cu9 t10).

# ${out_var} = the report's top-level `key` (string(JSON GET); a missing key is an error).
function(json_field report key out_var)
  string(JSON v GET "${report}" ${key})
  set(${out_var} "${v}" PARENT_SCOPE)
endfunction()

# The bench_*.png of `dir_a` and `dir_b` must be the same set of names, at least 12, and identical byte for byte;
# FATAL_ERROR naming `label` otherwise. ${out_var} = the sorted names (of dir_a).
function(compare_png_sets dir_a dir_b label out_var)
  file(GLOB pngs_a RELATIVE "${dir_a}" "${dir_a}/bench_*.png")
  file(GLOB pngs_b RELATIVE "${dir_b}" "${dir_b}/bench_*.png")
  list(SORT pngs_a)
  list(SORT pngs_b)
  list(LENGTH pngs_a n)
  if(NOT pngs_a STREQUAL pngs_b OR n LESS 12)
    message(FATAL_ERROR "${label}: PNG sets differ or too few (${n}): ${pngs_a} vs ${pngs_b}")
  endif()
  set(diff "")
  foreach(p IN LISTS pngs_a)
    file(SHA256 "${dir_a}/${p}" ha)
    file(SHA256 "${dir_b}/${p}" hb)
    if(NOT ha STREQUAL hb)
      list(APPEND diff ${p})
    endif()
  endforeach()
  if(diff)
    message(FATAL_ERROR "${label}: frames differ between the two runs: ${diff}")
  endif()
  set(${out_var} "${pngs_a}" PARENT_SCOPE)
endfunction()
