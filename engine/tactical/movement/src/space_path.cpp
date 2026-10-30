#include "alamo/tactical/movement/space_path.hpp"

#include <algorithm>

namespace alamo::tactical::movement {

namespace {

constexpr float kTwoPi = 6.28318530717958647692f;

float seg_point_dist_sq(V2 a, V2 b, V2 c) {
    const V2 ab = b - a;
    const float l2 = length_sq(ab);
    float t = l2 > 0.0f ? dot(c - a, ab) / l2 : 0.0f;
    t = std::clamp(t, 0.0f, 1.0f);
    return length_sq(a + ab * t - c);
}

// min |d0 + v s|^2 over s in [0, du]
float min_dist_sq(V2 d0, V2 v, float du) {
    const float vv = length_sq(v);
    float s = 0.0f;
    if (vv > 0.0f && du > 0.0f) s = std::clamp(-dot(d0, v) / vv, 0.0f, du);
    return length_sq(d0 + v * s);
}

}  // namespace

// ---------------------------------------------------------------- occupancy
void SpaceOccupancy::set_box(V2 min_xy, V2 max_xy) {
    box_min_ = min_xy;
    box_max_ = max_xy;
    has_box_ = true;
}

void SpaceOccupancy::add_static(ObjectId id, V2 center, float radius, uint32_t bits) {
    statics_[id] = Static{center, radius, bits};
}
bool SpaceOccupancy::remove_static(ObjectId id) { return statics_.erase(id) != 0; }

void SpaceOccupancy::set_track(ObjectId id, uint32_t layer, float radius, uint32_t start_frame, std::vector<SpaceWaypoint> wps) {
    if (wps.empty()) {
        tracks_.erase(id);
        return;
    }
    tracks_[id] = Track{layer, radius, start_frame, std::move(wps)};
}
void SpaceOccupancy::clear_track(ObjectId id) { tracks_.erase(id); }

uint32_t SpaceOccupancy::query_static(V2 a, V2 b, float radius) const {
    uint32_t bits = 0;
    for (const auto& [id, s] : statics_) {
        const float rr = radius + s.r;
        if (seg_point_dist_sq(a, b, s.c) < rr * rr) bits |= s.bits;
    }
    return bits;
}

uint32_t SpaceOccupancy::query_segment(V2 a, V2 b, float t0, float t1, float radius, uint32_t layer, ObjectId self, bool with_static,
                                       uint32_t now) const {
    uint32_t bits = 0;
    if (with_static) {
        for (const auto& [id, s] : statics_) {
            if (id == self) continue;
            const float rr = radius + s.r;
            if (seg_point_dist_sq(a, b, s.c) < rr * rr) bits |= s.bits;
        }
    }
    if (t1 < t0) std::swap(t0, t1);
    const float horizon = static_cast<float>(now) + static_cast<float>(window_);
    if (t0 > horizon) return bits;
    const float qdt = t1 - t0;
    const V2 qv = qdt > 0.0f ? (b - a) * (1.0f / qdt) : V2{};
    auto q_at = [&](float u) { return qdt > 0.0f ? a + qv * (u - t0) : a; };
    for (const auto& [id, tr] : tracks_) {
        if (id == self || (tr.layer & layer) == 0) continue;
        const float rr = radius + tr.r;
        const float rr2 = rr * rr;
        const float base = static_cast<float>(tr.start);
        const size_t n = tr.wps.size();
        // Pieces: before the first waypoint, each segment, after the last.
        for (size_t i = 0; i <= n; ++i) {
            float ps, pe;
            V2 p0, pv{};
            uint32_t bit = kSpaceStatic;
            if (i == 0) {
                ps = -3.0e38f;
                pe = base + tr.wps[0].time;
                p0 = tr.wps[0].pos;
            } else if (i == n) {
                ps = base + tr.wps[n - 1].time;
                pe = 3.0e38f;
                p0 = tr.wps[n - 1].pos;
            } else {
                ps = base + tr.wps[i - 1].time;
                pe = base + tr.wps[i].time;
                p0 = tr.wps[i - 1].pos;
                const float d = pe - ps;
                if (d > 0.0f) pv = (tr.wps[i].pos - tr.wps[i - 1].pos) * (1.0f / d);
                if (!(tr.wps[i].pos == tr.wps[i - 1].pos)) bit = kSpaceMoving;
            }
            const float u0 = std::max(t0, ps), u1 = std::min(t1, pe);
            if (u0 > u1) continue;
            const V2 tp0 = (i == 0 || i == n) ? p0 : p0 + pv * (u0 - ps);
            const V2 d0 = q_at(u0) - tp0;
            if (min_dist_sq(d0, qv - pv, u1 - u0) < rr2) bits |= bit;
        }
    }
    return bits;
}

void SpaceOccupancy::hash(StateHasher& h) const {
    h.u32(static_cast<uint32_t>(statics_.size()));
    for (const auto& [id, s] : statics_) {
        h.i32(id);
        h.f32(s.c.x);
        h.f32(s.c.y);
        h.f32(s.r);
        h.u32(s.bits);
    }
    h.u32(static_cast<uint32_t>(tracks_.size()));
    for (const auto& [id, t] : tracks_) {
        h.i32(id);
        h.u32(t.layer);
        h.f32(t.r);
        h.u32(t.start);
        h.u32(static_cast<uint32_t>(t.wps.size()));
        for (const SpaceWaypoint& w : t.wps) {
            h.f32(w.pos.x);
            h.f32(w.pos.y);
            h.f32(w.heading);
            h.f32(w.speed);
            h.f32(w.time);
        }
    }
}

// ---------------------------------------------------------------- planner
namespace {

struct TryParams {
    float rot_div = 1.0f;
    float step_scale = 1.0f;
    uint32_t budget = 3500;
    float heuristic_div = 1.0f;
    float weight_dist = 0.0f;
    uint32_t mask = kSpaceDefaultMask;
    float speed = 0.0f;
    bool facing = false;  // OI tactical: final facing still required (try 1)
};

uint64_t vkey(int32_t cx, int32_t cy, int32_t b) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(cx + (1 << 20)) & 0x1FFFFFu) << 40) |
           (static_cast<uint64_t>(static_cast<uint32_t>(cy + (1 << 20)) & 0x1FFFFFu) << 16) |
           static_cast<uint64_t>(static_cast<uint32_t>(b) & 0xFFFFu);
}

