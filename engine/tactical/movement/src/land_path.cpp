#include "alamo/tactical/movement/land_path.hpp"

#include "alamo/tactical/movement/corridor.hpp"

#include <algorithm>
#include <limits>

namespace alamo::tactical::movement {

namespace {

constexpr float kInfF = std::numeric_limits<float>::max();

inline uint64_t pair_key(int32_t from, int32_t to) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(from)) << 32) | static_cast<uint32_t>(to);
}

inline float cell_dist(int32_t ax, int32_t ay, int32_t bx, int32_t by) {
    const float dx = static_cast<float>(bx - ax), dy = static_cast<float>(by - ay);
    return std::sqrt(dx * dx + dy * dy);
}

inline int32_t clampi(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : (v > hi ? hi : v); }

}  // namespace

// ---------------------------------------------------------------- region map
void RegionMap::build(const PassabilityGrid& grid, std::span<const uint32_t> masks, const ParallelFor& pf) {
    w_ = grid.width();
    h_ = grid.height();
    grid_version_ = grid.version();
    const size_t n = static_cast<size_t>(w_) * h_;
    cell_region_.assign(n, -1);
    regions_.clear();
    adj_begin_.clear();
    adj_.clear();
    masks_.assign(masks.begin(), masks.end());
    comp_.clear();
    clear_.clear();

    const int32_t W = w_ > 0 ? static_cast<int32_t>(w_) - 1 : 0;  // last column not covered
    const int32_t H = h_ > 0 ? static_cast<int32_t>(h_) - 1 : 0;  // last row not covered
    auto at = [this](int32_t x, int32_t y) -> int32_t& { return cell_region_[static_cast<size_t>(y) * w_ + static_cast<size_t>(x)]; };
    auto ok = [&](int32_t x, int32_t y, uint32_t v) { return at(x, y) < 0 && grid.effective(x, y) == v; };
    for (int32_t y = 0; y < H; ++y) {
        for (int32_t x = 0; x < W; ++x) {
            if (at(x, y) >= 0) continue;
            const uint32_t v = grid.effective(x, y);
            int32_t s = 1;
            for (;;) {
                if (x + s >= W || y + s >= H) break;
                bool good = true;
                for (int32_t k = 0; k <= s && good; ++k) good = ok(x + s, y + k, v);
                for (int32_t k = 0; k < s && good; ++k) good = ok(x + k, y + s, v);
                if (!good) break;
                ++s;
            }
            int32_t rw = s, rh = s;
            while (rw < 2 * rh && x + rw < W) {
                bool good = true;
                for (int32_t k = 0; k < rh && good; ++k) good = ok(x + rw, y + k, v);
                if (!good) break;
                ++rw;
            }
            if (rw == rh) {
                while (rh < 2 * rw && y + rh < H) {
                    bool good = true;
                    for (int32_t k = 0; k < rw && good; ++k) good = ok(x + k, y + rh, v);
                    if (!good) break;
                    ++rh;
                }
            }
            if ((rw == 1 && rh == 2) || (rw == 2 && rh == 1)) rw = rh = 1;
            const int32_t id = static_cast<int32_t>(regions_.size());
            for (int32_t yy = y; yy < y + rh; ++yy)
                for (int32_t xx = x; xx < x + rw; ++xx) at(xx, yy) = id;
            Region r;
            r.x = static_cast<uint16_t>(x);
            r.y = static_cast<uint16_t>(y);
            r.w = static_cast<uint16_t>(rw);
            r.h = static_cast<uint16_t>(rh);
            r.bits = v;
            regions_.push_back(r);
        }
    }

    // Adjacency (4-connected shared borders), ascending region index.
    adj_begin_.reserve(regions_.size() + 1);
    std::vector<int32_t> tmp;
    for (size_t ri = 0; ri < regions_.size(); ++ri) {
        adj_begin_.push_back(static_cast<uint32_t>(adj_.size()));
        const Region& r = regions_[ri];
        tmp.clear();
        auto add = [&](int32_t x, int32_t y) {
            const int32_t o = region_at(x, y);
            if (o >= 0 && o != static_cast<int32_t>(ri)) tmp.push_back(o);
        };
        for (int32_t k = 0; k < r.h; ++k) {
            add(r.x - 1, r.y + k);
            add(r.x + r.w, r.y + k);
        }
        for (int32_t k = 0; k < r.w; ++k) {
            add(r.x + k, r.y - 1);
            add(r.x + k, r.y + r.h);
        }
        std::sort(tmp.begin(), tmp.end());
        tmp.erase(std::unique(tmp.begin(), tmp.end()), tmp.end());
        adj_.insert(adj_.end(), tmp.begin(), tmp.end());
    }
    adj_begin_.push_back(static_cast<uint32_t>(adj_.size()));

    // Per mask: components (BFS in region index order) and clearance; one job per mask, each writing its own slot.
    comp_.resize(masks_.size());
    clear_.resize(masks_.size());
    pf(static_cast<uint32_t>(masks_.size()), 1, [&](uint32_t mb, uint32_t me) {
        std::vector<int32_t> queue;
        std::vector<int32_t> d;
        for (uint32_t mi = mb; mi < me; ++mi) {
            const uint32_t mask = masks_[mi];
            std::vector<int32_t>& comp = comp_[mi];
            comp.assign(regions_.size(), -1);
            int32_t next = 0;
            for (size_t ri = 0; ri < regions_.size(); ++ri) {
                if (comp[ri] >= 0 || !passable(regions_[ri].bits, mask)) continue;
                queue.clear();
                queue.push_back(static_cast<int32_t>(ri));
                comp[ri] = next;
                for (size_t qi = 0; qi < queue.size(); ++qi) {
                    for (int32_t nb : neighbors(queue[qi])) {
                        if (comp[static_cast<size_t>(nb)] >= 0 || !passable(regions_[static_cast<size_t>(nb)].bits, mask)) continue;
                        comp[static_cast<size_t>(nb)] = next;
                        queue.push_back(nb);
                    }
                }
                ++next;
            }

            // Chamfer 3-4 distance to the nearest blocked cell; outside the grid counts as blocked.
            constexpr int32_t kBig = 1 << 28;
            d.assign(n, kBig);
            for (uint32_t y = 0; y < h_; ++y)
                for (uint32_t x = 0; x < w_; ++x)
                    if (!grid.passable_cell(static_cast<int32_t>(x), static_cast<int32_t>(y), mask)) d[static_cast<size_t>(y) * w_ + x] = 0;
            auto get = [&](int32_t x, int32_t y) -> int32_t {
                if (x < 0 || y < 0 || static_cast<uint32_t>(x) >= w_ || static_cast<uint32_t>(y) >= h_) return 0;
                return d[static_cast<size_t>(y) * w_ + static_cast<size_t>(x)];
            };
            for (int32_t y = 0; y < static_cast<int32_t>(h_); ++y) {
                for (int32_t x = 0; x < static_cast<int32_t>(w_); ++x) {
                    int32_t& c = d[static_cast<size_t>(y) * w_ + static_cast<size_t>(x)];
                    c = std::min({c, get(x - 1, y) + 3, get(x, y - 1) + 3, get(x - 1, y - 1) + 4, get(x + 1, y - 1) + 4});
                }
            }
            for (int32_t y = static_cast<int32_t>(h_) - 1; y >= 0; --y) {
                for (int32_t x = static_cast<int32_t>(w_) - 1; x >= 0; --x) {
                    int32_t& c = d[static_cast<size_t>(y) * w_ + static_cast<size_t>(x)];
                    c = std::min({c, get(x + 1, y) + 3, get(x, y + 1) + 3, get(x + 1, y + 1) + 4, get(x - 1, y + 1) + 4});
                }
            }
            std::vector<float>& cl = clear_[mi];
            cl.assign(n, 0.0f);
            for (size_t i = 0; i < n; ++i)
                cl[i] = d[i] <= 0 ? 0.0f : static_cast<float>(d[i]) * (kCellSize / 3.0f) - kHalfCell;
        }
    });
}

