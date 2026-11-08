// alamo::dmath tests: special values, error vs the CRT, and bit-identical outputs across compilers.
//
//   alamo_core_dmath_tests [--golden FILE]        check outputs against the golden digests
//   alamo_core_dmath_tests --write-golden FILE    (re)generate the golden file (MSVC build)
//
// Each function runs on a fixed input set (splitmix64-generated, several domains); the output bit
// patterns are hashed with StateHasher. The golden file holds one digest per function, so MSVC,
// clang and gcc builds must all reproduce it bit for bit. The CRT is used ONLY as an accuracy
// reference here (tests are not sim code).
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <string>
#include <vector>

#include "alamo/core/bytes.hpp"
#include "alamo/core/dmath.hpp"
#include "alamo/core/sim_crc.hpp"
#include "alamo/test/support.hpp"

namespace dm = alamo::dmath;

using ::alamo::test::g_fail;

namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr float kInfF = std::numeric_limits<float>::infinity();

struct Rng {
    uint64_t s;
    uint64_t next() {
        uint64_t z = (s += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    double unit() { return static_cast<double>(next() >> 11) * 0x1p-53; }  // [0,1)
    double uniform(double a, double b) { return a + (b - a) * unit(); }
    // |x| log-uniform in [2^lo, 2^hi), random sign if `sign`.
    double logu(int lo, int hi, bool sign) {
        double m = 1.0 + unit();
        const int e = lo + static_cast<int>(next() % static_cast<uint64_t>(hi - lo));
        double x = std::ldexp(m, e);
        if (sign && (next() & 1)) x = -x;
        return x;
    }
};

uint64_t ordered(double x) {
    uint64_t b;
    std::memcpy(&b, &x, 8);
    return (b >> 63) ? ~b + 1 + 0x8000000000000000ull : b + 0x8000000000000000ull;  // monotonic map
}
uint64_t ulp_diff(double a, double b) {
    if (std::isnan(a) && std::isnan(b)) return 0;
    if (std::isnan(a) || std::isnan(b)) return ~0ull;
    if (a == b) return 0;  // +-0
    const uint64_t oa = ordered(a), ob = ordered(b);
    return oa > ob ? oa - ob : ob - oa;
}
uint64_t ulp_diff_f(float a, float b) { return ulp_diff(static_cast<double>(a), static_cast<double>(b)); }

struct Result {
    std::string name;
    uint64_t digest = 0;
    uint64_t count = 0;
    uint64_t max_ulp = 0;
    double worst_input = 0, worst_input2 = 0;
};

using Fn1 = double (*)(double);
using Fn2 = double (*)(double, double);
using Fn1f = float (*)(float);
using Fn2f = float (*)(float, float);

// Error is measured only where the reference is a normal number (subnormal results have fewer
// significant bits, ulp distances there are not comparable).
bool measurable(double ref) {
    return std::isfinite(ref) && (ref == 0.0 || std::fabs(ref) >= std::numeric_limits<double>::min());
}

Result run1(const char* name, Fn1 ours, double (*ref)(double), const std::vector<double>& in,
            const std::vector<double>& measure_mask_hint = {}) {
    (void)measure_mask_hint;
    Result r;
    r.name = name;
    alamo::StateHasher h;
    for (double x : in) {
        const double y = ours(x);
        h.f64(y);
        const double e = ref(x);
        if (!measurable(e)) continue;
        const uint64_t u = ulp_diff(y, e);
        if (u > r.max_ulp) {
            r.max_ulp = u;
            r.worst_input = x;
        }
    }
    r.digest = h.digest();
    r.count = in.size();
    return r;
}
Result run2(const char* name, Fn2 ours, double (*ref)(double, double), const std::vector<double>& a,
            const std::vector<double>& b) {
    Result r;
    r.name = name;
    alamo::StateHasher h;
    for (size_t i = 0; i < a.size(); ++i) {
        const double y = ours(a[i], b[i]);
        h.f64(y);
        const double e = ref(a[i], b[i]);
        if (!measurable(e)) continue;
        const uint64_t u = ulp_diff(y, e);
        if (u > r.max_ulp) {
            r.max_ulp = u;
            r.worst_input = a[i];
            r.worst_input2 = b[i];
        }
    }
    r.digest = h.digest();
    r.count = a.size();
    return r;
}
// Float: reference is the CRT double function rounded to float (the CRT's float functions are
// less accurate than that on some runtimes).
Result run1f(const char* name, Fn1f ours, double (*ref)(double), const std::vector<double>& in) {
    Result r;
    r.name = name;
    alamo::StateHasher h;
    for (double xd : in) {
        const float x = static_cast<float>(xd);
        const float y = ours(x);
        h.f32(y);
        const double ed = ref(static_cast<double>(x));
        const float e = static_cast<float>(ed);
        if (!std::isfinite(e) || (e != 0.0f && std::fabs(e) < std::numeric_limits<float>::min())) continue;
        const uint64_t u = ulp_diff_f(y, e);
        if (u > r.max_ulp) {
            r.max_ulp = u;
            r.worst_input = static_cast<double>(x);
        }
    }
    r.digest = h.digest();
    r.count = in.size();
    return r;
}
Result run2f(const char* name, Fn2f ours, double (*ref)(double, double), const std::vector<double>& a,
             const std::vector<double>& b) {
    Result r;
    r.name = name;
    alamo::StateHasher h;
    for (size_t i = 0; i < a.size(); ++i) {
        const float x = static_cast<float>(a[i]), y2 = static_cast<float>(b[i]);
        const float y = ours(x, y2);
        h.f32(y);
        const float e = static_cast<float>(ref(static_cast<double>(x), static_cast<double>(y2)));
        if (!std::isfinite(e) || (e != 0.0f && std::fabs(e) < std::numeric_limits<float>::min())) continue;
        const uint64_t u = ulp_diff_f(y, e);
        if (u > r.max_ulp) {
            r.max_ulp = u;
            r.worst_input = static_cast<double>(x);
            r.worst_input2 = static_cast<double>(y2);
        }
    }
    r.digest = h.digest();
    r.count = a.size();
    return r;
}

double crt_sin(double x) { return std::sin(x); }
double crt_cos(double x) { return std::cos(x); }
double crt_tan(double x) { return std::tan(x); }
double crt_asin(double x) { return std::asin(x); }
double crt_acos(double x) { return std::acos(x); }
double crt_atan(double x) { return std::atan(x); }
double crt_exp(double x) { return std::exp(x); }
double crt_exp2(double x) { return std::exp2(x); }
double crt_log(double x) { return std::log(x); }
double crt_log2(double x) { return std::log2(x); }
double crt_log10(double x) { return std::log10(x); }
double crt_sqrt(double x) { return std::sqrt(x); }
double crt_atan2(double y, double x) { return std::atan2(y, x); }
double crt_pow(double x, double y) { return std::pow(x, y); }
double crt_hypot(double x, double y) { return std::hypot(x, y); }
double ds_sin(double x) { double s, c; dm::sincos(x, s, c); return s; }
double ds_cos(double x) { double s, c; dm::sincos(x, s, c); return c; }

template <class F>
std::vector<double> gen(size_t n, F&& f) {
    std::vector<double> v;
    v.reserve(n);
    for (size_t i = 0; i < n; ++i) v.push_back(f());
    return v;
}

void special_values() {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    CHECK(dm::sin(0.0) == 0.0 && !std::signbit(dm::sin(0.0)));
    CHECK(dm::sin(-0.0) == 0.0 && std::signbit(dm::sin(-0.0)));
    CHECK(std::isnan(dm::sin(kInf)) && std::isnan(dm::cos(-kInf)) && std::isnan(dm::tan(nan)));
    CHECK(dm::cos(0.0) == 1.0);
    CHECK(dm::exp(0.0) == 1.0 && dm::exp(-kInf) == 0.0 && dm::exp(kInf) == kInf && dm::exp(1000.0) == kInf);
    CHECK(dm::exp(-1000.0) == 0.0 && std::isnan(dm::exp(nan)));
    CHECK(dm::exp2(10.0) == 1024.0 && dm::exp2(-1074.0) == std::ldexp(1.0, -1074) && dm::exp2(0.5) > 1.414);
    CHECK(dm::log(1.0) == 0.0 && dm::log(0.0) == -kInf && std::isnan(dm::log(-1.0)) && dm::log(kInf) == kInf);
    CHECK(dm::log2(8.0) == 3.0 && dm::log2(std::ldexp(1.0, -1074)) == -1074.0 && dm::log2(0.5) == -1.0);
    CHECK(dm::log10(1000.0) == 3.0 && dm::log10(1.0) == 0.0);
    CHECK(dm::pow(2.0, 10.0) == 1024.0 && dm::pow(3.0, 2.0) == 9.0 && dm::pow(-2.0, 3.0) == -8.0);
    CHECK(dm::pow(nan, 0.0) == 1.0 && dm::pow(1.0, nan) == 1.0 && std::isnan(dm::pow(-2.0, 0.5)));
    CHECK(dm::pow(0.0, -1.0) == kInf && dm::pow(-0.0, -3.0) == -kInf && dm::pow(-0.0, 3.0) == 0.0 &&
          std::signbit(dm::pow(-0.0, 3.0)));
    CHECK(dm::pow(-1.0, kInf) == 1.0 && dm::pow(0.5, kInf) == 0.0 && dm::pow(2.0, -kInf) == 0.0 &&
          dm::pow(0.5, -kInf) == kInf);
    CHECK(dm::pow(-kInf, 3.0) == -kInf && dm::pow(-kInf, 2.0) == kInf && dm::pow(kInf, -1.0) == 0.0);
    CHECK(dm::pow(10.0, 400.0) == kInf && dm::pow(10.0, -400.0) == 0.0 && dm::pow(-10.0, 401.0) == -kInf);
    CHECK(dm::atan2(0.0, -1.0) == dm::kPi && dm::atan2(-0.0, -1.0) == -dm::kPi && dm::atan2(-0.0, 1.0) == 0.0 &&
          std::signbit(dm::atan2(-0.0, 1.0)));
    CHECK(dm::atan2(1.0, 0.0) == dm::kHalfPi && dm::atan2(-1.0, 0.0) == -dm::kHalfPi);
    CHECK(dm::atan2(kInf, -kInf) > 2.35 && dm::atan2(-kInf, kInf) < -0.78 && dm::atan2(1.0, -kInf) == dm::kPi);
    CHECK(std::isnan(dm::atan2(nan, 1.0)) && dm::atan(kInf) == dm::kHalfPi && dm::atan(-0.0) == 0.0 &&
          std::signbit(dm::atan(-0.0)));
    CHECK(dm::asin(1.0) == dm::kHalfPi && dm::acos(1.0) == 0.0 && dm::acos(-1.0) == dm::kPi && std::isnan(dm::asin(1.5)));
    CHECK(dm::hypot(3.0, 4.0) == 5.0 && dm::hypot(kInf, nan) == kInf && dm::hypot(1e300, 1e300) > 1.4e300 &&
          dm::hypot(-0.0, 0.0) == 0.0);
    CHECK(dm::hypot(3.0f, 4.0f) == 5.0f && dm::sqrt(4.0f) == 2.0f && dm::exp(0.0f) == 1.0f && dm::sin(kInfF) != dm::sin(kInfF));
    CHECK(dm::pow(2.0f, 0.5f) == static_cast<float>(std::sqrt(2.0)));
    // Retail fast inverse sqrt (0x5f375a86, 2 Newton steps): golden bits from an independent model.
    const float rs_in[] = {1.0f, 2.0f, 4.0f, 0.25f, 100.0f, 3.0e-5f, 12345.678f};
    const uint32_t rs_out[] = {0x3f7fffb7u, 0x3f3504f3u, 0x3effffb7u, 0x3fffffb7u, 0x3dcccc9cu, 0x433692fbu, 0x3c1374b3u};
    for (size_t i = 0; i < 7; ++i) {
        const float y = dm::fast_rsqrt_retail(rs_in[i]);
        uint32_t b;
        std::memcpy(&b, &y, 4);
        CHECK(b == rs_out[i]);
    }
    float s = 0, c = 0;
    dm::sincos(1.0f, s, c);
    CHECK(s == dm::sin(1.0f) && c == dm::cos(1.0f));
}

}  // namespace

int main(int argc, char** argv) {
    std::string golden_path, write_path;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--golden" && i + 1 < argc) golden_path = argv[++i];
        else if (a == "--write-golden" && i + 1 < argc) write_path = argv[++i];
        else {
            std::fprintf(stderr, "usage: alamo_core_dmath_tests [--golden FILE | --write-golden FILE]\n");
            return 2;
        }
    }