// §2.1: a closed state is re-expanded only by ops 1, 4, 5, 6, 7, 8, 10, 11, 14, 15 and 17-21 (OI tactical: the doc's
// list; the S7 port also listed 9).
bool reexpand_op(uint8_t op) {
    return op == 1 || op == 4 || op == 5 || op == 6 || op == 7 || op == 8 || op == 10 || op == 11 || op == 14 || op == 15 ||
           (op >= 17 && op <= 21);
}
bool rejects_on_hit(uint8_t op) { return op == 4 || op == 15 || op == 16; }  // "they also reject ops 4, 15 and 16"
constexpr float kPi = 3.14159265358979323846f;
// REMAINING_GAPS §4.2: compass table 0x140a43b80 (ops 25-32), not normalised.
constexpr float kCompass[8][2] = {{0.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 0.0f}, {1.0f, -1.0f},
                                  {0.0f, -1.0f}, {-1.0f, -1.0f}, {-1.0f, 0.0f}, {-1.0f, 1.0f}};
bool compass_op(uint8_t op) { return op >= 25 && op <= 32; }

struct Search {
    const SpaceOccupancy& occ;
    const MovementConstants& k;
    const SpaceRequest& req;
    const TryParams& tp;
    SpaceScratch& sc;

    int32_t buckets = 24;
    float step_deg = 15.0f;
    float D = 300.0f;
    float R = 0.0f;
    float wait_speed = 0.0f;
    float sagitta = 0.0f;
    float far_from_start = 0.0f;
    bool endpoints_inside = true;
    int32_t gcx = 0, gcy = 0;
    float dist0 = 0.0f;
    float best_progress = 0.0f;
    uint32_t pushes = 0;
    // OI tactical: speed change and facing mode
    float accel = 0.0f, decel = 0.0f;
    bool facing = false;
    float goal_facing = 0.0f;
    int32_t goal_bucket = 0;
    float rs = 0.0f;          // slow-turn radius (goal-facing circles)
    float sagitta_s = 0.0f;   // sagitta of one turn step on the slow circles
    V2 circ_c[2] = {};        // 0 = left (counter-clockwise), 1 = right (clockwise)
    bool threat = false;      // REMAINING_GAPS §4.2: threat mode (ops 25-32 only)
    ThreatCost tcost{};

    int32_t cell(float v, float s) const { return static_cast<int32_t>(std::floor((v - s) * 1.5f / D)); }
    int32_t bucket(float h) const {
        return static_cast<int32_t>(std::floor(norm_deg(h) * static_cast<float>(buckets + 1) / 360.0f));
    }
    float weight(float travelled) const { return travelled < tp.weight_dist ? 1.0f / tp.heuristic_div : 1.0f; }

    // Validates a motion and returns its cost multiplier (<= 0: rejected).
    float motion_factor(V2 a, V2 b, float t0, float t1, uint8_t op) const { return motion_factor_r(a, b, t0, t1, op, sagitta); }
    float motion_factor_r(V2 a, V2 b, float t0, float t1, uint8_t op, float inflate) const {
        float f = 1.0f;
        const bool use_static = (tp.mask & 0x3Eu) != 0;
        uint32_t bits = occ.query_segment(a, b, static_cast<float>(req.now) + t0, static_cast<float>(req.now) + t1,
                                          req.radius + inflate, req.layer, req.self, use_static, req.now);
        bits &= tp.mask;
        if (bits != 0) {
            const bool far = distance(b, req.start) > far_from_start;
            if (rejects_on_hit(op) || ((bits & ~kSpaceMoving) != 0 && far)) return -1.0f;
            f *= k.min_obstacle_cost_space / k.current_path_cost_coefficient_space;
        }
        if (!occ.inside_box(b)) {
            if (rejects_on_hit(op)) return -1.0f;
            if (endpoints_inside && distance(b, req.start) > far_from_start + 100.0f) return -1.0f;
            f *= k.off_map_cost_penalty;
        }
        return f;
    }

    // Heuristic (frames): distance / speed; facing mode: (R x turn angle (x 1.5 beyond pi) + distance x 0.6) / speed.
    float hval(V2 pos, float heading) const {
        const float dg = distance(pos, req.goal);
        if (!facing) return dg / tp.speed;
        const float to = dg > 0.0f ? heading_deg(req.goal - pos) : heading;
        const float a = (std::fabs(wrap_deg(to - heading)) + std::fabs(wrap_deg(goal_facing - to))) * kDegToRad;
        float arc_len = R * a;
        if (a > kPi) arc_len *= k.final_facing_180_penalty;
        return (arc_len + dg * k.special_aligned_operator_bonus) / tp.speed;
    }