int32_t RegionMap::mask_slot(uint32_t mask) const {
    for (size_t i = 0; i < masks_.size(); ++i)
        if (masks_[i] == mask) return static_cast<int32_t>(i);
    return -1;
}

float RegionMap::clearance(int32_t slot, int32_t cx, int32_t cy) const {
    if (slot < 0 || cx < 0 || cy < 0 || static_cast<uint32_t>(cx) >= w_ || static_cast<uint32_t>(cy) >= h_) return 0.0f;
    return clear_[static_cast<size_t>(slot)][static_cast<size_t>(cy) * w_ + static_cast<size_t>(cx)];
}

// ---------------------------------------------------------------- visibility
bool segment_passable(const PassabilityGrid& grid, V2 a, V2 b, uint32_t mask, float clearance, const RegionMap* regions,
                      int32_t slot) {
    const bool use_clear = clearance > 0.0f && regions && slot >= 0;
    const auto ok = [&](int32_t cx, int32_t cy) {
        if (!grid.passable_cell(cx, cy, mask)) return false;
        if (use_clear && regions->clearance(slot, cx, cy) < clearance) return false;
        return true;
    };
    if (!walk_cells(a, b, ok)) return false;
    // Thick test: the two parallels at +-kSegmentBand also pass, so every point within the band
    // (a locomotor step a float rounding off the line, §3.5 step 4) lies on a checked cell.
    const V2 d = b - a;
    if (!(length_sq(d) > 0.0f)) return true;
    const V2 n = perp_left(normalize_retail(d)) * kSegmentBand;
    return walk_cells(a + n, b + n, ok) && walk_cells(a - n, b - n, ok);
}

float path_length(std::span<const V2> points) {
    float l = 0.0f;
    for (size_t i = 1; i < points.size(); ++i) l += distance(points[i - 1], points[i]);
    return l;
}

V2 lane_dir(std::span<const V2> fpath, size_t i) {
    const size_t n = fpath.size();
    if (n < 2 || i >= n) return {};
    if (i == 0) {
        const V2 d = normalize_retail(fpath[1] - fpath[0]);
        const float l = length(d);
        return l > 0.0f ? V2{d.x / l, d.y / l} : V2{};
    }
    const V2 din = normalize_retail(fpath[i] - fpath[i - 1]);
    return i + 1 < n ? normalize_retail(din + normalize_retail(fpath[i + 1] - fpath[i])) : din;
}

float LaneShape::offset(float s) const {
    if (shift.empty()) return left;
    const float r = s > 0.0f ? s / kCorridorSampleStep : 0.0f;
    const size_t k = std::min(static_cast<size_t>(r), shift.size() - 1);
    const uint32_t e = k < entry.size() ? entry[k] : 0u;
    return shift[k] + (e != 0 && e < entry_left.size() ? entry_left[e] : left);
}

namespace {
// One lane point: origin o, unit direction n, the pull-in of 0x413340.
V2 lane_point(const PassabilityGrid& grid, bool no_grid, V2 o, V2 n, float left, uint32_t mask) {
    const V2 side = perp_left(n);
    const float step = left / 5.0f;
    float l = left;
    V2 p = o + side * l;
    while (!no_grid && !grid.passable_world(p, mask)) {
        l -= step;
        if (!(std::fabs(l) > std::fabs(step))) return o;
        p = o + side * l;
    }
    return p;
}
}  // namespace