    special_values();

    constexpr size_t N = 60000;
    Rng rng{0x5EED0001ull};
    // Angles: small, game-range, wide (up to 2^26 - exact reduction range) and huge (fmod path,
    // excluded from the accuracy measurement but part of the digest).
    std::vector<double> ang = gen(N, [&] {
        switch (rng.next() % 4) {
            case 0: return rng.uniform(-4.0, 4.0);
            case 1: return rng.uniform(-1000.0, 1000.0);
            case 2: return rng.logu(-30, 26, true);
            default: return rng.uniform(-1e6, 1e6);
        }
    });
    std::vector<double> huge = gen(2000, [&] { return rng.logu(26, 1000, true); });
    std::vector<double> expin = gen(N, [&] { return (rng.next() & 1) ? rng.uniform(-745.0, 709.7) : rng.uniform(-2.0, 2.0); });
    std::vector<double> exp2in = gen(N, [&] { return (rng.next() & 1) ? rng.uniform(-1074.0, 1023.9) : rng.uniform(-2.0, 2.0); });
    std::vector<double> pos = gen(N, [&] {
        switch (rng.next() % 3) {
            case 0: return rng.logu(-1074, 1024, false);
            case 1: return rng.uniform(0.5, 2.0);
            default: return rng.logu(-20, 20, false);
        }
    });
    std::vector<double> unit = gen(N, [&] {
        return (rng.next() & 1) ? rng.uniform(-1.0, 1.0) : (rng.next() & 1 ? 1.0 : -1.0) * (1.0 - rng.logu(-50, -1, false));
    });
    std::vector<double> atin = gen(N, [&] { return rng.logu(-40, 40, true); });
    std::vector<double> ya = gen(N, [&] { return rng.logu(-60, 60, true); });
    std::vector<double> xa = gen(N, [&] { return rng.logu(-60, 60, true); });
    std::vector<double> px, py;
    for (size_t i = 0; i < N; ++i) {
        if (i % 4 == 3) {  // negative base, integer exponent
            px.push_back(-rng.logu(-8, 8, false));
            py.push_back(std::trunc(rng.uniform(-60.0, 60.0)));
        } else {
            const double x = rng.logu(-30, 30, false);
            px.push_back(x);
            const double lim = 700.0 / std::max(1e-3, std::fabs(std::log(x)));
            py.push_back(rng.uniform(-std::min(lim, 200.0), std::min(lim, 200.0)));
        }
    }
    std::vector<double> hx = gen(N, [&] { return rng.logu(-1000, 1000, true); });
    std::vector<double> hy;
    for (size_t i = 0; i < N; ++i) hy.push_back(i % 2 ? hx[i] * rng.uniform(0.0, 2.0) : rng.logu(-1000, 1000, true));
    // Float domains.
    std::vector<double> angf = gen(N, [&] { return (rng.next() & 1) ? rng.uniform(-10.0, 10.0) : rng.uniform(-1e5, 1e5); });
    std::vector<double> expf_in = gen(N, [&] { return rng.uniform(-103.0, 88.7); });
    std::vector<double> posf = gen(N, [&] { return rng.logu(-149, 128, false); });
    std::vector<double> pxf, pyf;
    for (size_t i = 0; i < N; ++i) {
        const double x = rng.logu(-10, 10, false);
        pxf.push_back(x);
        pyf.push_back(rng.uniform(-8.0, 8.0));
    }

