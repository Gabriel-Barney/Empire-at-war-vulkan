#include "alamo/tactical/movement/land_path.hpp"

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
void RegionMap::build(const PassabilityGrid& grid, std::span<const uint32_t> masks) {
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

    // Per mask: components (BFS in region index order) and clearance.
    std::vector<int32_t> queue;
    for (uint32_t mask : masks_) {
        std::vector<int32_t> comp(regions_.size(), -1);
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
        comp_.push_back(std::move(comp));

        // Chamfer 3-4 distance to the nearest blocked cell; outside the grid counts as blocked.
        constexpr int32_t kBig = 1 << 28;
        std::vector<int32_t> d(n, kBig);
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
        std::vector<float> cl(n, 0.0f);
        for (size_t i = 0; i < n; ++i)
            cl[i] = d[i] <= 0 ? 0.0f : static_cast<float>(d[i]) * (kCellSize / 3.0f) - kHalfCell;
        clear_.push_back(std::move(cl));
    }
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

}  // namespace alamo::tactical::movement