namespace {
struct LanePt {
    V2 p;      // the lane point
    float s;   // its arc length on the formation path
    V2 n;      // the direction its line is crossed in (the waypoint line's normal)
    V2 o;      // bkm1 t01: its origin on the formation path
};

// bkm1 t01 (b0095, MOVEMENT §3.5.2 D, port choice (1) of PHASE4 s78p): 0x413340 tests each lane point alone, so a lane
// segment shifted sideways by L can cross a pillar or a wall corner while both its points pass. Retail keeps the passable
// formation path as the member's path and only steers toward the lane (the lane term), so its member never walks such a
// segment; the port walks the lane, so a segment that fails the unit's class (the static line test 0x5a4c10) falls back
// onto the formation path - its far point to its origin, and while the segment before it then fails, the point before
// too, backwards. The formation path's segments pass the formation's class, which lies inside each member's.
// `keep_last`: the last point is the member's stop point (MOVEMENT §4.3.3 item 5: never moved), so a failing last segment
// falls back on the points before it only.
void lane_fallback(const PassabilityGrid& grid, std::vector<V2>& p, const std::vector<V2>& o, uint32_t mask,
                   bool keep_last = false) {
    for (size_t j = 1; j < p.size(); ++j) {
        if (line_passable(grid, p[j - 1], p[j], mask)) continue;
        if (!(keep_last && j + 1 == p.size())) p[j] = o[j];
        for (size_t k = j; k > 0 && !line_passable(grid, p[k - 1], p[k], mask); --k) p[k - 1] = o[k - 1];
    }
}
// The lane with its arc lengths: one point per waypoint, plus (s78p [port]) inside a segment whose straight lane line fails
// the unit's class (0x5a4c10), a point at each squeeze row (arc 30 k) whose offset differs from a neighbouring row's -
// retail's member takes the row's offset as it walks (the row packs F +0xe0[k], the shift F +0xb0[k]), so a squeeze in the
// middle of a long segment narrows its lane there.
std::vector<LanePt> lane_points(const PassabilityGrid& grid, std::span<const V2> fpath, const LaneShape& shape, uint32_t mask) {
    std::vector<LanePt> out;
    const bool no_grid = grid.width() == 0 || grid.height() == 0;
    const size_t rows = shape.shift.size();
    const auto row_offset = [&](size_t k) { return shape.offset(static_cast<float>(k) * kCorridorSampleStep); };
    float s = 0.0f;
    for (size_t i = 0; i < fpath.size(); ++i) {
        const float s0 = s;
        if (i > 0) s += distance(fpath[i - 1], fpath[i]);
        const V2 n = lane_dir(fpath, i);
        const LanePt w{lane_point(grid, no_grid, fpath[i], n, shape.offset(s), mask), s, n, fpath[i]};
        if (i > 0 && rows > 1 && !no_grid && s > s0 && !line_passable(grid, out.back().p, w.p, mask)) {
            const float l = s - s0;
            const V2 d{(fpath[i].x - fpath[i - 1].x) / l, (fpath[i].y - fpath[i - 1].y) / l};
            for (size_t k = 1; k + 1 < rows; ++k) {
                const float a = static_cast<float>(k) * kCorridorSampleStep;
                if (!(a > s0) || !(a < s)) continue;
                const float o = row_offset(k);
                if (o == row_offset(k - 1) && o == row_offset(k + 1)) continue;
                const V2 ro = fpath[i - 1] + d * (a - s0);
                out.push_back({lane_point(grid, no_grid, ro, d, o, mask), a, d, ro});
            }
        }
        out.push_back(w);
    }
    if (!no_grid && out.size() > 1) {
        std::vector<V2> p(out.size()), o(out.size());
        for (size_t j = 0; j < out.size(); ++j) {
            p[j] = out[j].p;
            o[j] = out[j].o;
        }
        lane_fallback(grid, p, o, mask);
        for (size_t j = 0; j < out.size(); ++j) out[j].p = p[j];
    }
    return out;
}
}  // namespace

std::vector<V2> formation_lane(const PassabilityGrid& grid, std::span<const V2> fpath, const LaneShape& shape, uint32_t mask) {
    std::vector<V2> out;
    for (const LanePt& q : lane_points(grid, fpath, shape, mask)) out.push_back(q.p);
    return out;
}

V2 lane_end(std::span<const V2> fpath, const LaneShape& shape, float back) {
    const size_t n = fpath.size();
    if (n == 0) return {};
    V2 d1{};
    for (size_t k = n - 1; k > 0 && length_sq(d1) == 0.0f; --k) {
        const V2 d = fpath[k] - fpath[k - 1];
        const float l = length(d);
        if (l > 0.0f) d1 = V2{d.x / l, d.y / l};
    }
    const float total = path_length(fpath);
    return fpath[n - 1] - d1 * back + perp_left(d1) * shape.offset(total - back);
}

std::vector<V2> member_lane(const PassabilityGrid& grid, std::span<const V2> fpath, const LaneShape& shape, float back,
                            uint32_t mask, V2 pos) {
    std::vector<V2> out{pos};
    std::vector<V2> org{pos};  // bkm1 t01: each point's origin on the formation path (the start: itself)
    const size_t n = fpath.size();
    if (n == 0) return out;
    const std::vector<LanePt> lane = lane_points(grid, fpath, shape, mask);
    const float total = lane.back().s;
    bool leading = true, passed = false;
    for (size_t i = 1; i < lane.size(); ++i) {
        if (!(total - lane[i].s > back)) break;  // rest falls along the path
        if (leading && dot(pos - lane[i].p, lane[i].n) >= 0.0f) {
            out[0] = lane[i].p;  // passed: the lane goes on from this point
            org[0] = lane[i].o;
            passed = true;
            continue;
        }
        leading = false;
        // a point that lies behind the last one across its own line (a squeeze row's lateral jump on a short step, or a
        // pull-in at a sharp corner) would turn the member back: skipped ([port])
        if (out.size() > 1 && !(dot(lane[i].p - out.back(), lane[i].n) > 0.0f)) continue;
        out.push_back(lane[i].p);
        org.push_back(lane[i].o);
    }
    if (out.size() == 1 && !passed) {
        out[0] = lane[0].p;
        org[0] = lane[0].o;
    }
    // bkm1 t02 (b0096, MOVEMENT §4.3.3 items 2 - 5 [H]): the member stops on the formation path's last waypoint line moved
    // along the last segment by its depth - G + d1 (-back) + d1_perp L, L its lateral at that arc position (slot +
    // squeeze); not along the path's arc, not pulled in, not snapped, not reserved (lane_end)
    out.push_back(lane_end(fpath, shape, back));
    org.push_back(out.back());
    if (grid.width() != 0 && grid.height() != 0) lane_fallback(grid, out, org, mask, true);  // bkm1 t01: the end is kept
    lane_start(out, pos);
    return out;
}