    void push(const SpaceScratch::Node& parent, int32_t parent_index, V2 pos, float heading, float dt, float dist, float mult,
              uint8_t op, float node_speed, const SpaceScratch::Node* arc = nullptr) {
        if (mult <= 0.0f || pushes >= tp.budget) return;
        SpaceScratch::Node n;
        if (arc) {
            n.arc_c = arc->arc_c;
            n.arc_r = arc->arc_r;
            n.arc_sweep = arc->arc_sweep;
        }
        n.pos = pos;
        n.heading = wrap_deg(heading);
        n.time = parent.time + dt;
        n.g = parent.g + dt * mult;
        n.travelled = parent.travelled + dist;
        n.parent = parent_index;
        n.op = op;
        n.speed = node_speed;
        const uint64_t key = vkey(cell(pos.x, req.start.x), cell(pos.y, req.start.y), bucket(n.heading));
        auto it = sc.visited.find(key);
        if (it != sc.visited.end()) {
            if (it->second.closed) {
                if (!reexpand_op(op) || n.g > it->second.g) return;
                it->second.closed = false;
                it->second.g = n.g;
            } else {
                if (!(n.g < it->second.g)) return;
                it->second.g = n.g;
            }
        } else {
            sc.visited.emplace(key, SpaceScratch::Visit{n.g, false});
        }
        const float h = hval(pos, n.heading);
        sc.nodes.push_back(n);
        ++pushes;
        sc.heap.push(n.g + weight(n.travelled) * h, static_cast<int32_t>(sc.nodes.size() - 1));
    }

    // Arc of `ang` degrees (positive = left) from (p, h).
    void arc(V2 p, float h, float ang, V2& np, float& nh) const {
        const V2 left = perp_left(dir_from_deg(h));
        nh = h + ang;
        const V2 nleft = perp_left(dir_from_deg(nh));
        if (ang >= 0.0f) {
            const V2 c = p + left * R;
            np = c - nleft * R;
        } else {
            const V2 c = p - left * R;
            np = c + nleft * R;
        }
    }

    // OI tactical: ops 5 / 6 / 19 / 20 - speed change along the heading (§2.1).
    void speed_change(const SpaceScratch::Node& n, int32_t ni, float v1, uint8_t op) {
        const float v0 = n.speed;
        const float a = v1 > v0 ? accel : decel;
        if (!(a > 0.0f)) return;
        const float dist = std::fabs(v1 * v1 - v0 * v0) / (2.0f * a);
        const float dt = std::fabs(v1 - v0) / a;
        const V2 np = n.pos + dir_from_deg(n.heading) * dist;
        push(n, ni, np, n.heading, dt, dist, motion_factor(n.pos, np, n.time, n.time + dt, op), op, v1);
    }

    // Tangent point from p onto goal circle `side` such that p -> T joins the circle in its direction of travel.
    bool tangent_point(V2 p, int side, V2& t) const {
        const V2 cc = circ_c[side];
        const V2 d = p - cc;
        const float l2 = length_sq(d);
        if (!(l2 > rs * rs)) return false;
        const float l = std::sqrt(l2);
        const V2 u = d * (1.0f / l);
        const V2 v = perp_left(u);
        const float cr = rs / l;
        const float sr = std::sqrt(std::max(0.0f, 1.0f - cr * cr));
        for (const float sg : {1.0f, -1.0f}) {
            const V2 tp_ = cc + (u * cr + v * (sg * sr)) * rs;
            const V2 radial = tp_ - cc;
            const V2 vel = side == 0 ? perp_left(radial) : perp_left(radial) * -1.0f;
            if (dot(tp_ - p, vel) > 0.0f) {
                t = tp_;
                return true;
            }
        }
        return false;
    }

    // Ops 11 / 12 (full speed) and 15 / 16 (wait speed): straight tangent onto a goal circle.
    void tangent_op(const SpaceScratch::Node& n, int32_t ni, int side, uint8_t op, float v) {
        V2 t;
        if (!tangent_point(n.pos, side, t)) return;
        const float L = distance(n.pos, t);
        if (!(L > 0.0f)) return;
        const float hd = heading_deg(t - n.pos);
        if (std::fabs(wrap_deg(hd - n.heading)) > step_deg) return;  // must already head along it [port]
        const float dt = L / v;
        push(n, ni, t, hd, dt, L, k.special_aligned_operator_bonus * motion_factor(n.pos, t, n.time, n.time + dt, op), op, v);
    }

    // Ops 17 / 21 / 22: final arc along goal circle `side` from n.pos onto the goal pose, at the wait speed.
    void final_arc(const SpaceScratch::Node& n, int32_t ni, int side, uint8_t op) {
        if (!(wait_speed > 0.0f)) return;
        const V2 cc = circ_c[side];
        const float o = side == 0 ? 1.0f : -1.0f;
        const float a0 = heading_deg(n.pos - cc);
        const float sweep = norm_deg(o * (heading_deg(req.goal - cc) - a0));  // [0, 360) in the travel direction
        const float L = rs * sweep * kDegToRad;
        const float dt = L / wait_speed;
        const uint32_t steps = std::max<uint32_t>(1u, static_cast<uint32_t>(std::ceil(sweep / step_deg)));
        float f = 0.0f;
        V2 prev = n.pos;
        for (uint32_t j = 1; j <= steps; ++j) {
            const V2 pj = j == steps ? req.goal : cc + dir_from_deg(a0 + o * sweep * static_cast<float>(j) / static_cast<float>(steps)) * rs;
            const float t0 = n.time + dt * static_cast<float>(j - 1) / static_cast<float>(steps);
            const float t1 = n.time + dt * static_cast<float>(j) / static_cast<float>(steps);
            const float m = motion_factor_r(prev, pj, t0, t1, op, sagitta_s);
            if (m <= 0.0f) return;
            f = std::max(f, m);
            prev = pj;
        }
        SpaceScratch::Node arc;
        arc.arc_c = cc;
        arc.arc_r = rs;
        arc.arc_sweep = o * sweep;
        push(n, ni, req.goal, goal_facing, dt, L, k.wait_operator_speed_coefficient * f, op, wait_speed, &arc);
    }

