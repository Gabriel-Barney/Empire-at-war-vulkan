#include "alamo/tactical/movement/path_around.hpp"

#include <algorithm>

#include "alamo/core/dmath.hpp"
#include "alamo/tactical/movement/fighter.hpp"
#include "alamo/tactical/movement/heap.hpp"
#include "alamo/tactical/movement/land_path.hpp"

namespace alamo::tactical::movement {

// ---------------------------------------------------------------- relation code 0x679750
uint8_t relation_code(const RelationInput& in) {
    if (!in.p0_moving) return kRelStanding;
    if (in.p0_cap == 0.0f) return kRelHeld;
    const float c = dot(in.h0, in.h1);
    if (c > 0.5f) return in.v0 < in.v1 ? kRelSameWaySlower : kRelSameWay;
    if (dot(in.rel, in.h0) >= in.hw1 || in.own_record) return c <= -0.5f ? kRelHeadOn : kRelCrossing;
    return kRelSameWay;
}

// ---------------------------------------------------------------- swept box 0x677650
FootRect swept_box(const SweptInput& in) {
    if (in.turning) {
        const float m = std::max(in.hx, in.hy) + 1.0f;
        const V2 a = length_sq(in.wanted) > 0.0f ? in.wanted : dir_from_deg(in.yaw);
        return {in.pos, a, m, m, false};
    }
    if (in.sweep > 0.0f && in.poly.size() >= 2) {
        // The polyline point at arc length `sweep` (its end when shorter), and the largest side offset on the way.
        float left = in.sweep;
        V2 at = in.poly[0];
        size_t last = 0;  // the last polyline point passed
        for (size_t i = 1; i < in.poly.size() && left > 0.0f; ++i) {
            const V2 d = in.poly[i] - at;
            const float l = length(d);
            if (l <= left) {
                at = in.poly[i];
                left -= l;
                last = i;
            } else {
                at = at + d * (left / l);
                left = 0.0f;
            }
        }
        const V2 chord = at - in.pos;
        const float len = length(chord);
        if (len > 0.0f) {
            const V2 a = normalize_retail(chord);
            float side = std::fabs(cross(a, at - in.pos));
            for (size_t i = 1; i <= last; ++i) side = std::max(side, std::fabs(cross(a, in.poly[i] - in.pos)));
            return {in.pos + a * (0.5f * len), a, in.hx + 1.0f + 0.5f * len, in.hy + 1.0f + side, false};
        }
    }
    return {in.pos, dir_from_deg(in.yaw), in.hx + 1.0f, in.hy + 1.0f, false};
}

bool rect_contains(const FootRect& r, V2 p, float grow) {
    const V2 d = p - r.c;
    return std::fabs(dot(d, r.a)) < r.hx + grow && std::fabs(dot(d, perp_left(r.a))) < r.hy + grow;
}

namespace {
// The parameter range [t0, t1] of the line o + t d inside the box |x| < hx, |y| < hy (local coordinates); false when
// the line misses the open box.
bool slab_range(float ox, float oy, float dx, float dy, float hx, float hy, float& t0, float& t1) {
    t0 = -3.0e38f;
    t1 = 3.0e38f;
    const float o[2] = {ox, oy}, d[2] = {dx, dy}, h[2] = {hx, hy};
    for (int k = 0; k < 2; ++k) {
        if (d[k] == 0.0f) {
            if (!(std::fabs(o[k]) < h[k])) return false;
            continue;
        }
        float a = (-h[k] - o[k]) / d[k], b = (h[k] - o[k]) / d[k];
        if (a > b) std::swap(a, b);
        t0 = std::max(t0, a);
        t1 = std::min(t1, b);
    }
    return t0 < t1;
}
}  // namespace

bool segment_meets_rect(const FootRect& r, V2 p, V2 q) {
    const V2 b = perp_left(r.a);
    const V2 d = q - p;
    const V2 m = (p + q) * 0.5f - r.c;  // the segment box's centre, relative to the rectangle's
    // separated along x when the centres' distance exceeds the two projected half extents (equal: touching, no gap)
    const auto separated = [&](V2 x) {
        const float reach = r.hx * std::fabs(dot(r.a, x)) + r.hy * std::fabs(dot(b, x)) + 0.5f * std::fabs(dot(d, x));
        return std::fabs(dot(m, x)) > reach;
    };
    if (separated(r.a) || separated(b)) return false;
    return !(length_sq(d) > 0.0f) || !(separated(d) || separated(perp_left(d)));
}

bool ray_leaves_rect(const FootRect& r, V2 origin, V2 dir, V2& out) {
    if (!(r.hx > 0.0f) || !(r.hy > 0.0f) || !(length_sq(dir) > 0.0f)) return false;
    const V2 b = perp_left(r.a);
    const V2 lp = origin - r.c;
    float t0 = 0.0f, t1 = 0.0f;
    if (!slab_range(dot(lp, r.a), dot(lp, b), dot(dir, r.a), dot(dir, b), r.hx, r.hy, t0, t1)) return false;
    if (!(t1 > 0.0f)) return false;  // the rectangle lies behind the ray
    out = origin + dir * t1;
    return true;
}

// ---------------------------------------------------------------- ring search 0x440f90
bool ring_search(const RingSetup& s, const std::function<bool(V2 p, uint32_t ring)>& accept, V2& out) {
    constexpr float kPi = 3.1415927f;
    float max_step = s.r * 1.5f;
    if (max_step == 0.0f) max_step = 5.0f;
    max_step = std::min(max_step, 10.0f);
    // Orientation (0x20acd0): yaw in degrees in [0, 360), then radians as the search reads it.
    float yaw_deg = 0.0f;
    if (s.dir.x != 0.0f || s.dir.y != 0.0f) {
        yaw_deg = dmath::atan2(s.dir.y, s.dir.x) * 57.295776f;
        if (yaw_deg < 0.0f) yaw_deg += 360.0f;
    }
    const float yaw = yaw_deg * 0.017453292f;
    const float sy = fighter_sin(yaw), cy = fighter_cos(yaw);
    float ring_r = 0.0f, step = 1.0f;
    for (uint32_t ring = 0; ring < s.rings; ++ring) {
        uint32_t n = 1;
        if (s.r > 0.0f) n = static_cast<uint32_t>(static_cast<int64_t>(ring_r * 2.0f * kPi / s.r)) + 1u;
        if (ring != 0) n = std::max(4u, n);
        for (uint32_t i = 0; i < n; ++i) {
            const float th = (static_cast<float>(i) / static_cast<float>(n)) * kPi * 2.0f;
            const float rho = th > s.cone ? s.outside : ring_r;
            const float st = fighter_sin(th), ct = fighter_cos(th);
            const V2 p{s.centre.x + (ct * cy - st * sy) * rho, s.centre.y + (st * cy + ct * sy) * rho};
            if (accept(p, ring)) {
                out = p;
                return true;
            }
        }
        ring_r += step;
        step = std::min(step + 0.8f, max_step);
    }
    return false;
}

bool ring_blocked(V2 sample, float reach, const FootRect& rec) {
    // the nearest point of the swept sample S -> S + (1, 0) to the record's centre
    const float t = std::clamp(rec.c.x - sample.x, 0.0f, 1.0f);
    const V2 near{sample.x + t, sample.y};
    const V2 line = normalize_retail(rec.c - sample);
    const float rho = rec.round ? rec.hx : foot_rho(rec.hx, rec.hy, std::fabs(dot(line, rec.a)));
    return distance(near, rec.c) <= reach + rho;
}

// ---------------------------------------------------------------- path record cut 0x54e780
bool cut_points(std::vector<V2>& pts, float keep) {
    if (pts.size() < 2) return false;
    if (!(keep > 0.0f)) {
        pts.resize(1);
        return false;
    }
    float acc = 0.0f;
    for (size_t i = 1; i < pts.size(); ++i) {
        const float l = distance(pts[i - 1], pts[i]);
        if (!(l > 0.0f)) continue;
        if (keep < acc + l) {
            const V2 at = lerp(pts[i - 1], pts[i], (keep - acc) / l);
            pts.resize(i);
            if (!(keep > acc)) return false;
            pts.push_back(at);
            return true;
        }
        acc += l;
    }
    return false;
}

// ---------------------------------------------------------------- rejoin candidates along the path
void rejoin_on_path(std::span<const V2> base, uint32_t cur, V2 pos, float d, uint32_t max_count, std::vector<RejoinPoint>& out) {
    out.clear();
    if (static_cast<size_t>(cur) + 1 >= base.size()) return;
    const V2 seg = base[cur + 1] - base[cur];
    const float seg_len = length(seg);
    float proj = 0.0f;
    if (seg_len > 0.0f) proj = std::clamp(dot(pos - base[cur], seg) / seg_len, 0.0f, seg_len);
    float total = 0.0f;
    for (size_t i = cur + 1; i < base.size(); ++i) total += distance(base[i - 1], base[i]);
    float s = proj + d + 2.0f;
    size_t i = cur + 1;       // the end waypoint of the segment the candidate lies on
    float before = 0.0f;      // arc length up to base[i - 1]
    for (; out.size() < max_count && s < total; s += 10.0f) {
        while (i + 1 < base.size() && before + distance(base[i - 1], base[i]) < s) {
            before += distance(base[i - 1], base[i]);
            ++i;
        }
        const V2 e = base[i] - base[i - 1];
        const float l = length(e);
        const float t = l > 0.0f ? std::clamp((s - before) / l, 0.0f, 1.0f) : 0.0f;
        out.push_back({base[i - 1] + e * t, static_cast<uint32_t>(i), s});
    }
}

// ---------------------------------------------------------------- the visibility-graph search
CellBox around_window(V2 start, V2 goal, uint32_t bound, bool giant, int32_t grid_w, int32_t grid_h) {
    const int32_t sx = world_to_cell(start.x), sy = world_to_cell(start.y);
    const int32_t gx = world_to_cell(goal.x), gy = world_to_cell(goal.y);
    const int32_t grow = static_cast<int32_t>(bound) + (giant ? 2 : 0);
    CellBox b{std::min(sx, gx) - grow, std::min(sy, gy) - grow, std::max(sx, gx) + grow, std::max(sy, gy) + grow};
    const float w = static_cast<float>(b.x1 - b.x0 + 1), h = static_cast<float>(b.y1 - b.y0 + 1);
    const float ratio = w / h;
    if (ratio > 1.5f) {
        const int32_t k = static_cast<int32_t>(ratio - 0.5f);
        b.y0 -= k;
        b.y1 += k;
    } else if (ratio < 2.0f / 3.0f) {
        const int32_t k = static_cast<int32_t>(1.0f / ratio - 0.5f);
        b.x0 -= k;  // (as coded in 0x67a0d0: the far x side is not widened, y1 is)
        b.y1 += k;
    }
    b.x0 = std::max(b.x0, 0);
    b.y0 = std::max(b.y0, 0);
    b.x1 = std::min(b.x1, grid_w - 1);
    b.y1 = std::min(b.y1, grid_h - 1);
    return b;
}

namespace {

struct Box {
    float x0, y0, x1, y1;
};
Box rect_aabb(const FootRect& r) {
    const V2 b = perp_left(r.a);
    const float ex = std::fabs(r.a.x) * r.hx + std::fabs(b.x) * r.hy, ey = std::fabs(r.a.y) * r.hx + std::fabs(b.y) * r.hy;
    return {r.c.x - ex, r.c.y - ey, r.c.x + ex, r.c.y + ey};
}
bool boxes_overlap(const Box& a, const Box& b) { return a.x0 <= b.x1 && a.x1 >= b.x0 && a.y0 <= b.y1 && a.y1 >= b.y0; }
Box cell_box_world(const CellBox& w) {
    return {static_cast<float>(w.x0) * kCellSize - kHalfCell, static_cast<float>(w.y0) * kCellSize - kHalfCell,
            static_cast<float>(w.x1) * kCellSize + kHalfCell, static_cast<float>(w.y1) * kCellSize + kHalfCell};
}
// Corner k of a rectangle in the fixed order (+a +b), (+a -b), (-a -b), (-a +b).
V2 rect_corner(const FootRect& r, uint32_t k) {
    const V2 b = perp_left(r.a);
    const float sa = k < 2 ? 1.0f : -1.0f, sb = (k == 0 || k == 3) ? 1.0f : -1.0f;
    return r.c + r.a * (sa * r.hx) + b * (sb * r.hy);
}

// s78n t20 (MOVEMENT §1.5.1 item 4a): the corner vector n (+0x30) of corner k at (sa ha, sb hb): the unit vector of
// (-sa a - sb b), pointing from the corner into the rectangle at 45 degrees to its axes.
V2 corner_inward(const FootRect& r, uint32_t k) {
    const float sa = k < 2 ? 1.0f : -1.0f, sb = (k == 0 || k == 3) ? 1.0f : -1.0f;
    return normalize_retail(r.a * -sa + perp_left(r.a) * -sb);
}

struct Vertex {
    V2 p;                // the emitted point: the rect1 corner ([port] when f > 0, MOVEMENT §1.5.1 "Not read")
    int32_t obs = -1;    // index into the window's obstacle list (-1: terrain corner, start or goal)
    uint32_t corner = 0;
    V2 p2;               // s78n t20 (item 4a, +0x0c): the rect2 corner, which the edge and crossing tests use
};

// Edges 0x678e80 and the cost multiplier 0x67a3d0 between two vertices (MOVEMENT §1.5.1 item 4a): 1 none, 2 plain, 3
// crossing (x the overlap penalty, pass 2 only). `win` = the window's obstacles (indices into q.obstacles), `tracker` =
// their tracker rectangles (shape + q.tracker; the temporary records of 0x6b4810, not rect1).
uint8_t edge_class(const PassabilityGrid& grid, const AroundRequest& q, std::span<const uint32_t> win,
                   std::span<const FootRect> tracker, const Vertex& a, const Vertex& b) {
    // cos(0.7679449 rad) = cos 44 degrees (cosf of the float 0x3f449809)
    constexpr float kInwardCone = 0.7193398f;
    const bool same = a.obs == b.obs;  // one obstacle, or two obstacle-less vertices
    if (same && a.obs >= 0 && ((a.corner + 4u - b.corner) & 3u) == 2u) return 1;  // a rectangle's diagonal
    if (!same) {
        // no edge from a corner into its inward cone: d . n >= cos 44, d = the unit vector from the corner to the other
        // vertex; tested from each end that is a corner
        const auto inward = [&](const Vertex& c, const Vertex& other) {
            if (c.obs < 0) return false;
            const V2 n = corner_inward(q.obstacles[win[static_cast<size_t>(c.obs)]].shape, c.corner);
            return dot(normalize_retail(other.p2 - c.p2), n) >= kInwardCone;
        };
        if (inward(a, b) || inward(b, a)) return 1;
    }
    if (!line_passable(grid, a.p2, b.p2, q.mask)) return 1;
    // the side between the front corners (+a +-b) of an obstacle in relation 2 / 4 / 8 / 0x20 always costs 1
    if (same && a.obs >= 0 && a.corner + b.corner == 1u && q.obstacles[win[static_cast<size_t>(a.obs)]].front_free) return 2;
    // crossing: the segment meets (touching counts) the tracker rectangle of an obstacle that is neither end's own
    // ([port] a linear scan in window order stands for the tracker tree query; the same set)
    for (size_t i = 0; i < tracker.size(); ++i) {
        const int32_t o = static_cast<int32_t>(i);
        if (o == a.obs || o == b.obs) continue;
        if (segment_meets_rect(tracker[i], a.p2, b.p2)) return 3;
    }
    return 2;
}

}  // namespace

bool around_search(const PassabilityGrid& grid, const MovementConstants& k, const AroundRequest& q, AroundResult& out) {
    out = AroundResult{};
    const int32_t gw = static_cast<int32_t>(grid.width()), gh = static_cast<int32_t>(grid.height());
    if (gw <= 0 || gh <= 0) return false;
    if (q.start == q.goal) return false;
    const int32_t scx = world_to_cell(q.start.x), scy = world_to_cell(q.start.y);
    const int32_t gcx = world_to_cell(q.goal.x), gcy = world_to_cell(q.goal.y);
    if (!grid.passable_cell(scx, scy, q.mask) || !grid.passable_cell(gcx, gcy, q.mask)) return false;
    const float grow2 = q.grow + q.f;
    for (const AroundObstacle& o : q.obstacles)
        if (rect_contains(o.shape, q.goal, grow2)) return false;  // a goal inside an obstacle's rect2: the next candidate
    std::vector<Box> obox(q.obstacles.size());
    for (size_t i = 0; i < q.obstacles.size(); ++i) obox[i] = rect_aabb(rect_grown(q.obstacles[i].shape, grow2));
    const CellBox base = around_window(q.start, q.goal, k.dynamic_avoidance_rectangle_bound, q.giant, gw, gh);
    std::vector<uint8_t> reach;
    std::vector<int32_t> stack;
    for (int32_t attempt = 0; attempt < 5; ++attempt) {
        CellBox w = base;
        // 0x67cf20: each side grows by one cell while an obstacle overlaps the strip 10 units outside it (at most 20
        // cells per dimension)
        int32_t grown_x = 0, grown_y = 0;
        for (bool grew = true; grew;) {
            grew = false;
            const Box wb = cell_box_world(w);
            const Box strips[4] = {{wb.x0 - 10.0f, wb.y0, wb.x0, wb.y1},
                                   {wb.x1, wb.y0, wb.x1 + 10.0f, wb.y1},
                                   {wb.x0, wb.y0 - 10.0f, wb.x1, wb.y0},
                                   {wb.x0, wb.y1, wb.x1, wb.y1 + 10.0f}};
            for (int side = 0; side < 4; ++side) {
                int32_t& grown = side < 2 ? grown_x : grown_y;
                if (grown >= 20) continue;
                bool hit = false;
                for (const Box& b : obox) hit = hit || boxes_overlap(b, strips[side]);
                if (!hit) continue;
                if (side == 0 && w.x0 > 0) --w.x0;
                else if (side == 1 && w.x1 < gw - 1) ++w.x1;
                else if (side == 2 && w.y0 > 0) --w.y0;
                else if (side == 3 && w.y1 < gh - 1) ++w.y1;
                else continue;
                ++grown;
                grew = true;
            }
        }
        // retries 2 - 5 add 3 cells on every side
        w.x0 = std::max(w.x0 - 3 * attempt, 0);
        w.y0 = std::max(w.y0 - 3 * attempt, 0);
        w.x1 = std::min(w.x1 + 3 * attempt, gw - 1);
        w.y1 = std::min(w.y1 + 3 * attempt, gh - 1);
        const int32_t bw = w.x1 - w.x0 + 1, bh = w.y1 - w.y0 + 1;
        if (bw <= 0 || bh <= 0) return false;
        const auto inside = [&](int32_t cx, int32_t cy) { return cx >= w.x0 && cy >= w.y0 && cx <= w.x1 && cy <= w.y1; };
        const auto li = [&](int32_t cx, int32_t cy) {
            return static_cast<size_t>(cy - w.y0) * static_cast<size_t>(bw) + static_cast<size_t>(cx - w.x0);
        };
        if (!inside(scx, scy) || !inside(gcx, gcy)) continue;
        // Feasibility 0x677e40: a 4-neighbour flood fill over the window's passable cells from the start cell
        reach.assign(static_cast<size_t>(bw) * static_cast<size_t>(bh), 0);
        stack.clear();
        reach[li(scx, scy)] = 1;
        stack.push_back(scx);
        stack.push_back(scy);
        while (!stack.empty()) {
            const int32_t cy = stack.back();
            stack.pop_back();
            const int32_t cx = stack.back();
            stack.pop_back();
            static constexpr int32_t kDx[4] = {1, 0, -1, 0};
            static constexpr int32_t kDy[4] = {0, 1, 0, -1};
            for (int d = 0; d < 4; ++d) {
                const int32_t nx = cx + kDx[d], ny = cy + kDy[d];
                if (!inside(nx, ny) || reach[li(nx, ny)] || !grid.passable_cell(nx, ny, q.mask)) continue;
                reach[li(nx, ny)] = 1;
                stack.push_back(nx);
                stack.push_back(ny);
            }
        }
        if (!reach[li(gcx, gcy)]) continue;
        // ---- vertices, in the fixed order
        const Box wb = cell_box_world(w);
        std::vector<uint32_t> win;  // the window's obstacles (indices into q.obstacles), ascending
        for (size_t i = 0; i < q.obstacles.size(); ++i)
            if (boxes_overlap(obox[i], wb)) win.push_back(static_cast<uint32_t>(i));
        std::vector<FootRect> rect1(win.size());
        for (size_t i = 0; i < win.size(); ++i) rect1[i] = rect_grown(q.obstacles[win[i]].shape, q.grow);
        std::vector<Vertex> vs;
        // (i) terrain corners, row-major over the reached cells: a blocked diagonal neighbour with both adjacent
        // orthogonals passable ([port] convex corners only), at the cell centre
        for (int32_t cy = w.y0; cy <= w.y1; ++cy) {
            for (int32_t cx = w.x0; cx <= w.x1; ++cx) {
                if (!reach[li(cx, cy)]) continue;
                bool corner = false;
                for (int32_t dy = -1; dy <= 1 && !corner; dy += 2)
                    for (int32_t dx = -1; dx <= 1 && !corner; dx += 2)
                        corner = !grid.passable_cell(cx + dx, cy + dy, q.mask) && grid.passable_cell(cx + dx, cy, q.mask) &&
                                 grid.passable_cell(cx, cy + dy, q.mask);
                if (corner) vs.push_back({cell_center(cx, cy), -1, 0, cell_center(cx, cy)});
            }
        }
        // (ii) per obstacle the corners of rect1, kept when the matching rect2 corner passes and the corner lies inside
        // no other obstacle's tracker rectangle
        for (size_t i = 0; i < win.size(); ++i) {
            const FootRect rect2 = rect_grown(q.obstacles[win[i]].shape, grow2);
            for (uint32_t c = 0; c < 4; ++c) {
                const V2 p = rect_corner(rect1[i], c), p2 = rect_corner(rect2, c);
                if (!grid.passable_world(p2, q.mask)) continue;
                bool covered = false;
                for (size_t j = 0; j < win.size() && !covered; ++j)
                    covered = j != i && rect_contains(q.obstacles[win[j]].shape, p, q.tracker);
                if (!covered) vs.push_back({p, static_cast<int32_t>(i), c, p2});
            }
        }
        const uint32_t corners = static_cast<uint32_t>(vs.size());
        const uint32_t vstart = corners, vgoal = corners + 1;
        vs.push_back({q.start, -1, 0, q.start});  // (iv)
        vs.push_back({q.goal, -1, 0, q.goal});     // (v)
        const uint32_t nv = static_cast<uint32_t>(vs.size());
        out.vertices = nv;
        out.window = win;
        // (iv) a start inside an obstacle's rect2: the search starts at the nearest vertex instead.
        // s78n t20 F3 [port] (until §5.4.7 replaces it): only a vertex whose leg from the start passes the grid
        // (line_passable) is taken; with none the call fails. Retail does not test that leg.
        uint32_t from = vstart;
        bool start_inside = false;
        for (size_t i = 0; i < win.size() && !start_inside; ++i)
            start_inside = rect_contains(q.obstacles[win[i]].shape, q.start, grow2);
        if (start_inside) {
            float best = 3.0e38f;
            from = nv;
            for (uint32_t i = 0; i < corners; ++i) {
                const float d2 = length_sq(vs[i].p - q.start);
                if (!(d2 < best) || !line_passable(grid, q.start, vs[i].p, q.mask)) continue;
                if (d2 < best) {
                    best = d2;
                    from = i;
                }
            }
            if (from == nv) return false;  // no vertex to start from
        }
        // ---- edges 0x678e80 and costs 0x67a3d0 (edge_class), evaluated on demand: 1 none, 2 plain, 3 crossing
        std::vector<uint8_t> edges(static_cast<size_t>(nv) * nv, 0);
        std::vector<FootRect> tracker(win.size());
        for (size_t i = 0; i < win.size(); ++i) tracker[i] = rect_grown(q.obstacles[win[i]].shape, q.tracker);
        const auto edge = [&](uint32_t i, uint32_t j) -> uint8_t {
            const uint32_t lo = std::min(i, j), hi = std::max(i, j);
            uint8_t& e = edges[static_cast<size_t>(lo) * nv + hi];
            if (e == 0) e = edge_class(grid, q, win, tracker, vs[lo], vs[hi]);
            return e;
        };
        const auto seg_len = [&](uint32_t i, uint32_t j) {
            const V2 d = vs[j].p - vs[i].p;
            if (!q.height) return length(d);
            const float dz = q.height(vs[j].p.x, vs[j].p.y) - q.height(vs[i].p.x, vs[i].p.y);
            return std::sqrt(d.x * d.x + d.y * d.y + dz * dz);
        };
        // ---- A* 0x67b8a0
        std::vector<float> g(nv);
        std::vector<int32_t> parent(nv);
        std::vector<uint8_t> closed(nv);
        const auto run = [&](bool allow_crossing) {
            out.complexity += nv;
            std::fill(g.begin(), g.end(), 3.0e38f);
            std::fill(parent.begin(), parent.end(), -1);
            std::fill(closed.begin(), closed.end(), uint8_t{0});
            if (from != vstart) closed[vstart] = 1;  // the real start is no node then
            RetailHeap<int32_t> heap;
            uint32_t pool = 3u * nv;
            g[from] = 0.0f;
            heap.push(distance(vs[from].p, q.goal), static_cast<int32_t>(from));
            --pool;
            while (!heap.empty()) {
                const uint32_t i = static_cast<uint32_t>(heap.pop().value);
                if (closed[i]) continue;
                closed[i] = 1;
                if (i == vgoal) return true;
                for (uint32_t j = 0; j < nv; ++j) {
                    if (j == i || closed[j]) continue;
                    const uint8_t e = edge(i, j);
                    if (e == 1 || (e == 3 && !allow_crossing)) continue;
                    const float ng = g[i] + seg_len(i, j) * (e == 3 ? k.dynamic_obstacle_overlap_penalty : 1.0f);
                    if (!(ng < g[j])) continue;
                    if (pool == 0) return false;  // the node pool (3 x vertices) is exhausted
                    --pool;
                    g[j] = ng;
                    parent[j] = static_cast<int32_t>(i);
                    heap.push(ng + distance(vs[j].p, q.goal), static_cast<int32_t>(j));
                }
            }
            return false;
        };
        bool found = run(false);
        if (!found) {
            found = run(true);
            out.crossing = found;
        }
        if (!found) return false;
        // ---- output 0x674870: the vertex positions, unsmoothed
        std::vector<uint32_t> chain;
        for (int32_t v = static_cast<int32_t>(vgoal); v >= 0; v = parent[static_cast<size_t>(v)]) chain.push_back(static_cast<uint32_t>(v));
        std::reverse(chain.begin(), chain.end());
        out.cost = g[vgoal];
        size_t first = 0;
        if (from != vstart) {
            // started from the nearest vertex: the real start and the foot of the start on the first segment first
            out.points.push_back(q.start);
            if (chain.size() >= 2) {
                const V2 a = vs[chain[0]].p, d = vs[chain[1]].p - a;
                const float l2 = length_sq(d);
                const float t = l2 > 0.0f ? std::clamp(dot(q.start - a, d) / l2, 0.0f, 1.0f) : 0.0f;
                const V2 foot = a + d * t;
                // s78n t20 F3 [port]: the foot only when the start sees it (else the tested start-to-vertex leg)
                if (foot == q.start) {
                    first = 1;
                } else if (line_passable(grid, q.start, foot, q.mask)) {
                    out.points.push_back(foot);
                    first = 1;
                }
            }
        }
        for (size_t c = first; c < chain.size(); ++c) {
            const Vertex& v = vs[chain[c]];
            if (!out.points.empty() && out.points.back() == v.p) continue;
            if (v.obs >= 0 && v.corner < 2) {  // a front corner: the obstacle's id and hold line
                const AroundObstacle& o = q.obstacles[win[static_cast<size_t>(v.obs)]];
                out.tags.push_back({static_cast<uint32_t>(out.points.size()), o.id, v.p - o.shape.a * (o.foot_hx + q.mover_hx), o.shape.a});
            }
            out.points.push_back(v.p);
        }
        return out.points.size() >= 2;
    }
    return false;
}

uint8_t around_edge_class(const PassabilityGrid& grid, const AroundRequest& q, const AroundVertexRef& a, const AroundVertexRef& b) {
    std::vector<uint32_t> win(q.obstacles.size());
    std::vector<FootRect> tracker(q.obstacles.size());
    for (size_t i = 0; i < q.obstacles.size(); ++i) {
        win[i] = static_cast<uint32_t>(i);
        tracker[i] = rect_grown(q.obstacles[i].shape, q.tracker);
    }
    const auto vertex = [&](const AroundVertexRef& r) {
        if (r.obs < 0 || static_cast<size_t>(r.obs) >= q.obstacles.size()) return Vertex{r.p, -1, 0, r.p};
        const FootRect& s = q.obstacles[static_cast<size_t>(r.obs)].shape;
        const uint32_t c = r.corner & 3u;
        return Vertex{rect_corner(rect_grown(s, q.grow), c), r.obs, c, rect_corner(rect_grown(s, q.grow + q.f), c)};
    };
    return edge_class(grid, q, win, tracker, vertex(a), vertex(b));
}

}  // namespace alamo::tactical::movement