    std::vector<Result> res;
    res.push_back(run1("sin", dm::sin, crt_sin, ang));
    res.push_back(run1("cos", dm::cos, crt_cos, ang));
    res.push_back(run1("tan", dm::tan, crt_tan, ang));
    res.push_back(run1("sincos.s", ds_sin, crt_sin, ang));
    res.push_back(run1("sincos.c", ds_cos, crt_cos, ang));
    {
        Result r = run1("sin.huge", dm::sin, crt_sin, huge);
        r.max_ulp = 0;  // fmod reduction: digest only (see dmath.hpp)
        res.push_back(r);
        Result c = run1("cos.huge", dm::cos, crt_cos, huge);
        c.max_ulp = 0;
        res.push_back(c);
    }
    res.push_back(run1("asin", dm::asin, crt_asin, unit));
    res.push_back(run1("acos", dm::acos, crt_acos, unit));
    res.push_back(run1("atan", dm::atan, crt_atan, atin));
    res.push_back(run2("atan2", dm::atan2, crt_atan2, ya, xa));
    res.push_back(run1("exp", dm::exp, crt_exp, expin));
    res.push_back(run1("exp2", dm::exp2, crt_exp2, exp2in));
    res.push_back(run1("log", dm::log, crt_log, pos));
    res.push_back(run1("log2", dm::log2, crt_log2, pos));
    res.push_back(run1("log10", dm::log10, crt_log10, pos));
    res.push_back(run2("pow", dm::pow, crt_pow, px, py));
    res.push_back(run2("hypot", dm::hypot, crt_hypot, hx, hy));
    res.push_back(run1("sqrt", dm::sqrt, crt_sqrt, pos));
    res.push_back(run1f("sinf", dm::sin, crt_sin, angf));
    res.push_back(run1f("cosf", dm::cos, crt_cos, angf));
    res.push_back(run1f("tanf", dm::tan, crt_tan, angf));
    res.push_back(run1f("asinf", dm::asin, crt_asin, unit));
    res.push_back(run1f("acosf", dm::acos, crt_acos, unit));
    res.push_back(run1f("atanf", dm::atan, crt_atan, atin));
    res.push_back(run2f("atan2f", dm::atan2, crt_atan2, ya, xa));
    res.push_back(run1f("expf", dm::exp, crt_exp, expf_in));
    res.push_back(run1f("exp2f", dm::exp2, crt_exp2, expf_in));
    res.push_back(run1f("logf", dm::log, crt_log, posf));
    res.push_back(run1f("log2f", dm::log2, crt_log2, posf));
    res.push_back(run1f("log10f", dm::log10, crt_log10, posf));
    res.push_back(run2f("powf", dm::pow, crt_pow, pxf, pyf));
    res.push_back(run2f("hypotf", dm::hypot, crt_hypot, hx, hy));
    res.push_back(run1f("sqrtf", dm::sqrt, crt_sqrt, posf));
    {
        // Retail rsqrt: digest only (it is an approximation by design; relative error printed).
        Result r;
        r.name = "rsqrt_retail";
        alamo::StateHasher h;
        double worst = 0;
        for (double xd : posf) {
            const float x = static_cast<float>(xd);
            const float y = dm::fast_rsqrt_retail(x);
            h.f32(y);
            const double rel = std::fabs(static_cast<double>(y) * std::sqrt(static_cast<double>(x)) - 1.0);
            if (x >= std::numeric_limits<float>::min() && rel > worst) worst = rel;  // normals only
        }
        r.digest = h.digest();
        r.count = posf.size();
        std::printf("  rsqrt_retail max relative error %.3g\n", worst);
        CHECK(worst < 1e-5);
        res.push_back(r);
    }