void lane_start(std::vector<V2>& path, V2 pos) {
    if (path.size() < 2) {
        if (!path.empty()) path[0] = pos;
        return;
    }
    const V2 a = path[0], t = path[1];
    const float l = distance(a, t);
    if (!(l > 0.0f)) {
        path[0] = pos;
        return;
    }
    const V2 d{(t.x - a.x) / l, (t.y - a.y) / l};
    const V2 foot = a + d * dot(pos - a, d);
    path[0] = dot(t - foot, d) > 0.0f ? foot : pos;
}

// ---------------------------------------------------------------- region A*
bool region_astar(const PassabilityGrid& grid, const RegionMap& regions, V2 start, V2 goal, uint32_t mask,
                  LandPathScratch& sc, std::vector<V2>& points, uint32_t max_nodes, uint32_t* expansions,
                  const ThreatCost* threat) {
    (void)grid;
    points.clear();
    const int32_t scx = world_to_cell(start.x), scy = world_to_cell(start.y);
    const int32_t gcx = world_to_cell(goal.x), gcy = world_to_cell(goal.y);
    const int32_t rs = regions.region_at(scx, scy), rg = regions.region_at(gcx, gcy);
    if (rs < 0 || rg < 0) return false;
    if (!passable(regions.region(rs).bits, mask) || !passable(regions.region(rg).bits, mask)) return false;
    if (rs == rg) {
        points = {start, goal};
        return true;
    }
    sc.nodes.clear();
    sc.heap.clear();
    sc.heap.set_capacity(0);
    sc.best.clear();
    LandPathScratch::Node s0;
    s0.region = rs;
    s0.from = -1;
    s0.cx = s0.xx = scx;
    s0.cy = s0.xy = scy;
    sc.nodes.push_back(s0);
    sc.best[pair_key(-1, rs)] = {0.0f, false};
    sc.heap.push(cell_dist(scx, scy, gcx, gcy), 0);
    uint32_t exp = 0;
    int32_t found = -1;
    while (!sc.heap.empty()) {
        const int32_t ni = sc.heap.pop().value;
        const LandPathScratch::Node n = sc.nodes[static_cast<size_t>(ni)];
        LandPathScratch::Best& b = sc.best[pair_key(n.from, n.region)];
        if (n.g > b.g || b.closed) continue;  // stale entry
        b.closed = true;
        ++exp;
        if (n.region == rg) {
            found = ni;
            break;
        }
        const Region A = regions.region(n.region);
        for (int32_t nb : regions.neighbors(n.region)) {
            const Region B = regions.region(nb);
            if (!passable(B.bits, mask)) continue;
            int32_t ex = 0, ey = 0, xx = 0, xy = 0;
            if (B.x == A.x + A.w || B.x + B.w == A.x) {
                const int32_t lo = std::max<int32_t>(A.y, B.y), hi = std::min<int32_t>(A.y + A.h, B.y + B.h) - 1;
                if (lo > hi) continue;
                ey = xy = clampi(n.cy, lo, hi);
                if (B.x == A.x + A.w) {
                    ex = B.x;
                    xx = B.x - 1;
                } else {
                    ex = A.x - 1;
                    xx = A.x;
                }
            } else if (B.y == A.y + A.h || B.y + B.h == A.y) {
                const int32_t lo = std::max<int32_t>(A.x, B.x), hi = std::min<int32_t>(A.x + A.w, B.x + B.w) - 1;
                if (lo > hi) continue;
                ex = xx = clampi(n.cx, lo, hi);
                if (B.y == A.y + A.h) {
                    ey = B.y;
                    xy = B.y - 1;
                } else {
                    ey = A.y - 1;
                    xy = A.y;
                }
            } else {
                continue;
            }
            float step = cell_dist(n.cx, n.cy, ex, ey);
            if (threat && threat->active()) step *= threat->factor(cell_center(ex, ey));  // §4.2 (0x5a38a0)
            const float g = n.g + step;
            const uint64_t k = pair_key(n.region, nb);
            auto it = sc.best.find(k);
            if (it != sc.best.end() && !(g < it->second.g)) continue;
            if (sc.nodes.size() >= max_nodes) {
                if (expansions) *expansions = exp;
                return false;
            }
            sc.best[k] = {g, false};
            LandPathScratch::Node m;
            m.region = nb;
            m.from = n.region;
            m.cx = ex;
            m.cy = ey;
            m.xx = xx;
            m.xy = xy;
            m.g = g;
            m.parent = ni;
            sc.nodes.push_back(m);
            sc.heap.push(g + cell_dist(ex, ey, gcx, gcy), static_cast<int32_t>(sc.nodes.size() - 1));
        }
    }
    if (expansions) *expansions = exp;
    if (found < 0) return false;
    std::vector<int32_t> chain;
    for (int32_t i = found; i >= 0; i = sc.nodes[static_cast<size_t>(i)].parent) chain.push_back(i);
    std::reverse(chain.begin(), chain.end());
    points.push_back(start);
    for (size_t i = 1; i < chain.size(); ++i) {
        const LandPathScratch::Node& m = sc.nodes[static_cast<size_t>(chain[i])];
        points.push_back(cell_center(m.xx, m.xy));
        points.push_back(cell_center(m.cx, m.cy));
    }
    points.push_back(goal);
    points.erase(std::unique(points.begin(), points.end()), points.end());
    if (points.size() < 2) points = {start, goal};
    return true;
}

