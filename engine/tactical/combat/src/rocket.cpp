// refixf t01 - ROCKET / MPTL_ROCKET flight maths (docs/re/S9_DEATH_CLONE_SHIELDS.md §2.4). See rocket.hpp.
#include "alamo/tactical/combat/rocket.hpp"

#include <bit>
#include <cmath>
#include <cstdint>
#include <vector>

#include "alamo/core/dmath.hpp"

namespace alamo::tactical::combat {

namespace {

constexpr float kTwoPiDraw = 6.2831855f;     // Get_Float upper bound of the jitter angle (§2.4.5)
constexpr float kDegToRad = 0.017453292f;    // FIGHTER_FLYING §1.1
constexpr float kRadToDeg = 57.295776f;      // FIGHTER_FLYING §1.2
constexpr int kLengthMaxDepth = 40;          // [port] cap of the adaptive length recursion (never reached on finite data)

float abs_f(float v) { return v < 0.0f ? -v : v; }

// sin / cos with the tiny-angle shortcut (FIGHTER_FLYING §1.1): |x| < 2^-69 -> sin x = x, cos x = 1. [port] dmath instead
// of the CRT sinf / cosf.
void sin_cos(float x, float& s, float& c) {
    if ((std::bit_cast<uint32_t>(x) & 0x7F800000u) < 0x1D000001u) {
        s = x;
        c = 1.0f;
        return;
    }
    s = dmath::sin(x);
    c = dmath::cos(x);
}

// |v| with the squares summed y, x, z (the order the re-plan code adds them, §2.4).
float len_yxz(Vec3 v) { return dmath::sqrt((v.y * v.y + v.x * v.x) + v.z * v.z); }
float len2_yxz(Vec3 v) { return (v.y * v.y + v.x * v.x) + v.z * v.z; }

// Facing direction of the initial plans (§2.4.10 step 2.3): the angles of `f` from the origin (0x20acd0, FIGHTER_FLYING
// §1.2: yaw 0 when x and y are 0, pitch only when z or x differ - the y-only quirk), then the identity rotated by yaw
// and pitch: (cos Y cos Pi, sin Y cos Pi, -sin Pi), positive pitch nose down.
Vec3 facing_forward(Vec3 f) {
    float yaw = 0.0f, pitch = 0.0f;
    if (!(f.x == 0.0f && f.y == 0.0f)) {
        yaw = dmath::atan2(f.y, f.x) * kRadToDeg;
        if (yaw < 0.0f) yaw = yaw + 360.0f;
    }
    if (f.z != 0.0f || f.x != 0.0f) pitch = -(dmath::atan2(f.z, dmath::sqrt(f.y * f.y + f.x * f.x)) * kRadToDeg);
    float sy = 0.0f, cy = 1.0f, sp = 0.0f, cp = 1.0f;
    sin_cos(yaw * kDegToRad, sy, cy);
    sin_cos(pitch * kDegToRad, sp, cp);
    return Vec3{cy * cp, sy * cp, -sp};
}

Vec3 half_sum(Vec3 a, Vec3 b) { return Vec3{(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f, (a.z + b.z) * 0.5f}; }
Vec3 on_sphere(Vec3 E, float r, Vec3 dir) { return Vec3{E.x + r * dir.x, E.y + r * dir.y, E.z + r * dir.z}; }

// Truncation of a (re-)plan (§2.4.6 step 8, §2.4.10 step 2.6): when `at` < total and sample(at) gives S in segment
// j < count, control points j..end are erased and true is returned with S (the caller adjusts S.z and appends it).
bool truncate_at(RocketSpline& path, float at, Vec3& S) {
    if (!(at < path.total())) return false;
    uint32_t j = 0;
    if (!path.sample(at, S, &j)) return false;
    if (!(static_cast<size_t>(j) < path.count())) return false;
    path.erase_from(j);
    return true;
}

// Bail-outs, A / B and the exit point I of a re-plan (§2.4.6 steps 1-3). False = bail.
struct ReplanGeometry {
    Vec3 D, A, B, I;
    float r = 0.0f;
};

bool replan_geometry(Vec3 P, Vec3 E, Vec3 last_velocity, float k, ReplanGeometry& g) {
    g.D = rocket_fast_normalize(last_velocity);
    const Vec3 D = g.D;
    // [M] D = 0 (the rocket has not moved yet): retail's exit parameter is NaN and bails; bail here explicitly.
    if (D.x == 0.0f && D.y == 0.0f && D.z == 0.0f) return false;
    if (dot(D, E - P) < 0.0f) return false;  // emitter behind
    g.A = Vec3{P.x + D.x * k, P.y + D.y * k, P.z + D.z * k};
    g.B = Vec3{g.A.x + D.x * k, g.A.y + D.y * k, g.A.z + D.z * k};
    // Ray-sphere 0x20c280: sphere centre E through B (radius |B - E|, not the shield radius).
    const Vec3 O{g.B.x + D.x * 0.5f, g.B.y + D.y * 0.5f, g.B.z + D.z * 0.5f};
    const Vec3 F{g.B.x + D.x * 1e6f, g.B.y + D.y * 1e6f, g.B.z + D.z * 1e6f};
    const Vec3 R = F - O;
    const float len = len_yxz(R);
    if (!(len > 0.0f)) return false;
    const Vec3 Rn = R * (1.0f / len);
    g.r = len_yxz(g.B - E);
    const Vec3 w = O - E;
    const float b = dot(w, Rn) * 2.0f;
    const float disc = b * b - (dot(w, w) - g.r * g.r) * 4.0f;
    if (!(disc >= 0.0f)) return false;
    const float t = ((dmath::sqrt(disc) - b) * 0.5f) / len;
    if (!(t >= 0.0f && t < 1.0f)) return false;
    g.I = Vec3{O.x + R.x * t, O.y + R.y * t, O.z + R.z * t};
    return true;
}

// Arc directions U, V, M, Q, W, Q8 (§2.4.6 step 6).
struct ArcDirs {
    Vec3 U, V, M, Q, W, Q8;
};

ArcDirs arc_dirs(const ReplanGeometry& g, Vec3 E) {
    ArcDirs a;
    a.U = rocket_fast_normalize(g.B - E);
    a.V = rocket_fast_normalize(g.I - E);
    const Vec3 m = half_sum(a.V, a.U);
    if (len2_yxz(m) <= 1e-6f) {
        // [port] Degenerate arc (§2.4.9 open item): a rocket aimed at the emitter centre gives V = -U and M = fn(~0)
        // (noise or zero in retail). Pass on the horizontal side instead: M = fn(U x +z), or fn(U x +x) for a vertical U.
        Vec3 side = cross(a.U, Vec3{0.0f, 0.0f, 1.0f});
        if (len2_yxz(side) <= 1e-12f) side = cross(a.U, Vec3{1.0f, 0.0f, 0.0f});
        a.M = rocket_fast_normalize(side);
    } else {
        a.M = rocket_fast_normalize(m);
    }
    a.Q = rocket_fast_normalize(half_sum(a.M, a.U));
    a.W = rocket_fast_normalize(half_sum(a.M, a.V));
    a.Q8 = rocket_fast_normalize(half_sum(a.Q, a.U));
    return a;
}

}  // namespace

// ---- SplineClass (§2.4.3) --------------------------------------------------------------------------------------------

Vec3 RocketSpline::eval(uint32_t segment, float u) const {
    const std::array<float, 12>& k = segs_[segment].k;
    Vec3 p;
    p.x = ((u * k[3] + k[2]) * u + k[1]) * u + k[0];
    p.y = ((u * k[7] + k[6]) * u + k[5]) * u + k[4];
    p.z = ((u * k[11] + k[10]) * u + k[9]) * u + k[8];
    return p;
}

float RocketSpline::seg_length(uint32_t i, float t0, float t1, int depth) const {
    // 0x2313b0: adaptive chord length; the two convergence tests are made in double against 1e-6.
    const Vec3 p0 = eval(i, t0), p1 = eval(i, t1);
    const Vec3 dc = p0 - p1;
    const float chord = dmath::sqrt(dot(dc, dc));
    if (static_cast<double>(chord) < 1e-6) return chord;
    const float m = (t0 + t1) * 0.5f;
    const Vec3 pm = eval(i, m);
    const Vec3 da = p0 - pm, db = p1 - pm;
    const float a = dmath::sqrt(dot(da, da));
    const float b = dmath::sqrt(dot(db, db));
    const float sum = b + a;
    if (std::fabs(static_cast<double>(chord / sum - 1.0f)) <= 1e-6 || std::fabs(static_cast<double>(chord - sum)) <= 1e-6)
        return sum;
    if (depth >= kLengthMaxDepth) return sum;  // [port] guard (non-finite coefficients)
    return seg_length(i, t0, m, depth + 1) + seg_length(i, m, t1, depth + 1);
}

void RocketSpline::build(int32_t spacing) {
    const size_t n = pts_.size();
    if (n < 3) return;  // changes nothing (§2.4.3)
    segs_.assign(n - 1, Segment{});
    std::vector<float> gamma(n), delta(n), K(n), y(n);
    for (int axis = 0; axis < 3; ++axis) {
        for (size_t i = 0; i < n; ++i) y[i] = axis_of(pts_[i], axis);
        // 0x230950: natural cubic spline, float
        gamma[0] = 0.5f;
        for (size_t i = 1; i + 1 < n; ++i) gamma[i] = 1.0f / (4.0f - gamma[i - 1]);
        gamma[n - 1] = 1.0f / (2.0f - gamma[n - 2]);
        delta[0] = ((y[1] - y[0]) * 3.0f) * gamma[0];
        for (size_t i = 1; i + 1 < n; ++i) delta[i] = ((y[i + 1] - y[i - 1]) * 3.0f - delta[i - 1]) * gamma[i];
        delta[n - 1] = ((y[n - 1] - y[n - 2]) * 3.0f - delta[n - 2]) * gamma[n - 1];
        K[n - 1] = delta[n - 1];
        for (size_t i = n - 1; i-- > 0;) K[i] = delta[i] - gamma[i] * K[i + 1];
        const auto base = static_cast<size_t>(axis) * 4u;
        for (size_t i = 0; i + 1 < n; ++i) {
            std::array<float, 12>& k = segs_[i].k;
            k[base + 0] = y[i];
            k[base + 1] = K[i];
            k[base + 2] = ((y[i + 1] - y[i]) * 3.0f - K[i] * 2.0f) - K[i + 1];
            k[base + 3] = ((y[i] - y[i + 1]) * 2.0f + K[i]) + K[i + 1];
        }
    }
    total_ = 0.0f;
    for (uint32_t i = 0; i < static_cast<uint32_t>(segs_.size()); ++i) {
        segs_[i].length = seg_length(i, 0.0f, 1.0f, 0);
        total_ = total_ + segs_[i].length;
    }
    // Polyline table (+0x90): entry 0 at segment 0, u = 0; then c = ceil(L / spacing) clamped to 1..100 entries per
    // segment at u = j / c.
    poly_.clear();
    poly_.push_back(eval(0, 0.0f));
    const auto sp = static_cast<float>(spacing);
    for (uint32_t i = 0; i < static_cast<uint32_t>(segs_.size()); ++i) {
        const float q = std::ceil(segs_[i].length / sp);
        const int32_t c = !(q >= 1.0f) ? 1 : (q > 100.0f ? 100 : static_cast<int32_t>(q));
        for (int32_t j = 1; j <= c; ++j) poly_.push_back(eval(i, static_cast<float>(j) / static_cast<float>(c)));
    }
}

bool RocketSpline::sample(float s, Vec3& out, uint32_t* segment) const {
    float cum = 0.0f;
    for (uint32_t i = 0; i < static_cast<uint32_t>(segs_.size()); ++i) {
        const float L = segs_[i].length;
        if (s < L + cum) {
            out = eval(i, (s - cum) / L);
            if (segment) *segment = i;
            return true;
        }
        cum = cum + L;
    }
    return false;
}

// ---- helpers ---------------------------------------------------------------------------------------------------------

Vec3 rocket_fast_normalize(Vec3 v) {
    const float s = (v.x * v.x + v.y * v.y) + v.z * v.z;
    if (!(s > 0.0f)) return v;
    const float y = dmath::fast_rsqrt_retail(s);
    return Vec3{v.x * y, v.y * y, v.z * y};
}

void rocket_jitter(Vec3& p, Vec3 n1, Vec3 n2, float amp, SimRng& rng) {
    const float theta = rng.range_float(0.0f, kTwoPiDraw);  // first draw
    float s = 0.0f, c = 1.0f;
    sin_cos(theta, s, c);
    const float a = rng.range_float(0.0f, amp);  // second draw (also for amp 0)
    p.x = p.x + (c * n1.x + s * n2.x) * a;
    p.y = p.y + (c * n1.y + s * n2.y) * a;
    p.z = p.z + (c * n1.z + s * n2.z) * a;
}

void rocket_arc_subdivide(Vec3 a, Vec3 b, float r, float thr, std::vector<Vec3>& out, int depth) {
    float c = (a.x * b.x + a.y * b.y) + a.z * b.z;
    c = c > 1.0f ? 1.0f : (c < -1.0f ? -1.0f : c);  // [port] clamp: acos of a dot just above 1 is NaN in retail
    if (dmath::acos(c) * r <= thr) return;
    if (depth >= kRocketArcMaxDepth) return;  // [port] guard
    const Vec3 mid = rocket_fast_normalize(half_sum(a, b));
    rocket_arc_subdivide(a, mid, r, thr, out, depth + 1);
    out.push_back(mid);
    rocket_arc_subdivide(mid, b, r, thr, out, depth + 1);
}

// ---- leg 0x4c9990 (§2.4.4) -------------------------------------------------------------------------------------------

void rocket_leg(RocketSpline& path, Vec3 from, Vec3 to, float amp, float len_scale, int mode, bool ground_end,
                bool plain_end, const RocketContext& ctx) {
    const RocketParams& rp = *ctx.params;
    const float ampP = amp * rp.curve_offset;
    const float step = len_scale * rp.curve_distance;
    const Vec3 d = from - to;
    const Vec3 m{(from.x + to.x) * 0.5f + 1.0f, (from.y + to.y) * 0.5f + 1.0f, (from.z + to.z) * 0.5f + 1.0f};
    const Vec3 c1 = cross(m, d);
    const Vec3 c2 = cross(c1, d);
    Vec3 n1 = c1, n2 = c2;
    const float l1 = len2_yxz(c1), l2 = len2_yxz(c2);
    if (l1 > 0.0f) n1 = c1 * (1.0f / dmath::sqrt(l1));
    if (l2 > 0.0f) n2 = c2 * (1.0f / dmath::sqrt(l2));
    // n = (int)ceilf(|d| / (lenScale x CD)); nothing when n <= 0. [port] A quotient that is NaN, infinite or >= 2^31
    // (CD = 0) converts to 0x80000000 on x64 (n <= 0): nothing is appended.
    const float q = std::ceil(dmath::sqrt(dot(d, d)) / step);
    if (!(q >= 1.0f) || !(q < 2147483648.0f)) return;
    const auto n = static_cast<int32_t>(q);
    const float inv = 1.0f / static_cast<float>(n);
    float t = inv;
    // Mode 1 (land terrain following): the heightmap box shrunk by (amp' + 1), the launch altitude above ground.
    const RocketTerrain& tr = *ctx.terrain;
    float hxs = 0.0f, hys = 0.0f, alt = 0.0f, z_prev = 0.0f;
    if (mode == 1) {
        hxs = tr.hx - (ampP + 1.0f);
        hys = tr.hy - (ampP + 1.0f);
        alt = ctx.launch.z - tr.h(ctx.launch.x, ctx.launch.y);
        z_prev = path.count() > 0 ? path.points().back().z : from.z;
    }
    const float k07 = 1.0f - 0.3f;  // the float 0x140a2bba8 (computed this way in retail)
    for (int32_t k = 1; k <= n; ++k) {
        Vec3 p{(1.0f - t) * from.x + t * to.x, (1.0f - t) * from.y + t * to.y, (1.0f - t) * from.z + t * to.z};
        const bool last = k == n;
        if (mode != 1) {
            if (!last) rocket_jitter(p, n1, n2, ampP, *ctx.rng);
            path.append(p);
        } else if (tr.has_box && (abs_f(p.x - tr.cx) > hxs || abs_f(p.y - tr.cy) > hys)) {
            z_prev = p.z;  // outside the shrunk box: not appended, no draws
        } else if (last && plain_end) {
            if (ground_end) p.z = tr.h(p.x, p.y);
            path.append(p);
            z_prev = p.z;
        } else {
            rocket_jitter(p, n1, n2, ampP, *ctx.rng);
            const float g = tr.h(p.x, p.y) + alt;
            const float inner = p.z > g ? p.z : g;
            const float lo = k07 * z_prev;
            p.z = lo > inner ? lo : inner;
            path.append(p);
            z_prev = p.z;
        }
        t = t + inv;
    }
}

// ---- initial plans (§2.4.10) -----------------------------------------------------------------------------------------

void rocket_plan_land(RocketSpline& path, Vec3 P, Vec3 T, Vec3 facing, float& budget, const RocketContext& ctx) {
    const RocketParams& rp = *ctx.params;
    const RocketTerrain& tr = *ctx.terrain;
    const float mfd = rp.max_flight_distance;
    budget = mfd;
    const float dy = P.y - T.y, dx = P.x - T.x;
    const float dh = dmath::sqrt((dy * dy + dx * dx) + 0.0f);
    const float L1 = dh * 0.5f;  // Projectile_Rocket_Straight_Distance is not read on land
    path.clear();
    path.append(P);
    Vec3 K = P;
    if (L1 != 0.0f) {
        const Vec3 f = facing_forward(facing);
        K = Vec3{P.x + f.x * L1, P.y + f.y * L1, P.z + f.z * L1};
        rocket_leg(path, P, K, 0.1f, 1.1f, 1, false, false, ctx);  // leg 1: the last point is jittered too
    }
    rocket_leg(path, K, T, 1.0f, 1.1f, 1, rp.target_on_terrain, true, ctx);  // leg 2
    if (rp.explode_at_target) {
        path.build(10);
        budget = path.total();  // launch -> target length
    }
    if (!rp.target_on_terrain) {
        path.build(20);
        const Vec3 H = rocket_fast_normalize(Vec3{T.x - K.x, T.y - K.y, 0.0f});
        const float e = mfd - path.total();
        const Vec3 X{T.x + e * H.x, T.y + e * H.y, T.z};
        rocket_leg(path, T, X, 0.3f, 1.1f, 1, true, true, ctx);  // leg 3 (extension to MFD), from T exactly
        path.build(10);
        Vec3 S;
        if (truncate_at(path, mfd, S)) {
            S.z = tr.h(S.x, S.y);
            path.append(S);
        }
    }
    path.build(10);
}

void rocket_plan_space(RocketSpline& path, Vec3 P, Vec3 T, Vec3 facing, float& budget, const RocketContext& ctx) {
    const RocketParams& rp = *ctx.params;
    const float mfd = rp.max_flight_distance;
    budget = mfd;
    const float dy = P.y - T.y, dx = P.x - T.x;
    const float dh = dmath::sqrt((dy * dy + dx * dx) + 0.0f);
    const float half = dh * 0.5f;
    const float L1 = half < rp.straight_distance ? half : rp.straight_distance;  // minss: Straight wins ties
    path.clear();
    path.append(P);
    Vec3 K = P;
    if (L1 != 0.0f) {
        const Vec3 f = facing_forward(facing);
        K = Vec3{P.x + f.x * L1, P.y + f.y * L1, P.z + f.z * L1};
        rocket_leg(path, P, K, 0.1f, 1.1f, 0, false, false, ctx);
    }
    rocket_leg(path, K, T, 1.0f, 1.1f, 0, false, true, ctx);
    if (!rp.explode_at_target) {  // with the flag the path ends at the target and the budget stays MFD
        path.build(10);
        // X = T + e x (last polyline step), not normalised (about `spacing` = 10 long).
        Vec3 stepv;
        const std::vector<Vec3>& poly = path.polyline();
        if (poly.size() >= 2) {
            stepv = poly[poly.size() - 1] - poly[poly.size() - 2];
        } else {
            // [port] The path was never built (fewer than 3 control points): retail reads past an empty polyline. Use
            // the last control step scaled to the spacing (10) instead; nothing at all with a single point.
            const std::vector<Vec3>& pts = path.points();
            if (pts.size() >= 2) stepv = rocket_fast_normalize(pts[pts.size() - 1] - pts[pts.size() - 2]) * 10.0f;
        }
        const float e = mfd - path.total();
        const Vec3 X{T.x + e * stepv.x, T.y + e * stepv.y, T.z + e * stepv.z};
        rocket_leg(path, T, X, 0.3f, 1.1f, 0, false, true, ctx);  // leg 3
        path.build(10);
        Vec3 S;
        if (truncate_at(path, mfd, S)) path.append(S);  // no height change in space
    }
    path.build(10);
}

// ---- missile-shield re-plans (§2.4.6 / §2.4.7) -----------------------------------------------------------------------

bool rocket_replan_land(RocketSpline& path, Vec3 P, Vec3 E, Vec3 last_velocity, float& s, float& budget,
                        const RocketContext& ctx) {
    constexpr float k = 20.0f;
    const RocketParams& rp = *ctx.params;
    const RocketTerrain& tr = *ctx.terrain;
    const float s0 = s;
    ReplanGeometry g;
    if (!replan_geometry(P, E, last_velocity, k, g)) return false;
    path.clear();
    path.append(P);
    rocket_leg(path, P, g.A, 0.0f, 1.0f, 0, false, false, ctx);  // leg 1: ceilf(k / CD) points
    const ArcDirs a = arc_dirs(g, E);
    // Arc: Q8, sub(Q8, W), W at radius r, threshold CD x 0.5.
    std::vector<Vec3> arc;
    arc.push_back(a.Q8);
    rocket_arc_subdivide(a.Q8, a.W, g.r, rp.curve_distance * 0.5f, arc);
    arc.push_back(a.W);
    for (const Vec3& dir : arc) path.append(on_sphere(E, g.r, dir));
    const Vec3 last = on_sphere(E, g.r, a.W);
    const Vec3 H = rocket_fast_normalize(Vec3{g.D.x, g.D.y, 0.0f});
    if (!rp.target_on_terrain) {
        // [port] kept as read: with MFD - s0 < k the end leg points backwards and a truncation at a negative distance
        // leaves < 3 control points, so the final build keeps the coefficients of Build(10) (well defined here).
        const float rem = rp.max_flight_distance - s0;
        const float q = rem - 20.0f;
        const Vec3 T{last.x + H.x * q, last.y + H.y * q, last.z};
        rocket_leg(path, last, T, 1.0f, 1.0f, 1, true, true, ctx);  // leg 2
        path.build(10);
        Vec3 S;
        if (path.total() > rem && truncate_at(path, rem, S)) {
            if (len2_yxz(S - E) > g.r * g.r) S.z = tr.h(S.x, S.y) - 5.0f;
            path.append(S);
        }
    } else {
        const float q = (g.r - len_yxz(last - E)) + 20.0f;
        const float x = last.x + H.x * q, y = last.y + H.y * q;
        path.append(Vec3{x, y, tr.h(x, y) - 5.0f});  // the ground point; no leg 2, no draws
    }
    if (rp.explode_at_target) budget = budget - s0;  // the remaining budget carries over
    path.build(20);
    s = 0.0f;
    return true;
}

bool rocket_replan_space(RocketSpline& path, Vec3 P, Vec3 E, Vec3 last_velocity, float& s, float& budget,
                         const RocketContext& ctx) {
    constexpr float k = 30.0f;
    const RocketParams& rp = *ctx.params;
    const float s0 = s;
    ReplanGeometry g;
    if (!replan_geometry(P, E, last_velocity, k, g)) return false;
    path.clear();
    path.append(P);
    rocket_leg(path, P, g.A, 0.0f, 1.0f, 0, false, false, ctx);
    const ArcDirs a = arc_dirs(g, E);
    // Arc: Q8, sub(U, W), W at threshold CD; with more than two entries the first two are dropped.
    std::vector<Vec3> arc;
    arc.push_back(a.Q8);
    rocket_arc_subdivide(a.U, a.W, g.r, rp.curve_distance, arc);
    arc.push_back(a.W);
    const size_t first = arc.size() > 2 ? 2u : 0u;
    for (size_t i = first; i < arc.size(); ++i) path.append(on_sphere(E, g.r, arc[i]));
    const Vec3 last = on_sphere(E, g.r, a.W);
    const Vec3 H = rocket_fast_normalize(Vec3{g.D.x, g.D.y, 0.0f});
    const float q = (rp.max_flight_distance - s0) - 30.0f;
    const Vec3 T{last.x + H.x * q, last.y + H.y * q, last.z};
    rocket_leg(path, last, T, 1.0f, 1.0f, 0, false, true, ctx);
    path.build(10);
    Vec3 S;
    if (path.total() > rp.max_flight_distance && truncate_at(path, rp.max_flight_distance, S)) path.append(S);
    if (rp.explode_at_target) budget = budget - s0;
    path.build(10);
    s = 0.0f;
    return true;
}

// ---- S8 t06: the terrain / water segment test 0x135840 (docs/re/LOB_SOLVER.md §6.6) ----

bool ray_triangle(Vec3 O, Vec3 D, Vec3 v0, Vec3 v1, Vec3 v2, float& best) {
    // 0x20c410: Moeller-Trumbore without back-face culling, in the doc's float order (no FMA).
    const Vec3 e1{v1.x - v0.x, v1.y - v0.y, v1.z - v0.z};
    const Vec3 e2{v2.x - v0.x, v2.y - v0.y, v2.z - v0.z};
    const Vec3 P{D.y * e2.z - D.z * e2.y, D.z * e2.x - D.x * e2.z, D.x * e2.y - D.y * e2.x};
    const float det = (P.y * e1.y + P.x * e1.x) + P.z * e1.z;
    if (abs_f(det) < 0.0001f) return false;
    const float inv = 1.0f / det;
    const Vec3 T{O.x - v0.x, O.y - v0.y, O.z - v0.z};
    const float u = ((T.y * P.y + T.x * P.x) + T.z * P.z) * inv;
    if (!(u >= 0.0f && u <= 1.0f)) return false;
    const Vec3 Q{T.y * e1.z - T.z * e1.y, T.z * e1.x - T.x * e1.z, T.x * e1.y - T.y * e1.x};
    const float v = ((Q.y * D.y + Q.x * D.x) + Q.z * D.z) * inv;
    if (!(v >= 0.0f && v + u <= 1.0f)) return false;
    const float t = ((Q.y * e2.y + Q.x * e2.x) + Q.z * e2.z) * inv;
    if (!(t > 0.0f && t < best)) return false;
    best = t;
    return true;
}

namespace {

// 0x135a70: grid vertex (x, y); the height lookup clamps x / y to the grid, the position does not.
Vec3 grid_vertex(const GroundGrid& g, int64_t x, int64_t y) {
    const int64_t w = g.width, h = g.height;
    const int64_t cx = x < 0 ? 0 : (x >= w ? w - 1 : x);
    const int64_t cy = y < 0 ? 0 : (y >= h ? h - 1 : y);
    const int16_t raw = g.heights[static_cast<size_t>(cy * w + cx)];
    return Vec3{static_cast<float>(x) * g.cell, static_cast<float>(y) * g.cell,
                static_cast<float>(static_cast<int32_t>(raw)) * g.height_scale};
}

// First / last cell index (inclusive) whose closed span [i c, (i + 1) c] meets [lo, hi], clamped to [0, cells - 1].
// False when none.
bool cell_span(float lo, float hi, float c, uint32_t cells, int64_t& first, int64_t& last) {
    if (!(c > 0.0f) || cells == 0) return false;
    const float n = static_cast<float>(cells);
    float a = std::ceil(lo / c) - 1.0f, b = std::floor(hi / c);
    if (!(a <= n) || !(b >= 0.0f)) return false;  // also NaN
    if (a < 0.0f) a = 0.0f;
    if (b > n - 1.0f) b = n - 1.0f;
    first = static_cast<int64_t>(a);
    last = static_cast<int64_t>(b);
    return first <= last;
}

// 0x20c180: the plane z = h, both directions.
bool ray_plane_z(Vec3 O, Vec3 D, float h, float& best) {
    const float denom = D.z;
    if (!(abs_f(denom) >= 0.0001f)) return false;
    const float t = -(O.z - h) / denom;
    if (!(t > 0.0f && t < best)) return false;
    best = t;
    return true;
}

}  // namespace

bool ground_segment(const GroundGrid& g, Vec3 from, Vec3 to, GroundHit& out) {
    // 1. O = from, D = to - from (not normalised); the best t starts at 1.
    const Vec3 O = from;
    const Vec3 D{to.x - from.x, to.y - from.y, to.z - from.z};
    float best = 1.0f;
    bool hit = false, water = false;
    if (g.present()) {
        // 2.-5. Every cell the segment's XY box touches: the patch / BVH walk visits a superset of the cells the segment
        // can hit and keeps the nearest. [port] Cells in y, then x order; the first of equal t is kept.
        int64_t x0 = 0, x1 = -1, y0 = 0, y1 = -1;
        const float minx = from.x < to.x ? from.x : to.x, maxx = from.x < to.x ? to.x : from.x;
        const float miny = from.y < to.y ? from.y : to.y, maxy = from.y < to.y ? to.y : from.y;
        if (cell_span(minx, maxx, g.cell, g.width - 1, x0, x1) && cell_span(miny, maxy, g.cell, g.height - 1, y0, y1)) {
            for (int64_t y = y0; y <= y1; ++y)
                for (int64_t x = x0; x <= x1; ++x) {
                    const Vec3 v00 = grid_vertex(g, x, y), v10 = grid_vertex(g, x + 1, y);
                    const Vec3 v11 = grid_vertex(g, x + 1, y + 1), v01 = grid_vertex(g, x, y + 1);
                    if (ray_triangle(O, D, v00, v10, v11, best)) hit = true;  // T1
                    if (ray_triangle(O, D, v00, v11, v01, best)) hit = true;  // T2
                }
        }
        // 6. The water plane (the toggle byte 0x140a123a1 is always set).
        if (g.water_type != 0 && ray_plane_z(O, D, g.water_height, best)) hit = water = true;
    } else if (ray_plane_z(O, D, 0.0f, best)) {  // [port] no grid: flat ground at z 0
        hit = true;
    }
    if (!hit) return false;
    out.t = best;
    out.point = Vec3{best * D.x + O.x, best * D.y + O.y, best * D.z + O.z};
    out.water = water;
    return true;
}

}  // namespace alamo::tactical::combat