    bool on_circle(const SpaceScratch::Node& n, int side) const {
        const V2 cc = circ_c[side];
        const float d = distance(n.pos, cc);
        if (std::fabs(d - rs) > 0.05f * rs + 1.0f) return false;
        const V2 radial = n.pos - cc;
        const V2 vel = side == 0 ? perp_left(radial) : perp_left(radial) * -1.0f;
        return length_sq(vel) > 0.0f && std::fabs(wrap_deg(heading_deg(vel) - n.heading)) <= step_deg;
    }

    // OI tactical: facing-mode expansion, ops 10-22 in index order (+ 23 / 24 on the first pop), §2.1.
    void expand_facing(const SpaceScratch::Node& n, int32_t ni, V2 fwd, float dg, bool first, const bool* have_escape,
                       const float* escape) {
        const float speed = tp.speed;
        {  // op 10: full step D
            const V2 np = n.pos + fwd * D;
            const float dt = D / speed;
            push(n, ni, np, n.heading, dt, D, motion_factor(n.pos, np, n.time, n.time + dt, 10), 10, speed);
        }
        for (int side = 0; side < 2; ++side) tangent_op(n, ni, side, static_cast<uint8_t>(11 + side), speed);  // ops 11 / 12
        for (int side = 0; side < 2; ++side) {  // ops 13 / 14: arcs left / right one turn step
            V2 np;
            float nh;
            arc(n.pos, n.heading, side == 0 ? step_deg : -step_deg, np, nh);
            const float L = R * step_deg * kDegToRad;
            const float dt = L / speed;
            const float m = 1.0f + 0.5f / std::max(1.0f, dg / 400.0f);
            const auto op = static_cast<uint8_t>(13 + side);
            push(n, ni, np, nh, dt, L, m * motion_factor(n.pos, np, n.time, n.time + dt, op), op, speed);
        }
        if (wait_speed > 0.0f && n.speed <= wait_speed)  // ops 15 / 16: tangents at the wait speed
            for (int side = 0; side < 2; ++side) tangent_op(n, ni, side, static_cast<uint8_t>(15 + side), wait_speed);
        const bool after_tangent = n.op == 11 || n.op == 12 || n.op == 15 || n.op == 16;
        const int tangent_side = (n.op == 11 || n.op == 15) ? 0 : 1;
        if (after_tangent) final_arc(n, ni, tangent_side, 17);  // op 17
        if (req.wait_operators && wait_speed > 0.0f) {          // op 18: wait
            const float dt = k.wait_operator_base_frame_time;
            const float L = std::min(wait_speed * dt, dg);
            const V2 np = n.pos + fwd * L;
            push(n, ni, np, n.heading, dt, L, k.wait_operator_cost_coefficient * motion_factor(n.pos, np, n.time, n.time + dt, 18),
                 18, wait_speed);
        }
        if (n.speed < speed) speed_change(n, ni, speed, 19);                                  // op 19
        if (n.speed > wait_speed && wait_speed > 0.0f) speed_change(n, ni, wait_speed, 20);  // op 20
        for (int side = 0; side < 2; ++side)  // ops 21 / 22
            if (!(after_tangent && side == tangent_side) && on_circle(n, side)) final_arc(n, ni, side, static_cast<uint8_t>(21 + side));
        if (first) {  // ops 23 / 24: turn in place to the escape headings
            for (int side = 0; side < 2; ++side) {
                if (!have_escape[side]) continue;
                const float ang = std::fabs(wrap_deg(escape[side] - n.heading));
                const float dt = req.turn_rate > 0.0f ? ang / req.turn_rate : 0.0f;
                push(n, ni, n.pos, escape[side], dt, 0.0f, 1.0f, static_cast<uint8_t>(23 + side), 0.0f);
            }
        }
    }

    // REMAINING_GAPS §4.2 (0x596d40 cases 0x19-0x20, cost 0x599380): eight compass moves by min(distance, D).
    void expand_compass(const SpaceScratch::Node& n, int32_t ni, float dg) {
        const float d = std::min(dg, D);
        for (uint8_t c = 0; c < 8; ++c) {
            const V2 step{kCompass[c][0] * d, kCompass[c][1] * d};
            const V2 np = n.pos + step;
            if (!occ.inside_box(np)) continue;  // reject a successor outside the playable box
            const float L = length(step);
            const float dt = L / tp.speed;
            const auto op = static_cast<uint8_t>(25 + c);
            const float m = motion_factor(n.pos, np, n.time, n.time + dt, op);
            if (m <= 0.0f) continue;
            push(n, ni, np, n.heading, dt, L, tcost.factor(np) * m, op, tp.speed);
        }
    }

