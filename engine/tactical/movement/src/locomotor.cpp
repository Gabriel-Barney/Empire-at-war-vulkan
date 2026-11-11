#include "alamo/tactical/movement/locomotor.hpp"

#include <algorithm>
#include <span>

#include "alamo/tactical/movement/formation.hpp"
#include "alamo/tactical/movement/land_path.hpp"

namespace alamo::tactical::movement {

float remaining_length(const LandMotion& m) {
    if (m.next >= m.path.size()) return 0.0f;
    float l = distance(m.pos, m.path[m.next]);
    for (size_t i = m.next + 1; i < m.path.size(); ++i) l += distance(m.path[i - 1], m.path[i]);
    return l;
}

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

namespace {
// MOVEMENT §3.5.2 notation: line(i) = the waypoint line 0x5c4ec0 through path[i] (the port's waypoint i is retail idx + 1),
// normal = the bisector of its two segments; the last waypoint: its segment's direction (fast normalise).
void waypoint_line(const std::vector<V2>& path, size_t i, V2& l0, V2& n) {
    l0 = path[i];
    const V2 din = normalize_retail(path[i] - path[i - 1]);
    n = i + 1 < path.size() ? normalize_retail(din + normalize_retail(path[i + 1] - path[i])) : din;
}
}  // namespace

bool walk_wanted_dir(std::span<const V2> path, uint32_t next, V2 pos, const WalkExtras* x, V2& dir) {
    if (next == 0 || next >= path.size()) return false;  // on the last waypoint (idx + 1 >= count): no direction
    const V2 a = path[next - 1];
    const V2 sd = normalize_retail(path[next] - a);
    float k = 0.0f;
    if (x && x->lane) {
        // 0x41dbb0: e = (the slot's side offset (0 [port]) + the jiggle's side on a formation path) - the unit's signed side
        // offset from its segment; |e| <= 0.1 -> 0, else (e -+ 0.1) / 30
        float e = -cross(sd, pos - a);
        if (x->formation_path) e += cross(sd, x->jiggle);
        k = formation_side_correction(e);
    }
    const V2 base = normalize_retail(sd + perp_left(sd) * k);  // item 2, renormalised (fast normalise)
    dir = base;
    if (x && x->avoid && length_sq(base) > 0.0f) dir = base + x->avoid(x->avoid_ctx, base);  // item 3: + 0x509810
    return true;
}

bool walk_wanted_dir(const LandMotion& m, const WalkExtras* x, V2& dir) {
    if (!m.started) return false;
    return walk_wanted_dir(std::span<const V2>(m.path), m.next, m.pos, x, dir);
}

uint32_t resume_index(const LandMotion& q, uint8_t kind, uint32_t resume) {
    uint32_t next = q.next;
    if (kind == 2) next = std::max(next, std::min(resume, static_cast<uint32_t>(q.path.size())));
    return next == 0 ? 1u : next;
}

WalkResult walk_step(LandMotion& m, const LocomotorParams& p, float speed_cap, const PassabilityGrid* grid, const WalkExtras* x) {
    WalkResult r;
    if (m.next == 0 && !m.path.empty()) m.next = 1;  // path[0] is the start point (retail idx = next - 1)
    const float vmax = p.max_speed;
    const float vmax_u = x && x->vmax_u > 0.0f ? x->vmax_u : vmax;
    const float acc = p.accel > 0.0f ? p.accel : vmax;
    const float dec = p.decel > 0.0f ? p.decel : vmax;
    LandMotion* below = x ? x->below : nullptr;
    bool forced = x && x->forced;
    const auto yaw_of = [&](const LandMotion& k) {
        V2 wd;
        return walk_wanted_dir(k, x, wd) && length_sq(wd) > 0.0f ? heading_deg(wd) : k.yaw;
    };
    // §3.5.1 "Arrival turn, details" item 1 (state 0x15): End Move at a speed at or below one frame's deceleration - speed
    // 0; with a started path, heading for the last waypoint and dot(pos - W, n) > -5 (n = the last segment's direction) the
    // index steps on; on the last waypoint the unit has arrived where it stands (the caller runs the arrival turn / pop),
    // else the move restarts from state 0 (the start rule below, in the same frame). No started path: idle 0x16.
    if (m.state == LocoState::WalkEndMove && !(m.speed > dec)) {
        m.speed = 0.0f;
        const size_t n = m.path.size();
        if (!m.started || n == 0) {
            m.state = LocoState::WalkStopped;
            return r;
        }
        if (n >= 2 && static_cast<size_t>(m.next) + 1 == n &&
            dot(m.pos - m.path[n - 1], normalize_retail(m.path[n - 1] - m.path[n - 2])) > -5.0f) {
            m.next = static_cast<uint32_t>(n);
            m.turn_done = false;
        }
        if (m.next >= n) {
            m.state = LocoState::WalkStopped;
            r.arrived = true;
            return r;
        }
        m.state = LocoState::None;
        r.restart = true;
    }
    // §3.5.2 A: the start rule in state 0 (also idle 0x16, and a turn in place the caller dropped): no top path -> the frame
    // ends; an unstarted path is started (the 0x16 handler); |delta| <= R, or |delta| <= CE with the current waypoint's
    // speed fraction > 0 -> Start Move (0x12 -> 0x13 and the move step in the same call); else the turn in place 0xF / 0x10
    // / 0x11 (the caller runs it, WalkResult::turn). Start Move (0x12, a deployed walk's start) goes to 0x13 with no start
    // test. A started path on its last waypoint: 0x12 -> 0x15, arrived.
    const LocoState s0 = m.state;
    if (s0 == LocoState::WalkStopped || s0 == LocoState::None || s0 == LocoState::WalkStartTurn ||
        s0 == LocoState::WalkTurning || s0 == LocoState::WalkEndTurn) {
        if (m.path.empty()) {
            m.speed = 0.0f;
            m.state = LocoState::WalkStopped;
            return r;
        }
        m.started = true;
        if (m.next >= m.path.size()) {
            m.speed = 0.0f;
            m.state = LocoState::WalkStopped;
            r.arrived = true;
            return r;
        }
        const float desired = yaw_of(m);
        const float err = std::fabs(wrap_deg(desired - m.yaw));
        const float cur = wp_frac(m, m.next - 1);
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
    // §3.5.2 C.1: vmax_u = 0 -> nothing; P not started -> End Move; the speed controller B.
    if (!(vmax_u > 0.0f)) return r;
    const bool have_p = !m.path.empty();
    if (have_p && !m.started) m.state = LocoState::WalkEndMove;
    WalkSpeedIn si;
    si.v = m.speed;
    si.vmax = vmax;
    si.acc = acc;
    si.a = dec;
    if (x) si.formation = x->formation;
    si.cap = speed_cap;
    si.now = x && x->cap_now;
    si.started = have_p && m.started;
    si.turn_done = m.turn_done;
    si.rate = p.turn_rate;
    si.state = m.state;
    if (si.started && m.next < m.path.size()) {
        si.yaw_err = std::fabs(wrap_deg(yaw_of(m) - m.yaw));
        si.frac_cur = wp_frac(m, m.next - 1);
        si.frac_next = wp_frac(m, m.next);
        if (si.frac_next * vmax < m.speed) {  // B.5: D along d to line(idx + 1)
            V2 l0, n;
            waypoint_line(m.path, m.next, l0, n);
            const V2 d = normalize_retail(m.path[m.next] - m.path[m.next - 1]);
            if (length_sq(d) > 0.0f && length_sq(n) > 0.0f) si.line_d = line_distance_along(m.pos, d, l0, n);
        }
    }
    const WalkSpeedOut so = walk_speed(si);
    m.speed = so.v;
    if (so.end_move) m.state = LocoState::WalkEndMove;
    const float v = m.speed;
    // C.2: the turn budget B = R, x f = v / vmax_u in End Move (0x12 never runs the step: no Walk_Transition); s78n t26.5
    // (E): a land team container's budget is R with no f scaling. C.3: s = v.
    const bool team = x && x->container;
    float budget = p.turn_rate;
    if (m.state == LocoState::WalkEndMove && !team) budget *= v / vmax_u;
    float s = v;
    if (!(v > 0.0f)) return r;
    bool no_turn = false;
    const int32_t dirs = x ? x->directions : 1;
    for (size_t guard = 0; guard < 4 * m.path.size() + 16; ++guard) {
        // C.4: nothing to walk (no path, on the last waypoint, not started): pop into a started path below (not in End
        // Move; no more turning in this call), else End Move and the coast along the last frame's displacement (a zero
        // vector stays zero: the point is still tested and committed). s78n t26.5 (E): a container coasts along its facing.
        if (m.path.empty() || m.next >= m.path.size() || !m.started) {
            if (!below || !below->started || m.state == LocoState::WalkEndMove) {
                m.state = LocoState::WalkEndMove;
                r.coast = true;
                const V2 np = m.pos + (team ? dir_from_deg(m.yaw) : normalize_retail(m.last_dir)) * s;
                if (!forced && grid && !grid->passable_world(np, p.mask)) {
                    r.blocked = true;
                    return r;
                }
                if (!(np == m.pos)) r.moved = true;
                m.pos = np;
                return r;
            }
            std::swap(m.path, below->path);
            std::swap(m.next, below->next);
            std::swap(m.frac, below->frac);
            std::swap(m.turn_done, below->turn_done);
            std::swap(m.started, below->started);
            m.next = resume_index(m, x->below_kind, x->below_resume);
            forced = x->below_forced;
            below = nullptr;
            r.popped = true;
            no_turn = true;
            // s78n t26.5 (FIGHTER_FLYING §7.2 caller 3): the slots are re-laid along the path below's segment
            if (x->relay && m.next >= 1 && m.next < m.path.size()) x->relay->push_back(m.path[m.next] - m.path[m.next - 1]);
            continue;
        }
        // C.5: n . (pos - L0) >= -0.001 -> the waypoint is passed (0x5c4920: the turn-done byte cleared)
        V2 l0, n;
        waypoint_line(m.path, m.next, l0, n);
        if (dot(n, m.pos - l0) >= -0.001f) {
            ++m.next;
            m.turn_done = false;
            continue;
        }
        // C.6: turn toward the wanted yaw by min(budget, |delta|) (the step that covers the rest lands exactly); the leftover
        // carries over to the next waypoint of this call; leftover > 0 -> the turn-done byte
        if (!no_turn && budget > 0.0f) {
            const float w = yaw_of(m);
            const float e = std::fabs(wrap_deg(w - m.yaw));
            m.yaw = turn_toward(m.yaw, w, budget);
            budget = e > budget ? 0.0f : budget - e;
            if (budget > 0.0f) m.turn_done = true;
        }
        // C.7: the travel direction vf +0x288 (0x61e0f0): Movement_Directions_Count 1 = the facing; N > 1 = of the N
        // directions spaced 360 / N from the facing the one with the largest dot product with the wanted direction (the
        // first on ties); <= 0 = the wanted direction
        // s78n t26.5 (E): a container travels along its facing (cos yaw, sin yaw)
        V2 t = team ? dir_from_deg(m.yaw) : normalize_retail(dir_from_deg(m.yaw));
        if (dirs != 1 && !team) {
            V2 wd;
            if (walk_wanted_dir(m, x, wd) && length_sq(wd) > 0.0f) {
                if (dirs <= 0) {
                    t = normalize_retail(wd);
                } else {
                    float best = dot(t, wd);
                    for (int32_t k = 1; k < dirs; ++k) {
                        const V2 c = normalize_retail(dir_from_deg(m.yaw + 360.0f * static_cast<float>(k) / static_cast<float>(dirs)));
                        if (dot(c, wd) > best) {
                            best = dot(c, wd);
                            t = c;
                        }
                    }
                }
            }
        }
        // C.8: a degenerate segment (with d normalised exactly: along the line, or perpendicular to its normal) is skipped
        const V2 seg = m.path[m.next] - m.path[m.next - 1];
        const float sl = length(seg);
        const V2 de = sl > 0.0f ? V2{seg.x / sl, seg.y / sl} : V2{};
        if (std::fabs(n.x * de.y - n.y * de.x) == 1.0f || dot(de, n) == 0.0f) {
            ++m.next;
            m.turn_done = false;
            if (r.degen_skips < 255) ++r.degen_skips;
            continue;
        }
        // C.9: D = |pos - X|, X the point of line(idx + 1) on the line through pos along d (fast normalise); tau = t . d,
        // tau = 0 -> the waypoint is skipped (quirk); s' = |D / tau|
        const V2 d = normalize_retail(seg);
        const float dn = dot(d, n);
        const float tau = dot(t, d);
        if (tau == 0.0f || dn == 0.0f) {
            ++m.next;
            m.turn_done = false;
            if (tau == 0.0f && r.tau_skips < 255) ++r.tau_skips;
            continue;
        }
        const float mu = cross(l0 - m.pos, d) / dn;
        const V2 xp = l0 + perp_left(n) * mu;
        const float sp = std::fabs(length(m.pos - xp) / tau);
        // C.10: pos += t min(s, s'); not forced (path +0x5c, or the planner's escape leg) and the point fails 0x50e280 ->
        // blocked, no commit
        const V2 np = m.pos + t * std::min(s, sp);
        if (!(forced || m.next < m.escape_until) && grid && !grid->passable_world(np, p.mask)) {
            r.blocked = true;
            return r;
        }
        m.pos = np;
        r.moved = true;
        if (s <= sp) {
            // C.11: the budget ends before the line; rest = (s' - s) + the lengths after path[next] ([port] 2-D);
            // v^2 / (2a) > rest -> End Move unless a started path lies below
            float rest = sp - s;
            for (size_t i = static_cast<size_t>(m.next) + 1; i < m.path.size(); ++i) rest += distance(m.path[i - 1], m.path[i]);
            if ((v * v) / (2.0f * dec) > rest && !(below && below->started)) m.state = LocoState::WalkEndMove;
            return r;
        }
        ++m.next;  // s > s': on the line - the next waypoint with the rest of the budget
        m.turn_done = false;
        s -= sp;
        // s78n t26.5 (E, FIGHTER_FLYING §7.2 caller 3): a container re-lays its slots along the new segment
        if (x && x->relay && m.next < m.path.size()) x->relay->push_back(m.path[m.next] - m.path[m.next - 1]);
    }
    return r;
}

}  // namespace alamo::tactical::movement
