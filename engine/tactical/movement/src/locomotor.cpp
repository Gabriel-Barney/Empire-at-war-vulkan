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
    const V2 to_wp = wp(m.next) - m.pos;
    const float desired = length_sq(to_wp) > 0.0f ? heading_deg(to_wp) : m.yaw;
    // Start rule (§3.5 step 1).
    if (m.state == LocoState::WalkStopped || m.state == LocoState::WalkStartTurn) {
        const float err = std::fabs(wrap_deg(desired - m.yaw));
        if (err <= p.turn_rate || err <= p.close_enough_angle) {
            m.state = LocoState::WalkMoving;
        } else {
            m.state = LocoState::WalkStartTurn;
            m.yaw = turn_toward(m.yaw, desired, p.turn_rate);
            m.speed = 0.0f;
            return r;
        }
    }
    // Speed controller (§3.5 step 2).
    const float remaining = remaining_length(m);
    const float tail = x && x->tail > 0.0f ? x->tail : 0.0f;  // s78n t20: a started path goes on under this record
    if (!(remaining > 1e-3f)) {  // already on the last waypoint
        if (m.next >= m.escape_until && !move_ok(grid, m.pos, m.path.back(), p.mask)) {
            r.blocked = true;
            return r;
        }
        m.pos = m.path.back();
        m.next = static_cast<uint32_t>(m.path.size());
        if (!(tail > 0.0f)) {
            m.speed = 0.0f;
            m.state = LocoState::WalkStopped;
        }
        r.moved = true;
        r.arrived = true;
        return r;
    }
    float target = std::min(p.max_speed, speed_cap);
    if (p.decel > 0.0f) {
        const float vb = std::sqrt(2.0f * p.decel * (remaining + tail));
        if (vb < target) target = vb;
    }
    if (target < 0.0f) target = 0.0f;
    if (m.speed < target) m.speed = std::min(target, m.speed + (p.accel > 0.0f ? p.accel : p.max_speed));
    else if (x && x->cap_now) m.speed = target;  // s78n t20: the collision cap's "at once" flag
    else m.speed = std::max(target, m.speed - (p.decel > 0.0f ? p.decel : p.max_speed));
    m.speed = std::clamp(m.speed, 0.0f, p.max_speed);
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
    }
    // Passability check before commit (§3.5 step 4).
    if (m.next >= m.escape_until && !move_ok(grid, m.pos, pos, p.mask)) {
        m.speed = 0.0f;
        r.blocked = true;
        return r;
    }
    m.pos = pos;
    m.next = next;
    r.moved = true;
    if (length_sq(travel) > 0.0f) m.yaw = turn_toward(m.yaw, heading_deg(travel), p.turn_rate);
    if (m.next >= m.path.size()) {
        r.arrived = true;
        if (tail > 0.0f) {
            m.state = LocoState::WalkMoving;
        } else {
            m.speed = 0.0f;
            m.state = LocoState::WalkStopped;
        }
    } else if (remaining + tail - m.speed <= (m.speed * m.speed) / (2.0f * (p.decel > 0.0f ? p.decel : p.max_speed))) {
        m.state = LocoState::WalkEndMove;
    } else {
        m.state = LocoState::WalkMoving;
    }
    return r;
}

bool try_nudge(LandMotion& m, V2 offset, uint32_t mask, const PassabilityGrid* grid) {
    if (!(length_sq(offset) > 0.0f)) return false;
    const V2 np = m.pos + offset;
    if (!move_ok(grid, m.pos, np, mask)) return false;
    m.pos = np;
    return true;
}

}  // namespace alamo::tactical::movement
