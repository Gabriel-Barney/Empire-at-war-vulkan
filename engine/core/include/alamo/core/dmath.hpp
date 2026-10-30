// Engine-owned deterministic math (Q1). Sim code (anything whose results game logic reads, or
// that feeds the per-tick CRC) uses these instead of <cmath> transcendentals: CRT sin/cos/pow/...
// differ between MSVC, clang and gcc runtimes even with /fp:precise, these do not.
//
// How: every function is a fixed sequence of IEEE-754 double +, -, *, / (round to nearest),
// std::sqrt (correctly rounded by IEEE), and exact helpers (std::fmod, bit manipulation). No CRT
// transcendental is called. The build forbids FMA contraction (/fp:precise, -ffp-contract=off),
// so every compiler produces the same bits. Polynomials are truncated Taylor / atanh series with
// constants computed at compile time (1/n!, 1/(2k+1)), range reduction uses multi-part pi/2 and
// ln2 constants; pow/log keep a double-double intermediate.
//
// Float overloads compute in double and round once to float (nearly correctly rounded).
// Measured error vs the CRT (engine/core/tests/test_dmath.cpp prints them):
//   double: sin/cos/tan/exp/exp2/log/log2/log10/pow/atan/atan2/asin/acos/hypot <= ~2 ulp
//   float : <= 1 ulp vs (float)CRT-double
// Range notes: sin/cos/tan reduce exactly for |x| < 2^26 (~6.7e7); beyond that x is first reduced
// with fmod(x, 2*pi as double) - still deterministic, but not the mathematically exact value.
// Special values (NaN, +-inf, +-0, subnormals) follow C99 Annex F for every function.
//
// sqrt is IEEE-exact everywhere, so dmath::sqrt is just std::sqrt (kept here so sim code needs
// a single include). floor/ceil/trunc/round/fmod/fabs from <cmath> are exact and may be used
// directly. tools/lint/check_sim_math.cmake (ctest alamo_sim_math_lint) rejects direct CRT
// transcendental calls in the sim modules.
#pragma once
#include <cstdint>

namespace alamo::dmath {

inline constexpr double kPi = 0x1.921fb54442d18p+1;
inline constexpr double kHalfPi = 0x1.921fb54442d18p+0;
inline constexpr double kTwoPi = 0x1.921fb54442d18p+2;
inline constexpr float kPiF = 3.14159265358979323846f;
inline constexpr float kHalfPiF = 1.57079632679489661923f;
inline constexpr float kTwoPiF = 6.28318530717958647692f;

double sqrt(double x);
double sin(double x);
double cos(double x);
double tan(double x);
void sincos(double x, double& s, double& c);
double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);
double exp(double x);
double exp2(double x);
double log(double x);
double log2(double x);
double log10(double x);
double pow(double x, double y);
double hypot(double x, double y);

float sqrt(float x);
float sin(float x);
float cos(float x);
float tan(float x);
void sincos(float x, float& s, float& c);
float asin(float x);
float acos(float x);
float atan(float x);
float atan2(float y, float x);
float exp(float x);
float exp2(float x);
float log(float x);
float log2(float x);
float log10(float x);
float pow(float x, float y);
float hypot(float x, float y);

// Retail movement normalisation (docs/re/MOVEMENT.md §3.5 step 5 / §7): fast inverse square root,
// magic 0x5f375a86, two Newton steps, all float:
//     x2 = 0.5f * x;  i = 0x5f375a86 - (bits(x) >> 1);  y = float(i);
//     y = y * (1.5f - (x2 * y) * y);   (twice)
// Must be reproduced bit-exactly (it feeds positions). PROVISIONAL detail: the doc pins the magic
// and the step count; the product order inside the Newton step ((x2*y)*y, the classic form) is
// assumed until RE confirms it.
float fast_rsqrt_retail(float x);

}  // namespace alamo::dmath
