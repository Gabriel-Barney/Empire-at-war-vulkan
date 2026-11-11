// s78n t20 (docs/re/MOVEMENT.md §1.5.1, §5.4.2, §5.4.3): the shared geometry of the land collision resolutions - the
// relation code 0x679750, the swept box 0x677650, the free-position ring search 0x440f90 with its occupancy test, the
// rejoin candidates along a path (0x67ad20 part a) and the dynamic pathfinder's visibility-graph search (0x67b310:
// window 0x67a0d0 / 0x67cf20, flood fill 0x677e40, edges 0x678e80, costs 0x67a3d0, A* 0x67b8a0, output 0x674870).
// Everything here is a pure function of its inputs: serial, no allocation shared between calls, floats through + - x /,
// IEEE sqrt and the retail fast normalise; ordered vectors and index tie-breaks only (determinism rule 3). The
// orchestration (which units are obstacles, the candidates past the path end, the claim and the path push) is
// MovementSystem::path_around.
#pragma once
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include "alamo/tactical/movement/collision.hpp"
#include "alamo/tactical/movement/constants.hpp"
#include "alamo/tactical/movement/passability.hpp"

namespace alamo::tactical::movement {

// ---- relation code 0x679750(p0, p1) (MOVEMENT §5.4.2) ----
inline constexpr uint8_t kRelStanding = 0;   // p0 is not moving
inline constexpr uint8_t kRelHeld = 2;       // p0 moves but its collision records hold it at speed 0
inline constexpr uint8_t kRelHeadOn = 4;
inline constexpr uint8_t kRelCrossing = 8;
inline constexpr uint8_t kRelSameWay = 0x10;        // headings within 60 degrees, p0 not slower
inline constexpr uint8_t kRelSameWaySlower = 0x20;  // headings within 60 degrees, p0 slower
struct RelationInput {
    bool p0_moving = false;
    float p0_cap = 3.0e38f;   // 0x509ea0: the lowest speed p0's collision records answer it
    V2 h0, h1;                // unit wanted facings (vf +0x1f8)
    float v0 = 0.0f, v1 = 0.0f;  // formation speed when in a formation, else max speed
    V2 rel;                   // p1.P - p0.P
    float hw1 = 0.0f;         // p1's half width 0x396b70
    bool own_record = false;  // the pair's own record is a MovingVsMoving in state 0, 1 or 4
};
// p0 not moving -> 0; its cap 0 -> 2; c = h0 . h1 > 0.5 -> 0x20 when v0 < v1 else 0x10; c <= 0.5: p1 at least hw1 ahead of
// p0 along h0, or own_record -> 4 when c <= -0.5 else 8; otherwise 0x10.
uint8_t relation_code(const RelationInput& in);

// ---- swept box 0x677650 (MOVEMENT §1.5.1 item 3, §5.4.6 "Pass hold lines") ----
// The obstacle's footprint rectangle with both half extents + 1.0. Turning in place by more than 10 degrees: a square of
// max(hx, hy) + 1 along its wanted facing. sweep > 0 (a mover closing on the pathing unit): stretched from its centre
// along its path polyline by the path length `sweep` - axis = the chord from its position to the polyline point at that
// arc length, the rectangle runs from (hx + 1) behind its position to (hx + 1) past the chord's end, hy + 1 widened by
// the largest side offset of the polyline points passed. The result's front centre is c + a hx, its rear centre c - a hx.
struct SweptInput {
    V2 pos;
    float yaw = 0.0f;
    float hx = 0.0f, hy = 0.0f;
    bool turning = false;
    V2 wanted;                 // unit wanted facing (read while turning)
    float sweep = 0.0f;
    std::span<const V2> poly;  // the mover's stack polyline, poly[0] = pos
};
FootRect swept_box(const SweptInput& in);

// Rectangle helpers (FootRect::round is not read: every obstacle of the dynamic pathfinder is a rectangle).
// Strict interior of the rectangle with both half extents + grow.
bool rect_contains(const FootRect& r, V2 p, float grow = 0.0f);
// s78n t20 (MOVEMENT §1.5.1 item 4a, the crossing query 0x6b1d00): the segment p -> q as a zero-width oriented box
// against the closed rectangle - a separating-axis test with a closed comparison, so touching counts.
bool segment_meets_rect(const FootRect& r, V2 p, V2 q);
// Where the ray origin + t dir (t >= 0) leaves the rectangle; false when it never touches it.
bool ray_leaves_rect(const FootRect& r, V2 origin, V2 dir, V2& out);
inline FootRect rect_grown(const FootRect& r, float g) { return {r.c, r.a, r.hx + g, r.hy + g, false}; }

// ---- free-position ring search 0x440f90 (A1_READINGS §40.2, MOVEMENT §5.4.3) ----
// Rings R = 0, 1, 2.8, 5.4, 8.8, 13, ... (step 1.0, + 0.8 per ring, capped at min(1.5 r, 10); 5 when r is 0); n =
// trunc(2 pi R / r) + 1 samples (at least 4 after ring 0) at theta_i = 2 pi i / n counter-clockwise from `dir`; a sample
// with theta <= cone lies at the ring radius, the others at `outside` from the centre. The first sample `accept` takes
// is the spot. Set-ups: the push (centre = the blocker, cone pi / 4, outside 0.33), StoppedVsStopped (centre = the unit
// moved, Infantry class cone pi / outside 1.0, others 3 pi / 4 / 0.75), Path Around's rejoin spot (full circle).
struct RingSetup {
    V2 centre;
    V2 dir;                    // the search orientation (zero: yaw 0)
    float r = 0.0f;            // footprint radius 0x3969f0 of the object being placed
    float cone = 6.2831855f;   // +0x3c (ctor 2 pi: the full circle)
    float outside = 1.0f;      // +0x40
    uint32_t rings = 20;
};
bool ring_search(const RingSetup& s, const std::function<bool(V2 p, uint32_t ring)>& accept, V2& out);
// Occupancy 0x538330 -> 0x6b24f0 (MOVEMENT §5.4.3 "Occupancy, settled"): the record `rec` blocks the sample when its
// centre C comes within reach + rho of the sample swept one unit along world +x (the segment S -> S + (1, 0)); reach =
// the placed object's footprint radius + the occupancy margin, rho = hy + (hx - hy) cos^2 of the angle between the
// record's axis and the line S -> C (a round record: hx).
bool ring_blocked(V2 sample, float reach, const FootRect& rec);

// ---- s78n t20: the path record cut 0x54e780(points, keep) (MOVEMENT §5.4.4 item 5) ----
// Keeps the arc length `keep` measured from the first point: walking the segments from the start (zero-length ones
// skipped), at the first segment with keep < length so far + its length every point after the segment's first point is
// erased, and when keep > the length so far one point is appended at that arc length (linear interpolation; keep exactly
// on a point adds none). keep <= 0: only the first point is left; keep >= the total length or fewer than 2 points:
// nothing changes. True when a point was appended (the new last point carries no obstacle tag).
bool cut_points(std::vector<V2>& pts, float keep);

// ---- rejoin candidates along the base path (0x67ad20 part a, MOVEMENT §1.5.1 item 2) ----
// base = the path points, cur = the index of the current waypoint (the start of the segment the unit is on; port: next -
// 1). Arc lengths run from base[cur]: s0 = clamp(projection of pos on the current segment, 0, its length) + d + 2.0,
// candidates at s0, s0 + 10, ... while < the length from base[cur] to the end, at most `max_count`; `next` = the index
// of the end waypoint of the candidate's segment. Nothing when cur + 1 >= base.size() (on the last waypoint).
struct RejoinPoint {
    V2 p;
    uint32_t next = 0;
    float arc = 0.0f;
};
void rejoin_on_path(std::span<const V2> base, uint32_t cur, V2 pos, float d, uint32_t max_count, std::vector<RejoinPoint>& out);

// ---- the visibility-graph search 0x67b310 (MOVEMENT §1.5.1 item 4 - 5) ----
// A front-corner waypoint of an obstacle carries its id and hold line (waypoint +0x2c; MovingVsMoving states 1 / 4 / 16).
struct HoldTag {
    uint32_t index = 0;     // index of the waypoint in the path
    ObjectId obstacle = -1;
    V2 pt, dir;             // the line: point = corner - (hx_obstacle + hx_mover) a, direction a
};
struct AroundObstacle {
    ObjectId id = -1;
    FootRect shape;           // swept_box
    float foot_hx = 0.0f;     // the obstacle's own footprint hx (the hold line)
    bool front_free = false;  // relation 2 / 4 / 8 / 0x20: its front edge always costs 1
};
struct AroundRequest {
    V2 start, goal;
    uint32_t mask = kPassClear;
    bool giant = false;
    float grow = 0.5f;      // rect1 = shape + grow on both axes (the mover's half width 0x396b70 + 0.5)
    float f = 0.0f;         // MovementBoxExpansionFactor: rect2 = rect1 + f
    float tracker = 0.5f;   // tracker rectangle = shape + tracker (min(hx, hy) of the mover + 0.5 + f)
    float mover_hx = 0.0f;  // hold lines
    std::span<const AroundObstacle> obstacles;  // ascending id (the fixed vertex order)
    std::function<float(float, float)> height;  // optional: 3-D segment lengths
};
struct AroundResult {
    std::vector<V2> points;        // start -> goal, unsmoothed
    std::vector<HoldTag> tags;
    std::vector<uint32_t> window;  // indices into the request's obstacles: those of the searched window
    uint32_t vertices = 0;         // of the searched window
    uint32_t complexity = 0;       // the vertex count per A* pass run
    bool crossing = false;         // found only by pass 2 (segments crossing an obstacle at x the overlap penalty)
    float cost = 0.0f;
};
struct CellBox {
    int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};
// Window 0x67a0d0 before the obstacle growth: the cell box of start and goal +- (bound, + 2 only for Giant); w / h > 1.5
// -> y0 -= k, y1 += k, k = int(ratio - 0.5); w / h < 2 / 3 -> x0 -= k and y1 += k (as coded), k = int(1 / ratio - 0.5);
// clamped to the grid.
CellBox around_window(V2 start, V2 goal, uint32_t bound, bool giant, int32_t grid_w, int32_t grid_h);
// False: the goal cell does not pass, the goal lies inside an obstacle's rect2, start = goal, five windows without a
// feasible flood fill, or the search fails (heap empty / node pool of 3 x vertices exhausted). `out.complexity` is valid
// either way.
bool around_search(const PassabilityGrid& grid, const MovementConstants& k, const AroundRequest& q, AroundResult& out);

// Test accessor for the edge rules of around_search (0x678e80 / 0x67a3d0, MOVEMENT §1.5.1 item 4a): a graph vertex is
// corner `corner` ((+a +b), (+a -b), (-a -b), (-a +b)) of q.obstacles[obs], or with obs -1 the point p (the start, the
// goal or a terrain corner). Every obstacle of q counts as in the window. 1 no edge, 2 an edge at x 1, 3 a crossing edge
// (x DynamicObstacleOverlapPenalty, pass 2 only).
struct AroundVertexRef {
    int32_t obs = -1;
    uint32_t corner = 0;
    V2 p;
};
uint8_t around_edge_class(const PassabilityGrid& grid, const AroundRequest& q, const AroundVertexRef& a, const AroundVertexRef& b);

}  // namespace alamo::tactical::movement