// ---------------------------------------------------------------- cell A*
bool cell_astar(const PassabilityGrid& grid, const RegionMap& regions, V2 start, V2 goal, uint32_t mask, float min_clearance,
                LandPathScratch& sc, std::vector<V2>& points, uint32_t max_nodes, uint32_t* expansions) {
    points.clear();
    const uint32_t W = grid.width(), H = grid.height();
    const int32_t scx = world_to_cell(start.x), scy = world_to_cell(start.y);
    const int32_t gcx = world_to_cell(goal.x), gcy = world_to_cell(goal.y);
    if (!grid.passable_cell(scx, scy, mask) || !grid.passable_cell(gcx, gcy, mask)) return false;
    if (scx == gcx && scy == gcy) {
        points = {start, goal};
        return true;
    }
    const size_t n = static_cast<size_t>(W) * H;
    if (sc.cell_g.size() != n) {
        sc.cell_g.assign(n, 0.0f);
        sc.cell_gen.assign(n, 0);
        sc.cell_parent.assign(n, -1);
        sc.cell_closed.assign(n, 0);
        sc.gen = 0;
    }
    if (++sc.gen == 0) {
        std::fill(sc.cell_gen.begin(), sc.cell_gen.end(), 0u);
        std::fill(sc.cell_closed.begin(), sc.cell_closed.end(), 0u);
        sc.gen = 1;
    }
    const uint32_t gen = sc.gen;
    const int32_t slot = min_clearance > 0.0f ? regions.mask_slot(mask) : -1;
    auto index = [W](int32_t x, int32_t y) { return static_cast<size_t>(y) * W + static_cast<size_t>(x); };
    auto heur = [&](int32_t x, int32_t y) {
        return static_cast<float>((x > gcx ? x - gcx : gcx - x) + (y > gcy ? y - gcy : gcy - y));
    };
    sc.heap.clear();
    sc.heap.set_capacity(0);
    const size_t si = index(scx, scy), gi = index(gcx, gcy);
    sc.cell_gen[si] = gen;
    sc.cell_g[si] = 0.0f;
    sc.cell_parent[si] = -1;
    sc.heap.push(heur(scx, scy), static_cast<int32_t>(si));
    static constexpr int32_t kDx[4] = {1, 0, -1, 0};
    static constexpr int32_t kDy[4] = {0, 1, 0, -1};
    uint32_t exp = 0, pushed = 1;
    bool found = false;
    while (!sc.heap.empty()) {
        const size_t ci = static_cast<size_t>(sc.heap.pop().value);
        if (sc.cell_closed[ci] == gen) continue;
        sc.cell_closed[ci] = gen;
        ++exp;
        if (ci == gi) {
            found = true;
            break;
        }
        const int32_t cx = static_cast<int32_t>(ci % W), cy = static_cast<int32_t>(ci / W);
        const float g0 = sc.cell_g[ci];
        for (int k = 0; k < 4; ++k) {
            const int32_t nx = cx + kDx[k], ny = cy + kDy[k];
            if (!grid.passable_cell(nx, ny, mask)) continue;
            const size_t ni = index(nx, ny);
            if (slot >= 0 && ni != gi && regions.clearance(slot, nx, ny) < min_clearance) continue;
            if (sc.cell_closed[ni] == gen) continue;
            const float g = g0 + 1.0f;
            if (sc.cell_gen[ni] == gen && !(g < sc.cell_g[ni])) continue;
            if (pushed >= max_nodes) {
                if (expansions) *expansions = exp;
                return false;
            }
            sc.cell_gen[ni] = gen;
            sc.cell_g[ni] = g;
            sc.cell_parent[ni] = static_cast<int32_t>(ci);
            sc.heap.push(g + heur(nx, ny), static_cast<int32_t>(ni));
            ++pushed;
        }
    }
    if (expansions) *expansions = exp;
    if (!found) return false;
    std::vector<size_t> cells;
    for (int32_t c = static_cast<int32_t>(gi); c >= 0; c = sc.cell_parent[static_cast<size_t>(c)]) {
        cells.push_back(static_cast<size_t>(c));
        if (static_cast<size_t>(c) == si) break;
    }
    std::reverse(cells.begin(), cells.end());
    points.push_back(start);
    for (size_t i = 1; i + 1 < cells.size(); ++i)
        points.push_back(cell_center(static_cast<int32_t>(cells[i] % W), static_cast<int32_t>(cells[i] / W)));
    points.push_back(goal);
    return true;
}

// ---------------------------------------------------------------- smoothing
namespace {

struct Smoother {
    const PassabilityGrid& grid;
    uint32_t mask;
    float clearance;
    const RegionMap* regions;
    int32_t slot;
    const ThreatCost* threat = nullptr;
    bool visible(V2 a, V2 b) const {
        if (!segment_passable(grid, a, b, mask, clearance, regions, slot)) return false;
        if (!threat || !threat->active() || !threat->fn || !*threat->fn) return true;
        const ThreatFn& f = *threat->fn;
        const float limit = std::max(threat->threshold, std::max(f(a), f(b)));
        const int32_t ax = world_to_cell(a.x), ay = world_to_cell(a.y), bx = world_to_cell(b.x), by = world_to_cell(b.y);
        return walk_cells(a, b, [&](int32_t cx, int32_t cy) {
            return (cx == ax && cy == ay) || (cx == bx && cy == by) || f(cell_center(cx, cy)) <= limit;
        });
    }

