// s78n t25.1: the attack approach on land, pure geometry (docs/re/MOVEMENT.md §4.3.2 items 2-4, 6 and 7).
#include "alamo/tactical/movement/attack_approach.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

#include "alamo/tactical/movement/formation.hpp"

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

namespace {

float path_length(std::span<const V2> path) {
    float len = 0.0f;
    for (size_t i = 1; i < path.size(); ++i) len += distance(path[i - 1], path[i]);
    return len;
}

}  // namespace

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

}  // namespace alamo::tactical::movement
