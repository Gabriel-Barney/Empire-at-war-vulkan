// Lint fixture: alamo_core_sim_math_lint_selftest expects check_sim_math.cmake to FAIL on this file.
#include <cmath>
float ok(float x) { return alamo::dmath::sin(x) + std::sqrt(x); }  // allowed
float bad(float x) { return std::atan2(x, 1.0f); }                  // violation