    // Point at arc length s; seg = index of the segment start (points[seg] .. points[seg + 1]).
    static V2 at(const std::vector<V2>& p, float s, size_t& seg) {
        float acc = 0.0f;
        for (size_t i = 1; i < p.size(); ++i) {
            const float l = distance(p[i - 1], p[i]);
            if (s <= acc + l || i + 1 == p.size()) {
                seg = i - 1;
                const float t = l > 0.0f ? std::clamp((s - acc) / l, 0.0f, 1.0f) : 0.0f;
                return lerp(p[i - 1], p[i], t);
            }
            acc += l;
        }
        seg = 0;
        return p.empty() ? V2{} : p.front();
    }

    void shortcut(std::vector<V2>& p, float a, float b, int depth) const {
        if (b - a < 15.0f || depth > 10 || p.size() < 3) return;
        size_t sa = 0, sb = 0;
        const V2 pa = at(p, a, sa);
        const V2 pb = at(p, b, sb);
        if (sb <= sa) return;  // no interior vertex in (a, b)
        // Skip rules (§1.4 pass (b); PROVISIONAL reading [L]): nothing to gain when every interior
        // turn is nearly straight (dot > 0.9), or when an end of the span is closer than 20 to the
        // nearest interior vertex (micro-shortcuts at a corner).
        bool all_straight = true;
        for (size_t i = sa + 1; i <= sb && all_straight; ++i) {
            const V2 d0 = normalize_retail(p[i] - p[i - 1]), d1 = normalize_retail(p[i + 1] - p[i]);
            if (dot(d0, d1) <= 0.9f) all_straight = false;
        }
        if (all_straight) return;
        if (distance(pa, p[sa + 1]) < 20.0f || distance(p[sb], pb) < 20.0f) {
            // Still try the halves: the span may contain a farther corner worth cutting.
            if (sb > sa + 1) {
                const float l0 = path_length(p);
                shortcut(p, a, a + 0.66f * (b - a), depth + 1);
                const float l1 = path_length(p);
                const float k = l0 > 0.0f ? l1 / l0 : 1.0f;
                shortcut(p, (a + 0.33f * (b - a)) * k, b * k, depth + 1);
            }
            return;
        }
        // The two cut points lie on existing segments; re-check the sub-segments too, since a point
        // rounded off its segment can touch a different corner cell.
        if (visible(pa, pb) && visible(p[sa], pa) && visible(pb, p[sb + 1])) {
            std::vector<V2> q;
            q.reserve(p.size());
            q.insert(q.end(), p.begin(), p.begin() + static_cast<std::ptrdiff_t>(sa) + 1);
            if (!(q.back() == pa)) q.push_back(pa);
            if (!(q.back() == pb)) q.push_back(pb);
            for (size_t i = sb + 1; i < p.size(); ++i)
                if (!(q.back() == p[i])) q.push_back(p[i]);
            p.swap(q);
            return;
        }
        const float l0 = path_length(p);
        shortcut(p, a, a + 0.66f * (b - a), depth + 1);
        const float l1 = path_length(p);
        const float k = l0 > 0.0f ? l1 / l0 : 1.0f;
        shortcut(p, (a + 0.33f * (b - a)) * k, b * k, depth + 1);
    }
};

}  // namespace

void smooth_path(const PassabilityGrid& grid, std::vector<V2>& p, uint32_t mask, float clearance, const RegionMap* regions,
                 int32_t slot, const ThreatCost* threat) {
    if (p.size() < 3) return;
    const Smoother sm{grid, mask, clearance, regions, slot, threat};
    // Pass (a): furthest visible later waypoint by binary search.
    std::vector<V2> q;
    q.push_back(p.front());
    size_t i = 0;
    while (i + 1 < p.size()) {
        size_t lo = i + 1, hi = p.size() - 1;
        while (lo < hi) {
            const size_t mid = (lo + hi + 1) / 2;
            if (sm.visible(p[i], p[mid])) lo = mid;
            else hi = mid - 1;
        }
        q.push_back(p[lo]);
        i = lo;
    }
    p.swap(q);
    // Pass (b): recursive arc-length shortcut.
    sm.shortcut(p, 0.0f, path_length(p), 0);
}

bool snap_to_component(const PassabilityGrid& grid, const RegionMap& regions, V2 p, uint32_t mask, int32_t component,
                       int32_t max_rings, V2& out) {
    const int32_t slot = regions.mask_slot(mask);
    const int32_t cx = world_to_cell(p.x), cy = world_to_cell(p.y);
    float best = kInfF;
    bool found = false;
    int32_t found_ring = -1;
    for (int32_t r = 0; r <= max_rings; ++r) {
        if (found && r > found_ring + 1) break;
        for (int32_t y = cy - r; y <= cy + r; ++y) {
            for (int32_t x = cx - r; x <= cx + r; ++x) {
                if (std::max(x > cx ? x - cx : cx - x, y > cy ? y - cy : cy - y) != r) continue;
                if (!grid.passable_cell(x, y, mask)) continue;
                if (component >= 0 && (slot < 0 || regions.component_at(slot, x, y) != component)) continue;
                if (component < 0 && slot >= 0 && regions.component_at(slot, x, y) < 0) continue;
                const float d = distance(p, cell_center(x, y));
                if (d < best) {
                    best = d;
                    out = cell_center(x, y);
                    if (!found) found_ring = r;
                    found = true;
                }
            }
        }
    }
    return found;
}

// ---------------------------------------------------------------- s78n t20 batch 2: zones (MOVEMENT §4.3.1 item 7)
int32_t zone_at(const RegionMap& regions, int32_t slot, int32_t cx, int32_t cy) {
    if (slot < 0) return -1;
    const int32_t r = regions.region_at(cx, cy);
    if (r < 0) return -1;
    const int32_t c = regions.component(slot, r);
    return c < 0 ? 0 : c + 1;
}

