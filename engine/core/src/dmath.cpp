// Deterministic math: see dmath.hpp for the contract. Only IEEE +,-,*,/, std::sqrt, std::fmod
// (exact) and bit manipulation are used here - never a CRT transcendental.
#include "alamo/core/dmath.hpp"

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

namespace alamo::dmath {
namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr uint64_t kSignBit = 0x8000000000000000ull;

// Constants (pi/2, ln2 splits computed with 120-digit arithmetic). A "split" part has its low
// mantissa bits zero so n * part is exact for the n range used.
constexpr double kInvPio2 = 0x1.45f306dc9c883p-1;   // 2/pi
constexpr double kPio2_1 = 0x1.921fb54000000p+0;    // pi/2, 27-bit parts: n * part exact for |n| < 2^26
constexpr double kPio2_2 = 0x1.10b4610000000p-30;
constexpr double kPio2_3 = 0x1.a626330000000p-58;
constexpr double kPio2_4 = 0x1.45c06e0e68948p-86;   // rest, rounded
constexpr double kPiHi = 0x1.921fb54442d18p+1;      // pi = hi + lo
constexpr double kPiLo = 0x1.1a62633145c07p-53;
constexpr double kPio2Hi = 0x1.921fb54442d18p+0;    // pi/2 = hi + lo
constexpr double kPio2Lo = 0x1.1a62633145c07p-54;
constexpr double kPio4Hi = 0x1.921fb54442d18p-1;
constexpr double kPio6Hi = 0x1.0c152382d7366p-1;    // pi/6 = hi + lo
constexpr double kPio6Lo = -0x1.ee6913347c2a6p-55;
constexpr double kSqrt3 = 0x1.bb67ae8584caap+0;
constexpr double kSqrt2 = 0x1.6a09e667f3bcdp+0;
constexpr double kTwoPiD = 0x1.921fb54442d18p+2;
constexpr double kLn2Hi32 = 0x1.62e42fee00000p-1;   // ln2, 32-bit high part: k * hi exact for |k| < 2^21
constexpr double kLn2Lo32 = 0x1.a39ef35793c76p-33;
constexpr double kLn2Hi = 0x1.62e42fefa39efp-1;     // ln2 = hi + lo
constexpr double kLn2Lo = 0x1.abc9e3b39803fp-56;
constexpr double kInvLn2 = 0x1.71547652b82fep+0;    // 1/ln2 = hi + lo
constexpr double kInvLn2Lo = 0x1.777d0ffda0d24p-56;
constexpr double kInvLn10Hi = 0x1.bcb7b1526e50ep-2; // 1/ln10 = hi + lo
constexpr double kInvLn10Lo = 0x1.95355baaafad3p-57;

inline uint64_t bits(double x) { return std::bit_cast<uint64_t>(x); }
inline double from_bits(uint64_t b) { return std::bit_cast<double>(b); }
inline double fabs_(double x) { return from_bits(bits(x) & ~kSignBit); }
inline double copysign_(double mag, double sgn) {
    return from_bits((bits(mag) & ~kSignBit) | (bits(sgn) & kSignBit));
}
inline bool is_nan(double x) { return (bits(x) & ~kSignBit) > 0x7ff0000000000000ull; }
inline bool is_finite(double x) { return (bits(x) & ~kSignBit) < 0x7ff0000000000000ull; }

// Round to nearest integer (ties to even), |v| < 2^51.
inline double rint_(double v) {
    constexpr double kMagic = 0x1.8p52;
    return (v + kMagic) - kMagic;
}

// x * 2^k without overflow in the intermediate steps; a single rounding when the result is subnormal.
double scale2(double x, int k) {
    if (k > 1023) {
        x *= 0x1p1023;
        k -= 1023;
        if (k > 1023) {
            x *= 0x1p1023;
            k -= 1023;
            if (k > 1023) k = 1023;
        }
    } else if (k < -1022) {
        x *= 0x1p-969;  // 2^-1022 * 2^53: stays normal, exact
        k += 969;
        if (k < -1022) {
            x *= 0x1p-969;
            k += 969;
            if (k < -1022) k = -1022;
        }
    }
    return x * from_bits(static_cast<uint64_t>(k + 1023) << 52);
}

struct DD {
    double hi, lo;
};
inline DD two_sum(double a, double b) {
    const double s = a + b;
    const double bb = s - a;
    return {s, (a - (s - bb)) + (b - bb)};
}
inline DD fast_two_sum(double a, double b) {  // |a| >= |b|
    const double s = a + b;
    return {s, b - (s - a)};
}
inline void veltkamp(double a, double& h, double& l) {
    const double c = 134217729.0 * a;  // 2^27 + 1
    h = c - (c - a);
    l = a - h;
}
inline DD two_prod(double a, double b) {  // Dekker; no FMA (contraction is disabled)
    const double p = a * b;
    double ah, al, bh, bl;
    veltkamp(a, ah, al);
    veltkamp(b, bh, bl);
    return {p, ((ah * bh - p) + ah * bl + al * bh) + al * bl};
}

// ---- exp ---------------------------------------------------------------------------------------
// exp(h + l), |l| a few ulp of h at most.
double exp_dd(double h, double l) {
    if (is_nan(h)) return h;
    if (h > 709.8) return kInf;
    if (h < -745.2) return 0.0;
    const double k = rint_(h * kInvLn2);
    const double hi = h - k * kLn2Hi32;  // exact
    const double lo = k * kLn2Lo32;
    const double r = hi - lo;
    const double rc = ((hi - r) - lo) + l;
    // exp(r) - 1 = r + r^2 * (1/2! + r/3! + ... + r^13/15!), |r| <= 0.35: truncation < 2^-62.
    double p = 1.0 / 1307674368000.0;
    p = p * r + 1.0 / 87178291200.0;
    p = p * r + 1.0 / 6227020800.0;
    p = p * r + 1.0 / 479001600.0;
    p = p * r + 1.0 / 39916800.0;
    p = p * r + 1.0 / 3628800.0;
    p = p * r + 1.0 / 362880.0;
    p = p * r + 1.0 / 40320.0;
    p = p * r + 1.0 / 5040.0;
    p = p * r + 1.0 / 720.0;
    p = p * r + 1.0 / 120.0;
    p = p * r + 1.0 / 24.0;
    p = p * r + 1.0 / 6.0;
    p = p * r + 0.5;
    const double em1 = r + (r * r) * p;
    const double y = 1.0 + (em1 + rc * (1.0 + em1));
    return scale2(y, static_cast<int>(k));
}

// ---- log ---------------------------------------------------------------------------------------
// x finite > 0: x = 2^k * m, m in (sqrt(1/2), sqrt(2)]; logm = log(m) as a double-double.
void log_parts(double x, int& k, DD& logm) {
    k = 0;
    uint64_t b = bits(x);
    if (b < (1ull << 52)) {  // subnormal
        x *= 0x1p54;
        k = -54;
        b = bits(x);
    }
    k += static_cast<int>((b >> 52) & 0x7ff) - 1023;
    double m = from_bits((b & 0x000fffffffffffffull) | 0x3ff0000000000000ull);
    if (m > kSqrt2) {
        m *= 0.5;
        ++k;
    }
    const double f = m - 1.0;  // exact (Sterbenz)
    // s = f / (2 + f) as double-double; log(m) = 2 atanh(s) = 2s + 2s^3/3 + 2s^5/5 + ...
    const DD d = fast_two_sum(2.0, f);
    const double q1 = f / d.hi;
    const DD p = two_prod(q1, d.hi);
    const double rem = ((f - p.hi) - p.lo) - q1 * d.lo;
    const double q2 = rem / d.hi;
    const double z = q1 * q1;  // |s| <= 0.1716, z <= 0.0295: 13 terms, truncation < 2^-64 relative
    double t = 1.0 / 27.0;
    t = t * z + 1.0 / 25.0;
    t = t * z + 1.0 / 23.0;
    t = t * z + 1.0 / 21.0;
    t = t * z + 1.0 / 19.0;
    t = t * z + 1.0 / 17.0;
    t = t * z + 1.0 / 15.0;
    t = t * z + 1.0 / 13.0;
    t = t * z + 1.0 / 11.0;
    t = t * z + 1.0 / 9.0;
    t = t * z + 1.0 / 7.0;
    t = t * z + 1.0 / 5.0;
    t = t * z + 1.0 / 3.0;
    const double tail = (2.0 * q1) * (z * t);
    logm = fast_two_sum(2.0 * q1, 2.0 * q2 + tail);
}

// log(x) as a double-double, x finite > 0.
DD log_dd(double x) {
    int k = 0;
    DD lm{};
    log_parts(x, k, lm);
    const double dk = static_cast<double>(k);
    const DD s = two_sum(dk * kLn2Hi32, lm.hi);
    return fast_two_sum(s.hi, s.lo + (lm.lo + dk * kLn2Lo32));
}

// ---- sin / cos ---------------------------------------------------------------------------------
// sin(x + xl), |x| <= ~pi/4. Taylor through x^17 (truncation < 2^-62 relative).
double ksin(double x, double xl) {
    const double z = x * x;
    double p = 1.0 / 355687428096000.0;
    p = p * z - 1.0 / 1307674368000.0;
    p = p * z + 1.0 / 6227020800.0;
    p = p * z - 1.0 / 39916800.0;
    p = p * z + 1.0 / 362880.0;
    p = p * z - 1.0 / 5040.0;
    p = p * z + 1.0 / 120.0;
    p = p * z - 1.0 / 6.0;
    return x + ((x * z) * p + xl * (1.0 - 0.5 * z));
}
// cos(x + xl), |x| <= ~pi/4. Taylor through x^16.
double kcos(double x, double xl) {
    const double z = x * x;
    double p = 1.0 / 20922789888000.0;
    p = p * z - 1.0 / 87178291200.0;
    p = p * z + 1.0 / 479001600.0;
    p = p * z - 1.0 / 3628800.0;
    p = p * z + 1.0 / 40320.0;
    p = p * z - 1.0 / 720.0;
    p = p * z + 1.0 / 24.0;
    const double hz = 0.5 * z;
    const double w = 1.0 - hz;
    return w + (((1.0 - w) - hz) + (z * (z * p) - x * xl));
}
// x finite: x = n*pi/2 + (rh + rl), returns n mod 4.
int reduce_pio2(double x, double& rh, double& rl) {
    if (fabs_(x) <= kPio4Hi) {
        rh = x;
        rl = 0.0;
        return 0;
    }
    if (fabs_(x) >= 0x1p26) x = std::fmod(x, kTwoPiD);  // exact remainder (deterministic)
    const double n = rint_(x * kInvPio2);
    const double a = x - n * kPio2_1;  // exact: |n| < 2^26, Sterbenz
    const DD s1 = two_sum(a, -(n * kPio2_2));
    const DD s2 = two_sum(s1.hi, -(n * kPio2_3));
    const double lo = (s1.lo + s2.lo) - n * kPio2_4;
    const DD r = fast_two_sum(s2.hi, lo);
    rh = r.hi;
    rl = r.lo;
    return static_cast<int>(static_cast<int64_t>(n) & 3);
}

// ---- atan --------------------------------------------------------------------------------------
// atan(u), |u| <= 0.268: series through u^29 (truncation < 2^-62 relative).
double atan_series(double u) {
    const double z = u * u;
    double p = -1.0 / 29.0;
    p = p * z + 1.0 / 27.0;
    p = p * z - 1.0 / 25.0;
    p = p * z + 1.0 / 23.0;
    p = p * z - 1.0 / 21.0;
    p = p * z + 1.0 / 19.0;
    p = p * z - 1.0 / 17.0;
    p = p * z + 1.0 / 15.0;
    p = p * z - 1.0 / 13.0;
    p = p * z + 1.0 / 11.0;
    p = p * z - 1.0 / 9.0;
    p = p * z + 1.0 / 7.0;
    p = p * z - 1.0 / 5.0;
    p = p * z + 1.0 / 3.0;
    return u - (u * z) * p;
}
// atan(t), 0 <= t <= 1. Above tan(pi/12): atan(t) = pi/6 + atan((t*sqrt3 - 1) / (t + sqrt3)).
double atan_unit(double t) {
    if (t <= 0.2679491924311227) return atan_series(t);
    const double u = (t * kSqrt3 - 1.0) / (t + kSqrt3);
    return kPio6Hi + (atan_series(u) + kPio6Lo);
}

bool is_odd_integer(double y) {  // y finite integer
    if (fabs_(y) >= 0x1p53) return false;
    const double h = y * 0.5;
    return std::trunc(h) != h;
}

}  // namespace

