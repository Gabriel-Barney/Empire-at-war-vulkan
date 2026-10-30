#include "alamo/tactical/combat/formulas.hpp"

#include <cmath>

#include "alamo/core/dmath.hpp"

namespace alamo::tactical::combat {

uint32_t next_scan_frame(uint32_t now, uint32_t fps, SimRng& rng) {
    const int32_t extra = rng.range_int(0, static_cast<int32_t>(fps / 2));
    return now + fps + static_cast<uint32_t>(extra);
}

Recharge hp_pulse_recharge(float pulse_delay_seconds, int32_t pulses_left, float W, float F, uint32_t fps) {
    Recharge r;
    int32_t frames = static_cast<int32_t>(static_cast<float>(fps) * pulse_delay_seconds);
    frames = static_cast<int32_t>(static_cast<float>(frames) * W + 0.5f);  // W unconditional on this branch
    if (F <= 0.0f) return r;                                                 // frames and pulses become 0
    r.frames = static_cast<int32_t>(static_cast<float>(frames) / F + 0.5f);
    r.pulses = pulses_left;
    return r;
}

Recharge hp_full_recharge(float min_seconds, float max_seconds, int32_t pulse_count, float W, float F, uint32_t fps,
                          SimRng& rng) {
    Recharge r;
    const int32_t lo = static_cast<int32_t>(min_seconds * 100.0f);  // cvttss2si: truncated, not rounded
    const int32_t hi = static_cast<int32_t>(max_seconds * 100.0f);
    const int32_t v = rng.range_int(lo, hi);
    const auto product = static_cast<int32_t>(static_cast<float>(v) * static_cast<float>(fps));
    int32_t frames = static_cast<int32_t>(static_cast<uint32_t>(product) / 100u);  // unsigned divide
    if (W < 1.0f) frames = static_cast<int32_t>(static_cast<float>(frames) * W + 0.5f);  // only shortens
    if (F > 0.0f && F < 1.0f) frames = static_cast<int32_t>(static_cast<float>(frames) / F + 0.5f);
    r.frames = frames;
    r.pulses = static_cast<int32_t>(static_cast<float>(pulse_count) * (F > 0.0f ? F : 0.0f) + 0.5f);
    return r;
}

int32_t hp_first_cooldown(float max_seconds, uint32_t fps, SimRng& rng) {
    return rng.range_int(0, static_cast<int32_t>(static_cast<float>(fps) * max_seconds));
}

Recharge weapon_reset(float recharge_seconds, int32_t pulse_count, float W, float F, uint32_t fps, SimRng& rng) {
    Recharge r;
    int32_t frames = static_cast<int32_t>(recharge_seconds * static_cast<float>(fps)) + rng.range_int(0, 10);
    if (W < 1.0f) frames = static_cast<int32_t>(static_cast<float>(frames) * W + 0.5f);
    if (F <= 0.0f) return r;  // [L]
    r.frames = static_cast<int32_t>(static_cast<float>(frames) / F);          // no +0.5
    r.pulses = static_cast<int32_t>(F * static_cast<float>(pulse_count));     // no +0.5
    return r;
}

int32_t weapon_pulse_delay(float pulse_delay_seconds, float F, uint32_t fps) {
    const int32_t frames = static_cast<int32_t>(static_cast<float>(fps) * pulse_delay_seconds);
    if (F <= 0.0f) return frames;
    return static_cast<int32_t>(static_cast<float>(frames) / F);
}

float inaccuracy_f(GameMode mode, float ai_accuracy) {
    float f = 1.0f;
    if (mode == GameMode::Land) f = f - ai_accuracy;
    return f;
}

Vec3 inaccuracy_offset(float r, SimRng& rng) {
    Vec3 o;
    o.x = rng.range_float(-r, r);
    o.y = rng.range_float(-r, r);
    o.z = rng.range_float(-r, r);
    return o;
}

float distance_xy(Vec3 a, Vec3 b) { return dmath::sqrt(distance2_xy(a, b)); }

float distance3(Vec3 a, Vec3 b) {
    const Vec3 d = a - b;
    return dmath::sqrt(dot(d, d));
}

Vec3 linear_lead(Vec3 muzzle, Vec3 target, Vec3 v, float s) {
    const Vec3 d = target - muzzle;
    const float a = dot(v, v) - s * s;
    const float b = 2.0f * dot(d, v);
    const float c = dot(d, d);
    float t = -1.0f;
    if (std::fabs(a) < 1e-6f) {
        if (b < 0.0f) t = -c / b;
    } else {
        const float disc = b * b - 4.0f * a * c;
        if (disc >= 0.0f) {
            const float q = dmath::sqrt(disc);
            const float t1 = (-b - q) / (2.0f * a);
            const float t2 = (-b + q) / (2.0f * a);
            const float lo = t1 < t2 ? t1 : t2;
            const float hi = t1 < t2 ? t2 : t1;
            t = lo >= 0.0f ? lo : hi;
        }
    }
    if (!(t >= 0.0f)) return target;
    return target + v * t;
}

Vec3 circular_lead(Vec3 muzzle, Vec3 p0, Vec3 p1, Vec3 p2, Vec3 offset, float s, uint32_t fps) {
    const float f = static_cast<float>(fps);
    const Vec3 v = (p2 - p1) * f;
    const Vec3 linear = linear_lead(muzzle, p2 + offset, v, s);
    // XY circumcircle of p0, p1, p2.
    const float ax = p1.x - p0.x, ay = p1.y - p0.y;
    const float bx = p2.x - p1.x, by = p2.y - p1.y;
    const float cross = ax * by - ay * bx;
    if (cross == 0.0f) return linear;  // straight (or repeated points)
    const float cx = p2.x - p0.x, cy = p2.y - p0.y;
    const float la = dmath::sqrt(ax * ax + ay * ay), lb = dmath::sqrt(bx * bx + by * by), lc = dmath::sqrt(cx * cx + cy * cy);
    const float r = la * lb * lc / (2.0f * std::fabs(cross));
    if (!(r <= kLeadMaxCircleRadius)) return linear;
    // Centre: intersection of the perpendicular bisectors (standard circumcentre formula, relative to p0).
    const float d = 2.0f * (ax * cy - ay * cx);
    if (d == 0.0f) return linear;
    const float a2 = ax * ax + ay * ay, c2 = cx * cx + cy * cy;
    const float ux = (cy * a2 - ay * c2) / d, uy = (ax * c2 - cx * a2) / d;
    const float ox = p0.x + ux, oy = p0.y + uy;
    // Angular rate of the last step (radians per second, sign = turn direction).
    const float r1x = p1.x - ox, r1y = p1.y - oy, r2x = p2.x - ox, r2y = p2.y - oy;
    const float w = dmath::atan2(r1x * r2y - r1y * r2x, r1x * r2x + r1y * r2y) * f;
    const float vz = (p2.z - p1.z) * f;
    auto pos_at = [&](float t) {
        float sn = 0.0f, cs = 1.0f;
        dmath::sincos(w * t, sn, cs);
        return Vec3{ox + (r2x * cs - r2y * sn) + offset.x, oy + (r2x * sn + r2y * cs) + offset.y, p2.z + vz * t + offset.z};
    };
    auto fn = [&](float t) {
        const Vec3 dd = pos_at(t) - muzzle;
        return dmath::sqrt(dot(dd, dd)) - s * t;
    };
    // Bracketed false position with the Illinois modification (superlinear; plain bisection cannot reach the
    // 0.001 tolerance over [0.1, 1000] in 15 steps).
    float a = kLeadRootLo, b = kLeadRootHi;
    float fa = fn(a);
    if (!(fa > 0.0f)) return pos_at(a);  // the shot is there by the lower bound
    float fb = fn(b);
    if (!(fb < 0.0f)) return linear;     // no sign change in the bracket
    float t = a;
    int side = 0;
    for (int i = 0; i < kLeadRootIterations; ++i) {
        t = (fa * b - fb * a) / (fa - fb);
        const float ft = fn(t);
        if (std::fabs(ft) < kLeadRootTolerance || b - a < kLeadRootTolerance) break;
        if (ft < 0.0f) {  // same sign as f(b)
            b = t;
            fb = ft;
            if (side == -1) fa = fa * 0.5f;
            side = -1;
        } else {
            a = t;
            fa = ft;
            if (side == 1) fb = fb * 0.5f;
            side = 1;
        }
    }
    return pos_at(t);
}

Vec3 lob_direction(Vec3 muzzle, Vec3 aim, float v, float g) {
    const Vec3 d = aim - muzzle;
    const float dh = dmath::sqrt(d.x * d.x + d.y * d.y);
    if (!(dh > 0.0f) || !(g > 0.0f) || !(v > 0.0f)) {
        const float l = dmath::sqrt(dot(d, d));
        return l > 0.0f ? d * (1.0f / l) : Vec3{1.0f, 0.0f, 0.0f};
    }
    const float v2 = v * v;
    const float disc = v2 * v2 - g * (g * dh * dh + 2.0f * d.z * v2);
    const float tan_t = disc >= 0.0f ? (v2 + dmath::sqrt(disc)) / (g * dh) : 1.0f;
    const float c = 1.0f / dmath::sqrt(1.0f + tan_t * tan_t);
    const float hx = d.x / dh, hy = d.y / dh;
    return Vec3{hx * c, hy * c, tan_t * c};
}

float area_falloff(float d, float R, int32_t tiers) {
    if (!(R > 0.0f)) return 1.0f;
    const float k = std::floor(d * static_cast<float>(tiers) / R);
    float f = 1.0f - k / static_cast<float>(tiers + 1);
    if (f < 0.0f) f = 0.0f;
    if (f > 1.0f) f = 1.0f;
    return f;
}

float area_delay_seconds(float d, float R, float max_secs) {
    if (!(d >= 1.0f) || !(R > 0.0f)) return 0.0f;
    return d / R * max_secs;
}

int32_t delay_frames(float delay_seconds, uint32_t fps) {
    const int32_t f = static_cast<int32_t>(static_cast<float>(fps) * delay_seconds);
    return f < 1 ? 1 : f;
}

int32_t shield_regen_interval_frames(uint32_t fps, float regen_interval_mult, float recharge_interval_secs) {
    return static_cast<int32_t>(static_cast<float>(fps) * regen_interval_mult * recharge_interval_secs + 0.5f);
}

bool shield_lockout_active(uint32_t frame, uint32_t depletion_frame, uint32_t fps, float disable_time) {
    const float elapsed = static_cast<float>(frame - depletion_frame) / static_cast<float>(fps);
    return elapsed < disable_time;
}

float shield_regen_step(float current, float amount, float max_shield_points, bool lockout, float depleted_cap) {
    float n = current + amount;
    if (n < 0.0f) n = 0.0f;
    if (n > max_shield_points) n = max_shield_points;
    if (lockout && depleted_cap < n) n = depleted_cap;  // minss with the absolute cap
    return n;
}

}  // namespace alamo::tactical::combat
