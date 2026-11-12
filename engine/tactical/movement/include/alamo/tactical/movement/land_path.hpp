// Land pathfinding (docs/re/MOVEMENT.md §1.4, §1.5).
//
// RegionMap - rectangle decomposition of the passability grid (§1.4 "Build"): raster scan (y
//   outer, x inner); at each unassigned cell a square of identical effective passability grows
//   (width and height together), then one side extends up to a 2:1 aspect; a 1x2 / 2x1 result
//   collapses to 1x1; indices in creation order; the last row and column are not covered (-1).
//   Adjacency (4-connected shared borders) is listed in ascending region index = build order.
//   PROVISIONAL [M]: the "edges line up with already-assigned neighbours" condition of the side
//   extension is not modelled (the extension only needs unassigned cells of the same value).
//   Per movement mask the map also keeps region components (reachability) and a clearance map
//   (distance from each cell centre to the nearest blocked cell, chamfer 3-4, world units) for the
//   large-unit re-plan. Everything is precomputed in build(), so queries are const and
//   thread-safe (planning runs in parallel).
//
// find_land_path - the main land search (§1.4):
//   1. start/goal cells; a blocked goal snaps to the nearest passable cell of the start's
//      component (ring search, PROVISIONAL rule for the untraced 0x5a1910);
//   2. same region -> direct path; else A* over (region, entry cell) nodes: entry = the cell of the
//      next region on the shared border closest to the current entry, clamped to the overlap;
//      edge cost = Euclidean cell distance between successive entries; h = Euclidean cell
//      distance to the goal; open list = RetailHeap (retail tie rules); closed set keyed by
//      (from region, to region), re-push only on a strictly smaller g; neighbours in adjacency order;
//   3. path build: start, then per region crossing the exit cell (last cell in the old region)
//      and the entry cell (first cell in the new one), then the goal - every segment lies in one
//      rectangle or is a unit axis step, so the raw path is passable by construction;
//   4. smoothing pass (a): from each waypoint binary-search the furthest later visible waypoint;
//      pass (b): recursive arc-length shortcut over [a, b] when b - a >= 15, recursing on
//      [a, a + 0.66(b - a)] and [a + 0.33(b - a), b] (rescaled) on failure. Visibility is the
//      conservative grid DDA (every crossed cell, both neighbours on exact corner crossings, must
//      pass the mask rule; with a clearance radius, also the clearance map);
//   5. large units (clearance radius > 0): when any point sampled every 20 units along the path
//      has clearance below radius - 0.1, the path is re-planned with a 4-connected cell A* over
//      cells whose clearance >= radius (then smoothed with clearance-aware visibility) and
//      accepted if its length <= 1.4 x the unconstrained path (§1.4 "Large units"; PROVISIONAL:
//      the retail re-plans only the low-clearance sub-span through the region search).
// cell A* variant (§1.4): 4-connected, step cost 1, Manhattan heuristic, same heap rules.
// PROVISIONAL [L]: the neighbour order of the retail 4-direction table is unread; this port uses
// +x, +y, -x, -y.
#pragma once
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

#include "alamo/core/parallel.hpp"
#include "alamo/tactical/movement/heap.hpp"
#include "alamo/tactical/movement/passability.hpp"

namespace alamo::tactical::movement {

struct Region {
    uint16_t x = 0, y = 0, w = 0, h = 0;
    uint32_t bits = 0;
    bool contains(int32_t cx, int32_t cy) const {
        return cx >= x && cy >= y && cx < static_cast<int32_t>(x) + w && cy < static_cast<int32_t>(y) + h;
    }
};

class RegionMap {
public:
    // Builds regions, adjacency and, for each mask, components and clearance. The masks are independent: one `pf` job
    // each, per-mask outputs (OPT3 t05), so the result is the same for any worker count.
    void build(const PassabilityGrid& grid, std::span<const uint32_t> masks, const ParallelFor& pf = serial_for);
    uint64_t grid_version() const { return grid_version_; }