// ---- public double API ---------------------------------------------------------------------------
double sqrt(double x) { return std::sqrt(x); }

double sin(double x) {
    if (!is_finite(x)) return x - x;
    if (fabs_(x) < 0x1p-27) return x;
    double rh, rl;
    switch (reduce_pio2(x, rh, rl)) {
        case 0: return ksin(rh, rl);
        case 1: return kcos(rh, rl);
        case 2: return -ksin(rh, rl);
        default: return -kcos(rh, rl);
    }
}

double cos(double x) {
    if (!is_finite(x)) return x - x;
    if (fabs_(x) < 0x1p-27) return 1.0;
    double rh, rl;
    switch (reduce_pio2(x, rh, rl)) {
        case 0: return kcos(rh, rl);
        case 1: return -ksin(rh, rl);
        case 2: return -kcos(rh, rl);
        default: return ksin(rh, rl);
    }
}

void sincos(double x, double& s, double& c) {
    if (!is_finite(x)) {
        s = c = x - x;
        return;
    }
    if (fabs_(x) < 0x1p-27) {
        s = x;
        c = 1.0;
        return;
    }
    double rh, rl;
    const int q = reduce_pio2(x, rh, rl);
    const double ks = ksin(rh, rl), kc = kcos(rh, rl);
    switch (q) {
        case 0: s = ks; c = kc; break;
        case 1: s = kc; c = -ks; break;
        case 2: s = -ks; c = -kc; break;
        default: s = -kc; c = ks; break;
    }
}