    bool run(std::vector<SpaceScratch::Node>& chain) {
        const float speed = tp.speed;
        buckets = std::max<int32_t>(1, static_cast<int32_t>(std::floor(static_cast<float>(k.max_rotations_space) / tp.rot_div)));
        step_deg = 360.0f / static_cast<float>(buckets);
        dist0 = distance(req.start, req.goal);
        D = std::max(speed, k.xy_expansion_distance_space * tp.step_scale);
        if (dist0 < 2.0f * D) D = std::max(50.0f, dist0 * 0.5f);
        threat = req.threat_mode();
        if (threat) {
            D = k.threat_expansion_distance;  // §4.2
            tcost = ThreatCost::make(req.threat, req.threat_threshold);
        }
        const float frames_per_turn = req.turn_rate > 0.0f ? 360.0f / req.turn_rate : 1.0e6f;
        R = speed * frames_per_turn / kTwoPi;
        wait_speed = speed * k.wait_operator_speed_coefficient;
        sagitta = R * (1.0f - dmath::cos(step_deg * 0.5f * kDegToRad));
        far_from_start = req.radius * k.occupation_radius_coefficient_space + R;
        endpoints_inside = occ.inside_box(req.start) && occ.inside_box(req.goal);
        gcx = cell(req.goal.x, req.start.x);
        gcy = cell(req.goal.y, req.start.y);
        accel = req.accel > 0.0f ? req.accel : speed;  // §2.1: default = speed
        decel = req.decel > 0.0f ? req.decel : speed;
        // [port]: threat mode replaces the facing operators (its set is 25-32 only), so the facing goal test is off too.
        facing = tp.facing && req.goal_facing != kNoFacing && !req.threat_mode();
        if (facing) {
            goal_facing = wrap_deg(req.goal_facing);
            goal_bucket = bucket(goal_facing);
            rs = R * k.wait_operator_speed_coefficient;  // slow-turn radius
            sagitta_s = rs * (1.0f - dmath::cos(step_deg * 0.5f * kDegToRad));
            const V2 gl = perp_left(dir_from_deg(goal_facing));
            circ_c[0] = req.goal + gl * rs;
            circ_c[1] = req.goal - gl * rs;
        }

        sc.nodes.clear();
        sc.heap.clear();
        sc.heap.set_capacity(std::max<uint32_t>(tp.budget, 16u));
        sc.visited.clear();
        SpaceScratch::Node s;
        s.pos = req.start;
        s.heading = wrap_deg(req.start_heading);
        s.speed = speed;
        sc.nodes.push_back(s);
        sc.visited.emplace(vkey(cell(s.pos.x, req.start.x), cell(s.pos.y, req.start.y), bucket(s.heading)),
                           SpaceScratch::Visit{0.0f, false});
        sc.heap.push(hval(s.pos, s.heading), 0);
        pushes = 1;

        // Escape headings when the start overlaps an obstacle (§2.1 "Escape headings").
        bool overlapped = (occ.query_point(req.start, static_cast<float>(req.now), req.radius, req.layer, req.self, req.now) &
                           tp.mask & ~kSpaceMoving) != 0;
        float escape[2] = {0.0f, 0.0f};
        bool have_escape[2] = {false, false};
        if (overlapped) {
            for (int side = 0; side < 2; ++side) {
                for (float a = 10.0f; a <= 180.0f; a += 10.0f) {
                    const float hh = s.heading + (side == 0 ? a : -a);
                    const V2 e = req.start + dir_from_deg(hh) * (req.radius * 2.0f + D);
                    if ((occ.query_static(req.start + dir_from_deg(hh) * req.radius, e, req.radius) & tp.mask) == 0) {
                        escape[side] = hh;
                        have_escape[side] = true;
                        break;
                    }
                }
            }
        }

        bool first = true;
        int32_t found = -1;
        while (!sc.heap.empty()) {
            const int32_t ni = sc.heap.pop().value;
            const SpaceScratch::Node n = sc.nodes[static_cast<size_t>(ni)];
            const uint64_t key = vkey(cell(n.pos.x, req.start.x), cell(n.pos.y, req.start.y), bucket(n.heading));
            auto vit = sc.visited.find(key);
            if (vit != sc.visited.end()) {
                if (vit->second.closed || n.g > vit->second.g) continue;
                vit->second.closed = true;
            }
            const float dg = distance(n.pos, req.goal);
            best_progress = std::max(best_progress, dist0 - dg);  // furthest progress toward the goal
            if (cell(n.pos.x, req.start.x) == gcx && cell(n.pos.y, req.start.y) == gcy &&
                (!facing || bucket(n.heading) == goal_bucket ||
                 std::fabs(wrap_deg(n.heading - goal_facing)) <= k.match_facing_delta_space)) {
                found = ni;
                break;
            }
            if (pushes >= tp.budget) break;
            const V2 fwd = dir_from_deg(n.heading);
            if (threat) {  // §4.2: ops 25-32 only
                expand_compass(n, ni, dg);
                first = false;
                continue;
            }
            if (facing) {
                expand_facing(n, ni, fwd, dg, first, have_escape, escape);
                first = false;
                continue;
            }
            const float herr = wrap_deg(heading_deg(req.goal - n.pos) - n.heading);
            // op 0: straight ahead by min(distance to goal, D)
            {
                const float L = std::min(dg, D);
                const V2 np = n.pos + fwd * L;
                const float dt = L / speed;
                push(n, ni, np, n.heading, dt, L, motion_factor(n.pos, np, n.time, n.time + dt, 0), 0, speed);
            }
            // op 1: arc toward the goal
            if (std::fabs(herr) > 0.01f && std::fabs(herr) <= step_deg) {
                V2 np;
                float nh;
                arc(n.pos, n.heading, herr, np, nh);
                const float L = R * std::fabs(herr) * kDegToRad;
                const float dt = L / speed;
                push(n, ni, np, nh, dt, L, 0.99f * motion_factor(n.pos, np, n.time, n.time + dt, 1), 1, speed);
            }
            // ops 2 / 3: arcs left / right one turn step
            for (int side = 0; side < 2; ++side) {
                V2 np;
                float nh;
                arc(n.pos, n.heading, side == 0 ? step_deg : -step_deg, np, nh);
                const float L = R * step_deg * kDegToRad;
                const float dt = L / speed;
                const float m = 1.0f + 0.5f / std::max(1.0f, dg / 400.0f);
                push(n, ni, np, nh, dt, L, m * motion_factor(n.pos, np, n.time, n.time + dt, static_cast<uint8_t>(2 + side)),
                     static_cast<uint8_t>(2 + side), speed);
            }
            // op 4: straight to the goal, after op 1
            if (n.op == 1 || (ni == 0 && std::fabs(herr) <= 0.01f)) {
                const float dt = dg / speed;
                push(n, ni, req.goal, heading_deg(req.goal - n.pos), dt, dg,
                     0.98f * motion_factor(n.pos, req.goal, n.time, n.time + dt, 4), 4, speed);
            }
            // OI tactical: ops 5 / 6 - speed up to max (after a wait / slow-down), slow down to the wait speed.
            if (n.speed < speed) speed_change(n, ni, speed, 5);
            if (n.speed > wait_speed && wait_speed > 0.0f) speed_change(n, ni, wait_speed, 6);
            // op 7: wait (slow straight for WaitOperatorBaseFrameTime frames)
            if (req.wait_operators && wait_speed > 0.0f) {
                const float dt = k.wait_operator_base_frame_time;
                const float L = std::min(wait_speed * dt, dg);
                const V2 np = n.pos + fwd * L;
                push(n, ni, np, n.heading, dt, L,
                     k.wait_operator_cost_coefficient * motion_factor(n.pos, np, n.time, n.time + dt, 7), 7, wait_speed);
            }
            // ops 8 / 9: turn in place to the escape headings (first expansion only)
            if (first) {
                for (int side = 0; side < 2; ++side) {
                    if (!have_escape[side]) continue;
                    const float ang = std::fabs(wrap_deg(escape[side] - n.heading));
                    const float dt = req.turn_rate > 0.0f ? ang / req.turn_rate : 0.0f;
                    push(n, ni, n.pos, escape[side], dt, 0.0f, 1.0f, static_cast<uint8_t>(8 + side), 0.0f);
                }
            }
            first = false;
        }
        if (found < 0) return false;
        chain.clear();
        for (int32_t i = found; i >= 0; i = sc.nodes[static_cast<size_t>(i)].parent) chain.push_back(sc.nodes[static_cast<size_t>(i)]);
        std::reverse(chain.begin(), chain.end());
        return true;
    }
};