    uint32_t width() const { return w_; }
    uint32_t height() const { return h_; }
    int32_t region_at(int32_t x, int32_t y) const {
        if (x < 0 || y < 0 || static_cast<uint32_t>(x) >= w_ || static_cast<uint32_t>(y) >= h_) return -1;
        return cell_region_[static_cast<size_t>(y) * w_ + static_cast<size_t>(x)];
    }
    uint32_t region_count() const { return static_cast<uint32_t>(regions_.size()); }
    const Region& region(int32_t r) const { return regions_[static_cast<size_t>(r)]; }
    std::span<const int32_t> neighbors(int32_t r) const {
        return {adj_.data() + adj_begin_[static_cast<size_t>(r)],
                adj_begin_[static_cast<size_t>(r) + 1] - adj_begin_[static_cast<size_t>(r)]};
    }
    // Index of the mask's precomputed data; -1 if the mask was not passed to build().
    int32_t mask_slot(uint32_t mask) const;
    // Component of a region for a mask slot (-1 when the region is not passable for it).
    int32_t component(int32_t slot, int32_t region) const {
        return (slot < 0 || region < 0) ? -1 : comp_[static_cast<size_t>(slot)][static_cast<size_t>(region)];
    }
    // Component of the cell under a world point (-1 blocked / uncovered).
    int32_t component_at(int32_t slot, int32_t cx, int32_t cy) const { return component(slot, region_at(cx, cy)); }
    // Clearance (world units, cell centre to the nearest blocked cell edge; 0 on blocked cells).
    float clearance(int32_t slot, int32_t cx, int32_t cy) const;

private:
    uint32_t w_ = 0, h_ = 0;
    uint64_t grid_version_ = 0;
    std::vector<int32_t> cell_region_;
    std::vector<Region> regions_;
    std::vector<uint32_t> adj_begin_;
    std::vector<int32_t> adj_;
    std::vector<uint32_t> masks_;
    std::vector<std::vector<int32_t>> comp_;
    std::vector<std::vector<float>> clear_;
};

// Calls fn(cx, cy) for every cell the segment a -> b touches (Amanatides-Woo walk in cell space;
// on an exact corner crossing both side cells are visited too). Stops early when fn returns false;
// returns false in that case.
template <class Fn>
bool walk_cells(V2 a, V2 b, Fn&& fn);

// Conservative visibility: every cell touched by the segment or by its two parallels at
// +-kSegmentBand passes the mask rule (and, if clearance > 0, has clearance >= clearance in
// `regions` for the mask's slot). The band makes planned segments robust to the locomotor's
// float rounding (a step a few ulps off the line never touches an unchecked corner cell).
inline constexpr float kSegmentBand = 0.05f;
bool segment_passable(const PassabilityGrid& grid, V2 a, V2 b, uint32_t mask, float clearance = 0.0f,
                      const RegionMap* regions = nullptr, int32_t slot = -1);

struct LandPathRequest {
    V2 start, goal;
    uint32_t mask = kPassClear;
    float clearance_radius = 0.0f;  // > 0: large-unit clearance re-plan
    bool smooth = true;
    bool use_cell_astar = false;    // request +0x145
    bool snap_goal = true;          // blocked goal -> nearest reachable passable cell
    uint32_t max_nodes = 200000;    // node pool (exhausting it fails the search)
    // REMAINING_GAPS §4.2: threat-aware region search (0x5a38a0, fields +0x134 / +0x138, cell-centre query 0x5a37d0):
    // each region edge costs x ThreatCost::factor(entry cell centre). Inactive by default.
    ThreatCost threat;
};

struct LandPath {
    bool ok = false;
    std::vector<V2> points;   // world XY, points[0] = start, back() = goal_used
    V2 goal_used;             // the goal after snapping
    uint32_t expansions = 0;  // nodes popped
    bool clearance_replanned = false;
    bool start_blocked = false;  // the start cell is not passable: points[1] is the nearest passable cell

};

// Per-thread scratch (reuse between requests; never shared between concurrent searches).
struct LandPathScratch {
    struct Node {
        int32_t region = -1, from = -1;
        int32_t cx = 0, cy = 0;    // entry cell (in `region`)
        int32_t xx = 0, xy = 0;    // exit cell (in `from`)
        float g = 0.0f;
        int32_t parent = -1;
    };
    std::vector<Node> nodes;
    RetailHeap<int32_t> heap;
    struct Best {
        float g;
        bool closed;
    };
    std::unordered_map<uint64_t, Best> best;  // probed only, never iterated
    // cell A*
    std::vector<float> cell_g;
    std::vector<uint32_t> cell_gen;
    std::vector<int32_t> cell_parent;
    std::vector<uint32_t> cell_closed;  // == gen: closed in the current search
    uint32_t gen = 0;
};

bool find_land_path(const PassabilityGrid& grid, const RegionMap& regions, const LandPathRequest& req,
                    LandPathScratch& scratch, LandPath& out);

// Building blocks (exposed for tests).
// Region A* only (no smoothing / clearance); points as built in step 3. False if unreachable.
bool region_astar(const PassabilityGrid& grid, const RegionMap& regions, V2 start, V2 goal, uint32_t mask,
                  LandPathScratch& scratch, std::vector<V2>& points, uint32_t max_nodes, uint32_t* expansions,
                  const ThreatCost* threat = nullptr);
// 4-connected cell A* between cell centres (start / goal exact points prepended / appended).
// `min_clearance` > 0 restricts it to cells with that clearance (slot of `mask`).
bool cell_astar(const PassabilityGrid& grid, const RegionMap& regions, V2 start, V2 goal, uint32_t mask,
                float min_clearance, LandPathScratch& scratch, std::vector<V2>& points, uint32_t max_nodes,
                uint32_t* expansions);
// `threat` (active): a shortcut must also keep every crossed cell centre below max(T, threat at either end) (MOVEMENT
// §1.4 "stay below max(endpoint threat) x factor(+0x13C)"; [port]: factor 1, floor T - the factor value is unread).
void smooth_path(const PassabilityGrid& grid, std::vector<V2>& points, uint32_t mask, float clearance = 0.0f,
                 const RegionMap* regions = nullptr, int32_t slot = -1, const ThreatCost* threat = nullptr);
float path_length(std::span<const V2> points);

// s78p (b0049, MOVEMENT §3.5.2 D, s78n t26 RE 2; 0x413340): the lane of a formation member - one point per waypoint of the
// formation path `fpath`. Point i: origin fpath[i], direction n = lane_dir(fpath, i) (i = 0: unit(wp1 - wp0), fast then
// exact normalise; i > 0: the waypoint line's bisector of 0x5c5060, the last waypoint its incoming direction); row k =
// min(int(s_i / 30), rows - 1), s_i = the 2-D arc length to i; lateral offset L0 = LaneShape::offset(s_i) (+ = toward
// (-n.y, n.x), retail's sign); point = origin + L (-n.y, n.x); while the point fails the unit's `mask` (0x50e0c0, the
// cell test) L -= L0 / 5, and once |L| <= |L0 / 5| L = 0 (the origin). z = 0 for every point (the port's paths are 2-D).
// An empty grid (unit tests without terrain) passes every point. s78p [port]: with a squeeze, inside a segment whose
// straight lane line fails the class (line_passable), a point is also placed at each row (arc 30 k) whose offset differs
// from a neighbouring row's - retail's member takes the row's offset as it walks, so a squeeze in the middle of a long
// segment narrows the lane there too.
struct LaneShape {
    float left = 0.0f;                  // the slot's lateral offset (+ = left; a port Slot's lateral is to the right: -lateral)
    std::span<const float> shift;       // the corridor squeeze's per-row shift F +0xb0[k].+4 (+ = left; empty: no squeeze)
    std::span<const uint32_t> entry;    // its per-row width-table entry (0: the full width)
    std::span<const float> entry_left;  // the unit's offset in each entry's pack F +0xe0[k] (+ = left; entry 0 unused)
    // L0 at arc length s: shift[k] + the unit's offset in row k's pack (`left` for the full-width entry 0).
    float offset(float s) const;
};
V2 lane_dir(std::span<const V2> fpath, size_t i);
std::vector<V2> formation_lane(const PassabilityGrid& grid, std::span<const V2> fpath, const LaneShape& shape, uint32_t mask);
// s78p (b0049) [port]: the member's path on its lane - its start (lane_start from the last lane point it has passed), then
// the lane points whose waypoint it has not passed yet (the leading points with dot(pos - lane[i], lane_dir(i)) >= 0 are
// skipped: retail steps a waypoint whose line the unit has crossed) and whose rest length on the formation path (2-D)
// exceeds the member's depth `back` (a point behind the previous one across its own line is skipped: it would turn the
// member back), then the end point lane_end. bkm1 t01 (b0095): a segment that fails `mask` (line_passable) falls back onto
// the formation path (its far point to its origin, then backwards), never moving the end. Retail keeps the formation path
// as the unit's path, steers it onto its lane with the lane term and shifts the waypoint lines within |+0x68| of the end
// along their segments (MOVEMENT §4.3.3); the port walks the lane itself, and its end is the member's goal.
std::vector<V2> member_lane(const PassabilityGrid& grid, std::span<const V2> fpath, const LaneShape& shape, float back,
                            uint32_t mask, V2 pos);
// bkm1 t02 (b0096, MOVEMENT §4.3.3 items 2 - 5 [H]): where a formation member stops - the last waypoint G moved along the
// last segment's direction d1 by -back (back >= 0: the member's depth behind the front row, which stops on G) and across
// it by the shape's L at arc length total - back: G - d1 back + (-d1.y, d1.x) L. Not along the path's arc, not pulled in,
// not snapped to a cell, not reserved. [port] the final-facing rotation psi (0x411980) is not modelled (psi = 0).
V2 lane_end(std::span<const V2> fpath, const LaneShape& shape, float back);
// s78p (b0049) [port]: the start of a lane path for a unit at `pos` - path[0] := the foot of pos on the line path[0] ->
// path[1] when that foot lies before path[1], else pos. A member off its lane then walks the lane's direction and the lane
// term (0x41dbb0, error / 30) pulls it onto the line, as retail's member converges on its slot offset from the formation
// path; a member already past the lane's first point toward its end walks straight to it.
void lane_start(std::vector<V2>& path, V2 pos);
// Nearest passable cell (to `p`) with the given component, ring search up to max_rings; ties: the
// first in (ring, y, x) scan order. False if none.
bool snap_to_component(const PassabilityGrid& grid, const RegionMap& regions, V2 p, uint32_t mask, int32_t component,
                       int32_t max_rings, V2& out);

// s78n t20 batch 2 (MOVEMENT §4.3.1 item 7, 0x50da90 / 0x50ee10 / 0x50f480): zones are the region components of a mask.
// zone_at: -1 "none" (no slot for the mask, outside the grid, the uncovered last row / column), 0 a region that fails the
// mask, else the component + 1. zone_connected (0x50ee10, land): both cells have a region that passes the mask and the
// zones are equal - an impassable end point is never zone-connected to anything.
int32_t zone_at(const RegionMap& regions, int32_t slot, int32_t cx, int32_t cy);
bool zone_connected(const RegionMap& regions, uint32_t mask, V2 a, V2 b);
// 0x50f480(P, Q, style 0, class, 30, out), cells by world_to_cell: zone(Q) = none with Q's cell outside the grid, or
// zone(Q) = zone(P) -> out := P, true (passability is not looked at). Otherwise the square rings r = 1 .. 29 around P's
// cell, candidates in the retail order - for i = -r .. r the cell (px + i, py + r), then its mirror (px - i, py - r);
// for j = 1 - r .. r - 1 the cell (px + r, py + j), then its mirror (px - r, py - j); a mirror only when the first cell
// of its pair is inside the grid and has a region - kept when inside the grid with zone(Q). The first ring with a kept
// cell decides: out := the centre of the kept cell nearest to Q (strict <: the earliest wins a tie), true; no later ring
// is looked at. None in 29 rings -> false, out untouched.
bool zone_validate(const RegionMap& regions, V2 p, V2 q, uint32_t mask, V2& out);

// ---------------------------------------------------------------- template implementation
template <class Fn>
bool walk_cells(V2 a, V2 b, Fn&& fn) {
    // Cell space: cell i covers [i, i+1) in u = (w + 10) / 20.
    const float ax = (a.x + kHalfCell) / kCellSize, ay = (a.y + kHalfCell) / kCellSize;
    const float bx = (b.x + kHalfCell) / kCellSize, by = (b.y + kHalfCell) / kCellSize;
    int32_t cx = static_cast<int32_t>(std::floor(ax)), cy = static_cast<int32_t>(std::floor(ay));
    const int32_t ex = static_cast<int32_t>(std::floor(bx)), ey = static_cast<int32_t>(std::floor(by));
    if (!fn(cx, cy)) return false;
    const float dx = bx - ax, dy = by - ay;
    const int32_t sx = dx > 0.0f ? 1 : (dx < 0.0f ? -1 : 0);
    const int32_t sy = dy > 0.0f ? 1 : (dy < 0.0f ? -1 : 0);
    constexpr float kFar = 3.0e38f;
    const float tdx = sx != 0 ? static_cast<float>(sx) / dx : kFar;  // t per cell in x
    const float tdy = sy != 0 ? static_cast<float>(sy) / dy : kFar;
    float tmx = sx > 0 ? (static_cast<float>(cx + 1) - ax) / dx : (sx < 0 ? (ax - static_cast<float>(cx)) / -dx : kFar);
    float tmy = sy > 0 ? (static_cast<float>(cy + 1) - ay) / dy : (sy < 0 ? (ay - static_cast<float>(cy)) / -dy : kFar);
    // Bounded walk: at most |ex - cx| + |ey - cy| steps.
    int32_t steps = (ex > cx ? ex - cx : cx - ex) + (ey > cy ? ey - cy : cy - ey);
    while (steps > 0 && (cx != ex || cy != ey)) {
        if (tmx < tmy) {
            if (tmx > 1.0f) break;
            cx += sx;
            tmx += tdx;
            --steps;
        } else if (tmy < tmx) {
            if (tmy > 1.0f) break;
            cy += sy;
            tmy += tdy;
            --steps;
        } else {
            if (tmx > 1.0f) break;
            // Exact corner: touch both side cells, then step diagonally.
            if (!fn(cx + sx, cy)) return false;
            if (!fn(cx, cy + sy)) return false;
            cx += sx;
            cy += sy;
            tmx += tdx;
            tmy += tdy;
            steps -= 2;
        }
        if (!fn(cx, cy)) return false;
    }
    if (cx != ex || cy != ey) {
        // Float rounding left the walk short of the end cell: visit every cell of the remaining
        // bounding box (at most a few cells; conservative).
        const int32_t x0 = cx < ex ? cx : ex, x1 = cx < ex ? ex : cx;
        const int32_t y0 = cy < ey ? cy : ey, y1 = cy < ey ? ey : cy;
        for (int32_t y = y0; y <= y1; ++y)
            for (int32_t x = x0; x <= x1; ++x)
                if (!fn(x, y)) return false;
    }
    return true;
}

// s78n t20 (MOVEMENT §1.5.1 item 4, FIGHTER_FLYING §7.5): the terrain line test 0x5a4c10 for a movement class - every
// cell the segment touches passes the mask (a thin walk, the start cell included).
inline bool line_passable(const PassabilityGrid& grid, V2 a, V2 b, uint32_t mask) {
    return walk_cells(a, b, [&](int32_t cx, int32_t cy) { return grid.passable_cell(cx, cy, mask); });
}

// ---- s78n t26.3 (MOVEMENT §3.5.2 D, s78n t26 RE 2): the corner-speed pass 0x5070b0 ----
// Inputs of one pass: yaw0 = the unit's facing (degrees; corner 0's incoming direction), vmax = the effective max speed
// 0x370f00, vmin = Min_Speed x mode (0x3728f0), rate = R in degrees / frame (omega = R pi / 180), f_min =
// Min_Speed_Fraction_For_Turn, decel = a (the brake pass), seg = the arc probe's segment length (max(2, 10 x 0x397830)).
// line_ok = the static line test 0x5a4c10 for the unit's movement class, corridor_ok = the corridor test 0x5093c0 over the
// probe polyline (null: passes).
struct CornerPassIn {
    float yaw0 = 0.0f;
    float vmax = 0.0f;
    float vmin = 0.0f;
    float rate = 0.0f;
    float f_min = 0.0f;
    float decel = 0.0f;
    float seg = 2.0f;
    bool (*line_ok)(const void* ctx, V2 a, V2 b) = nullptr;
    bool (*corridor_ok)(const void* ctx, const std::vector<V2>& probe) = nullptr;
    const void* ctx = nullptr;
};
// zero / slowed: corners whose written fraction is 0 / in (0, 1) after item 6 (before the brake pass); stepdowns: v /= 1.5.
struct CornerPassStats {
    uint32_t zero = 0, slowed = 0, stepdowns = 0;
};
// Items 2-7 over the polyline `poly` (point 0 = the unit's own position): one speed fraction per point (the +4 entry; the
// last point 1.0). Corner j: d_in = the facing (j = 0) or unit(P_j - P_j-1); the leg to P_i (i = j + 1): L, d, theta =
// acos(clamp(d_in . d)), side = sign(d_in x d); with f_min > 0 the merge (while L < L_turn = 2 vmax sin|theta| / omega
// and a next point lies on the same side, it is taken) and the aim point A = B + d max(0, |P_i - B| - (L - L_turn)) when
// L > L_turn (else A = P_i); v = min(vmax, omega r / (1 - cos theta), L omega / (2 sin|theta|)), r = clamp(L / 2, 20,
// 100), theta = 0 -> vmax; while v > 0 the arc probe must pass the line test and (v < 0.2 vmax and j != 0, or the
// corridor test), else v /= 1.5 and below vmin or 0.1 vmax v = vmin (the last line test passed) or 0; f = v / vmax, f <
// f_min -> 0 on waypoint j alone, else f on waypoints j .. i - 1. The brake pass (decel > 0, at least 3 points) from the
// end: v = sqrt(2 a |seg k| + v^2), f_k = min(f_k, v / vmax), v = vmax f_k. Empty result: vmax or rate not > 0 (no pass).
std::vector<float> corner_fractions(const std::vector<V2>& poly, const CornerPassIn& in, CornerPassStats* stats = nullptr);
// The arc probe 0x505870 (s78n t26 RE 2): from P_j toward the aim point A at speed v, omega (radians / frame), signed theta
// (negative for a left turn, d_in x d > 0), segment length seg: N = (uint)(|theta| v / omega / seg + 3) points p_k = P_j +
// t (sin|theta| - sin((1 - 2u)|theta|)) v / omega + n (cos((2u - 1)|theta|) - cos|theta|) v / omega, u = k / N, t =
// unit(A - P_j), n its normal toward the incoming direction; it stops before a p_k past the aim line (cross(A - last, p_k -
// last) x s < 0), then A is appended. `out` starts with P_j. s78o (b0088) [port]: N is at most kCornerArcProbeMaxPoints
// (also for a non-finite N), so custom data cannot make the pass hang or the cast overflow.
inline constexpr uint32_t kCornerArcProbeMaxPoints = 16384;
void corner_arc_probe(V2 pj, V2 a, float v, float omega, float theta_s, float seg, std::vector<V2>& out);

}  // namespace alamo::tactical::movement