double tan(double x) {
    if (!is_finite(x)) return x - x;
    if (fabs_(x) < 0x1p-27) return x;
    double rh, rl;
    const int q = reduce_pio2(x, rh, rl);
    const double ks = ksin(rh, rl), kc = kcos(rh, rl);
    return (q & 1) ? -kc / ks : ks / kc;
}

double atan(double x) {
    if (is_nan(x)) return x;
    const double ax = fabs_(x);
    if (ax < 0x1p-27) return x;
    const double r = ax <= 1.0 ? atan_unit(ax) : kPio2Hi + (kPio2Lo - atan_unit(1.0 / ax));
    return copysign_(r, x);
}

double atan2(double y, double x) {
    if (is_nan(x) || is_nan(y)) return x + y;
    const double ax = fabs_(x), ay = fabs_(y);
    const bool xneg = (bits(x) & kSignBit) != 0;
    double r;
    if (ay == 0.0) {
        r = xneg ? kPiHi : 0.0;  // atan2(+-0, -0 or x<0) = +-pi; atan2(+-0, +0 or x>0) = +-0
    } else if (ax == kInf) {
        if (ay == kInf) r = xneg ? 3.0 * (kPio2Hi * 0.5) : kPio2Hi * 0.5;  // +-3pi/4, +-pi/4
        else r = xneg ? kPiHi : 0.0;
    } else if (ay == kInf || ax == 0.0) {
        r = kPio2Hi;
    } else if (ay <= ax) {
        const double v = atan_unit(ay / ax);
        r = xneg ? (kPiHi - v) + kPiLo : v;
    } else {
        const double v = atan_unit(ax / ay);
        r = xneg ? (kPio2Hi + v) + kPio2Lo : (kPio2Hi - v) + kPio2Lo;
    }
    return copysign_(r, y);
}

