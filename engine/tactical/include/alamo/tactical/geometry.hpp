// Shapes and exact overlap predicates shared by the spatial tree and its brute-force reference.
//
// Determinism: every predicate is a fixed sequence of float compares, +, -, * (no FMA: the build
// forbids contraction), no division except the one 1/dir per ray (done once in PreparedRay), no
// transcendentals. The tree prunes with the SAME predicates applied to node boxes; because IEEE
// rounding is monotone, a box that fails a predicate cannot contain a sub-box that passes it, so
// tree results equal brute-force results bit for bit (tests check this on random scenes).
#pragma once
#include <cmath>
#include <cstdint>
#include <limits>

namespace alamo::tactical {

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float axis_of(Vec3 v, int axis) { return axis == 0 ? v.x : (axis == 1 ? v.y : v.z); }

inline bool is_finite(float f) { return f - f == 0.0f; }  // false for +-inf and NaN

// Axis-aligned box, inclusive on both ends. Valid boxes are finite with min <= max per axis.
struct Aabb {
    Vec3 min, max;
};

inline bool valid(const Aabb& b) {
    return is_finite(b.min.x) && is_finite(b.min.y) && is_finite(b.min.z) && is_finite(b.max.x) &&
           is_finite(b.max.y) && is_finite(b.max.z) && b.min.x <= b.max.x && b.min.y <= b.max.y &&
           b.min.z <= b.max.z;
}
inline bool overlaps(const Aabb& a, const Aabb& b) {
    return a.min.x <= b.max.x && a.max.x >= b.min.x && a.min.y <= b.max.y && a.max.y >= b.min.y &&
           a.min.z <= b.max.z && a.max.z >= b.min.z;
}
inline bool contains(const Aabb& outer, const Aabb& inner) {
    return outer.min.x <= inner.min.x && outer.min.y <= inner.min.y && outer.min.z <= inner.min.z &&
           outer.max.x >= inner.max.x && outer.max.y >= inner.max.y && outer.max.z >= inner.max.z;
}
inline bool operator==(const Aabb& a, const Aabb& b) {
    return a.min.x == b.min.x && a.min.y == b.min.y && a.min.z == b.min.z && a.max.x == b.max.x &&
           a.max.y == b.max.y && a.max.z == b.max.z;
}
inline Aabb merge(const Aabb& a, const Aabb& b) {
    return {{a.min.x < b.min.x ? a.min.x : b.min.x, a.min.y < b.min.y ? a.min.y : b.min.y,
             a.min.z < b.min.z ? a.min.z : b.min.z},
            {a.max.x > b.max.x ? a.max.x : b.max.x, a.max.y > b.max.y ? a.max.y : b.max.y,
             a.max.z > b.max.z ? a.max.z : b.max.z}};
}
inline Aabb expand(const Aabb& b, float margin) {
    return {{b.min.x - margin, b.min.y - margin, b.min.z - margin}, {b.max.x + margin, b.max.y + margin, b.max.z + margin}};
}
// Half surface area (insertion cost heuristic).
inline float half_area(const Aabb& b) {
    const float dx = b.max.x - b.min.x, dy = b.max.y - b.min.y, dz = b.max.z - b.min.z;
    return dx * dy + dy * dz + dz * dx;
}
inline Vec3 center(const Aabb& b) {
    return {(b.min.x + b.max.x) * 0.5f, (b.min.y + b.max.y) * 0.5f, (b.min.z + b.max.z) * 0.5f};
}

// ---- sphere / radius ---------------------------------------------------------------------
struct Sphere {
    Vec3 center;
    float radius = 0.0f;
};

inline float dist2_point_aabb(Vec3 p, const Aabb& b) {
    const float dx = p.x < b.min.x ? b.min.x - p.x : (p.x > b.max.x ? p.x - b.max.x : 0.0f);
    const float dy = p.y < b.min.y ? b.min.y - p.y : (p.y > b.max.y ? p.y - b.max.y : 0.0f);
    const float dz = p.z < b.min.z ? b.min.z - p.z : (p.z > b.max.z ? p.z - b.max.z : 0.0f);
    return dx * dx + dy * dy + dz * dz;
}
inline bool overlaps(const Sphere& s, const Aabb& b) { return dist2_point_aabb(s.center, b) <= s.radius * s.radius; }

// ---- 2D circle (XY, any Z) ----------------------------------------------------------------
// The original GOM radius query (TACTICAL_COMBAT.md 1.6) queries centre +- r in X and Y and
// +-1e18 in Z (Z is up), i.e. it is a 2D query. This shape is its exact-circle form: an entry
// matches when its box is within r of the centre in the XY plane, whatever its Z.
struct CircleXY {
    Vec3 center;  // z ignored
    float radius = 0.0f;
};
inline float dist2_xy_point_aabb(Vec3 p, const Aabb& b) {
    const float dx = p.x < b.min.x ? b.min.x - p.x : (p.x > b.max.x ? p.x - b.max.x : 0.0f);
    const float dy = p.y < b.min.y ? b.min.y - p.y : (p.y > b.max.y ? p.y - b.max.y : 0.0f);
    return dx * dx + dy * dy;
}
inline bool overlaps(const CircleXY& c, const Aabb& b) { return dist2_xy_point_aabb(c.center, b) <= c.radius * c.radius; }

// ---- frustum (6 planes) ------------------------------------------------------------------
// A point p is inside a plane when dot(n, p) + d >= 0. Normals need not be unit length.
struct Plane {
    Vec3 n;
    float d = 0.0f;
};
struct Frustum {
    Plane planes[6];
};

// dot(n, positive vertex) + d: the box is fully outside the plane when this is < 0.
inline float plane_max(const Plane& p, const Aabb& b) {
    return p.n.x * (p.n.x >= 0.0f ? b.max.x : b.min.x) + p.n.y * (p.n.y >= 0.0f ? b.max.y : b.min.y) +
           p.n.z * (p.n.z >= 0.0f ? b.max.z : b.min.z) + p.d;
}
// dot(n, negative vertex) + d: the box is fully inside the plane when this is >= 0.
inline float plane_min(const Plane& p, const Aabb& b) {
    return p.n.x * (p.n.x >= 0.0f ? b.min.x : b.max.x) + p.n.y * (p.n.y >= 0.0f ? b.min.y : b.max.y) +
           p.n.z * (p.n.z >= 0.0f ? b.min.z : b.max.z) + p.d;
}
// Conservative "positive vertex" test (as the original's Classify_AABB_vs_Frustum): true unless
// the box is fully outside one plane.
inline bool overlaps(const Frustum& f, const Aabb& b) {
    for (const Plane& p : f.planes)
        if (plane_max(p, b) < 0.0f) return false;
    return true;
}

// Frustum from its 8 corners (near quad then far quad, each in winding order). Planes face the
// corners' centroid; normals are not normalised (no sqrt), which the tests above do not need.
inline Frustum frustum_from_corners(const Vec3 near_q[4], const Vec3 far_q[4]) {
    Vec3 c{};
    for (int i = 0; i < 4; ++i) c = c + near_q[i] + far_q[i];
    c = c * 0.125f;
    auto make = [&c](Vec3 a, Vec3 b, Vec3 d) {
        Plane p;
        p.n = cross(b - a, d - a);
        p.d = -dot(p.n, a);
        if (dot(p.n, c) + p.d < 0.0f) {
            p.n = Vec3{} - p.n;
            p.d = -p.d;
        }
        return p;
    };
    Frustum f;
    f.planes[0] = make(near_q[0], near_q[1], near_q[2]);
    f.planes[1] = make(far_q[0], far_q[1], far_q[2]);
    for (int i = 0; i < 4; ++i) f.planes[2 + i] = make(near_q[i], near_q[(i + 1) & 3], far_q[i]);
    return f;
}

// ---- ray / segment -----------------------------------------------------------------------
// Points origin + dir * t for t in [0, max_t]. A segment a->b is {a, b - a, 1}.
struct Ray {
    Vec3 origin;
    Vec3 dir;
    float max_t = 1.0f;
};

// Ray with 1/dir precomputed once. Axes whose |dir| is below FLT_MIN (incl. subnormals) are
// treated as parallel, so no inf/NaN products ever reach the slab compares.
struct PreparedRay {
    float o[3];
    float inv[3];
    bool parallel[3];
    float max_t;

