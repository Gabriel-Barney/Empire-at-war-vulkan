#include "alamo/tactical/movement/collision.hpp"

#include <algorithm>

#include "alamo/tactical/movement/heap.hpp"
#include "alamo/tactical/movement/land_path.hpp"
#include "collision_key.hpp"

namespace alamo::tactical::movement {

namespace {
bool one_side(int32_t obstacle_class, int32_t other) {
    switch (obstacle_class) {
        case kUccBuildPad: return false;
        case kUccLowObstacle: return other == kUccInfantry;
        case kUccMediumObstacle: return other >= kUccInfantry && other <= kUccLargeVehicle;
        case kUccHighObstacle: return other >= kUccInfantry && other <= kUccGiantVehicle;
        default: return true;
    }
}

// Smallest u in [0, len] with |s + dir u - c| < r (dir unit), or -1.
float ray_circle(V2 s, V2 dir, float len, V2 c, float r) {
    const V2 m = s - c;
    const float cc = length_sq(m) - r * r;
    if (cc < 0.0f) return 0.0f;
    const float b = dot(m, dir);
    if (b > 0.0f) return -1.0f;
    const float disc = b * b - cc;
    if (disc < 0.0f) return -1.0f;
    const float u = -b - std::sqrt(disc);
    return u <= len ? std::max(u, 0.0f) : -1.0f;
}
}  // namespace

bool collision_classes_interact(int32_t a, int32_t b) { return one_side(a, b) && one_side(b, a); }

bool predict_moving_contact(V2 pa, V2 va, V2 pb, V2 vb, float radius_sum, float frames, float& t_contact) {
    const V2 p = pb - pa, v = vb - va;
    const float c = length_sq(p) - radius_sum * radius_sum;
    if (c < 0.0f) {
        t_contact = 0.0f;
        return true;
    }
    const float a = length_sq(v);
    if (!(a > 0.0f)) return false;
    const float b = dot(p, v);
    if (b >= 0.0f) return false;  // separating
    const float disc = b * b - a * c;
    if (disc < 0.0f) return false;
    const float t = (-b - std::sqrt(disc)) / a;
    if (t > frames) return false;
    t_contact = std::max(t, 0.0f);
    return true;
}

float path_contact_distance(V2 pos, std::span<const V2> path, uint32_t next, V2 obstacle, float radius_sum, float max_dist) {
    float acc = 0.0f;
    V2 s = pos;
    for (size_t i = next; i < path.size() && acc <= max_dist; ++i) {
        const V2 d = path[i] - s;
        const float len = length(d);
        if (len > 0.0f) {
            const V2 dir = d * (1.0f / len);
            const float u = ray_circle(s, dir, len, obstacle, radius_sum);
            if (u >= 0.0f) return acc + u <= max_dist ? acc + u : -1.0f;
        }
        acc += len;
        s = path[i];
    }
    return -1.0f;
}

float overlap_ratio(V2 a, float ra, V2 b, float rb) {
    const float d = distance(a, b);
    const float pen = ra + rb - d;
    if (!(pen > 0.0f)) return 0.0f;
    const float dia = 2.0f * std::min(ra, rb);
    return dia > 0.0f ? pen / dia : 1.0f;
}

bool dynamic_repath(const PassabilityGrid& grid, V2 start, V2 target, uint32_t mask, float self_radius, bool giant,
                    std::span<const DynamicObstacle> obstacles, const MovementConstants& k, std::vector<V2>& out,
                    uint32_t& complexity) {
    out.clear();
    complexity = 0;
    const int32_t sx = world_to_cell(start.x), sy = world_to_cell(start.y);
    const int32_t tx = world_to_cell(target.x), ty = world_to_cell(target.y);
    if (!grid.passable_cell(tx, ty, mask)) return false;
    const int32_t half = static_cast<int32_t>(std::ceil(self_radius / kCellSize));
    int32_t grow = static_cast<int32_t>(k.dynamic_avoidance_rectangle_bound) + half + (giant ? 0 : 2);
    const int32_t W = static_cast<int32_t>(grid.width()), H = static_cast<int32_t>(grid.height());
    for (int attempt = 0; attempt < 20; ++attempt, grow += 3) {
        int32_t x0 = std::min(sx, tx) - grow, x1 = std::max(sx, tx) + grow;
        int32_t y0 = std::min(sy, ty) - grow, y1 = std::max(sy, ty) + grow;
        // Balance the aspect to within 1.5:1.
        const int32_t ww = x1 - x0 + 1, hh = y1 - y0 + 1;
        if (static_cast<float>(ww) > 1.5f * static_cast<float>(hh)) {
            const int32_t add = (static_cast<int32_t>(static_cast<float>(ww) / 1.5f) - hh + 1) / 2;
            y0 -= add;
            y1 += add;
        } else if (static_cast<float>(hh) > 1.5f * static_cast<float>(ww)) {
            const int32_t add = (static_cast<int32_t>(static_cast<float>(hh) / 1.5f) - ww + 1) / 2;
            x0 -= add;
            x1 += add;
        }
        x0 = std::max(x0, 0);
        y0 = std::max(y0, 0);
        x1 = std::min(x1, W - 1);
        y1 = std::min(y1, H - 1);
        const int32_t bw = x1 - x0 + 1, bh = y1 - y0 + 1;
        if (bw <= 0 || bh <= 0) return false;
        // Obstacles inside the window.
        uint32_t in_window = 0;
        const float wx0 = static_cast<float>(x0) * kCellSize - kHalfCell, wx1 = static_cast<float>(x1) * kCellSize + kHalfCell;
        const float wy0 = static_cast<float>(y0) * kCellSize - kHalfCell, wy1 = static_cast<float>(y1) * kCellSize + kHalfCell;
        for (const DynamicObstacle& o : obstacles)
            if (o.pos.x + o.radius >= wx0 && o.pos.x - o.radius <= wx1 && o.pos.y + o.radius >= wy0 && o.pos.y - o.radius <= wy1)
                ++in_window;
        complexity += 4 * in_window + 2;
        const size_t n = static_cast<size_t>(bw) * static_cast<size_t>(bh);
        std::vector<uint8_t> pen(n, 0), ok(n, 0), closed(n, 0);
        std::vector<float> g(n, 3.0e38f);
        std::vector<int32_t> parent(n, -1);
        for (int32_t y = 0; y < bh; ++y) {
            for (int32_t x = 0; x < bw; ++x) {
                const size_t i = static_cast<size_t>(y) * static_cast<size_t>(bw) + static_cast<size_t>(x);
                ok[i] = grid.passable_cell(x0 + x, y0 + y, mask) ? 1 : 0;
                const V2 c = cell_center(x0 + x, y0 + y);
                for (const DynamicObstacle& o : obstacles) {
                    const float r = o.radius + self_radius;
                    if (length_sq(c - o.pos) < r * r) {
                        pen[i] = 1;
                        break;
                    }
                }
            }
        }
        auto inside = [&](int32_t cx, int32_t cy) { return cx >= x0 && cy >= y0 && cx <= x1 && cy <= y1; };
        if (!inside(sx, sy) || !inside(tx, ty)) continue;
        auto li = [&](int32_t cx, int32_t cy) {
            return static_cast<size_t>(cy - y0) * static_cast<size_t>(bw) + static_cast<size_t>(cx - x0);
        };
        const size_t si = li(sx, sy), ti = li(tx, ty);
        auto h = [&](int32_t cx, int32_t cy) {
            const float dx = std::fabs(static_cast<float>(cx - tx)), dy = std::fabs(static_cast<float>(cy - ty));
            return std::max(dx, dy) + 0.41421356f * std::min(dx, dy);
        };
        RetailHeap<int32_t> heap;
        g[si] = 0.0f;
        heap.push(h(sx, sy), static_cast<int32_t>(si));
        static constexpr int32_t kDx[8] = {1, 0, -1, 0, 1, -1, -1, 1};
        static constexpr int32_t kDy[8] = {0, 1, 0, -1, 1, 1, -1, -1};
        bool found = false;
        while (!heap.empty()) {
            const size_t ci = static_cast<size_t>(heap.pop().value);
            if (closed[ci]) continue;
            closed[ci] = 1;
            if (ci == ti) {
                found = true;
                break;
            }
            const int32_t cx = x0 + static_cast<int32_t>(ci % static_cast<size_t>(bw));
            const int32_t cy = y0 + static_cast<int32_t>(ci / static_cast<size_t>(bw));
            for (int d = 0; d < 8; ++d) {
                const int32_t nx = cx + kDx[d], ny = cy + kDy[d];
                if (!inside(nx, ny)) continue;
                const size_t ni = li(nx, ny);
                if (!ok[ni] || closed[ni]) continue;
                if (d >= 4 && (!ok[li(cx + kDx[d], cy)] || !ok[li(cx, cy + kDy[d])])) continue;  // no corner cutting
                float step = d >= 4 ? 1.41421356f : 1.0f;
                if (pen[ni]) step *= k.dynamic_obstacle_overlap_penalty;
                const float ng = g[ci] + step;
                if (!(ng < g[ni])) continue;
                g[ni] = ng;
                parent[ni] = static_cast<int32_t>(ci);
                heap.push(ng + h(nx, ny), static_cast<int32_t>(ni));
            }
        }
        if (!found) continue;
        std::vector<size_t> cells;
        for (int32_t c = static_cast<int32_t>(ti); c >= 0; c = parent[static_cast<size_t>(c)]) {
            cells.push_back(static_cast<size_t>(c));
            if (static_cast<size_t>(c) == si) break;
        }
        std::reverse(cells.begin(), cells.end());
        out.push_back(start);
        for (size_t i = 1; i + 1 < cells.size(); ++i) {
            const int32_t cx = x0 + static_cast<int32_t>(cells[i] % static_cast<size_t>(bw));
            const int32_t cy = y0 + static_cast<int32_t>(cells[i] / static_cast<size_t>(bw));
            out.push_back(cell_center(cx, cy));
        }
        out.push_back(target);
        // Smooth: keep a shortcut only when it stays passable and avoids penalised cells.
        if (out.size() > 2) {
            auto vis = [&](V2 a, V2 b) {
                return walk_cells(a, b, [&](int32_t cx, int32_t cy) {
                    if (!grid.passable_cell(cx, cy, mask)) return false;
                    if (inside(cx, cy) && pen[li(cx, cy)]) return false;
                    return true;
                });
            };
            std::vector<V2> q{out.front()};
            size_t i = 0;
            while (i + 1 < out.size()) {
                size_t j = i + 1;
                for (size_t c = out.size() - 1; c > i + 1; --c) {
                    if (vis(out[i], out[c])) {
                        j = c;
                        break;
                    }
                }
                q.push_back(out[j]);
                i = j;
            }
            out.swap(q);
        }
        return true;
    }
    return false;
}

std::vector<uint32_t> find_crush_victims(std::span<const Box2> crusher_boxes, std::span<const Box2> victims) {
    std::vector<uint32_t> out;
    for (size_t i = 0; i < victims.size(); ++i) {
        const Box2& v = victims[i];
        for (const Box2& c : crusher_boxes) {
            if (v.min.x <= c.max.x && v.max.x >= c.min.x && v.min.y <= c.max.y && v.max.y >= c.min.y) {
                out.push_back(static_cast<uint32_t>(i));
                break;
            }
        }
    }
    return out;
}

// ---------------------------------------------------------------- s78n t18: footprint rectangles (MOVEMENT §5.3.1)
float foot_extent(const FootRect& r, V2 n) {
    if (r.round) return r.hx;
    const V2 b = perp_left(r.a);
    return std::fabs(dot(r.a, n)) * r.hx + std::fabs(dot(b, n)) * r.hy;
}

namespace {
// The four separating axes of two rectangles: f(|D . n|, R1 + R2) per axis; a false from f stops the walk.
template <class Fn>
bool foot_axes(const FootRect& r1, const FootRect& r2, Fn&& f) {
    const V2 d = r2.c - r1.c;
    V2 line = normalize_retail(d);  // a round footprint's axes: the centre line and its normal
    if (!(length_sq(line) > 0.0f)) line = {1.0f, 0.0f};
    const V2 a1 = r1.round ? line : r1.a, a2 = r2.round ? line : r2.a;
    const V2 axes[4] = {a1, perp_left(a1), a2, perp_left(a2)};
    for (const V2& n : axes)
        if (!f(std::fabs(dot(d, n)), foot_extent(r1, n) + foot_extent(r2, n))) return false;
    return true;
}
}  // namespace

bool foot_overlap(const FootRect& r1, const FootRect& r2) {
    return foot_axes(r1, r2, [](float sep, float sum) { return sep <= sum; });
}

float foot_overlap_coefficient(const FootRect& r1, const FootRect& r2) {
    float k = 1.0f;
    foot_axes(r1, r2, [&k](float sep, float sum) {
        const float v = sum > 0.0f && sep <= sum ? 1.0f - sep / sum : 0.0f;
        k = std::min(k, v);
        return true;
    });
    return k;
}

float foot_gap(const FootRect& r1, const FootRect& r2) {
    float g = 0.0f;
    foot_axes(r1, r2, [&g](float sep, float sum) {
        g = std::max(g, std::max(0.0f, sep - sum));
        return true;
    });
    return g;
}

double foot_swept_contact(V2 m, V2 d, float r0, float r1) {
    const double mx = m.x, my = m.y, dx = d.x, dy = d.y, ra = r0, dr = static_cast<double>(r1) - static_cast<double>(r0);
    const double c = mx * mx + my * my - ra * ra;
    if (c <= 0.0) return ra >= 0.0 ? 0.0 : -1.0;
    const double a = dx * dx + dy * dy - dr * dr;
    const double b = mx * dx + my * dy - ra * dr;  // q(s) = a s^2 + 2 b s + c with c > 0; contact where q <= 0
    const auto ok = [&](double s) { return s >= 0.0 && s <= 1.0 && ra + s * dr >= 0.0; };
    if (a == 0.0) {
        if (!(b < 0.0)) return -1.0;
        const double s = -c / (2.0 * b);
        return ok(s) ? s : -1.0;
    }
    const double disc = b * b - a * c;
    if (disc < 0.0) return -1.0;
    const double root = std::sqrt(disc);
    const double s0 = (-b - root) / a, s1 = (-b + root) / a;
    const double lo = std::min(s0, s1), hi = std::max(s0, s1);
    if (ok(lo)) return lo;
    if (ok(hi)) return hi;
    return -1.0;
}

float foot_path_contact(V2 pos, std::span<const V2> path, uint32_t next, V2 ha, const FootRect& b, float max_dist,
                        float turn_yaw) {
    const auto radius = [&](V2 e, V2 t) {
        const V2 u = normalize_retail(b.c - e);
        return foot_rho(ha.x, ha.y, std::fabs(dot(u, t))) + foot_rho(b.hx, b.hy, std::fabs(dot(u, b.a)));
    };
    if (turn_yaw != kNoFacing) {  // a zero-length segment while the mover turns in place
        const float r = radius(pos, dir_from_deg(turn_yaw));
        return max_dist > 0.0f && foot_swept_contact(pos - b.c, V2{}, r, r) >= 0.0 ? 0.0f : -1.0f;
    }
    float acc = 0.0f;
    V2 s = pos;
    for (size_t i = next; i < path.size() && acc < max_dist; ++i) {
        const V2 d = path[i] - s;
        const float len = length(d);
        if (!(len > 0.0f)) continue;
        const V2 t = normalize_retail(d);
        // the approximation's points lie kFootApproxStep apart along the segment
        const uint32_t n = static_cast<uint32_t>(len / kFootApproxStep) + 1u;
        V2 p0 = s;
        float r0 = radius(p0, t);
        for (uint32_t k = 1; k <= n && acc < max_dist; ++k) {
            const V2 p1 = k == n ? path[i] : s + d * (static_cast<float>(k) * kFootApproxStep / len);
            const float piece = distance(p0, p1);
            const float r1 = radius(p1, t);
            const double hit = foot_swept_contact(p0 - b.c, p1 - p0, r0, r1);
            if (hit >= 0.0) {
                const float at = acc + static_cast<float>(hit) * piece;
                return at < max_dist ? at : -1.0f;
            }
            acc += piece;
            p0 = p1;
            r0 = r1;
        }
        s = path[i];
    }
    return -1.0f;
}

bool foot_moving_contact(const FootTrack& p, const FootTrack& q, int32_t frames) {
    const V2 fp = dir_from_deg(p.yaw), fq = dir_from_deg(q.yaw);
    const V2 dir = normalize_retail(q.pos - p.pos);
    struct Cur {
        const FootTrack* t;
        V2 s;
        uint32_t i;
        float v;  // assumed speed: the effective max speed when facing toward the other party, else the current speed
        bool moves() const { return v > 0.0f && i < t->path.size(); }
        // One frame along the polyline; `dir_out` = the unit direction of the segment the frame starts on.
        V2 advance(V2& dir_out) {
            dir_out = V2{};
            float budget = v;
            V2 at = s;
            bool first = true;
            while (budget > 0.0f && i < t->path.size()) {
                const V2 d = t->path[i] - at;
                const float l = length(d);
                if (first && l > 0.0f) {
                    dir_out = normalize_retail(d);
                    first = false;
                }
                if (l <= budget) {
                    at = t->path[i];
                    budget -= l;
                    ++i;
                } else {
                    at = at + d * (budget / l);
                    budget = 0.0f;
                }
            }
            return at;
        }
    };
    Cur a{&p, p.pos, p.next, dot(fp, dir) >= 0.0f ? p.max_speed : p.speed};
    Cur b{&q, q.pos, q.next, dot(fq, dir) <= 0.0f ? q.max_speed : q.speed};
    if (!a.moves() && !b.moves())  // both approximations empty of motion at the very start: the rectangle overlap
        return foot_overlap(foot_rect(p.pos, p.yaw, p.hx, p.hy), foot_rect(q.pos, q.yaw, q.hx, q.hy));
    // Lane rule: the other party at least hy to the side of this one's travel line at the start and on the same side at
    // the end -> this one's radius is hy at both ends. (A standing unit has t = 0: its radius is hy.)
    const auto radii = [](const FootTrack& t, V2 dir_t, V2 u0, V2 u1, V2 rel0, V2 rel1, float& r0, float& r1) {
        r0 = foot_rho(t.hx, t.hy, std::fabs(dot(u0, dir_t)));
        r1 = foot_rho(t.hx, t.hy, std::fabs(dot(u1, dir_t)));
        const float c0 = cross(dir_t, rel0), c1 = cross(dir_t, rel1);
        if (std::fabs(c0) >= t.hy && ((c0 > 0.0f && c1 > 0.0f) || (c0 < 0.0f && c1 < 0.0f))) r0 = r1 = t.hy;
    };
    // [port] one step per frame (the retail steps run from approximation point to approximation point, whole frames).
    for (int32_t f = 0; f < frames; ++f) {
        const bool am = a.moves(), bm = b.moves();
        V2 ta{}, tb{};
        const V2 ea = am ? a.advance(ta) : a.s, eb = bm ? b.advance(tb) : b.s;
        const V2 m0 = b.s - a.s, m1 = eb - ea;
        const V2 us = normalize_retail(m0), ue = normalize_retail(m1);
        float ra0 = 0.0f, ra1 = 0.0f, rb0 = 0.0f, rb1 = 0.0f;
        radii(p, ta, us, ue, m0, m1, ra0, ra1);
        radii(q, tb, us, ue, V2{} - m0, V2{} - m1, rb0, rb1);
        if (foot_swept_contact(m0, m1 - m0, std::max(0.0f, ra0 + rb0), std::max(0.0f, ra1 + rb1)) >= 0.0) return true;
        if (!am && !bm) return false;  // both stand and do not touch
        a.s = ea;
        b.s = eb;
    }
    return false;
}

// ---------------------------------------------------------------- collision objects (TACTICAL_GAPS §6.7)
float collision_priority(float crowd, uint32_t now, uint32_t created) {
    return crowd - static_cast<float>(now - created);
}

bool CollisionLayer::has_pair(ObjectId a, ObjectId b) const {
    return std::binary_search(pairs_.begin(), pairs_.end(), collision_pair_key(a, b));
}

uint32_t CollisionLayer::add(CollisionKind kind, ObjectId a, ObjectId b, uint32_t frame) {
    const uint64_t key = collision_pair_key(a, b);
    auto it = std::lower_bound(pairs_.begin(), pairs_.end(), key);
    if (it != pairs_.end() && *it == key) return UINT32_MAX;
    pairs_.insert(it, key);
    CollisionObject c;
    c.seq = next_seq_++;
    c.kind = kind;
    c.a = a;
    c.b = b;
    c.created = frame;
    c.crowd_frame = frame;
    list_.push_back(c);
    return c.seq;
}

void CollisionLayer::drop_pair(const CollisionObject& c) {
    auto it = std::lower_bound(pairs_.begin(), pairs_.end(), collision_pair_key(c.a, c.b));
    if (it != pairs_.end() && *it == collision_pair_key(c.a, c.b)) pairs_.erase(it);
}

void CollisionLayer::run(uint32_t frame, const KeepFn& keep, const CountFn& count, const BudgetFn& budget,
                         const ResolveFn& resolve) {
    order_.clear();
    cancelled_ = 0;
    resolved_ = 0;
    // 1. Keep pass in list order; the rest are cancelled. Compaction keeps the list order.
    size_t w = 0;
    for (size_t r = 0; r < list_.size(); ++r) {
        if (list_[r].state != CollisionState::Resolved && keep(list_[r])) {
            if (w != r) list_[w] = list_[r];
            ++w;
        } else {
            drop_pair(list_[r]);
            ++cancelled_;
        }
    }
    list_.resize(w);
    // 2. Priorities (crowd refreshed every kCollisionCrowdRefreshFrames).
    for (CollisionObject& c : list_) {
        if (frame >= c.crowd_frame) {
            c.crowd = c.participants < 2 ? 0.0f : static_cast<float>(kCollisionCrowdWeight * count(c));
            c.crowd_frame = frame + kCollisionCrowdRefreshFrames;
        }
        c.priority = collision_priority(c.crowd, frame, c.created);
    }
    // 3. Ascending (priority, insertion order): a total order, so std::sort is deterministic here.
    sort_.resize(list_.size());
    for (uint32_t i = 0; i < sort_.size(); ++i) sort_[i] = i;
    std::sort(sort_.begin(), sort_.end(), [this](uint32_t x, uint32_t y) {
        const CollisionObject &cx = list_[x], &cy = list_[y];
        if (cx.priority != cy.priority) return cx.priority < cy.priority;
        return cx.seq < cy.seq;
    });
    // 4. Resolve in that order under the budget gate.
    for (uint32_t i : sort_) {
        const bool full = budget();
        resolve(list_[i], full);
        order_.push_back({list_[i].seq, full});
    }
    // [port] a finished resolution leaves the list at once (its pair can be found again by the next check).
    w = 0;
    for (size_t r = 0; r < list_.size(); ++r) {
        if (list_[r].state != CollisionState::Resolved) {
            if (w != r) list_[w] = list_[r];
            ++w;
        } else {
            drop_pair(list_[r]);
            ++resolved_;
        }
    }
    list_.resize(w);
}

void CollisionLayer::clear() {
    list_.clear();
    pairs_.clear();
    order_.clear();
    cancelled_ = resolved_ = 0;
}

void CollisionLayer::hash(StateHasher& h) const {
    h.u8(0xc7);  // section tag
    h.u32(next_seq_);
    h.u32(static_cast<uint32_t>(list_.size()));
    for (const CollisionObject& c : list_) {
        h.u32(c.seq);
        h.u8(static_cast<uint8_t>(c.kind));
        h.u8(static_cast<uint8_t>(c.state));
        h.u8(c.participants);
        h.i32(c.a);
        h.i32(c.b);
        h.u32(c.created);
        h.u32(c.crowd_frame);
        h.f32(c.crowd);
        h.f32(c.priority);
        h.u32(c.retry);
        h.f32(c.speed_a);  // s78n t18
        h.f32(c.speed_b);
        h.boolean(c.now_a);
        h.boolean(c.now_b);
        h.u8(c.master);
    }
}

}  // namespace alamo::tactical::movement