void chain_to_waypoints(const std::vector<SpaceScratch::Node>& chain, const SpaceRequest& req, float speed, float step_deg,
                        std::vector<SpaceWaypoint>& out) {
    out.clear();
    for (size_t ci = 0; ci < chain.size(); ++ci) {
        const SpaceScratch::Node& n = chain[ci];
        if (n.arc_r > 0.0f && ci > 0 && step_deg > 0.0f) {
            // OI tactical: final arcs are densified into turn-step increments (§2.4).
            const SpaceScratch::Node& p = chain[ci - 1];
            const float sweep = std::fabs(n.arc_sweep);
            const float o = n.arc_sweep < 0.0f ? -1.0f : 1.0f;
            const uint32_t steps = std::max<uint32_t>(1u, static_cast<uint32_t>(std::ceil(sweep / step_deg)));
            const float a0 = heading_deg(p.pos - n.arc_c);
            for (uint32_t j = 1; j < steps; ++j) {
                const float a = a0 + o * sweep * static_cast<float>(j) / static_cast<float>(steps);
                SpaceWaypoint w;
                w.pos = n.arc_c + dir_from_deg(a) * n.arc_r;
                w.heading = wrap_deg(a + o * 90.0f);
                w.speed = n.speed;
                w.time = p.time + (n.time - p.time) * static_cast<float>(j) / static_cast<float>(steps);
                out.push_back(w);
            }
        }
        SpaceWaypoint w;
        w.pos = n.pos;
        w.heading = n.heading;
        // §4.2 [port]: a compass step keeps the node heading; the waypoint takes its travel direction.
        if (compass_op(n.op) && ci > 0 && !(n.pos == chain[ci - 1].pos)) w.heading = heading_deg(n.pos - chain[ci - 1].pos);
        w.speed = n.speed > 0.0f ? n.speed : speed;
        w.time = n.time;
        out.push_back(w);
    }
    if (out.empty()) return;
    out.front().speed = speed;
    if (!(out.back().pos == req.goal)) {
        const V2 d = req.goal - out.back().pos;
        SpaceWaypoint w;
        w.pos = req.goal;
        w.heading = heading_deg(d);
        w.speed = speed;
        w.time = out.back().time + length(d) / speed;
        out.push_back(w);
    }
}

void straight_path(const SpaceRequest& req, float speed, std::vector<SpaceWaypoint>& out) {
    out.clear();
    const V2 d = req.goal - req.start;
    const float h = length_sq(d) > 0.0f ? heading_deg(d) : req.start_heading;
    SpaceWaypoint a;
    a.pos = req.start;
    a.heading = wrap_deg(req.start_heading);
    a.speed = speed;
    out.push_back(a);
    const float turn = std::fabs(wrap_deg(h - req.start_heading));
    float t = req.turn_rate > 0.0f ? turn / req.turn_rate : 0.0f;
    if (turn > 0.0f) {
        SpaceWaypoint b = a;
        b.heading = h;
        b.time = t;
        out.push_back(b);
    }
    SpaceWaypoint c;
    c.pos = req.goal;
    c.heading = h;
    c.speed = speed;
    t += speed > 0.0f ? length(d) / speed : 0.0f;
    c.time = t;
    out.push_back(c);
}

