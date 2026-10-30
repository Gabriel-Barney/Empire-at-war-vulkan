# Q1 lint: sim modules must not call CRT transcendentals (results differ between MSVC and
# clang runtimes). Use alamo/core/dmath.hpp (alamo::dmath::sin, ...) instead. sqrt, floor, ceil,
# trunc, round, fmod, fabs are exact and allowed.
#
#   cmake -DENGINE_DIR=<repo>/engine -P tools/lint/check_sim_math.cmake
#
# Scans <module>/src and <module>/include of the sim modules (tests, benches and tools are not
# sim code). Flags: std::f( / ::f( / C float+long variants (sinf, powl, ...) / unqualified f(
# after an operator, '(' ',' or 'return'. Qualified engine calls (dmath::sin) pass. Line comments
# are stripped first. Allowlist: core/src/dmath.cpp (the implementation itself).
if(NOT ENGINE_DIR)
  message(FATAL_ERROR "pass -DENGINE_DIR=<repo>/engine")
endif()

set(SIM_MODULES core jobs data galactic objects anim commands tactical ai lua apps/sim)
set(ALLOW "core/src/dmath.cpp")
set(FN "(sin|cos|tan|asin|acos|atan|atan2|sinh|cosh|tanh|asinh|acosh|atanh|exp|exp2|expm1|log|log2|log10|log1p|logb|pow|hypot|cbrt|erf|erfc|tgamma|lgamma)")

set(violations 0)
set(scanned 0)
foreach(mod ${SIM_MODULES})
  if(NOT EXISTS "${ENGINE_DIR}/${mod}")
    continue()
  endif()
  file(GLOB_RECURSE files
       "${ENGINE_DIR}/${mod}/src/*.cpp" "${ENGINE_DIR}/${mod}/src/*.hpp" "${ENGINE_DIR}/${mod}/src/*.h"
       "${ENGINE_DIR}/${mod}/include/*.hpp" "${ENGINE_DIR}/${mod}/include/*.h"
       # Sub-libraries such as tactical/movement and tactical/combat.
       "${ENGINE_DIR}/${mod}/*/src/*.cpp" "${ENGINE_DIR}/${mod}/*/src/*.hpp" "${ENGINE_DIR}/${mod}/*/src/*.h"
       "${ENGINE_DIR}/${mod}/*/include/*.hpp" "${ENGINE_DIR}/${mod}/*/include/*.h")
  list(REMOVE_DUPLICATES files)
  list(SORT files)
  foreach(f ${files})
    file(RELATIVE_PATH rel "${ENGINE_DIR}" "${f}")
    if(rel IN_LIST ALLOW)
      continue()
    endif()
    math(EXPR scanned "${scanned} + 1")
    file(STRINGS "${f}" lines ENCODING UTF-8)
    set(n 0)
    foreach(line IN LISTS lines)
      math(EXPR n "${n} + 1")
      string(REGEX REPLACE "//.*$" "" code "${line}")
      set(hit "")
      if(code MATCHES "std::${FN}[fl]?[ \t]*\\(")
        set(hit "${CMAKE_MATCH_0}")
      elseif(code MATCHES "(^|[^A-Za-z0-9_:])::${FN}[fl]?[ \t]*\\(")
        set(hit "${CMAKE_MATCH_0}")
      elseif(code MATCHES "(^|[^A-Za-z0-9_:.>])${FN}[fl][ \t]*\\(")
        set(hit "${CMAKE_MATCH_0}")
      elseif(code MATCHES "(return[ \t]+|&&[ \t]*|\\|\\|[ \t]*|[-=(,+*/!?][ \t]*)${FN}[ \t]*\\(")
        set(hit "${CMAKE_MATCH_0}")
      endif()
      if(hit)
        message("VIOLATION ${rel}:${n}: ${hit}  -> use alamo::dmath")
        math(EXPR violations "${violations} + 1")
      endif()
    endforeach()
  endforeach()
endforeach()

if(violations GREATER 0)
  message(FATAL_ERROR "check_sim_math: ${violations} CRT transcendental call(s) in sim code")
endif()
message("check_sim_math: OK (${scanned} files in: ${SIM_MODULES})")