bool zone_connected(const RegionMap& regions, uint32_t mask, V2 a, V2 b) {
    const int32_t slot = regions.mask_slot(mask);
    const int32_t za = zone_at(regions, slot, world_to_cell(a.x), world_to_cell(a.y));
    return za > 0 && za == zone_at(regions, slot, world_to_cell(b.x), world_to_cell(b.y));
}

bool zone_validate(const RegionMap& regions, V2 p, V2 q, uint32_t mask, V2& out) {
    const int32_t slot = regions.mask_slot(mask);
    const int32_t w = static_cast<int32_t>(regions.width()), h = static_cast<int32_t>(regions.height());
    const auto inside = [&](int32_t x, int32_t y) { return x >= 0 && y >= 0 && x < w && y < h; };
    const int32_t px = world_to_cell(p.x), py = world_to_cell(p.y), qx = world_to_cell(q.x), qy = world_to_cell(q.y);
    const int32_t zq = zone_at(regions, slot, qx, qy);
    if ((zq == -1 && !inside(qx, qy)) || zq == zone_at(regions, slot, px, py)) {
        out = p;
        return true;
    }
    for (int32_t r = 1; r < 30; ++r) {
        float best = 0.0f;
        bool found = false;
        V2 pick;
        const auto consider = [&](int32_t x, int32_t y) {
            if (!inside(x, y) || zone_at(regions, slot, x, y) != zq) return;
            const V2 c = cell_center(x, y);
            const float d = distance(q, c);
            if (!found || d < best) {
                best = d;
                pick = c;
                found = true;
            }
        };
        // a pair's mirror cell is looked at only when its first cell is inside the grid and has a region (as decompiled)
        const auto pair = [&](int32_t x, int32_t y, int32_t mx, int32_t my) {
            consider(x, y);
            if (inside(x, y) && regions.region_at(x, y) >= 0) consider(mx, my);
        };
        for (int32_t i = -r; i <= r; ++i) pair(px + i, py + r, px - i, py - r);
        for (int32_t j = 1 - r; j <= r - 1; ++j) pair(px + r, py + j, px - r, py - j);
        if (found) {
            out = pick;
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------- main entry
bool find_land_path(const PassabilityGrid& grid, const RegionMap& regions, const LandPathRequest& req, LandPathScratch& sc,
                    LandPath& out) {
    out = LandPath{};
    const uint32_t mask = req.mask;
    const int32_t slot = regions.mask_slot(mask);
    const int32_t max_rings = static_cast<int32_t>(std::max(grid.width(), grid.height()));
    V2 start = req.start;
    std::vector<V2> prefix;  // escape segment when the start cell is blocked
    int32_t scomp = slot >= 0 ? regions.component_at(slot, world_to_cell(start.x), world_to_cell(start.y)) : 0;
    if (!grid.passable_world(start, mask) || scomp < 0) {
        V2 s2;
        if (!snap_to_component(grid, regions, start, mask, -1, max_rings, s2)) return false;
        prefix.push_back(start);
        start = s2;
        out.start_blocked = true;
        scomp = slot >= 0 ? regions.component_at(slot, world_to_cell(start.x), world_to_cell(start.y)) : 0;
    }
    V2 goal = req.goal;
    const int32_t gcomp = slot >= 0 ? regions.component_at(slot, world_to_cell(goal.x), world_to_cell(goal.y)) : 0;
    if (!grid.passable_world(goal, mask) || gcomp != scomp) {
        if (!req.snap_goal) return false;
        V2 g2;
        if (!snap_to_component(grid, regions, goal, mask, slot >= 0 ? scomp : -1, max_rings, g2)) return false;
        goal = g2;
    }
    out.goal_used = goal;

    std::vector<V2> pts;
    bool ok = false;
    if (!req.use_cell_astar) ok = region_astar(grid, regions, start, goal, mask, sc, pts, req.max_nodes, &out.expansions, &req.threat);
    if (ok) {
        for (size_t i = 1; i < pts.size() && ok; ++i) ok = segment_passable(grid, pts[i - 1], pts[i], mask);
    }
    if (!ok) ok = cell_astar(grid, regions, start, goal, mask, 0.0f, sc, pts, req.max_nodes, &out.expansions);
    if (!ok) return false;
    if (req.smooth) smooth_path(grid, pts, mask, 0.0f, nullptr, -1, &req.threat);

    // Large units: clearance check sampled every 20 units, re-plan when below radius - 0.1.
    const float r = req.clearance_radius;
    if (r > 0.0f && slot >= 0) {
        bool low = false;
        const float total = path_length(pts);
        for (float s = r; s < total - r && !low; s += kCellSize) {
            size_t seg = 0;
            const V2 q = Smoother::at(pts, s, seg);
            if (regions.clearance(slot, world_to_cell(q.x), world_to_cell(q.y)) < r - 0.1f) low = true;
        }
        if (low) {
            std::vector<V2> alt;
            if (cell_astar(grid, regions, start, goal, mask, r, sc, alt, req.max_nodes, nullptr)) {
                if (req.smooth) smooth_path(grid, alt, mask, r, &regions, slot);
                if (path_length(alt) <= 1.4f * total) {
                    pts.swap(alt);
                    out.clearance_replanned = true;
                }
            }
        }
    }
    out.points = std::move(prefix);
    out.points.insert(out.points.end(), pts.begin(), pts.end());
    out.ok = true;
    return true;
}

// ---- s78n t26.3 (MOVEMENT §3.5.2 D items 2-7, s78n t26 RE 2): the corner-speed pass 0x5070b0 ----
void corner_arc_probe(V2 pj, V2 a, float v, float omega, float theta_s, float seg, std::vector<V2>& out) {
    out.clear();
    out.push_back(pj);
    const float th = std::fabs(theta_s);
    const float rad = v / omega;
    const V2 t = normalize_retail(a - pj);
    const bool left = theta_s >= 0.0f;
    const V2 n = left ? V2{-t.y, t.x} : V2{t.y, -t.x};
    const float s = left ? 1.0f : -1.0f;
    // s78o (b0088) [port]: the point count is capped at kCornerArcProbeMaxPoints (an arc of 2^14 segments of at least 2
    // units is longer than any map) and a non-finite count takes the cap: a tiny positive Max_Rate_Of_Turn or a corner
    // near pi in custom data no longer plans ~1e7 points per corner or casts past UINT32_MAX (undefined, MSVC != clang)
    const float nf = th * v / omega / seg + 3.0f;
    const uint32_t count = nf < static_cast<float>(kCornerArcProbeMaxPoints) ? static_cast<uint32_t>(nf) : kCornerArcProbeMaxPoints;
    const float sin_th = dmath::sin(th), cos_th = dmath::cos(th);
    for (uint32_t k = 1; k <= count; ++k) {
        const float u = static_cast<float>(k) / static_cast<float>(count);
        const V2 p = pj + t * ((sin_th - dmath::sin((1.0f - 2.0f * u) * th)) * rad) +
                     n * ((dmath::cos((2.0f * u - 1.0f) * th) - cos_th) * rad);
        const V2 last = out.back();
        if (cross(a - last, p - last) * s < 0.0f) break;  // p_k has passed the aim line
        out.push_back(p);
    }
    out.push_back(a);
}

std::vector<float> corner_fractions(const std::vector<V2>& poly, const CornerPassIn& in, CornerPassStats* stats) {
    std::vector<float> f;
    const size_t count = poly.size();
    const float omega = in.rate * dmath::kPiF / 180.0f;
    // [port] a unit that cannot turn (R <= 0) or move (vmax <= 0) gets no pass: its fractions stay 1.0
    if (count < 2 || !(in.vmax > 0.0f) || !(omega > 0.0f)) return f;
    f.assign(count, 1.0f);
    const auto angle = [](V2 din, V2 d) { return dmath::acos(std::clamp(dot(din, d), -1.0f, 1.0f)); };
    std::vector<V2> probe;
    size_t j = 0;
    while (j + 1 < count) {
        const V2 pj = poly[j];
        const V2 din = j == 0 ? dir_from_deg(in.yaw0) : normalize_retail(pj - poly[j - 1]);
        size_t i = j + 1;
        float len = distance(pj, poly[i]);
        V2 d = normalize_retail(poly[i] - pj);
        float theta = angle(din, d);
        const float cr = cross(din, d);
        const float side = cr > 0.0f ? 1.0f : (cr < 0.0f ? -1.0f : 0.0f);
        V2 aim = poly[i];
        if (in.f_min > 0.0f) {  // item 3: the merge and the aim point, only with f_min > 0
            float l_turn = 2.0f * in.vmax * dmath::sin(theta) / omega;
            V2 b = pj;
            while (len < l_turn && i + 1 < count) {
                if (side * cross(din, normalize_retail(poly[i + 1] - pj)) < 0.0f) break;  // the next point on the other side
                b = poly[i];
                ++i;
                len = distance(pj, poly[i]);
                d = normalize_retail(poly[i] - pj);
                theta = angle(din, d);
                l_turn = 2.0f * in.vmax * dmath::sin(theta) / omega;
            }
            if (len > l_turn) {
                aim = b + d * std::max(0.0f, distance(b, poly[i]) - (len - l_turn));
                len = distance(pj, aim);
                // [port] an aim point on P_j itself (l_turn 0: theta 0) keeps the leg's angle
                if (len > 0.0f) theta = angle(din, normalize_retail(aim - pj));
            }
        }
        // item 4: the corner speed
        float v = in.vmax;
        if (theta != 0.0f) {
            const float r = std::max(20.0f, std::min(len * 0.5f, 100.0f));
            v = std::min(in.vmax, std::min(omega * r / (1.0f - dmath::cos(theta)), len * omega / (2.0f * dmath::sin(theta))));
        }
        // item 5: the arc probe, the static line test and the corridor test; else v /= 1.5
        const float theta_s = cr > 0.0f ? -theta : theta;
        while (v > 0.0f) {
            corner_arc_probe(pj, aim, v, omega, theta_s, in.seg, probe);
            bool line = true;
            for (size_t k = 0; k + 1 < probe.size() && line; ++k)
                if (in.line_ok && !in.line_ok(in.ctx, probe[k], probe[k + 1])) line = false;
            if (line && ((v < 0.2f * in.vmax && j != 0) || !in.corridor_ok || in.corridor_ok(in.ctx, probe))) break;
            v /= 1.5f;
            if (stats) ++stats->stepdowns;
            if (v < in.vmin || v < 0.1f * in.vmax) {
                v = line ? in.vmin : 0.0f;
                break;
            }
        }
        // item 6: the fraction; below f_min 0 on the unmerged leg's start only
        const float fr = v / in.vmax;
        if (fr < in.f_min) {
            f[j] = 0.0f;
            if (stats) ++stats->zero;
            j += 1;
        } else {
            for (size_t k = j; k < i; ++k) f[k] = fr;
            if (stats) {
                if (fr <= 0.0f) ++stats->zero;
                else if (fr < 1.0f) ++stats->slowed;
            }
            j = i;
        }
    }
    f[count - 1] = 1.0f;
    // item 7: the brake pass (2-D segments)
    if (in.decel > 0.0f && count >= 3) {
        float v = 0.0f;
        for (size_t k = count - 1; k-- > 0;) {
            v = std::sqrt(2.0f * in.decel * distance(poly[k], poly[k + 1]) + v * v);
            f[k] = std::min(f[k], v / in.vmax);
            v = in.vmax * f[k];
        }
    }
    return f;
}

}  // namespace alamo::tactical::movement