    // Accuracy bounds vs the CRT (the CRT itself is not correctly rounded; these are loose).
    for (const auto& r : res) {
        const bool is_float = r.name.back() == 'f';
        const uint64_t bound = r.name.rfind("sqrt", 0) == 0 ? 0 : (is_float ? 1 : 4);
        std::printf("  %-9s n=%6llu  max %llu ulp vs CRT%s  digest %016llx", r.name.c_str(),
                    static_cast<unsigned long long>(r.count), static_cast<unsigned long long>(r.max_ulp),
                    is_float ? " (double, rounded)" : "", static_cast<unsigned long long>(r.digest));
        if (r.max_ulp) std::printf("  worst at %.17g %.17g", r.worst_input, r.worst_input2);
        std::printf("\n");
        if (r.max_ulp > bound) {
            std::printf("FAIL %s: %llu ulp > bound %llu\n", r.name.c_str(), static_cast<unsigned long long>(r.max_ulp),
                        static_cast<unsigned long long>(bound));
            ++g_fail;
        }
    }

    // Golden digests (identical for MSVC, clang and gcc).
    std::string text = "# alamo::dmath output digests (StateHasher over result bits); generated by alamo_core_dmath_tests --write-golden\n";
    for (const auto& r : res) {
        char line[128];
        std::snprintf(line, sizeof line, "%s %016llx %llu\n", r.name.c_str(), static_cast<unsigned long long>(r.digest),
                      static_cast<unsigned long long>(r.count));
        text += line;
    }
    if (!write_path.empty()) {
        std::string err;
        CHECK(alamo::write_text_file(write_path, text, &err));
        std::printf("wrote %s\n", write_path.c_str());
    }
    if (!golden_path.empty()) {
        std::string golden, err;
        if (!alamo::read_text_file(golden_path, golden, &err)) {
            std::printf("FAIL golden: %s\n", err.c_str());
            ++g_fail;
        } else {
            std::string g2;
            for (char ch : golden)
                if (ch != '\r') g2 += ch;
            if (g2 != text) {
                std::printf("FAIL golden digests differ from %s - this build computes different bits:\n%s",
                            golden_path.c_str(), text.c_str());
                ++g_fail;
            } else {
                std::printf("golden digests match (%zu functions)\n", res.size());
            }
        }
    }

    std::printf(g_fail ? "alamo_core_dmath_tests: %d FAILED\n" : "alamo_core_dmath_tests: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
