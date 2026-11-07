// S7 movement: shared 2D math, units and constants (docs/re/MOVEMENT.md).
//
// Units are FRAMES (30 Hz logic), never seconds: speeds are world units per frame, accelerations
// units per frame^2, turn rates degrees per frame, planner times frames (§0.1). Nothing is
// multiplied by a float dt.
//
// Determinism (rule 3, §7): every function here is a fixed sequence of float +, -, *, /, IEEE sqrt
// and alamo::dmath calls (no CRT transcendentals, no FMA). Direction normalisation that feeds
// positions uses the retail fast inverse square root (dmath::fast_rsqrt_retail, §3.5 step 5).
#pragma once
#include <cmath>
#include <cstdint>
#include <functional>

#include "alamo/core/dmath.hpp"

namespace alamo::tactical::movement {

using ObjectId = int32_t;  // gameplay object id (objects::ObjectId)

struct V2 {
    float x = 0.0f, y = 0.0f;
    friend bool operator==(V2 a, V2 b) { return a.x == b.x && a.y == b.y; }
};
inline V2 operator+(V2 a, V2 b) { return {a.x + b.x, a.y + b.y}; }
inline V2 operator-(V2 a, V2 b) { return {a.x - b.x, a.y - b.y}; }
inline V2 operator*(V2 a, float s) { return {a.x * s, a.y * s}; }
inline float dot(V2 a, V2 b) { return a.x * b.x + a.y * b.y; }
inline float cross(V2 a, V2 b) { return a.x * b.y - a.y * b.x; }
inline float length_sq(V2 a) { return a.x * a.x + a.y * a.y; }
inline float length(V2 a) { return std::sqrt(length_sq(a)); }  // IEEE sqrt: exact
inline float distance(V2 a, V2 b) { return length(b - a); }
inline V2 perp_left(V2 a) { return {-a.y, a.x}; }
inline V2 lerp(V2 a, V2 b, float t) { return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t}; }

// Retail normalisation (fast inverse sqrt, magic 0x5f375a86, two Newton steps). Zero stays zero.
inline V2 normalize_retail(V2 v) {
    const float l2 = length_sq(v);
    if (!(l2 > 0.0f)) return {0.0f, 0.0f};
    const float r = dmath::fast_rsqrt_retail(l2);
    return {v.x * r, v.y * r};
}

inline constexpr float kDegToRad = 0.017453292519943295f;
inline constexpr float kRadToDeg = 57.295779513082321f;

// Angle wrapped into (-180, 180].
inline float wrap_deg(float a) {
    float r = std::fmod(a, 360.0f);
    if (r > 180.0f) r -= 360.0f;
    if (r <= -180.0f) r += 360.0f;
    return r;
}
// Angle wrapped into [0, 360).
inline float norm_deg(float a) {
    float r = std::fmod(a, 360.0f);
    if (r < 0.0f) r += 360.0f;
    if (r >= 360.0f) r -= 360.0f;
    return r;
}
// Heading (degrees, 0 = +X, counter-clockwise) of a direction; 0 for the zero vector.
inline float heading_deg(V2 d) {
    if (d.x == 0.0f && d.y == 0.0f) return 0.0f;
    return dmath::atan2(d.y, d.x) * kRadToDeg;
}
inline V2 dir_from_deg(float deg) {
    float s = 0.0f, c = 0.0f;
    dmath::sincos(deg * kDegToRad, s, c);
    return {c, s};
}
// Step `from` toward `to` (degrees) by at most `max_step` >= 0; result wrapped into (-180, 180].
inline float turn_toward(float from, float to, float max_step) {
    const float err = wrap_deg(to - from);
    if (err > max_step) return wrap_deg(from + max_step);
    if (err < -max_step) return wrap_deg(from - max_step);
    return wrap_deg(to);
}

// ---- grid (§1.1) ---------------------------------------------------------------------------
inline constexpr float kCellSize = 20.0f;  // one cell per terrain vertex
inline constexpr float kHalfCell = 10.0f;  // cell (x, y) is centred on world (20x, 20y)

// cell = floor((world + 10) / 20) (port: floor everywhere, §1.1).
inline int32_t world_to_cell(float w) { return static_cast<int32_t>(std::floor((w + kHalfCell) / kCellSize)); }
inline V2 cell_center(int32_t x, int32_t y) {
    return {static_cast<float>(x) * kCellSize, static_cast<float>(y) * kCellSize};
}

// "No facing requested" (move parameter block +0x18 = 1e18, §6.1).
inline constexpr float kNoFacing = 1e18f;

enum class MoveMode : uint8_t { Land = 1, Space = 2 };

// ---- threat-aware planning (REMAINING_GAPS §4.2) ------------------------------------------------------------------
// Threshold T comes with the move order (Lua Move_To(target, T); 1e18 = off). Request (0x417520): weight = 1 / max(T,
// 1) [M]. Cost rule (space 0x599380 ops 25-32, land region planner 0x5a38a0): t = threat at the point for the owner
// (TacticalPerceptionGrid point query 0x653ff0); when t >= T the step cost is multiplied by max(1, t * weight + 1).
inline constexpr float kThreatOff = 1e18f;
using ThreatFn = std::function<float(V2 p)>;  // threat at a point for the requesting owner; const, thread-safe
struct ThreatCost {
    const ThreatFn* fn = nullptr;
    float threshold = kThreatOff;
    float weight = 0.0f;
    bool active() const { return threshold != kThreatOff; }
    // Cost multiplier at p (1 without a threat grid or below the threshold).
    float factor(V2 p) const {
        if (!active() || !fn || !*fn) return 1.0f;
        const float t = (*fn)(p);
        if (!(t >= threshold)) return 1.0f;
        const float m = t * weight + 1.0f;
        return m > 1.0f ? m : 1.0f;
    }
    static ThreatCost make(const ThreatFn* f, float threshold) {
        ThreatCost c;
        c.fn = f;
        c.threshold = threshold;
        c.weight = 1.0f / (threshold > 1.0f ? threshold : 1.0f);
        return c;
    }
};

}  // namespace alamo::tactical::movement