// Rescue route when every lattice try failed (PORT ADDITION [L]; the retail outcome after the
// last try is not traced): 8-connected A* over a 100-unit grid of the playable box whose cells are
// blocked when their centre is within radius + half a cell diagonal of a static entry (start and
// goal cells always open), string-pulled with static segment tests, flown as straight legs with
// turn-in-place corners (zero-length Hermite segments), so the ship never crosses a static entry
// the grid saw. False when the box is missing / too large or no route exists.
bool static_route(const SpaceOccupancy& occ, const SpaceRequest& req, float speed, std::vector<SpaceWaypoint>& out) {
    if (!occ.has_box()) return false;
    const float c = 100.0f;
    const V2 mn = occ.box_min();
    const int32_t W = static_cast<int32_t>(std::ceil((occ.box_max().x - mn.x) / c)) + 1;
    const int32_t H = static_cast<int32_t>(std::ceil((occ.box_max().y - mn.y) / c)) + 1;
    if (W <= 1 || H <= 1 || static_cast<int64_t>(W) * H > 250000) return false;
    auto cell_of = [&](V2 p) {
        const int32_t x = std::clamp(static_cast<int32_t>(std::floor((p.x - mn.x) / c + 0.5f)), 0, W - 1);
        const int32_t y = std::clamp(static_cast<int32_t>(std::floor((p.y - mn.y) / c + 0.5f)), 0, H - 1);
        return y * W + x;
    };
    auto centre = [&](int32_t i) { return V2{mn.x + static_cast<float>(i % W) * c, mn.y + static_cast<float>(i / W) * c}; };
    const int32_t si = cell_of(req.start), gi = cell_of(req.goal);
    const float inflate = req.radius + c * 0.7072f;
    std::vector<uint8_t> blocked(static_cast<size_t>(W) * static_cast<size_t>(H), 0);
    for (int32_t i = 0; i < W * H; ++i) {
        const V2 p = centre(i);
        blocked[static_cast<size_t>(i)] = (i != si && i != gi && (occ.query_static(p, p, inflate) & req.mask) != 0) ? 1 : 0;
    }
    std::vector<float> g(blocked.size(), 3.0e38f);
    std::vector<int32_t> parent(blocked.size(), -1);
    std::vector<uint8_t> closed(blocked.size(), 0);
    RetailHeap<int32_t> heap;
    const V2 gp = centre(gi);
    g[static_cast<size_t>(si)] = 0.0f;
    heap.push(distance(centre(si), gp), si);
    bool found = false;
    while (!heap.empty()) {
        const int32_t ci = heap.pop().value;
        if (closed[static_cast<size_t>(ci)]) continue;
        closed[static_cast<size_t>(ci)] = 1;
        if (ci == gi) {
            found = true;
            break;
        }
        const int32_t cx = ci % W, cy = ci / W;
        for (int32_t dy = -1; dy <= 1; ++dy) {
            for (int32_t dx = -1; dx <= 1; ++dx) {
                if (!dx && !dy) continue;
                const int32_t nx = cx + dx, ny = cy + dy;
                if (nx < 0 || ny < 0 || nx >= W || ny >= H) continue;
                const int32_t ni = ny * W + nx;
                if (blocked[static_cast<size_t>(ni)] || closed[static_cast<size_t>(ni)]) continue;
                if (dx && dy && (blocked[static_cast<size_t>(cy * W + nx)] || blocked[static_cast<size_t>(ny * W + cx)])) continue;
                const float ng = g[static_cast<size_t>(ci)] + ((dx && dy) ? 1.41421356f : 1.0f) * c;
                if (!(ng < g[static_cast<size_t>(ni)])) continue;
                g[static_cast<size_t>(ni)] = ng;
                parent[static_cast<size_t>(ni)] = ci;
                heap.push(ng + distance(centre(ni), gp), ni);
            }
        }
    }
    if (!found) return false;
    std::vector<V2> pts;
    for (int32_t i = gi; i >= 0; i = parent[static_cast<size_t>(i)]) {
        pts.push_back(centre(i));
        if (i == si) break;
    }
    std::reverse(pts.begin(), pts.end());
    pts.front() = req.start;
    pts.back() = req.goal;
    // String pull with the static segment test.
    std::vector<V2> pulled{pts.front()};
    size_t i = 0;
    while (i + 1 < pts.size()) {
        size_t j = i + 1;
        for (size_t k = pts.size() - 1; k > i + 1; --k) {
            if ((occ.query_static(pts[i], pts[k], req.radius) & req.mask) == 0) {
                j = k;
                break;
            }
        }
        pulled.push_back(pts[j]);
        i = j;
    }
    out.clear();
    float t = 0.0f, heading = wrap_deg(req.start_heading);
    SpaceWaypoint w;
    w.pos = pulled.front();
    w.heading = heading;
    w.speed = speed;
    out.push_back(w);
    for (size_t k = 1; k < pulled.size(); ++k) {
        const V2 d = pulled[k] - pulled[k - 1];
        if (!(length_sq(d) > 0.0f)) continue;
        const float h = heading_deg(d);
        const float turn = std::fabs(wrap_deg(h - heading));
        if (turn > 0.0f) {
            t += req.turn_rate > 0.0f ? turn / req.turn_rate : 0.0f;
            w.pos = pulled[k - 1];
            w.heading = h;
            w.time = t;
            out.push_back(w);
        }
        t += length(d) / speed;
        w.pos = pulled[k];
        w.heading = h;
        w.time = t;
        out.push_back(w);
        heading = h;
    }
    return true;
}

}  // namespace

