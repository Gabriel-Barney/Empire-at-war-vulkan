#include "alamo/tactical/movement/locomotor.hpp"

#include <algorithm>

#include "alamo/tactical/movement/formation.hpp"
#include "alamo/tactical/movement/land_path.hpp"

namespace alamo::tactical::movement {

float remaining_length(const LandMotion& m) {
    if (m.next >= m.path.size()) return 0.0f;
    float l = distance(m.pos, m.path[m.next]);
    for (size_t i = m.next + 1; i < m.path.size(); ++i) l += distance(m.path[i - 1], m.path[i]);
    return l;
}

namespace {
bool move_ok(const PassabilityGrid* grid, V2 a, V2 b, uint32_t mask) {
    if (!grid) return true;
    // Thin walk (planned segments were checked with the +-kSegmentBand thick test).
    return walk_cells(a, b, [&](int32_t cx, int32_t cy) { return grid->passable_cell(cx, cy, mask); });
}
}  // namespace

WalkSpeedOut walk_speed(const WalkSpeedIn& in) {
    WalkSpeedOut o;
    const float v = in.v;
    float t = in.vmax;  // B.1 / B.2
    if (in.formation < 3.0e38f) {
        t = in.formation;
        if (!(t > 0.0f)) o.end_move = true;
    }
    if (!in.started) t = in.a;  // B.3
    const bool start_move = in.state == LocoState::WalkStartMove;
    // B.4 corner cap: 0x20afe0 is |wrap(yaw - wanted)| < |R| (strict)
    if (!in.turn_done && !(in.yaw_err < std::fabs(in.rate)) && !start_move) {
        t = std::min(t, in.frac_cur * in.vmax);
        if (!(t > 0.0f)) o.end_move = true;
    }
    // B.5 brake-ahead toward line(idx + 1), the comparison in double
    if (!start_move) {
        const float vn = in.frac_next * in.vmax;
        if (vn < v && in.a > 0.0f) {
            const double bd = (static_cast<double>(v) * static_cast<double>(v) - static_cast<double>(vn) * static_cast<double>(vn)) /
                              (2.0 * static_cast<double>(in.a));
            if (static_cast<double>(in.line_d) <= bd) {
                t = std::min(t, vn);
                if (!(t > 0.0f)) o.end_move = true;
            }
        }
        t = std::min(t, in.cap);  // B.6
    }
    if (o.end_move || in.state == LocoState::WalkEndMove) t = in.a;  // B.7 (no Walk_Transition)
    float nv = v;  // B.8
    if (t > nv) nv += std::min(in.acc, t - nv);
    else if (t < nv) nv = in.now && !start_move ? t : nv - std::min(in.a, nv - t);
    nv = std::min(nv, in.vmax);
    if (!(nv > 0.0f)) nv = 0.0f;
    o.v = nv;
    return o;
}

float line_distance_along(V2 pos, V2 d, V2 l0, V2 n) {
    const float dn = dot(d, n);
    if (dn == 0.0f) return 3.0e38f;
    const float mu = cross(l0 - pos, d) / dn;
    const V2 xp = l0 + perp_left(n) * mu;
    return length(pos - xp);
}

WalkResult walk_step(LandMotion& m, const LocomotorParams& p, float speed_cap, const PassabilityGrid* grid, const WalkExtras* x) {
    WalkResult r;
    // OI tactical: steering point of waypoint i (formation jiggle on every waypoint but the last).
    const bool jig = x && (x->jiggle.x != 0.0f || x->jiggle.y != 0.0f);
    auto wp = [&](size_t i) {
        if (!jig || i + 1 >= m.path.size()) return m.path[i];
        const V2 q = m.path[i] + x->jiggle;
        return (!grid || grid->passable_world(q, p.mask)) ? q : m.path[i];
    };
    if (m.next >= m.path.size()) {
        m.speed = 0.0f;
        m.state = LocoState::WalkStopped;
        return r;
    }
    const float tail = x && x->tail > 0.0f ? x->tail : 0.0f;  // s78n t20: a started path goes on under this record
    const float vmax = p.max_speed;
    const float acc = p.accel > 0.0f ? p.accel : vmax;
    const float dec = p.decel > 0.0f ? p.decel : vmax;
    // s78n t26.2 (MOVEMENT §3.5.1 "Arrival turn, details" item 1, state 0x15): End Move at a speed at or below one frame's
    // deceleration stops (speed 0); away from the last point the move resumes from state 0 (the start rule below, in the
    // same frame) - after a new path taken over in End Move, or the End Move a fraction 0 sent it to. [port] until s78n
    // t26.4: heading for the last waypoint and dot(pos - W, n) > -5 (n = the last segment's direction), where retail
    // arrives where it stands, the unit goes on in End Move at T = a to the point (the snap).
    if (m.state == LocoState::WalkEndMove && !(m.speed > dec)) {
        const size_t n = m.path.size();
        const bool at_end = n >= 2 && static_cast<size_t>(m.next) + 1 == n &&
                            dot(m.pos - m.path[n - 1], normalize_retail(m.path[n - 1] - m.path[n - 2])) > -5.0f;
        if (!at_end) {
            m.speed = 0.0f;
            m.state = LocoState::None;
        }
    }
    const V2 to_wp = wp(m.next) - m.pos;
    const float desired = length_sq(to_wp) > 0.0f ? heading_deg(to_wp) : m.yaw;
    // s78n t26.2 (MOVEMENT §3.5.2 A): the start rule in state 0 (also idle 0x16, and a turn in place the caller dropped):
    // |delta| <= R, or |delta| <= CE with the current waypoint's speed fraction > 0 -> Start Move (0x12 -> 0x13 and the
    // move step in the same call); else the turn in place 0xF / 0x10 / 0x11 (the caller runs it, WalkResult::turn).
    // Start Move (0x12, a deployed walk's start) goes to 0x13 with no start test.
    const LocoState s0 = m.state;
    if (s0 == LocoState::WalkStopped || s0 == LocoState::None || s0 == LocoState::WalkStartTurn ||
        s0 == LocoState::WalkTurning || s0 == LocoState::WalkEndTurn) {
        const float err = std::fabs(wrap_deg(desired - m.yaw));
        const float cur = m.next >= 1 ? wp_frac(m, m.next - 1) : 1.0f;
        if (err <= p.turn_rate || (err <= p.close_enough_angle && cur > 0.0f)) {
            m.state = LocoState::WalkMoving;
        } else {
            m.state = LocoState::WalkStartTurn;
            m.speed = 0.0f;
            r.turn = true;
            r.want = desired;
            return r;
        }
    } else if (s0 == LocoState::WalkStartMove) {
        m.state = LocoState::WalkMoving;
    }
    const float remaining = remaining_length(m);
    if (!(remaining > 1e-3f)) {  // already on the last waypoint
        if (m.next >= m.escape_until && !move_ok(grid, m.pos, m.path.back(), p.mask)) {
            r.blocked = true;
            return r;
        }
        m.pos = m.path.back();
        m.next = static_cast<uint32_t>(m.path.size());
        m.turn_done = false;
        if (!(tail > 0.0f)) {
            m.speed = 0.0f;
            m.state = LocoState::WalkStopped;
        }
        r.moved = true;
        r.arrived = true;
        return r;
    }
    // s78n t26.2 (MOVEMENT §3.5.2 B): the speed controller. The wanted yaw is the direction the walk takes to its waypoint
    // ([port] until t26.4); line(idx + 1) is the waypoint line through path[next] (normal: the bisector of its two segments,
    // the last waypoint: its segment's direction), d the current segment's direction.
    WalkSpeedIn si;
    si.v = m.speed;
    si.vmax = vmax;
    si.acc = acc;
    si.a = dec;
    if (x) si.formation = x->formation;
    si.cap = speed_cap;
    si.now = x && x->cap_now;
    si.turn_done = m.turn_done;
    si.yaw_err = std::fabs(wrap_deg(desired - m.yaw));
    si.rate = p.turn_rate;
    si.frac_cur = m.next >= 1 ? wp_frac(m, m.next - 1) : 1.0f;
    si.frac_next = wp_frac(m, m.next);
    si.state = m.state;
    if (si.frac_next * vmax < m.speed && m.next >= 1) {
        const V2 w = wp(m.next);
        const V2 d = normalize_retail(w - wp(m.next - 1));
        const V2 n = static_cast<size_t>(m.next) + 1 < m.path.size()
                         ? normalize_retail(normalize_retail(w - wp(m.next - 1)) + normalize_retail(wp(m.next + 1) - w))
                         : d;
        if (length_sq(d) > 0.0f && length_sq(n) > 0.0f) si.line_d = line_distance_along(m.pos, d, w, n);
    }
    const WalkSpeedOut so = walk_speed(si);
    m.speed = so.v;
    if (so.end_move) m.state = LocoState::WalkEndMove;
    if (!(m.speed > 0.0f)) {
        m.yaw = turn_toward(m.yaw, desired, p.turn_rate);
        return r;  // held by the speed cap
    }
    // Move step: spend the budget along the waypoints.
    float budget = m.speed;
    V2 pos = m.pos;
    uint32_t next = m.next;
    V2 travel{};
    bool steered = false;
    while (budget > 0.0f && next < m.path.size()) {
        const V2 aim = wp(next);
        const V2 d = aim - pos;
        const float l = length(d);
        if (l <= budget) {
            pos = aim;
            budget -= l;
            ++next;
            if (l > 0.0f) travel = d;
        } else {
            V2 dir = normalize_retail(d);
            V2 heading_vec = d;
            if (x && x->side_error && next >= 1) {
                // OI tactical: side-error correction (§4.4): segment direction + normal x correction(error).
                const V2 a = wp(next - 1);
                const V2 sd = normalize_retail(aim - a);
                if (length_sq(sd) > 0.0f) {
                    const float c = formation_side_correction(-cross(sd, pos - a));
                    if (c != 0.0f) {
                        dir = normalize_retail(sd + perp_left(sd) * c);
                        heading_vec = dir;
                        steered = true;  // s78n t20: it walks beside its line - the waypoint is passed by its line
                    }
                }
            }
            if (x && x->avoid && next >= 1 && static_cast<size_t>(next) + 1 < m.path.size()) {
                // s78n t18 (MOVEMENT 5.4.1 "How Walk combines the vector", not on the last leg): the heading is the
                // segment direction + the lane term that brings the unit back to its line (0x41dbb0: the side error over
                // FormationMaximumSideError past FormationMinimumSideError), renormalised, + the vector 0x509810.
                const V2 from = wp(next - 1);
                const V2 sd = normalize_retail(aim - from);
                if (length_sq(sd) > 0.0f) {
                    const V2 base = normalize_retail(sd + perp_left(sd) * formation_side_correction(-cross(sd, pos - from)));
                    const V2 v = x->avoid(x->avoid_ctx, base);
                    if (v.x != 0.0f || v.y != 0.0f) {
                        dir = normalize_retail(base + v);
                        heading_vec = dir;
                        steered = true;
                    }
                }
            }
            pos = pos + dir * budget;
            budget = 0.0f;
            travel = heading_vec;
        }
    }
    if (steered && next >= 1 && static_cast<size_t>(next) + 1 < m.path.size()) {
        // a deflected unit passes its waypoint by crossing the waypoint's line (normal = the bisector of the two segments)
        const V2 w = wp(next);
        const V2 n = normalize_retail(normalize_retail(w - wp(next - 1)) + normalize_retail(wp(next + 1) - w));
        if (length_sq(n) > 0.0f && dot(pos - w, n) >= 0.0f) ++next;
    } else if (steered && next >= 1 && static_cast<size_t>(next) + 1 == m.path.size()) {
        // s78n t20 (MOVEMENT §5.4.1 "How Walk combines" item 4): the last waypoint too is passed by its line (normal = its
        // segment's direction) - a unit that walks beside its line (it resumed the path from a detour or a push) arrives
        // where it crosses it instead of walking on past the point
        const V2 w = wp(next);
        const V2 n = normalize_retail(w - wp(next - 1));
        if (length_sq(n) > 0.0f && dot(pos - w, n) >= 0.0f) ++next;
    }
    // Passability check before commit (§3.5 step 4).
    if (m.next >= m.escape_until && !move_ok(grid, m.pos, pos, p.mask)) {
        m.speed = 0.0f;
        r.blocked = true;
        return r;
    }
    m.pos = pos;
    if (next != m.next) m.turn_done = false;  // 0x5c4920: +0x6c := 0 at every index step
    m.next = next;
    r.moved = true;
    if (length_sq(travel) > 0.0f) {
        // s78n t26.2 (MOVEMENT §3.5.2 C.6): leftover rate after the turn (the exact landing) sets the turn-done byte
        const float want = heading_deg(travel);
        if (std::fabs(wrap_deg(want - m.yaw)) < p.turn_rate) m.turn_done = true;
        m.yaw = turn_toward(m.yaw, want, p.turn_rate);
    }
    if (m.next >= m.path.size()) {
        r.arrived = true;
        m.turn_done = false;
        if (tail > 0.0f) {
            m.state = LocoState::WalkMoving;
        } else {
            m.speed = 0.0f;
            m.state = LocoState::WalkStopped;
        }
    } else if (m.state != LocoState::WalkEndMove) {
        // s78n t26.2 (MOVEMENT §3.5.2 C.11): v^2 / (2a) > the rest of the path -> End Move, unless a started path lies below
        // (then it walks on into that path without braking). End Move holds until 0x15.
        const float rest = remaining_length(m);
        m.state = !(tail > 0.0f) && (m.speed * m.speed) / (2.0f * dec) > rest ? LocoState::WalkEndMove : LocoState::WalkMoving;
    }
    return r;
}

}  // namespace alamo::tactical::movement
