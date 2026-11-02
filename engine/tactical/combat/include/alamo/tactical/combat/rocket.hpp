// refixf t01 - ROCKET / MPTL_ROCKET flight (docs/re/S9_DEATH_CLONE_SHIELDS.md §2.4): the rocket spline (SplineClass,
// §2.4.3), the jittered leg builder (0x4c9990, §2.4.4 / §2.4.5), the land / space initial path builders (0x4c8b90 /
// 0x4c91a0, §2.4.10) and the missile-shield re-plans (0x4cceb0 / 0x4cdb00, §2.4.6 / §2.4.7). CombatSystem drives them
// (projectile step, §2.4.2); they are free functions here so the tests can pin the maths and the draw counts.
//
// Arithmetic is IEEE single in the order the doc gives (no FMA: /fp:precise, -ffp-contract=off). Transcendentals come
// from dmath (deterministic) instead of the CRT sinf / cosf / acosf the original calls [port: parity by behaviour, not
// bits]. Every synced draw is a SimRng::range_float (Get_Float 0x1ffbb0), two per jittered point, in path order.
#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

#include "alamo/core/sim_rng.hpp"
#include "alamo/tactical/geometry.hpp"

namespace alamo::tactical::combat {

// SplineClass (§2.4.3): control points, per-segment natural-cubic coefficients, segment lengths, total length.
class RocketSpline {
public:
    // One segment: per axis (x, y, z) the cubic a + b u + c u^2 + d u^3 as {a, b, c, d}.
    struct Segment {
        std::array<float, 12> k{};
        float length = 0.0f;
    };

    void clear() {
        pts_.clear();
        segs_.clear();
        poly_.clear();
        total_ = 0.0f;
    }
    void append(Vec3 p) { pts_.push_back(p); }  // 0x22fc80
    // Erase control points first..end (0x22fcc0 repeated; later points are dropped).
    void erase_from(size_t first) {
        if (first < pts_.size()) pts_.resize(first);
    }
    size_t count() const { return pts_.size(); }  // 0x22fe40
    const std::vector<Vec3>& points() const { return pts_; }
    const std::vector<Segment>& segments() const { return segs_; }
    float total() const { return total_; }
    bool built() const { return !segs_.empty(); }
    // Build 0x22ffc0(spacing): fewer than 3 control points -> returns and changes NOTHING (earlier coefficients,
    // lengths and polyline stay). Otherwise the natural cubic per axis, adaptive segment lengths and the polyline table.
    void build(int32_t spacing);
    // Sample 0x22fe70(s): the first segment i with s < L_i + cum gives u = (s - cum) / L_i. False past the end or when
    // the path was never built. A negative s lands in segment 0 with u < 0.
    bool sample(float s, Vec3& out, uint32_t* segment = nullptr) const;
    Vec3 eval(uint32_t segment, float u) const;  // the three cubics at u
    // Polyline table (+0x90) of the last build: x, y, z per entry (the length / derivative slots are not kept).
    // Plan-time scratch: only the space initial planner reads it, right after a build [port: not saved, not hashed].
    const std::vector<Vec3>& polyline() const { return poly_; }