bool plan_space_path(const SpaceOccupancy& occ, const MovementConstants& k, const SpaceRequest& req, SpaceScratch& sc,
                     SpacePath& out) {
    out = SpacePath{};
    const float dist = distance(req.start, req.goal);
    if (!(req.speed > 0.0f)) return false;
    if (dist < 40.0f) {
        straight_path(req, req.speed, out.wps);
        out.ok = true;
        out.skipped = true;
        return true;
    }
    TryParams tp;
    tp.budget = k.space_pathfind_max_expansions;
    tp.mask = req.mask;
    tp.speed = req.speed;
    std::vector<SpaceScratch::Node> chain;
    float budget_f = static_cast<float>(tp.budget);
    float best_progress = 0.0f;
    const uint32_t tries = std::max<uint32_t>(1, k.space_pathing_tries);
    for (uint32_t t = 1; t <= tries; ++t) {
        if (t >= 2) {
            tp.rot_div += k.space_failure_rotation_increment;
            tp.step_scale += k.space_failure_forward_increment;
            budget_f *= k.space_failure_max_expansions_coefficient;
            tp.budget = static_cast<uint32_t>(budget_f);
        }
        if (t >= 3) {
            tp.weight_dist = std::max(tp.weight_dist + k.space_failure_distance_cutoff_coefficient * dist, best_progress);
            tp.heuristic_div *= k.current_path_cost_coefficient_space;
        }
        if (t >= 4) tp.mask = kSpaceMoving | kSpaceStatic;
        if (t >= 5) tp.speed = req.speed * 0.5f;
        tp.facing = t == 1;  // OI tactical: from try 2 the facing requirement is dropped
        Search s{occ, k, req, tp, sc};
        const bool ok = s.run(chain);
        out.tries = t;
        out.pushes += s.pushes;
        best_progress = std::max(best_progress, s.best_progress);
        if (ok) {
            chain_to_waypoints(chain, req, tp.speed, s.step_deg, out.wps);
            out.ok = true;
            return true;
        }
    }
    out.ok = true;
    out.fallback = true;
    if (!static_route(occ, req, req.speed, out.wps)) straight_path(req, req.speed, out.wps);
    return true;
}

V2 find_space_destination(const SpaceOccupancy& occ, const MovementConstants& k, V2 goal, V2 approach_from, float radius,
                          uint32_t layer, ObjectId self, uint32_t now, std::span<const ReservedSpot> reserved) {
    const float clear = std::max(radius * k.occupation_radius_coefficient_space, 1.0f);
    auto is_free = [&](V2 p) {
        if (!occ.inside_box(p)) return false;
        if (occ.query_point(p, static_cast<float>(now), clear, layer, self, now) != 0) return false;
        for (const ReservedSpot& r : reserved) {
            const float rr = clear + r.radius;
            if (length_sq(p - r.pos) < rr * rr) return false;
        }
        return true;
    };
    if (is_free(goal)) return goal;
    const V2 ad = approach_from - goal;
    const float base = length_sq(ad) > 0.0f ? heading_deg(ad) : 0.0f;
    float r = radius * 1.34f;
    if (r < clear) r = clear;
    for (int ring = 0; ring < 40; ++ring, r += k.destination_search_radius_increment_space) {
        const uint32_t count = static_cast<uint32_t>(std::floor(kTwoPi * r / clear)) + 1u;
        for (uint32_t i = 0; i < count; ++i) {
            const float a = 360.0f * static_cast<float>(i) / static_cast<float>(count);
            const float rad = r * (0.5f + 0.25f * (1.0f + dmath::cos(a * kDegToRad)));
            const V2 p = goal + dir_from_deg(base + a) * rad;
            if (is_free(p)) return p;
        }
    }
    return goal;
}

SpacePose eval_space_path(std::span<const SpaceWaypoint> wps, float t) {
    SpacePose out;
    if (wps.empty()) {
        out.done = true;
        return out;
    }
    if (t >= wps.back().time || wps.size() == 1) {
        out.pos = wps.back().pos;
        out.heading = wps.back().heading;
        out.speed = 0.0f;
        out.done = t >= wps.back().time;
        return out;
    }
    if (t <= wps.front().time) {
        out.pos = wps.front().pos;
        out.heading = wps.front().heading;
        out.speed = wps.front().speed;
        return out;
    }
    // Bracketing pair: wps[k].time <= t < wps[k + 1].time.
    size_t lo = 0, hi = wps.size() - 1;
    while (hi - lo > 1) {
        const size_t mid = (lo + hi) / 2;
        if (wps[mid].time <= t) lo = mid;
        else hi = mid;
    }
    const SpaceWaypoint& a = wps[lo];
    const SpaceWaypoint& b = wps[hi];
    const float dur = b.time - a.time;
    const float s = dur > 0.0f ? (t - a.time) / dur : 1.0f;
    if (a.pos == b.pos) {
        out.pos = a.pos;
        out.heading = wrap_deg(a.heading + wrap_deg(b.heading - a.heading) * s);
        out.speed = 0.0f;
        return out;
    }
    const V2 m0 = dir_from_deg(a.heading) * (a.speed * dur);
    const V2 m1 = dir_from_deg(b.heading) * (b.speed * dur);
    const float s2 = s * s, s3 = s2 * s;
    const float h00 = 2.0f * s3 - 3.0f * s2 + 1.0f, h10 = s3 - 2.0f * s2 + s;
    const float h01 = -2.0f * s3 + 3.0f * s2, h11 = s3 - s2;
    out.pos = a.pos * h00 + m0 * h10 + b.pos * h01 + m1 * h11;
    const float d00 = 6.0f * s2 - 6.0f * s, d10 = 3.0f * s2 - 4.0f * s + 1.0f;
    const float d01 = -6.0f * s2 + 6.0f * s, d11 = 3.0f * s2 - 2.0f * s;
    const V2 dp = (a.pos * d00 + m0 * d10 + b.pos * d01 + m1 * d11) * (1.0f / dur);
    out.speed = length(dp);
    out.heading = length_sq(dp) > 0.0f ? heading_deg(dp) : a.heading;
    return out;
}

}  // namespace alamo::tactical::movement
