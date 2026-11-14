// cu8l t03: the campaign space fleet placement 0x4fbb10 (fleet_placement.hpp; docs/re/CU8K_T03_T08_T09_RE.md §1).
#include "alamo/tactical/movement/fleet_placement.hpp"

#include <algorithm>
#include <cmath>

#include "alamo/core/dmath.hpp"

namespace alamo::tactical::movement::fleet {

namespace {

constexpr float kDegToRadF = dmath::kTwoPiF / 360.0f;  // theta * (2 pi / 360), float

// Rot(theta) . (x, y) with theta in degrees.
V2 rotate(float theta_deg, V2 v) {
    const float t = theta_deg * kDegToRadF;
    const float c = dmath::cos(t), s = dmath::sin(t);
    return {c * v.x - s * v.y, s * v.x + c * v.y};
}

V2 polar(float theta_deg, float rho) {
    const float t = theta_deg * kDegToRadF;
    return {rho * dmath::cos(t), rho * dmath::sin(t)};
}

// §1.8 item 3: a team container's members at container + Rot(yaw) . Squadron_Offsets[j] (the yaw itself, one -360 wrap);
// an index past the offsets list stands at the container [M]. Capital grid: the +0x50 type, switching to the company type
// (+0x58) when the +0x50 type's Squadron_Units count is below the member count and the company's is not; screen: +0x50;
// lead: +0x58; the transport rows move no member.
void place_members(const FleetObject& o, Via via, float yaw_deg, Placed& p) {
    if (!o.team || via == Via::Transport) return;
    const size_t n = o.members.size();
    const std::vector<V2>* offs = &o.offsets_team;
    if (via == Via::Lead) offs = &o.offsets_company;
    if (via == Via::Capital && o.units_team < n && !(o.units_company < n)) offs = &o.offsets_company;
    float psi = yaw_deg;
    if (psi > 360.0f) psi = psi - 360.0f;
    p.members.resize(n);
    for (size_t j = 0; j < n; ++j) {
        const V2 off = j < offs->size() ? (*offs)[j] : V2{};
        p.members[j] = p.pos + rotate(psi, off);
    }
}

void emit(std::span<const FleetObject> objs, uint32_t index, Via via, V2 pos, float yaw_deg, std::vector<Placed>& out) {
    Placed p;
    p.index = index;
    p.via = via;
    p.pos = pos;
    place_members(objs[index], via, yaw_deg, p);
    out.push_back(std::move(p));
}

// |S| of §1.5 / §1.6 (Sz = 0).
float size_length(V2 s) { return std::sqrt(s.x * s.x + s.y * s.y); }

// trunc(q) capped at `cap` (q >= 0; a quotient past the cap, an infinite one included, gives the cap).
uint32_t trunc_capped(float q, uint32_t cap) {
    if (!(q < static_cast<float>(cap))) return cap;
    return q > 0.0f ? static_cast<uint32_t>(q) : 0u;
}

}  // namespace

int32_t list_of_class(int32_t ship_class) {
    switch (ship_class) {
        case 5: return 0;          // capital (walker)
        case 3: case 4: return 1;  // corvette, frigate (tank)
        case 0: case 1: return 3;  // fighter, bomber (infantry)
        case 2: return 4;          // transport (artillery)
        case 6: return 5;          // super
        default: return -1;        // -1, >= 7: not placed
    }
}

std::vector<uint32_t> interleave(std::span<const uint32_t> fighters, std::span<const uint32_t> bombers) {
    const size_t f = fighters.size(), b = bombers.size();
    std::vector<uint32_t> out;
    out.reserve(f + b);
    if (b > f) {
        const size_t h = (b - f) / 2;
        for (size_t i = 0; i < h; ++i) out.push_back(bombers[i]);
        for (size_t i = 0; i < f; ++i) {
            out.push_back(bombers[h + i]);
            out.push_back(fighters[i]);
        }
        for (size_t i = h + f; i < b; ++i) out.push_back(bombers[i]);
    } else {
        const size_t h = b / 2;
        for (size_t i = 0; i < h; ++i) {
            out.push_back(bombers[i]);
            out.push_back(fighters[i]);
        }
        for (size_t i = h; i < h + f - b; ++i) out.push_back(fighters[i]);
        for (size_t i = 0; i < b - h; ++i) {
            out.push_back(bombers[h + i]);
            out.push_back(fighters[h + f - b + i]);
        }
    }
    return out;
}

uint32_t grid_rows(uint32_t n) {
    if (n < 3) return 1;
    if (n < 7) return 2;
    if (n < 10) return 3;
    if (n < 17) return 4;
    if (n <= 25) return 5;
    return 6;
}

std::vector<uint32_t> grid_row_counts(uint32_t n) {
    const uint32_t rows = grid_rows(n);
    std::vector<uint32_t> c(rows, n / rows);
    const uint32_t e = n - rows * (n / rows);
    // 0x4fb9c0: the rows (0-based) that take one of the e extra ships, by row count R and remainder e.
    static constexpr int8_t kExtra[7][6][5] = {
        {},
        {},
        {{}, {0, -1}},
        {{}, {1, -1}, {0, 2, -1}},
        {{}, {0, -1}, {0, 2, -1}, {0, 1, 2, -1}},
        {{}, {0, -1}, {1, 3, -1}, {0, 2, 4, -1}, {0, 1, 2, 3, -1}},
        {{}, {0, -1}, {2, 3, -1}, {1, 3, 5, -1}, {1, 2, 3, 4, -1}, {0, 1, 2, 3, 4}},
    };
    if (e > 0 && rows < 7 && e < 6)
        for (uint32_t k = 0; k < e && k < 5; ++k) {
            const int8_t r = kExtra[rows][e][k];
            if (r < 0) break;
            ++c[static_cast<size_t>(r)];
        }
    return c;
}

V2 list_size(std::span<const FleetObject> objs, std::span<const uint32_t> list) {
    float p = 0.0f, q = 0.0f;
    for (const uint32_t i : list) {
        const FleetObject& o = objs[i];
        float a = 0.0f, b = 0.0f;
        if (!o.team) {
            a = o.box.hx;
            b = o.box.hy;
        } else {
            // The company type's offsets (team +0x58); the ranges start at 0, so they contain the origin.
            float xmin = 0.0f, xmax = 0.0f, ymin = 0.0f, ymax = 0.0f;
            for (size_t j = 0; j < o.members.size(); ++j) {
                const V2 off = j < o.offsets_company.size() ? o.offsets_company[j] : V2{};
                const HalfBox m = o.members[j];
                xmin = std::min(xmin, off.x - m.hy);
                xmax = std::max(xmax, off.x + m.hy);
                ymin = std::min(ymin, off.y - m.hx);
                ymax = std::max(ymax, off.y + m.hx);
            }
            a = (xmax - xmin) / 2.0f;
            b = (ymax - ymin) / 2.0f;
        }
        if (a > q) q = a;
        if (b > p) p = b;
    }
    return {2.0f * p, 2.0f * q};
}

float grid_theta(float yaw_deg) {
    float t = yaw_deg + 270.0f;
    if (t > 360.0f) t = t - 360.0f;
    return t;
}

float arc_theta(float yaw_deg, float a_deg) {
    float t = yaw_deg + a_deg + 270.0f;
    while (t > 360.0f) t = t - 360.0f;
    while (t < 0.0f) t = t + 360.0f;
    return t;
}

float capital_grid(std::span<const FleetObject> objs, std::span<const uint32_t> list, V2 size, V2 anchor, float yaw_deg,
                   std::vector<Placed>& out) {
    const uint32_t n = static_cast<uint32_t>(list.size());
    if (n == 0) return 0.0f;
    const std::vector<uint32_t> counts = grid_row_counts(n);
    const uint32_t rows = static_cast<uint32_t>(counts.size());
    uint32_t m = 0;
    for (const uint32_t c : counts) m = std::max(m, c);
    const float sx = size.x, sy = size.y;
    float y = static_cast<float>(rows) * sy * 0.5f - sy * 0.5f;
    uint32_t prev = 0;
    bool stagger = false;
    size_t k = 0;
    const float theta = grid_theta(yaw_deg);
    for (const uint32_t c : counts) {
        float x0 = sx * 0.5f - (static_cast<float>(c) * sx) / 2.0f;  // + (c - 1) * 0: no gap
        if (prev != 0) {
            if (prev == c) {
                stagger = !stagger;
                if (stagger) x0 = x0 + sx;
            } else if (stagger) {
                x0 = x0 - sx;
                stagger = false;
            }
        }
        prev = c;
        for (uint32_t s = 0; s < c && k < list.size(); ++s, ++k) {
            const V2 local{x0 + static_cast<float>(s) * sx, y};
            emit(objs, list[k], Via::Capital, anchor + rotate(theta, local), yaw_deg, out);
        }
        y = y - sy;
    }
    return m == 1 ? 0.5f * sx : static_cast<float>(m - 1) * 0.5f * sx;
}

float screen_arc(std::span<const FleetObject> objs, std::span<const uint32_t> list, float r, V2 anchor, float yaw_deg,
                 std::vector<Placed>& out) {
    const uint32_t n = static_cast<uint32_t>(list.size());
    if (n == 0) return r;
    const float s = size_length(list_size(objs, list));
    const float r0 = r + s / 2.0f;
    std::vector<uint32_t> rings;
    float big_r = r0;
    for (uint32_t remaining = n; remaining > 0;) {
        // [port] s = 0 (no box): one ring.
        uint32_t c = s > 0.0f ? trunc_capped(std::max(3.0f * s, 1.25f * dmath::kPiF * big_r) / (s / 2.0f), remaining) : remaining;
        if (c == 0) c = remaining;  // [port] never: the quotient is >= 6
        rings.push_back(c);
        remaining -= c;
        big_r = big_r + s / 2.0f;
    }
    size_t k = 0;
    for (size_t j = 0; j < rings.size(); ++j) {
        const uint32_t c = rings[j];
        const float rho = r0 + static_cast<float>(j) * s;
        float a = c == 1 ? 90.0f : 337.5f;
        const float step = c == 1 ? 0.0f : 225.0f / static_cast<float>(c - 1);
        for (uint32_t i = 0; i < c && k < list.size(); ++i, ++k) {
            emit(objs, list[k], Via::Screen, anchor + polar(arc_theta(yaw_deg, a), rho), yaw_deg, out);
            a = a + step;
        }
    }
    return big_r;
}

float lead_arc(std::span<const FleetObject> objs, std::span<const uint32_t> list, float r, V2 anchor, float yaw_deg,
               std::vector<Placed>& out) {
    const uint32_t n = static_cast<uint32_t>(list.size());
    if (n == 0) return r;
    const float s = size_length(list_size(objs, list));
    const float r0 = r + s / 2.0f;
    std::vector<uint32_t> rings;
    float big_r = r0;
    for (uint32_t remaining = n; remaining > 0;) {
        uint32_t c = remaining;
        if (s > 0.0f) {  // [port] s = 0 (no box): one ring
            const float q = rings.empty() ? std::max(2.0f * s, 2.0f * big_r) / s : (2.0f * big_r) / s;
            c = trunc_capped(q, remaining);
            c = std::min(c + 1u, remaining);
        }
        rings.push_back(c);
        remaining -= c;
        big_r = big_r + s;
    }
    size_t k = 0;
    for (size_t j = 0; j < rings.size(); ++j) {
        const uint32_t c = rings[j];
        const float rho = r0 + static_cast<float>(j) * s;
        float a = c == 1 ? 90.0f : 25.0f;
        const float step = c == 1 ? 0.0f : 130.0f / static_cast<float>(c - 1);
        for (uint32_t i = 0; i < c && k < list.size(); ++i, ++k) {
            emit(objs, list[k], Via::Lead, anchor + polar(arc_theta(yaw_deg, a), rho), yaw_deg, out);
            a = a + step;
        }
    }
    return big_r;
}

void transport_rows(std::span<const FleetObject> objs, std::span<const uint32_t> list, float w, V2 anchor, float yaw_deg,
                    std::vector<Placed>& out) {
    const uint32_t n = static_cast<uint32_t>(list.size());
    if (n == 0) return;
    const V2 size = list_size(objs, list);
    const float sx = size.x, sy = size.y;
    uint32_t width = static_cast<uint32_t>(std::sqrt(static_cast<float>(n)) + 0.99f);
    if (width < 2) width = 2;
    float y = -w - 200.0f;
    const float theta = grid_theta(yaw_deg);
    size_t k = 0;
    for (uint32_t remaining = n; remaining > 0;) {
        const uint32_t c = std::min(width, remaining);
        const float x0 = sx / 2.0f - (static_cast<float>(c) * sx + static_cast<float>(c - 1) * 20.0f) / 2.0f;
        for (uint32_t s = 0; s < c && k < list.size(); ++s, ++k) {
            const V2 local{x0 + static_cast<float>(s) * (sx + 20.0f), y};
            emit(objs, list[k], Via::Transport, anchor + rotate(theta, local), yaw_deg, out);
        }
        remaining -= c;
        y = y - (sy + 20.0f);
    }
}

std::vector<Placed> place_group(std::span<const FleetObject> objs, V2 anchor, float yaw_deg) {
    std::vector<uint32_t> lists[6];
    std::vector<uint32_t> fighters, bombers;
    for (uint32_t i = 0; i < objs.size(); ++i) {
        const int32_t l = list_of_class(objs[i].ship_class);
        if (l < 0) continue;
        lists[l].push_back(i);
        if (l == 3) (objs[i].ship_class == 0 ? fighters : bombers).push_back(i);
    }
    if (!fighters.empty() && !bombers.empty()) lists[3] = interleave(fighters, bombers);
    std::vector<Placed> out;
    (void)capital_grid(objs, lists[5], V2{}, anchor, yaw_deg, out);  // supers: every one at the anchor
    int32_t core = -1;
    for (int32_t i = 0; i < 4 && core < 0; ++i)
        if (!lists[i].empty()) core = i;
    if (core >= 0) {
        const std::vector<uint32_t>& lc = lists[core];
        const float w = capital_grid(objs, lc, list_size(objs, lc), anchor, yaw_deg, out);
        float r = std::sqrt(w * w + w * w);
        if (core < 3) {
            r = screen_arc(objs, lists[core + 1], r, anchor, yaw_deg, out);
            for (int32_t k = core + 2; k <= 3; ++k) r = lead_arc(objs, lists[k], r, anchor, yaw_deg, out);
        }
        transport_rows(objs, lists[4], w, anchor, yaw_deg, out);
    } else {
        (void)capital_grid(objs, lists[4], list_size(objs, lists[4]), anchor, yaw_deg, out);
    }
    return out;
}

}  // namespace alamo::tactical::movement::fleet
