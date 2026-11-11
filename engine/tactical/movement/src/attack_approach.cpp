// s78n t25.1: the attack approach on land, pure geometry (docs/re/MOVEMENT.md §4.3.2 items 2-4, 6 and 7).
#include "alamo/tactical/movement/attack_approach.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

#include "alamo/tactical/movement/formation.hpp"
#include "alamo/tactical/movement/land_path.hpp"
#include "alamo/tactical/movement/movement_system.hpp"
#include "alamo/tactical/movement/path_around.hpp"

namespace alamo::tactical::movement {

// s78n t25.1: §4.3.2 item 2, the coefficient loop (float32: six tries for 0.2).
std::vector<float> attack_arc_tries(float base) {
    std::vector<float> out;
    for (float f = base; f > -1.0f && out.size() < 64; f -= 0.2f) out.push_back(f);
    return out;
}

// s78n t25.1: §4.3.2 item 4, step(a, b) as written in 0x43b520.
float arc_step(float a, float b, float rho) {
    const float ab = a + b;
    return dmath::acos(dmath::sqrt(rho * rho - ab * ab / 4.0f) / rho);
}

V2 rot(V2 v, float th) {
    float s = 0.0f, c = 0.0f;
    dmath::sincos(th, s, c);
    return {v.x * c - v.y * s, v.x * s + v.y * c};
}

// s78n t25.1: 0x43b520, one ring (§4.3.2 item 4).
bool ring_place(std::span<RingForm> prefix, V2 c, V2 u, V2 d0, float rho_k, float half_arc, const SlotTestFn& test, bool commit) {
    const size_t n = prefix.size();
    if (n == 0) return true;
    // Left-most first: ascending projection on the right-hand normal of d0 (retail std::sort; [port] stable, id tie-break).
    std::vector<uint32_t> ord(n);
    std::iota(ord.begin(), ord.end(), 0u);
    const V2 rn{d0.y, -d0.x};
    std::stable_sort(ord.begin(), ord.end(), [&](uint32_t a, uint32_t b) {
        const float pa = dot(prefix[a].centroid, rn);
        const float pb = dot(prefix[b].centroid, rn);
        if (pa != pb) return pa < pb;
        return prefix[a].id < prefix[b].id;
    });
    std::vector<float> th(n, 0.0f);
    const auto pass = [&](const RingForm& f, float t) { return test ? test(f, arc_point(c, u, rho_k, t)) : true; };

    // Middle formation: 0 (odd n) or 2 step(HY_m, 0) (even n), then alternately on both sides in 5 deg steps.
    const size_t m = n >> 1;
    const RingForm& fm = prefix[ord[m]];
    const float t0 = (n & 1u) ? 0.0f : 2.0f * arc_step(fm.hy, 0.0f, rho_k);
    if (!(t0 <= half_arc)) return false;
    bool placed = false;
    for (float t = t0; t <= half_arc; t += kAttackArcStepRad) {
        if (pass(fm, t)) {
            th[m] = t;
            placed = true;
            break;
        }
        if (t != 0.0f && pass(fm, -t)) {
            th[m] = -t;
            placed = true;
            break;
        }
    }
    if (!placed) return false;

    // Left neighbours m - 1 .. 0: the factor 2 only on the first one (as coded), 5 deg further out on a failed test.
    float prev = th[m];
    float prev_hy = fm.hy;
    for (size_t i = m; i-- > 0;) {
        const RingForm& f = prefix[ord[i]];
        float t = prev - (i + 1 == m ? 2.0f : 1.0f) * arc_step(f.hy, prev_hy, rho_k);
        for (;;) {
            if (!(t >= -half_arc)) return false;
            if (pass(f, t)) break;
            t -= kAttackArcStepRad;
        }
        th[i] = t;
        prev = t;
        prev_hy = f.hy;
    }
    // Right neighbours m + 1 .. n - 1: the mirror image.
    prev = th[m];
    prev_hy = fm.hy;
    for (size_t i = m + 1; i < n; ++i) {
        const RingForm& f = prefix[ord[i]];
        float t = prev + (i == m + 1 ? 2.0f : 1.0f) * arc_step(f.hy, prev_hy, rho_k);
        for (;;) {
            if (!(t <= half_arc)) return false;
            if (pass(f, t)) break;
            t += kAttackArcStepRad;
        }
        th[i] = t;
        prev = t;
        prev_hy = f.hy;
    }
    if (commit) {
        for (size_t i = 0; i < n; ++i) {
            prefix[ord[i]].theta = th[i];
            prefix[ord[i]].rho = rho_k;
        }
    }
    return true;
}

// s78n t25.1: 0x43d380, the rings of one coefficient try (§4.3.2 item 3).
bool ring_layout(std::span<RingForm> l, V2 c, V2 u, V2 d0, float r, float f, float r_t, const SlotTestFn& test, bool commit) {
    const float r_min = std::max(r_t, r * f);
    const float half_arc = attack_half_arc(f);
    float rho = r;
    size_t start = 0;
    while (start < l.size()) {
        size_t fit = 0;
        float h = 0.0f;      // the largest HX of the prefix being examined
        float h_fit = 0.0f;  // ... of the longest prefix that fitted
        for (size_t k = 1; start + k <= l.size(); ++k) {
            h = std::max(h, l[start + k - 1].hx);
            if (rho - 2.0f * h < r_t) break;
            if (!ring_place(l.subspan(start, k), c, u, d0, rho - h, half_arc, test, false)) break;
            fit = k;
            h_fit = h;
        }
        if (fit == 0) return false;
        if (commit && !ring_place(l.subspan(start, fit), c, u, d0, rho - h_fit, half_arc, test, true)) return false;
        start += fit;
        rho -= 2.0f * h;  // h of the last prefix examined
        if (rho < r_min) break;
    }
    return start == l.size();
}

// s78n t25.1: a member's slot position on the laid-out path (§4.3.2 item 7).
bool trim_point(std::span<const V2> path, float s, float depth, float lateral, bool extend, V2& out) {
    if (path.empty()) return false;
    const float q = s + depth;
    // First / last non-zero segments and the total length (summed in path order, as the walk below).
    size_t first = 0, last = 0;
    float total = 0.0f;
    for (size_t i = 1; i < path.size(); ++i) {
        const float l = distance(path[i - 1], path[i]);
        if (!(l > 0.0f)) continue;
        if (first == 0) first = i;
        last = i;
        total += l;
    }
    if (first == 0) {
        if (!(q <= 0.0f) && !extend) return false;
        out = path[0];
        return true;
    }
    const auto dir_of = [&](size_t i) {
        const V2 d = path[i] - path[i - 1];
        const float l = length(d);
        return V2{d.x / l, d.y / l};
    };
    V2 p, dir;
    if (q < 0.0f) {
        dir = dir_of(first);
        p = path[first - 1] + dir * q;
    } else if (q > total) {
        if (!extend) return false;
        dir = dir_of(last);
        p = path[last] + dir * (q - total);
    } else {
        float acc = 0.0f;
        size_t seg = last;
        for (size_t i = first; i <= last; ++i) {
            const float l = distance(path[i - 1], path[i]);
            if (!(l > 0.0f)) continue;
            if (q <= acc + l) {
                seg = i;
                break;
            }
            acc += l;
        }
        const float l = distance(path[seg - 1], path[seg]);
        dir = dir_of(seg);
        p = lerp(path[seg - 1], path[seg], std::min(1.0f, (q - acc) / l));
    }
    out = p + formation_right(dir) * lateral;
    return true;
}

// s78n t25.1: 0x410e90, the fire trim (§4.3.2 item 7).
float fire_trim(std::span<const V2> path, std::span<const TrimMember> members, bool guard, float guard_range,
                const MemberFireFn& can_fire) {
    const float len = path_length(path);
    float s = len;
    float good = guard ? std::max(0.0f, len - guard_range) : len;
    for (;;) {
        bool all = true;
        for (const TrimMember& m : members) {
            if (!m.tested) continue;
            V2 p;
            if (!trim_point(path, s, m.depth, m.lateral, false, p)) continue;  // beyond L: not tested at this s
            if (can_fire && !can_fire(m, p)) {
                all = false;
                break;
            }
        }
        if (!all) return good;
        good = s;
        if (!(s > 0.0f)) return 0.0f;
        s = std::max(0.0f, s - kFireTrimStep);
    }
}

// s78n t25.1: §4.3.2 item 6, the slot leg point Q'.
bool slot_leg_point(V2 g, V2 v, float theta, float q, V2& out) {
    if (!(q > 0.0f)) return false;
    out = g + rot(v * -1.0f, theta) * q;
    return true;
}


// ================================================================ s78n t25.2: the attack block in MovementSystem
// (MOVEMENT §4.3.2 items 1-2, 4-8; §4.3.1 item 7 (b)). Every function below runs from move() (serial).

namespace {

constexpr float kAttackInf = 3.0e38f;

// [port] (§4.3.2 item 6, the value the terrain line test 0x5a4c10 "gets" [M]): the distance from a toward b to the entry
// of the first cell the line test refuses (the thin cell walk of line_passable); the full length when the line is
// clear, 0 when a's own cell is refused.
float line_reach(const PassabilityGrid& grid, V2 a, V2 b, uint32_t mask) {
    int32_t fx = 0, fy = 0;
    bool hit = false;
    walk_cells(a, b, [&](int32_t cx, int32_t cy) {
        if (grid.passable_cell(cx, cy, mask)) return true;
        fx = cx;
        fy = cy;
        hit = true;
        return false;
    });
    const float len = distance(a, b);
    if (!hit || !(len > 0.0f)) return hit ? 0.0f : len;
    const V2 d = (b - a) * (1.0f / len);
    const float x0 = static_cast<float>(fx) * kCellSize - kHalfCell, y0 = static_cast<float>(fy) * kCellSize - kHalfCell;
    const float x1 = x0 + kCellSize, y1 = y0 + kCellSize;
    float t = 0.0f;
    if (d.x > 0.0f) t = std::max(t, (x0 - a.x) / d.x);
    else if (d.x < 0.0f) t = std::max(t, (x1 - a.x) / d.x);
    if (d.y > 0.0f) t = std::max(t, (y0 - a.y) / d.y);
    else if (d.y < 0.0f) t = std::max(t, (y1 - a.y) / d.y);
    return std::min(t, len);
}

// The unit direction of a path's first / last non-zero segment ((0, 0) when it has none). [port] the port's paths may
// start with the start point and its cell centre; a zero first segment is skipped.
V2 first_dir(std::span<const V2> p) {
    for (size_t i = 1; i < p.size(); ++i)
        if (length_sq(p[i] - p[i - 1]) > 0.0f) return normalize_retail(p[i] - p[i - 1]);
    return {};
}
V2 last_dir(std::span<const V2> p) {
    for (size_t i = p.size(); i-- > 1;)
        if (length_sq(p[i] - p[i - 1]) > 0.0f) return normalize_retail(p[i] - p[i - 1]);
    return {};
}

}  // namespace

// s78n t25.2: the box loop of place_formation (0x43d6a0; the union of the slot rectangles), factored out for 0x438c10.
bool MovementSystem::formation_box(const std::vector<uint32_t>& idx, const LandForm& f, V2 axis, float& x0, float& x1,
                                   float& y0, float& y1) const {
    x0 = 3.0e38f;
    x1 = -3.0e38f;
    y0 = 3.0e38f;
    y1 = -3.0e38f;
    const V2 right = formation_right(axis);
    const float ca = std::fabs(dot(f.fwd, axis)), sa = std::fabs(dot(f.fwd, right));
    for (const Slot& s : f.slots) {
        const Unit& m = units_[idx[s.member]];
        const V2 h = foot_half(m);
        const V2 off = slot_world(V2{}, f.fwd, s);
        const float ox = dot(off, axis), oy = dot(off, right), ex = ca * h.x + sa * h.y, ey = sa * h.x + ca * h.y;
        x0 = std::min(x0, ox - ex);
        x1 = std::max(x1, ox + ex);
        y0 = std::min(y0, oy - ey);
        y1 = std::max(y1, oy + ey);
    }
    return !f.slots.empty();
}

const LandAttackUnit* MovementSystem::attack_entry(const MoveOrder& order, ObjectId id) const {
    for (const LandAttackUnit& e : order.attack.units)
        if (e.id == id) return &e;
    return nullptr;
}

// s78n t25.2 (§4.3.2 item 6, §4.3.1 item 7 (b)): G is re-validated against the lead's zone (0x50f480(G, lead, 30): a
// target on ground the lead cannot enter gives the reachable cell next to it nearest to the lead); the start is the
// members' centroid when the lead reaches it by a straight line or a path <= 1.9 x the distance, else the lead's
// position; the path start -> G as plan_land plans (the formation's class `mask`, 0x4779e0; [port] the lead's clearance).
bool MovementSystem::attack_path(const Unit& lead, uint32_t mask, V2 centroid, V2& g, std::vector<V2>& path) const {
    if (!zone_connected(regions_, mask, lead.m.pos, g)) {
        V2 v;
        if (!zone_validate(regions_, g, lead.m.pos, mask, v)) return false;
        g = v;
    }
    LandPathScratch sc;
    V2 start = lead.m.pos;
    if (!(centroid == lead.m.pos)) {
        if (line_passable(grid_, lead.m.pos, centroid, mask)) {
            start = centroid;
        } else {
            LandPathRequest q;
            q.start = lead.m.pos;
            q.goal = centroid;
            q.mask = mask;
            q.snap_goal = false;
            LandPath lp;
            find_land_path(grid_, regions_, q, sc, lp);
            if (lp.ok && path_length(lp.points) <= 1.9f * distance(lead.m.pos, centroid)) start = centroid;
        }
    }
    LandPathRequest q;
    q.start = start;
    q.goal = g;
    q.mask = mask;
    q.clearance_radius = lead.d.radius > kCellSize && !lead.d.team_container ? lead.d.radius : 0.0f;  // as plan_land
    LandPath lp;
    find_land_path(grid_, regions_, q, sc, lp);
    if (!lp.ok || lp.points.empty()) return false;
    path = std::move(lp.points);
    return true;
}

// s78n t25.2: 0x43a070, the attack-arc slot test (§4.3.2 item 5) of formation f with (HX, HY) at the slot point p, for
// the formation's class 0x4779e0 (LandForm::mask, s78n t26 review round 2): p passable, zone-connected to the lead, the
// terrain line p -> C clear (every crossed cell, the last included: a target on a stamped cell never gets a slot), and
// the object test - the layer objects within sqrt(HX^2 + HY^2) + E of p, without the target, the order's units, objects
// with a path record (an object without a locomotor always counts) and, for an all-Infantry / Bike or all-Giant
// formation that is not all squad containers, single Infantry-class units; a blocker's footprint (half extents + S)
// overlaps some member's slot rectangle (centre p + the slot offsets on a = unit(C - p), half extents + S) that cannot
// crush it; no UnitCollisionClass test. Pass iff the sum of the blockers' (hx + S)(hy + S) < 0.15 HX HY.
bool MovementSystem::arc_slot_test(const MoveOrder& order, const std::vector<uint32_t>& idx, const LandForm& f, float hx,
                                   float hy, V2 p) const {
    const Unit& lead = units_[idx[f.members[0]]];
    const uint32_t mask = f.mask;
    const V2 c = order.attack.c;
    if (!grid_.passable_world(p, mask)) return false;
    if (!zone_connected(regions_, mask, p, lead.m.pos)) return false;
    if (!line_passable(grid_, p, c, mask)) return false;
    const float sp = cfg_.constants.between_formation_spacing, ext = cfg_.constants.destination_query_extension;
    V2 a = normalize_retail(c - p);
    if (!(length_sq(a) > 0.0f)) a = {1.0f, 0.0f};
    bool all_inf = true, all_giant = true, all_containers = true;
    for (const Slot& s : f.slots) {
        const Unit& m = units_[idx[s.member]];
        const int32_t cls = m.d.collision_class;
        all_inf = all_inf && (cls == kUccInfantry || cls == kUccBike);
        all_giant = all_giant && cls == kUccGiantVehicle;
        all_containers = all_containers && m.d.team_container;
    }
    const bool skip_infantry = (all_inf || all_giant) && !all_containers;
    std::vector<uint32_t> cand;
    layer_box_query(p, std::sqrt(hx * hx + hy * hy) + ext, cand);
    float sum = 0.0f;
    for (const uint32_t ci : cand) {
        if (std::find(idx.begin(), idx.end(), ci) != idx.end()) continue;  // the order's own units
        const Unit& o = units_[ci];
        if (o.d.id == order.attack.target) continue;
        if (!o.d.no_locomotor && (o.has_goal || o.pushed)) continue;  // a path record on its stack: under way
        if (skip_infantry && o.d.collision_class == kUccInfantry && !o.d.team_container) continue;
        FootRect ro = foot_of(o);
        ro.hx += sp;
        ro.hy += sp;
        bool blocker = false;
        for (const Slot& s : f.slots) {
            const Unit& m = units_[idx[s.member]];
            if (crushes(m, o)) continue;
            const V2 h = foot_half(m);
            if (foot_overlap(FootRect{slot_world(p, a, s), a, h.x + sp, h.y + sp, false}, ro)) {
                blocker = true;
                break;
            }
        }
        if (blocker) {
            const V2 h = foot_half(o);
            sum += (h.x + sp) * (h.y + sp);
        }
    }
    return sum < 0.15f * hx * hy;
}

// s78n t25.2: 0x438c10 (§4.3.2 item 2) at a new non-guard attack order. L = the formations whose lead is a Land layer
// object and that have a member not in range (slots 44 / 45 at order time); R = the minimum engagement range over the
// order's units; the reference formation (nearest to the mean of L's centroids, strict <, first wins; [port] 2-D) is
// test-planned (attack_path); fewer than 2 points or a zero last segment -> nothing written. L sorted rear-most first
// (ascending (centroid - path start) . d0; [port] stable, lead-id tie-break), the six coefficient tries of ring_layout
// on the ring axis u = -d1 around C, the first success re-run with commit and written to every member of each formation.
// (HX, HY) = formation_box on unit(C - centroid) ([port] 0x411640's mode-0 plan axis; the layout's fwd points at the
// order's destination). No try succeeds -> nothing is written: the blocks stay plain.
// s78n t25.4 (item 2, guard): a guard block's L = the formations whose lead is farther from C than min(the lead's attack
// range, Land_Guard_Range) ([port] the 2-D distance, as the guard trim's); the rest as for an attack (R = the minimum
// engagement range, which the host caps at Land_Guard_Range for a guard block, item 1).
void MovementSystem::attack_slots(const MoveOrder& order, const std::vector<uint32_t>& idx, const std::vector<LandForm>& forms) {
    std::vector<uint32_t> l;
    const V2 c = order.attack.c;
    for (uint32_t i = 0; i < forms.size(); ++i) {
        const LandForm& f = forms[i];
        const Unit& lead = units_[idx[f.members[0]]];
        if (!land_layer_object(lead)) continue;
        bool out = false;
        if (order.attack.guard) {  // s78n t25.4
            const LandAttackUnit* e = attack_entry(order, lead.d.id);
            const float lim = std::min(e ? e->range : 0.0f, cfg_.constants.land_guard_range);
            out = distance(lead.m.pos, c) > lim;
        } else {
            for (const uint32_t m : f.members) {
                const LandAttackUnit* e = attack_entry(order, units_[idx[m]].d.id);
                out = out || !e || !e->in_range;
            }
        }
        if (out) l.push_back(i);
        else ++stats_.attack_in_range;
    }
    if (l.empty()) return;
    const auto unslotted = [&](const LandForm& rf) {
        stats_.attack_unslotted += static_cast<uint32_t>(l.size());
        if (!grid_.passable_world(c, rf.mask)) stats_.attack_unslotted_cell += static_cast<uint32_t>(l.size());
    };
    float r = 3.0e38f;
    for (const uint32_t i : idx)
        if (const LandAttackUnit* e = attack_entry(order, units_[i].d.id)) r = std::min(r, e->engage);
    V2 mean{};
    for (const uint32_t i : l) mean = mean + forms[i].centroid;
    mean = mean * (1.0f / static_cast<float>(l.size()));
    uint32_t ref = l[0];
    float best = 3.0e38f;
    for (const uint32_t i : l) {
        const float d = length_sq(forms[i].centroid - mean);
        if (d < best) {
            best = d;
            ref = i;
        }
    }
    const Unit& rlead = units_[idx[forms[ref].members[0]]];
    if (!(r < 3.0e38f)) {
        unslotted(forms[ref]);
        return;
    }
    V2 g = order.attack.g;
    std::vector<V2> path;
    if (!attack_path(rlead, forms[ref].mask, forms[ref].centroid, g, path) || path.size() < 2) {
        unslotted(forms[ref]);
        return;
    }
    // s78o (b0072): the first / last non-zero segment, as attack_plan takes them ([port] the port's paths may start with the
    // start point and its cell centre): a zero first segment no longer gives d0 = (0, 0) and the rear-most / left-to-right
    // ring order by id alone (crossed slots); no non-zero segment -> unslotted below
    const V2 d0 = first_dir(path);
    const V2 d1 = last_dir(path);
    if (!(length_sq(d1) > 0.0f)) {
        unslotted(forms[ref]);
        return;
    }
    const V2 base = path[0];
    std::stable_sort(l.begin(), l.end(), [&](uint32_t x, uint32_t y) {
        const float px = dot(forms[x].centroid - base, d0), py = dot(forms[y].centroid - base, d0);
        if (px != py) return px < py;
        return units_[idx[forms[x].members[0]]].d.id < units_[idx[forms[y].members[0]]].d.id;
    });
    std::vector<RingForm> rings;
    rings.reserve(l.size());
    for (const uint32_t i : l) {
        const LandForm& f = forms[i];
        V2 axis = normalize_retail(c - f.centroid);
        if (!(length_sq(axis) > 0.0f)) axis = d1;
        float x0 = 0.0f, x1 = 0.0f, y0 = 0.0f, y1 = 0.0f;
        formation_box(idx, f, axis, x0, x1, y0, y1);
        RingForm rf;
        rf.index = i;
        rf.id = units_[idx[f.members[0]]].d.id;
        rf.centroid = f.centroid;
        rf.hx = 0.5f * (x1 - x0);
        rf.hy = 0.5f * (y1 - y0);
        rings.push_back(rf);
    }
    const V2 u = d1 * -1.0f;
    const float r_t = std::sqrt(order.attack.t_hx * order.attack.t_hx + order.attack.t_hy * order.attack.t_hy);
    const SlotTestFn test = [&](const RingForm& rf, V2 p) { return arc_slot_test(order, idx, forms[rf.index], rf.hx, rf.hy, p); };
    for (const float f : attack_arc_tries(cfg_.constants.base_land_targeting_arc_angle_coefficient)) {
        if (!ring_layout(rings, c, u, d0, r, f, r_t, test, false)) continue;
        if (!ring_layout(rings, c, u, d0, r, f, r_t, test, true)) break;  // (the same tests: never)
        for (const RingForm& rf : rings) {
            for (const uint32_t m : forms[rf.index].members) {
                Unit& x = units_[idx[m]];
                x.atk_theta = rf.theta;
                x.atk_rho = rf.rho;
            }
            ++stats_.attack_slotted;
        }
        return;
    }
    unslotted(forms[ref]);
}

// s78n t25.2: one formation's attack plan (§4.3.2 items 6 - 7; 0x41c1a0 -> 0x417520 -> 0x40eb90 / 0x410e90), run at the
// order ([port] retail plans at the formation update that follows). Non-guard with the fire hook set and every member
// able to fire from where it stands (0x40edd0): the members stop as halt() stops them (End Move braking, the path stack
// cleared), arrived, the slot kept; no path. Else the path to G (attack_path; failure: the members are stopped and
// failed, the block kept); with a slot (the lead's rho > 0) the slot leg Q' = G + q rot(-v, theta) (v = the first path's
// last segment, q = line_reach toward G + rho rot(-v, theta)) is taken when a path start -> Q' succeeds and its first
// segment points the same way as the first path's (dot > 0): start -> ... -> Q' -> G. The fire trim (trim gate and hook
// set) cuts the path to fire_trim's keep length (cut_points); each member's goal is its slot point at arc length keep
// on the uncut path (trim_point, extended, no cell snap): the point the trim tested, also at keep 0 (the path cut to its
// first node, "the formation can fire from where it stands": every member keeps its own slot around the start). [port] the members then plan singly to those goals in plan_land (no shared
// formation path). An air lead keeps the plain move the slot loop gave (the plan is for land formations).
// s78n t26 review round 3: no item-6 stop when T is unseen (LandAttack::unseen: 0x40edd0's fog / stealth terms, FIGHTER_FLYING
// §4.11 E [H]); the members a re-path keeps (G 5, `kept`) get no goal and are not stopped by a failed plan.
void MovementSystem::attack_plan(const MoveOrder& order, const std::vector<uint32_t>& idx, const LandForm& f, uint32_t frame,
                                 const std::vector<uint8_t>& kept) {
    const Unit& lead = units_[idx[f.members[0]]];
    if (is_air(lead)) return;
    const ObjectId target = order.attack.target;
    const bool guard = order.attack.guard;
    std::vector<ObjectId> ids;
    for (const uint32_t m : f.members) ids.push_back(units_[idx[m]].d.id);
    if (fire_test_ && !guard && !order.attack.unseen) {
        bool all = true;
        for (const uint32_t m : f.members) {
            const Unit& x = units_[idx[m]];
            if (!fire_test_(x.d.id, x.m.pos, target, false)) {
                all = false;
                break;
            }
        }
        if (all) {
            halt(ids, frame);
            for (const uint32_t m : f.members) {
                Unit& x = units_[idx[m]];
                x.has_goal = false;
                x.need_path = false;
                x.arrived = true;
                x.failed = false;
            }
            ++stats_.attack_stops;
            return;
        }
    }
    V2 g = order.attack.g;
    std::vector<V2> path;
    if (!attack_path(lead, f.mask, f.centroid, g, path)) {
        // the plan fails: the members do not move ([port] stopped and failed, as the batch-2 zone failure; the block kept)
        for (const uint32_t m : f.members) {
            if (kept[m]) continue;
            const ObjectId id = units_[idx[m]].d.id;
            const ObjectId one[] = {id};
            Unit* x = unit_ptr(id);
            const float th = x->atk_theta, rh = x->atk_rho;
            const uint32_t af = x->atk_form;
            const ObjectId at = x->atk_target;
            stop(one);
            x->atk_theta = th;
            x->atk_rho = rh;
            x->atk_form = af;
            x->atk_target = at;
            x->failed = true;
            x->arrived = false;
            x->in_formation = false;
            x->formation_speed = kAttackInf;
        }
        return;
    }
    const uint32_t mask = f.mask;  // s78n t26 review round 2: the formation class 0x4779e0
    if (lead.atk_rho > 0.0f && path.size() >= 2) {
        const V2 v = last_dir(path);
        bool taken = false;
        if (length_sq(v) > 0.0f) {
            const float q = line_reach(grid_, g, g + rot(v * -1.0f, lead.atk_theta) * lead.atk_rho, mask);
            V2 qp;
            if (slot_leg_point(g, v, lead.atk_theta, q, qp)) {
                LandPathRequest rq;
                rq.start = path[0];
                rq.goal = qp;
                rq.mask = mask;
                rq.clearance_radius = lead.d.radius > kCellSize && !lead.d.team_container ? lead.d.radius : 0.0f;
                LandPathScratch sc;
                LandPath lp;
                find_land_path(grid_, regions_, rq, sc, lp);
                if (lp.ok && lp.points.size() >= 2 && dot(first_dir(lp.points), first_dir(path)) > 0.0f) {
                    path = std::move(lp.points);
                    path.push_back(g);
                    taken = true;
                }
            }
        }
        ++(taken ? stats_.attack_legs : stats_.attack_leg_refused);
    }
    std::vector<V2> uncut;  // the path before the fire trim's cut (empty: no trim)
    float goal_s = 0.0f;
    if (order.attack.trim && fire_test_) {
        std::vector<TrimMember> tm;
        // s78o (b0071): a member without TARGETING (the host's LandAttackUnit::targeting) is not tested
        for (const Slot& s : f.slots) {
            const LandAttackUnit* e = attack_entry(order, units_[idx[s.member]].d.id);
            tm.push_back({s.member, -s.back, s.lateral, e == nullptr || e->targeting});
        }
        const float total = path_length(path);
        const float guard_range = cfg_.constants.land_guard_range;
        const V2 c = order.attack.c;
        const float keep = fire_trim(path, tm, guard, guard_range, [&](const TrimMember& m, V2 p) {
            // guard (item 7): within the guard range of T (2-D) and the line of fire clear; else the fire test
            if (guard && distance(p, c) > guard_range) return false;
            return fire_test_(units_[idx[m.member]].d.id, p, target, guard);
        });
        // the member goals are the slot points the trim tested: on the uncut path at arc length keep (at keep 0 the cut
        // path is its first node alone and has no direction; the uncut one extends backwards along its first segment)
        uncut = path;
        cut_points(path, keep);
        goal_s = keep;
        ++stats_.attack_trims;
        if (!(keep > 0.0f)) ++stats_.attack_trims_first;
        stats_.attack_trim_sum += total - keep;
    }
    const std::vector<V2>& lay = uncut.empty() ? path : uncut;
    if (uncut.empty()) goal_s = path_length(path);
    for (const Slot& s : f.slots) {
        Unit& x = units_[idx[s.member]];
        if (is_air(x) || kept[s.member]) continue;
        V2 goal = path.back();
        trim_point(lay, goal_s, -s.back, s.lateral, true, goal);
        x.goal = goal;
        x.self_goal = goal;
        x.has_goal = true;
        x.need_path = true;
        x.arrived = false;
        x.failed = false;
        x.forced = false;
        x.self_arrive = false;
        x.facing = kNoFacing;
        x.final_facing = kNoFacing;
        if (x.d.locomotor != LocomotorKind::Default) x.sm.dest = goal;
    }
}

}  // namespace alamo::tactical::movement