double asin(double x) {
    if (is_nan(x)) return x;
    if (fabs_(x) > 1.0) return kNaN;
    if (fabs_(x) < 0x1p-27) return x;
    return atan2(x, std::sqrt((1.0 - x) * (1.0 + x)));
}

double acos(double x) {
    if (is_nan(x)) return x;
    if (fabs_(x) > 1.0) return kNaN;
    return atan2(std::sqrt((1.0 - x) * (1.0 + x)), x);
}

double exp(double x) { return exp_dd(x, 0.0); }

double exp2(double x) {
    if (is_nan(x)) return x;
    if (x > 1025.0) return kInf;
    if (x < -1080.0) return 0.0;
    const double k = rint_(x);
    const double f = x - k;  // exact, |f| <= 0.5
    DD p = two_prod(f, kLn2Hi);
    p.lo += f * kLn2Lo;
    return scale2(exp_dd(p.hi, p.lo), static_cast<int>(k));
}

double log(double x) {
    if (is_nan(x)) return x;
    if (x < 0.0) return kNaN;
    if (x == 0.0) return -kInf;
    if (x == kInf) return x;
    return log_dd(x).hi;
}

double log2(double x) {
    if (is_nan(x)) return x;
    if (x < 0.0) return kNaN;
    if (x == 0.0) return -kInf;
    if (x == kInf) return x;
    int k = 0;
    DD lm{};
    log_parts(x, k, lm);
    DD p = two_prod(lm.hi, kInvLn2);
    p.lo += lm.hi * kInvLn2Lo + lm.lo * kInvLn2;
    const DD s = two_sum(static_cast<double>(k), p.hi);
    return s.hi + (s.lo + p.lo);
}