    // Save / restore (combat_save.cpp): control points, segments and the total, verbatim (the "build with < 3 points
    // keeps the old coefficients" quirk makes the coefficients state of their own).
    void restore(std::vector<Vec3> pts, std::vector<Segment> segs, float total) {
        pts_ = std::move(pts);
        segs_ = std::move(segs);
        total_ = total;
        poly_.clear();
    }

private:
    float seg_length(uint32_t i, float t0, float t1, int depth) const;  // 0x2313b0
    std::vector<Vec3> pts_;
    std::vector<Segment> segs_;
    std::vector<Vec3> poly_;
    float total_ = 0.0f;
};

// The projectile type fields the rocket flight reads (pack / type offsets in §2.4).
struct RocketParams {
    float curve_distance = 0.0f;       // CD, Projectile_Rocket_Curve_Distance (type +0x1ff0)
    float curve_offset = 0.0f;         // CO, Projectile_Rocket_Curve_Offset (+0x1ff4)
    float straight_distance = 0.0f;    // Projectile_Rocket_Straight_Distance (+0x1fec), space initial plan only
    float max_flight_distance = 0.0f;  // MFD, Projectile_Max_Flight_Distance (+0x4bc)
    bool target_on_terrain = false;    // Projectile_Target_Point_On_Terrain (+0x7f)
    bool explode_at_target = false;    // Explode_When_Reached_Target_Radius (+0x7e)
};

// S8 t06 (docs/re/LOB_SOLVER.md §6.6): the terrain grid the projectile ground test 0x135840 intersects. Vertex (x, y) =
// (x cell, y cell, (float)heights[y width + x] x height_scale) (0x135a70; x / y clamped to the grid for the height lookup
// only); cells (x, y) with 0 <= x < width - 1, 0 <= y < height - 1 (the patches' cells). Water: a plane at z =
// water_height when water_type != 0 (1 water, 2 lava, 3 ice; TERRAIN_WATER_POST). Empty (width or height < 2, or too
// few heights) = no grid [port: the ground test then uses a plane at z 0].
struct GroundGrid {
    float cell = 20.0f;                  // terrain +0x9c (the port grid: terrain::kVertexSpacing)
    uint32_t width = 0, height = 0;      // vertices per row / column (terrain +0x10 / +0x14)
    std::vector<int16_t> heights;        // row-major, width x height
    float height_scale = 0.048828125f;   // terrain +0xa4 (terrain::kHeightScale)
    float water_height = 0.0f;           // terrain +0x200
    uint32_t water_type = 0;             // terrain +0x204
    bool present() const {
        return width >= 2 && height >= 2 && heights.size() >= static_cast<size_t>(width) * height;
    }
};

// Result of ground_segment: the segment fraction t in (0, 1) and the point t D + O.
struct GroundHit {
    float t = 1.0f;
    Vec3 point;
    bool water = false;  // the water plane (not a triangle) was nearest
};

// 0x135840: the segment from -> to (D = to - from, not normalised) against the two triangles of every grid cell its XY
// box touches (T1 = (v00, v10, v11), T2 = (v00, v11, v01); two-sided Moeller-Trumbore 0x20c410, det epsilon 0.0001,
// 0 <= u <= 1, v >= 0, v + u <= 1, 0 < t < best), then the water plane (0x20c180: |D.z| >= 0.0001, t = -(O.z - h) / D.z,
// 0 < t < best). Nearest t in (0, 1); false = no hit (t = 1). Float order as in the doc (no FMA).
bool ground_segment(const GroundGrid& g, Vec3 from, Vec3 to, GroundHit& out);
// 0x20c410 alone (exposed for the tests): true and best := t when the ray O + t D hits triangle (v0, v1, v2) with
// 0 < t < best.
bool ray_triangle(Vec3 O, Vec3 D, Vec3 v0, Vec3 v1, Vec3 v2, float& best);

// Land terrain for mode-1 legs and the land re-plan: h(x, y) (heightmap 0x135140 / 0x135350, bilinear) and the
// heightmap extent box (0x1320d0: centre and half sizes). [port] No height function = flat ground at z 0; no box = every
// point is inside. S8 t06: `grid` is the heightmap for the projectile ground test (ground_segment).
struct RocketTerrain {
    std::function<float(float, float)> height;
    bool has_box = false;
    float cx = 0.0f, cy = 0.0f, hx = 0.0f, hy = 0.0f;
    GroundGrid grid;
    float h(float x, float y) const { return height ? height(x, y) : 0.0f; }
};

// Shared inputs of one plan / re-plan call.
struct RocketContext {
    const RocketParams* params = nullptr;
    const RocketTerrain* terrain = nullptr;
    SimRng* rng = nullptr;
    Vec3 launch;  // pack +0x80..0x88 (the spawn position): the mode-1 altitude reference
};

// fn(v): the fast normalise 0x16f570 (FIGHTER_FLYING §1.3): s = (x x + y y) + z z, two-step retail rsqrt; a zero
// (or non-positive) vector is returned unchanged.
Vec3 rocket_fast_normalize(Vec3 v);

// Jitter 0x4c6ce0 (§2.4.5): theta = Get_Float(0, 6.2831855) FIRST, then a = Get_Float(0, amp) SECOND (both draw, also
// for amp 0); p += (cos theta n1 + sin theta n2) a.
void rocket_jitter(Vec3& p, Vec3 n1, Vec3 n2, float amp, SimRng& rng);

// Jittered straight leg 0x4c9990 (§2.4.4): appends the points from `from` (already the last control point) to `to`.
// mode 0 = free flight (the last point is never jittered), 1 = land terrain following (box, height clamp).
void rocket_leg(RocketSpline& path, Vec3 from, Vec3 to, float amp, float len_scale, int mode, bool ground_end,
                bool plain_end, const RocketContext& ctx);

// Initial plans (§2.4.10 steps 2 / 3) into a fresh `path`: P = launch position, T = FireAtPosition, `facing` = the
// rocket's facing (spawn: the muzzle direction). Writes the budget (+0x54). s restarts at 0 (the caller's).
void rocket_plan_land(RocketSpline& path, Vec3 P, Vec3 T, Vec3 facing, float& budget, const RocketContext& ctx);
void rocket_plan_space(RocketSpline& path, Vec3 P, Vec3 T, Vec3 facing, float& budget, const RocketContext& ctx);

// Missile-shield re-plans (§2.4.6 land, §2.4.7 space). P = rocket position, E = emitter position, `last_velocity` =
// pack +0x98 (D = fn of it). False = bail-out (path, s and budget unchanged; no draws). True: `path` replaced, s = 0,
// budget -= s0 when Explode_When_Reached_Target_Radius.
bool rocket_replan_land(RocketSpline& path, Vec3 P, Vec3 E, Vec3 last_velocity, float& s, float& budget,
                        const RocketContext& ctx);
bool rocket_replan_space(RocketSpline& path, Vec3 P, Vec3 E, Vec3 last_velocity, float& s, float& budget,
                         const RocketContext& ctx);

// The arc subdivision 0x4cc780 (§2.4.6 step 6): appends to `out`, in order, sub(a, mid), mid, sub(mid, b) while
// acos(a . b) x r > thr. [port] The dot is clamped to [-1, 1] (a dot just above 1 is NaN in retail and never stops) and
// the recursion depth is capped at kRocketArcMaxDepth.
inline constexpr int kRocketArcMaxDepth = 12;
void rocket_arc_subdivide(Vec3 a, Vec3 b, float r, float thr, std::vector<Vec3>& out, int depth = 0);

}  // namespace alamo::tactical::combat