    explicit PreparedRay(const Ray& r) : o{r.origin.x, r.origin.y, r.origin.z}, inv{}, parallel{}, max_t(r.max_t) {
        const float d[3] = {r.dir.x, r.dir.y, r.dir.z};
        for (int a = 0; a < 3; ++a) {
            parallel[a] = !(std::fabs(d[a]) >= std::numeric_limits<float>::min());
            inv[a] = parallel[a] ? 0.0f : 1.0f / d[a];
        }
    }
};

// Slab test. On a hit, t_enter is the entry parameter clamped to [0, max_t] (0 when the origin
// is inside the box).
inline bool ray_aabb(const PreparedRay& r, const Aabb& b, float& t_enter) {
    const float lo[3] = {b.min.x, b.min.y, b.min.z};
    const float hi[3] = {b.max.x, b.max.y, b.max.z};
    float tmin = 0.0f, tmax = r.max_t;
    for (int a = 0; a < 3; ++a) {
        if (r.parallel[a]) {
            if (r.o[a] < lo[a] || r.o[a] > hi[a]) return false;
            continue;
        }
        float t1 = (lo[a] - r.o[a]) * r.inv[a];
        float t2 = (hi[a] - r.o[a]) * r.inv[a];
        if (t1 > t2) {
            const float s = t1;
            t1 = t2;
            t2 = s;
        }
        if (t1 > tmin) tmin = t1;
        if (t2 < tmax) tmax = t2;
        if (tmin > tmax) return false;
    }
    t_enter = tmin;
    return true;
}

}  // namespace alamo::tactical