double log10(double x) {
    if (is_nan(x)) return x;
    if (x < 0.0) return kNaN;
    if (x == 0.0) return -kInf;
    if (x == kInf) return x;
    const DD l = log_dd(x);
    DD p = two_prod(l.hi, kInvLn10Hi);
    p.lo += l.hi * kInvLn10Lo + l.lo * kInvLn10Hi;
    return p.hi + p.lo;
}

double pow(double x, double y) {
    if (y == 0.0 || x == 1.0) return 1.0;
    if (is_nan(x) || is_nan(y)) return x + y;
    const double ax = fabs_(x), ay = fabs_(y);
    const bool y_int = ay == kInf || std::trunc(y) == y;
    const bool y_odd = ay != kInf && y_int && is_odd_integer(y);
    const bool xneg = (bits(x) & kSignBit) != 0;
    if (ay == kInf) {
        if (ax == 1.0) return 1.0;  // (-1)^+-inf
        return ((ax > 1.0) == (y > 0.0)) ? kInf : 0.0;
    }
    if (ax == 0.0) {
        const double r = y < 0.0 ? kInf : 0.0;
        return (y_odd && xneg) ? -r : r;
    }
    if (ax == kInf) {
        const double r = y < 0.0 ? 0.0 : kInf;
        return (y_odd && xneg) ? -r : r;
    }
    if (xneg && !y_int) return kNaN;
    const double sign = (xneg && y_odd) ? -1.0 : 1.0;
    const DD l = log_dd(ax);
    const double approx = y * l.hi;
    if (approx > 710.0) return sign * kInf;
    if (approx < -746.0) return sign * 0.0;
    DD p = two_prod(y, l.hi);
    p.lo += y * l.lo;
    const DD q = fast_two_sum(p.hi, p.lo);
    return sign * exp_dd(q.hi, q.lo);
}

double hypot(double x, double y) {
    double ax = fabs_(x), ay = fabs_(y);
    if (ax == kInf || ay == kInf) return kInf;
    if (is_nan(ax) || is_nan(ay)) return ax + ay;
    if (ax < ay) {
        const double t = ax;
        ax = ay;
        ay = t;
    }
    if (ay == 0.0) return ax;
    if (ay < ax * 0x1p-60) return ax + ay;  // (ay/ax)^2 below half an ulp: result rounds to ax
    double scale = 1.0;
    if (ax > 0x1p500) {
        ax *= 0x1p-600;
        ay *= 0x1p-600;
        scale = 0x1p600;
    } else if (ay < 0x1p-500) {
        ax *= 0x1p600;
        ay *= 0x1p600;
        scale = 0x1p-600;
    }
    // Double-double sum of squares, then one Newton correction of the root.
    const DD a2 = two_prod(ax, ax);
    const DD b2 = two_prod(ay, ay);
    const DD s = two_sum(a2.hi, b2.hi);
    const double lo = s.lo + (a2.lo + b2.lo);
    const DD sum = fast_two_sum(s.hi, lo);
    const double r = std::sqrt(sum.hi);
    const DD rr = two_prod(r, r);
    const double corr = ((sum.hi - rr.hi) - rr.lo + sum.lo) / (2.0 * r);
    return (r + corr) * scale;
}

// ---- float API: compute in double, round once ----------------------------------------------------
float sqrt(float x) { return std::sqrt(x); }
float sin(float x) { return static_cast<float>(sin(static_cast<double>(x))); }
float cos(float x) { return static_cast<float>(cos(static_cast<double>(x))); }
float tan(float x) { return static_cast<float>(tan(static_cast<double>(x))); }
void sincos(float x, float& s, float& c) {
    double ds, dc;
    sincos(static_cast<double>(x), ds, dc);
    s = static_cast<float>(ds);
    c = static_cast<float>(dc);
}
float asin(float x) { return static_cast<float>(asin(static_cast<double>(x))); }
float acos(float x) { return static_cast<float>(acos(static_cast<double>(x))); }
float atan(float x) { return static_cast<float>(atan(static_cast<double>(x))); }
float atan2(float y, float x) {
    return static_cast<float>(atan2(static_cast<double>(y), static_cast<double>(x)));
}
float exp(float x) { return static_cast<float>(exp(static_cast<double>(x))); }
float exp2(float x) { return static_cast<float>(exp2(static_cast<double>(x))); }
float log(float x) { return static_cast<float>(log(static_cast<double>(x))); }
float log2(float x) { return static_cast<float>(log2(static_cast<double>(x))); }
float log10(float x) { return static_cast<float>(log10(static_cast<double>(x))); }
float pow(float x, float y) {
    return static_cast<float>(pow(static_cast<double>(x), static_cast<double>(y)));
}
float hypot(float x, float y) {
    // Exact in double (no overflow: float^2 fits), one sqrt rounding, then one float rounding.
    const double dx = static_cast<double>(x), dy = static_cast<double>(y);
    if (fabs_(dx) == kInf || fabs_(dy) == kInf) return std::numeric_limits<float>::infinity();
    return static_cast<float>(std::sqrt(dx * dx + dy * dy));
}

float fast_rsqrt_retail(float x) {
    const float x2 = 0.5f * x;
    const uint32_t i = 0x5f375a86u - (std::bit_cast<uint32_t>(x) >> 1);
    float y = std::bit_cast<float>(i);
    y = y * (1.5f - (x2 * y) * y);
    y = y * (1.5f - (x2 * y) * y);
    return y;
}

}  // namespace alamo::dmath
